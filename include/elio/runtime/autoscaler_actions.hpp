#pragma once

#include "autoscaler_config.hpp"
#include <elio/log/macros.hpp>
#include <type_traits>
#include <functional>

namespace elio::runtime {

// === Combinators (forward declaration) ===

// Success branch - wraps a handler
template<typename Handler>
struct on_success {
    using handler_type = Handler;
};

// Failure branch - wraps a handler
template<typename Handler>
struct on_failure {
    using handler_type = Handler;
};

// === Type traits ===

template<typename T>
struct is_on_success : std::false_type {};

template<typename H>
struct is_on_success<on_success<H>> : std::true_type {};

template<typename T>
inline constexpr bool is_on_success_v = is_on_success<std::decay_t<T>>::value;

template<typename T>
struct is_on_failure : std::false_type {};

template<typename H>
struct is_on_failure<on_failure<H>> : std::true_type {};

template<typename T>
inline constexpr bool is_on_failure_v = is_on_failure<std::decay_t<T>>::value;

// === Actions ===

// Scale up action
template<typename... Handlers>
struct scale_up {};

// Scale down action
template<typename... Handlers>
struct scale_down {};

// Log action - generic
struct log {
    template<typename Scheduler, typename... Args>
    void operator()(Scheduler*, Args&&...) const {
        ELIO_LOG_WARNING("Autoscaler action triggered");
    }
};

// No-op
struct null {
    template<typename Scheduler, typename... Args>
    void operator()(Scheduler*, Args&&...) const {}
};

} // namespace elio::runtime
