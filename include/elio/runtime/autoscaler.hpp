#pragma once

#include "autoscaler_config.hpp"
#include "autoscaler_triggers.hpp"
#include "autoscaler_actions.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <stop_token>

namespace elio::runtime {

// Main autoscaler class
template<typename Scheduler, typename... Triggers>
class autoscaler_impl {
public:
    explicit autoscaler_impl(const autoscaler_config& config)
        : config_(config), running_(false), scheduler_(nullptr) {}

    ~autoscaler_impl() { stop(); }

    // Non-copyable / non-movable
    autoscaler_impl(const autoscaler_impl&) = delete;
    autoscaler_impl& operator=(const autoscaler_impl&) = delete;

    void start(Scheduler* sched) {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return;
        scheduler_ = sched;
        thread_ = std::jthread([this](std::stop_token st) { run(st); });
    }

    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) return;
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
    }

    autoscaler_config config() const {
        std::lock_guard<std::mutex> lock(config_mutex_);
        return config_;
    }
    void update_config(const autoscaler_config& config) {
        std::lock_guard<std::mutex> lock(config_mutex_);
        config_ = config;
    }

private:
    void run(std::stop_token st) {
        auto last_idle_time = std::chrono::steady_clock::now();

        while (!st.stop_requested() && running_.load()) {
            auto now = std::chrono::steady_clock::now();

            // Take a consistent snapshot of config under lock to avoid
            // torn reads from concurrent update_config() calls.
            autoscaler_config cfg;
            {
                std::lock_guard<std::mutex> lock(config_mutex_);
                cfg = config_;
            }

            // Collect metrics
            size_t pending = scheduler_->pending_tasks();
            size_t num_workers = scheduler_->num_threads();

            // Handle each trigger
            if constexpr (sizeof...(Triggers) > 0) {
                // Custom triggers provided
                ((handle_trigger<Triggers>(pending, num_workers, now, last_idle_time, cfg)), ...);
            } else {
                // Default behavior: scale up on overload, scale down on idle
                handle_default_overload(pending, num_workers, cfg);
                handle_default_idle(pending, num_workers, now, last_idle_time, cfg);
            }

            // Update idle time
            if (pending >= cfg.idle_threshold) {
                last_idle_time = now;
            }

            std::this_thread::sleep_for(cfg.tick_interval);
        }
    }

    // Compute proportional scale-up step.
    // Returns 0 when no scaling is needed, otherwise a positive step bounded by headroom.
    // Roughly: step = ceil(pending / overload_threshold) - 1, with a minimum of 1 when
    // we are already above overload_threshold, capped by (max_workers - num_workers).
    size_t compute_scale_up_step(size_t pending, size_t num_workers, const autoscaler_config& cfg) const noexcept {
        if (pending <= cfg.overload_threshold) return 0;
        if (num_workers >= cfg.max_workers) return 0;
        const size_t overload = (cfg.overload_threshold == 0) ? 1 : cfg.overload_threshold;
        // ceil(pending / overload)
        size_t target_ratio = (pending + overload - 1) / overload;
        // -1 because the existing num_workers already cover one "overload" worth of work
        size_t step = (target_ratio > 1) ? (target_ratio - 1) : 1;
        // Always make at least 1 step of progress while overloaded
        if (step == 0) step = 1;
        const size_t headroom = cfg.max_workers - num_workers;
        if (step > headroom) step = headroom;
        return step;
    }

    // Scale-down hysteresis: require pending strictly below half of idle_threshold
    // (computed without integer division so idle_threshold==1 still has a sensible
    // boundary). This prevents flapping when pending hovers near idle_threshold.
    bool below_idle_with_hysteresis(size_t pending, const autoscaler_config& cfg) const noexcept {
        // pending < idle_threshold / 2  <=>  pending * 2 < idle_threshold
        return pending * 2 < cfg.idle_threshold;
    }

    // Default overload handler - scale up when overloaded (proportional).
    void handle_default_overload(size_t pending, size_t num_workers, const autoscaler_config& cfg) {
        const size_t step = compute_scale_up_step(pending, num_workers, cfg);
        if (step > 0) {
            scheduler_->set_thread_count(num_workers + step);
        }
    }

    // Default idle handler - scale down when idle (with hysteresis).
    void handle_default_idle(size_t pending, size_t num_workers,
                            auto now, auto& last_idle_time, const autoscaler_config& cfg) {
        if (below_idle_with_hysteresis(pending, cfg) && num_workers > cfg.min_workers) {
            auto idle_duration = now - last_idle_time;
            if (idle_duration > cfg.idle_delay) {
                scheduler_->set_thread_count(num_workers - 1);
            }
        }
    }

    // Handle on_overload trigger
    template<typename... Actions>
    void handle_trigger(on_overload<Actions...>, size_t pending, size_t num_workers,
                       auto now, auto& last_idle_time, const autoscaler_config& cfg) {
        (void)num_workers;
        (void)now;
        (void)last_idle_time;

        if (pending > cfg.overload_threshold) {
            size_t current = scheduler_->num_threads();
            const size_t step = compute_scale_up_step(pending, current, cfg);
            if (step > 0) {
                scheduler_->set_thread_count(current + step);
                // Success detection via re-reading num_threads() is inherently
                // racy (another entity could change thread count between set and
                // read). Treat step > 0 as 'attempted' since the step computation
                // already validates headroom against max_workers.
                bool success = true;
                if constexpr (sizeof...(Actions) > 0) {
                    execute_overload_actions<Actions...>(success, pending);
                }
            }
        }
    }

    // Handle on_idle trigger
    template<typename... Actions>
    void handle_trigger(on_idle<Actions...>, size_t pending, size_t num_workers,
                       auto now, auto& last_idle_time, const autoscaler_config& cfg) {
        (void)num_workers;

        if (below_idle_with_hysteresis(pending, cfg)) {
            auto idle_duration = now - last_idle_time;
            if (idle_duration > cfg.idle_delay) {
                size_t current = scheduler_->num_threads();
                if (current > cfg.min_workers) {
                    scheduler_->set_thread_count(current - 1);
                    // Same rationale as on_overload: treat the attempt as success
                    // since we validated headroom before calling set_thread_count.
                    bool success = true;
                    if constexpr (sizeof...(Actions) > 0) {
                        auto idle_secs = std::chrono::duration_cast<std::chrono::seconds>(idle_duration);
                        execute_idle_actions<Actions...>(success, pending, idle_secs);
                    }
                }
            }
        }
    }

    // Handle on_block trigger
    template<typename... Actions>
    void handle_trigger(on_block<Actions...>, size_t pending, size_t num_workers,
                       auto now, auto& last_idle_time, const autoscaler_config& cfg) {
        (void)pending;
        (void)last_idle_time;

        for (size_t i = 0; i < num_workers; ++i) {
            auto* worker = scheduler_->get_worker(i);
            if (worker && !worker->is_idle()) {
                auto last_time = worker->last_task_time();
                auto blocked_duration = now - last_time;
                if (blocked_duration > cfg.block_threshold) {
                    // Execute block actions
                    if constexpr (sizeof...(Actions) > 0) {
                        auto blocked_ms = std::chrono::duration_cast<std::chrono::milliseconds>(blocked_duration);
                        execute_block_actions<Actions...>(i, blocked_ms);
                    }
                }
            }
        }
    }

    // Execute on_overload actions
    template<typename Action, typename... Rest>
    void execute_overload_actions(bool success, size_t pending) {
        execute_single_action<Action>(success, pending);
        if constexpr (sizeof...(Rest) > 0) {
            execute_overload_actions<Rest...>(success, pending);
        }
    }

    // Execute on_idle actions
    template<typename Action, typename... Rest>
    void execute_idle_actions(bool success, size_t pending, std::chrono::seconds idle_time) {
        execute_single_action<Action>(success, pending, idle_time);
        if constexpr (sizeof...(Rest) > 0) {
            execute_idle_actions<Rest...>(success, pending, idle_time);
        }
    }

    // Execute on_block actions
    template<typename Action, typename... Rest>
    void execute_block_actions(size_t worker_id, std::chrono::milliseconds blocked_time) {
        execute_single_block_action<Action>(worker_id, blocked_time);
        if constexpr (sizeof...(Rest) > 0) {
            execute_block_actions<Rest...>(worker_id, blocked_time);
        }
    }

    // Single action execution - overload
    template<typename Action>
    void execute_single_action(bool success, size_t pending) {
        using T = std::decay_t<Action>;

        if constexpr (std::is_same_v<T, log>) {
            Action{}(scheduler_, pending);
        } else if constexpr (std::is_same_v<T, null>) {
            // no-op
        } else if constexpr (is_on_success_v<Action>) {
            if (success) {
                using H = typename Action::handler_type;
                H{}(scheduler_, pending);
            }
        } else if constexpr (is_on_failure_v<Action>) {
            if (!success) {
                using H = typename Action::handler_type;
                H{}(scheduler_, pending);
            }
        } else if constexpr (std::is_invocable_v<Action, Scheduler*, size_t>) {
            Action{}(scheduler_, pending);
        }
    }

    // Single action execution - idle
    template<typename Action>
    void execute_single_action(bool success, size_t pending, std::chrono::seconds idle_time) {
        using T = std::decay_t<Action>;

        if constexpr (std::is_same_v<T, log>) {
            Action{}(scheduler_, pending, idle_time);
        } else if constexpr (std::is_same_v<T, null>) {
            // no-op
        } else if constexpr (is_on_success_v<Action>) {
            if (success) {
                using H = typename Action::handler_type;
                H{}(scheduler_, pending, idle_time);
            }
        } else if constexpr (is_on_failure_v<Action>) {
            if (!success) {
                using H = typename Action::handler_type;
                H{}(scheduler_, pending, idle_time);
            }
        } else if constexpr (std::is_invocable_v<Action, Scheduler*, size_t, std::chrono::seconds>) {
            Action{}(scheduler_, pending, idle_time);
        }
    }

    // Single action execution - block
    template<typename Action>
    void execute_single_block_action(size_t worker_id, std::chrono::milliseconds blocked_time) {
        using T = std::decay_t<Action>;

        if constexpr (std::is_same_v<T, log>) {
            Action{}(scheduler_, worker_id, blocked_time);
        } else if constexpr (std::is_same_v<T, null>) {
            // no-op
        } else if constexpr (std::is_invocable_v<Action, Scheduler*, size_t, std::chrono::milliseconds>) {
            Action{}(scheduler_, worker_id, blocked_time);
        }
    }

    autoscaler_config config_;
    mutable std::mutex config_mutex_;
    std::atomic<bool> running_;
    Scheduler* scheduler_;
    std::jthread thread_;
};

// Convenience type alias
template<typename Scheduler = runtime::scheduler, typename... Triggers>
using autoscaler = autoscaler_impl<Scheduler, Triggers...>;

} // namespace elio::runtime
