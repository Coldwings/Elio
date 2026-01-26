#pragma once

/// @file rpc.hpp
/// @brief Umbrella header for Elio RPC framework
///
/// This header includes all RPC-related headers for easy integration.
///
/// ## Quick Start
///
/// @code
/// #include <elio/rpc/rpc.hpp>
///
/// // Define message types
/// struct GetUserRequest {
///     int32_t user_id;
///     ELIO_RPC_FIELDS(GetUserRequest, user_id)
/// };
///
/// struct GetUserResponse {
///     std::string name;
///     int32_t age;
///     ELIO_RPC_FIELDS(GetUserResponse, name, age)
/// };
///
/// // Define method
/// using GetUser = ELIO_RPC_METHOD(1, GetUserRequest, GetUserResponse);
///
/// // Server side
/// elio::rpc::tcp_rpc_server server;
/// server.register_method<GetUser>([](const GetUserRequest& req)
///     -> elio::coro::task<GetUserResponse> {
///     GetUserResponse resp;
///     resp.name = "John";
///     resp.age = 30;
///     co_return resp;
/// });
///
/// // Client side
/// auto client = co_await elio::rpc::tcp_rpc_client::connect(ctx, addr);
/// auto result = co_await client->call<GetUser>(GetUserRequest{42});
/// if (result.ok()) {
///     std::cout << "Name: " << result->name << std::endl;
/// }
/// @endcode
///
/// ## Features
///
/// - **Zero-copy serialization**: Binary format with direct memory access
/// - **Out-of-order calls**: Responses matched by request ID
/// - **Per-call timeouts**: Individual timeout per RPC call
/// - **Nested types**: Support for complex nested structures
/// - **Variable-length data**: Strings, arrays, maps, and optionals
/// - **TCP and UDS**: Works over TCP sockets or Unix domain sockets
///
/// ## Wire Format
///
/// All messages use a binary wire format with:
/// - 18-byte frame header (magic, request_id, type, flags, method_id, length)
/// - Variable-length payload
/// - Little-endian byte order (host order)

#include "rpc_buffer.hpp"
#include "rpc_types.hpp"
#include "rpc_protocol.hpp"
#include "rpc_client.hpp"
#include "rpc_server.hpp"

namespace elio::rpc {

/// @defgroup rpc RPC Framework
/// @brief High-performance RPC over TCP/UDS streams
/// @{

/// @name Type System
/// @{
/// - @ref ELIO_RPC_FIELDS - Macro to define serializable fields
/// - @ref ELIO_RPC_METHOD - Macro to define RPC method descriptor
/// - @ref rpc_result - Result type for RPC calls
/// - @ref rpc_error - Error codes for RPC operations
/// @}

/// @name Serialization
/// @{
/// - @ref buffer_view - Zero-copy read view into serialized data
/// - @ref buffer_writer - Efficient buffer for serialization
/// - @ref iovec_buffer - Scatter-gather buffer for discontinuous I/O
/// - @ref serialize - Serialize a value to buffer
/// - @ref deserialize - Deserialize a value from buffer
/// @}

/// @name Protocol
/// @{
/// - @ref frame_header - Wire protocol frame header
/// - @ref message_type - Request, response, error, ping, pong
/// - @ref read_frame - Read a complete frame from stream
/// - @ref write_frame - Write a frame to stream
/// @}

/// @name Client
/// @{
/// - @ref tcp_rpc_client - RPC client over TCP
/// - @ref uds_rpc_client - RPC client over Unix domain socket
/// - @ref rpc_client::call - Make an RPC call
/// - @ref rpc_client::ping - Send keepalive ping
/// @}

/// @name Server
/// @{
/// - @ref tcp_rpc_server - RPC server over TCP
/// - @ref uds_rpc_server - RPC server over Unix domain socket
/// - @ref rpc_server::register_method - Register a method handler
/// - @ref rpc_server::serve - Start serving connections
/// @}

/// @}

} // namespace elio::rpc
