#pragma once

#include <type_traits>
#include "task.hpp"

namespace elio::detail {

template<typename T> struct task_value;
template<typename T> struct task_value<coro::task<T>> { using type = T; };
template<typename T> using task_value_t = typename task_value<T>::type;

template<typename T> struct is_task : std::false_type {};
template<typename T> struct is_task<coro::task<T>> : std::true_type {};
template<typename T> inline constexpr bool is_task_v = is_task<T>::value;

} // namespace elio::detail
