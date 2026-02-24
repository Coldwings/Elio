#pragma once

#include "autoscaler_config.hpp"
#include "autoscaler_triggers.hpp"
#include "autoscaler_actions.hpp"
#include <thread>
#include <atomic>
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
        if (running_.load()) return;
        scheduler_ = sched;
        running_.store(true);
        thread_ = std::jthread([this](std::stop_token st) { run(st); });
    }

    void stop() {
        if (!running_.load()) return;
        running_.store(false);
        if (thread_.joinable()) {
            thread_.request_stop();
            thread_.join();
        }
    }

    const autoscaler_config& config() const { return config_; }
    void update_config(const autoscaler_config& config) { config_ = config; }

private:
    void run(std::stop_token st) {
        auto last_idle_time = std::chrono::steady_clock::now();

        while (!st.stop_requested() && running_.load()) {
            auto now = std::chrono::steady_clock::now();

            // Collect metrics
            size_t pending = scheduler_->pending_tasks();
            size_t num_workers = scheduler_->num_threads();

            // Handle each trigger
            if constexpr (sizeof...(Triggers) > 0) {
                // Custom triggers provided
                ((handle_trigger<Triggers>(pending, num_workers, now, last_idle_time)), ...);
            } else {
                // Default behavior: scale up on overload, scale down on idle
                handle_default_overload(pending, num_workers);
                handle_default_idle(pending, num_workers, now, last_idle_time);
            }

            // Update idle time
            if (pending >= config_.idle_threshold) {
                last_idle_time = now;
            }

            std::this_thread::sleep_for(config_.tick_interval);
        }
    }

    // Default overload handler - scale up when overloaded
    void handle_default_overload(size_t pending, size_t num_workers) {
        if (pending > config_.overload_threshold && num_workers < config_.max_workers) {
            scheduler_->set_thread_count(num_workers + 1);
        }
    }

    // Default idle handler - scale down when idle
    void handle_default_idle(size_t pending, size_t num_workers,
                            auto now, auto& last_idle_time) {
        if (pending < config_.idle_threshold && num_workers > config_.min_workers) {
            auto idle_duration = now - last_idle_time;
            if (idle_duration > config_.idle_delay) {
                scheduler_->set_thread_count(num_workers - 1);
            }
        }
    }

    // Handle on_overload trigger
    template<typename... Actions>
    void handle_trigger(on_overload<Actions...>, size_t pending, size_t num_workers,
                       auto now, auto& last_idle_time) {
        (void)num_workers;
        (void)last_idle_time;

        if (pending > config_.overload_threshold) {
            // Try to scale up
            size_t current = scheduler_->num_threads();
            if (current < config_.max_workers) {
                bool success = scheduler_->set_thread_count(current + 1);
                // Execute actions
                if constexpr (sizeof...(Actions) > 0) {
                    execute_overload_actions<Actions...>(success, pending);
                }
            }
        }
    }

    // Handle on_idle trigger
    template<typename... Actions>
    void handle_trigger(on_idle<Actions...>, size_t pending, size_t num_workers,
                       auto now, auto& last_idle_time) {
        (void)num_workers;

        if (pending < config_.idle_threshold) {
            auto idle_duration = now - last_idle_time;
            if (idle_duration > config_.idle_delay) {
                // Try to scale down
                size_t current = scheduler_->num_threads();
                if (current > config_.min_workers) {
                    bool success = scheduler_->set_thread_count(current - 1);
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
                       auto now, auto& last_idle_time) {
        (void)pending;
        (void)last_idle_time;

        for (size_t i = 0; i < num_workers; ++i) {
            auto* worker = scheduler_->get_worker(i);
            if (worker && !worker->is_idle()) {
                auto last_time = worker->last_task_time();
                auto blocked_duration = now - last_time;
                if (blocked_duration > config_.block_threshold) {
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
    std::atomic<bool> running_;
    Scheduler* scheduler_;
    std::jthread thread_;
};

// Convenience type alias
template<typename Scheduler = runtime::scheduler, typename... Triggers>
using autoscaler = autoscaler_impl<Scheduler, Triggers...>;

} // namespace elio::runtime
