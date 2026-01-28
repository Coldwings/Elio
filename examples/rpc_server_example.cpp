/// @file rpc_server_example.cpp
/// @brief RPC Server Example
///
/// This example demonstrates how to build an RPC server using Elio's RPC framework.
/// The server exposes several methods: GetUser, CreateUser, ListUsers, and Calculate.
///
/// Usage: ./rpc_server_example [port]
/// Default port: 9000

#include <elio/elio.hpp>
#include <elio/rpc/rpc.hpp>
#include <atomic>
#include <map>

using namespace elio;
using namespace elio::coro;
using namespace elio::runtime;
using namespace elio::net;
using namespace elio::rpc;
using namespace elio::signal;

// ============================================================================
// Message definitions
// ============================================================================

// User data structure
struct User {
    int32_t id;
    std::string name;
    std::string email;
    int32_t age;
    std::vector<std::string> roles;
    
    ELIO_RPC_FIELDS(User, id, name, email, age, roles)
};

// GetUser method
struct GetUserRequest {
    int32_t user_id;
    ELIO_RPC_FIELDS(GetUserRequest, user_id)
};

struct GetUserResponse {
    bool found;
    std::optional<User> user;
    ELIO_RPC_FIELDS(GetUserResponse, found, user)
};

using GetUser = ELIO_RPC_METHOD(1, GetUserRequest, GetUserResponse);

// CreateUser method
struct CreateUserRequest {
    std::string name;
    std::string email;
    int32_t age;
    std::vector<std::string> roles;
    ELIO_RPC_FIELDS(CreateUserRequest, name, email, age, roles)
};

struct CreateUserResponse {
    bool success;
    int32_t user_id;
    std::string message;
    ELIO_RPC_FIELDS(CreateUserResponse, success, user_id, message)
};

using CreateUser = ELIO_RPC_METHOD(2, CreateUserRequest, CreateUserResponse);

// ListUsers method
struct ListUsersRequest {
    int32_t offset;
    int32_t limit;
    ELIO_RPC_FIELDS(ListUsersRequest, offset, limit)
};

struct ListUsersResponse {
    std::vector<User> users;
    int32_t total_count;
    ELIO_RPC_FIELDS(ListUsersResponse, users, total_count)
};

using ListUsers = ELIO_RPC_METHOD(3, ListUsersRequest, ListUsersResponse);

// Calculate method (demonstrates nested types)
struct CalculateRequest {
    std::string operation;  // "add", "subtract", "multiply", "divide"
    std::vector<double> operands;
    ELIO_RPC_FIELDS(CalculateRequest, operation, operands)
};

struct CalculateResponse {
    bool success;
    double result;
    std::string error;
    ELIO_RPC_FIELDS(CalculateResponse, success, result, error)
};

using Calculate = ELIO_RPC_METHOD(4, CalculateRequest, CalculateResponse);

// Echo method (for testing)
struct EchoRequest {
    std::string message;
    int32_t repeat;
    ELIO_RPC_FIELDS(EchoRequest, message, repeat)
};

struct EchoResponse {
    std::vector<std::string> messages;
    ELIO_RPC_FIELDS(EchoResponse, messages)
};

using Echo = ELIO_RPC_METHOD(5, EchoRequest, EchoResponse);

// ============================================================================
// Server implementation
// ============================================================================

// In-memory user store
class UserStore {
public:
    std::optional<User> get_user(int32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = users_.find(id);
        if (it != users_.end()) {
            return it->second;
        }
        return std::nullopt;
    }
    
    int32_t create_user(const std::string& name, const std::string& email,
                        int32_t age, const std::vector<std::string>& roles) {
        std::lock_guard<std::mutex> lock(mutex_);
        int32_t id = next_id_++;
        users_[id] = User{id, name, email, age, roles};
        return id;
    }
    
    std::vector<User> list_users(int32_t offset, int32_t limit) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<User> result;
        int32_t count = 0;
        for (const auto& [id, user] : users_) {
            if (count >= offset && result.size() < static_cast<size_t>(limit)) {
                result.push_back(user);
            }
            ++count;
        }
        return result;
    }
    
    int32_t total_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int32_t>(users_.size());
    }
    
private:
    std::mutex mutex_;
    std::map<int32_t, User> users_;
    int32_t next_id_ = 1;
};

// Global state
std::atomic<bool> g_running{true};
std::atomic<int> g_listener_fd{-1};
UserStore g_user_store;

/// Signal handler coroutine - waits for SIGINT/SIGTERM
task<void> signal_handler_task() {
    signal_set sigs{SIGINT, SIGTERM};
    signal_fd sigfd(sigs);
    
    ELIO_LOG_DEBUG("Signal handler started, waiting for SIGINT/SIGTERM...");
    
    auto info = co_await sigfd.wait();
    if (info) {
        ELIO_LOG_INFO("Received signal: {} - initiating shutdown", info->full_name());
    }
    
    g_running = false;
    
    // Close the listener to interrupt the pending accept
    int fd = g_listener_fd.exchange(-1);
    if (fd >= 0) {
        ::close(fd);
    }
    
    co_return;
}

task<void> server_main(uint16_t port, [[maybe_unused]] scheduler& sched) {
    // Bind TCP listener
    auto listener_result = tcp_listener::bind(ipv4_address(port));
    if (!listener_result) {
        ELIO_LOG_ERROR("Failed to bind to port {}: {}", port, strerror(errno));
        co_return;
    }
    
    auto& listener = *listener_result;
    g_listener_fd.store(listener.fd(), std::memory_order_release);
    
    // Create RPC server
    tcp_rpc_server server;
    
    // Register GetUser handler
    server.register_method<GetUser>([](const GetUserRequest& req) 
        -> task<GetUserResponse> {
        GetUserResponse resp;
        auto user = g_user_store.get_user(req.user_id);
        resp.found = user.has_value();
        resp.user = user;
        co_return resp;
    });
    
    // Register CreateUser handler
    server.register_method<CreateUser>([](const CreateUserRequest& req)
        -> task<CreateUserResponse> {
        CreateUserResponse resp;
        
        if (req.name.empty()) {
            resp.success = false;
            resp.user_id = -1;
            resp.message = "Name is required";
            co_return resp;
        }
        
        resp.user_id = g_user_store.create_user(req.name, req.email, req.age, req.roles);
        resp.success = true;
        resp.message = "User created successfully";
        
        ELIO_LOG_INFO("Created user {} with ID {}", req.name, resp.user_id);
        co_return resp;
    });
    
    // Register ListUsers handler
    server.register_method<ListUsers>([](const ListUsersRequest& req)
        -> task<ListUsersResponse> {
        ListUsersResponse resp;
        resp.users = g_user_store.list_users(req.offset, req.limit);
        resp.total_count = g_user_store.total_count();
        co_return resp;
    });
    
    // Register Calculate handler (synchronous)
    server.register_sync_method<Calculate>([](const CalculateRequest& req)
        -> CalculateResponse {
        CalculateResponse resp;
        
        if (req.operands.empty()) {
            resp.success = false;
            resp.error = "No operands provided";
            return resp;
        }
        
        double result = req.operands[0];
        
        if (req.operation == "add") {
            for (size_t i = 1; i < req.operands.size(); ++i) {
                result += req.operands[i];
            }
        } else if (req.operation == "subtract") {
            for (size_t i = 1; i < req.operands.size(); ++i) {
                result -= req.operands[i];
            }
        } else if (req.operation == "multiply") {
            for (size_t i = 1; i < req.operands.size(); ++i) {
                result *= req.operands[i];
            }
        } else if (req.operation == "divide") {
            for (size_t i = 1; i < req.operands.size(); ++i) {
                if (req.operands[i] == 0) {
                    resp.success = false;
                    resp.error = "Division by zero";
                    return resp;
                }
                result /= req.operands[i];
            }
        } else {
            resp.success = false;
            resp.error = "Unknown operation: " + req.operation;
            return resp;
        }
        
        resp.success = true;
        resp.result = result;
        return resp;
    });
    
    // Register Echo handler
    server.register_sync_method<Echo>([](const EchoRequest& req)
        -> EchoResponse {
        EchoResponse resp;
        for (int32_t i = 0; i < req.repeat; ++i) {
            resp.messages.push_back(req.message);
        }
        return resp;
    });
    
    ELIO_LOG_INFO("RPC server listening on port {}", port);
    ELIO_LOG_INFO("Available methods: GetUser(1), CreateUser(2), ListUsers(3), Calculate(4), Echo(5)");
    ELIO_LOG_INFO("Press Ctrl+C to stop");
    
    // Serve until shutdown
    co_await server.serve(listener);
}

int main(int argc, char* argv[]) {
    // Parse port
    uint16_t port = 9000;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }
    
    // Block signals BEFORE creating scheduler threads
    signal_set sigs{SIGINT, SIGTERM};
    sigs.block_all_threads();
    
    // Create and start scheduler
    scheduler sched(4);
    sched.start();
    
    // Spawn signal handler coroutine
    auto sig_handler = signal_handler_task();
    sched.spawn(sig_handler.release());
    
    // Run server
    auto server = server_main(port, sched);
    sched.spawn(server.release());
    
    // Wait for shutdown
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    auto& ctx = io::default_io_context();
    for (int i = 0; i < 10 && ctx.has_pending(); ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    
    ELIO_LOG_INFO("Server stopped");
    return 0;
}
