#pragma once

#include "promise_base.hpp"

namespace elio::coro::this_coro {

/// Return the cooperative cancellation token for the currently executing Elio
/// task. Code outside an active Elio runtime frame receives a default token that
/// is never cancelled.
[[nodiscard]] inline elio::coro::cancel_token cancel_token() noexcept {
    auto* frame = promise_base::current_frame();
    if (!frame) return {};

    auto context = frame->execution_context();
    return context ? context->get_cancel_token() : elio::coro::cancel_token{};
}

} // namespace elio::coro::this_coro
