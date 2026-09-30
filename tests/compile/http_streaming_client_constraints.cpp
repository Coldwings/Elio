#include <elio/http/http_client.hpp>

namespace {
using elio::coro::cancel_token;
using elio::coro::task;
using elio::http::client;
using elio::http::request;
using elio::http::response;
using elio::http::response_body_reader;
using elio::http::url;

struct value_handler {
    task<void> operator()(const response&, response_body_reader&, cancel_token) const {
        co_return;
    }
};

struct const_reference_handler {
    task<void> operator()(const response&, response_body_reader&, const cancel_token&) const {
        co_return;
    }
};

struct mutable_reference_handler {
    task<void> operator()(const response&, response_body_reader&, cancel_token&) const;
};

struct rvalue_reference_handler {
    task<void> operator()(const response&, response_body_reader&, cancel_token&&) const;
};

struct wrong_result_handler {
    int operator()(const response&, response_body_reader&, cancel_token) const;
};

template<typename Handler>
concept accepted_handler = requires(client& connection, request req, url target,
                                     cancel_token token, Handler handler) {
    connection.with_response(std::move(req), std::move(target), std::move(token),
                             std::move(handler));
};

static_assert(accepted_handler<value_handler>);
static_assert(accepted_handler<const_reference_handler>);
static_assert(!accepted_handler<mutable_reference_handler>);
static_assert(!accepted_handler<rvalue_reference_handler>);
static_assert(!accepted_handler<wrong_result_handler>);

[[maybe_unused]] auto instantiate_value_handler(client& connection) {
    return connection.with_response({}, {}, {}, value_handler{});
}

[[maybe_unused]] auto instantiate_const_reference_handler(client& connection) {
    return connection.with_response({}, {}, {}, const_reference_handler{});
}
} // namespace
