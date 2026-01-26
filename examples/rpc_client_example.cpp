/// @file rpc_client_example.cpp
/// @brief RPC Client Example
///
/// This example demonstrates how to use Elio's RPC client to make remote
/// procedure calls. It connects to the rpc_server_example and exercises
/// various RPC methods including concurrent (out-of-order) calls.
///
/// Usage: ./rpc_client_example [host] [port]
/// Default: localhost:9000

#include <elio/elio.hpp>
#include <elio/rpc/rpc.hpp>
#include <iostream>
#include <iomanip>

using namespace elio;
using namespace elio::coro;
using namespace elio::runtime;
using namespace elio::net;
using namespace elio::rpc;

// ============================================================================
// Message definitions (same as server)
// ============================================================================

struct User {
    int32_t id;
    std::string name;
    std::string email;
    int32_t age;
    std::vector<std::string> roles;
    
    ELIO_RPC_FIELDS(User, id, name, email, age, roles)
};

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

struct CalculateRequest {
    std::string operation;
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
// Helper functions
// ============================================================================

void print_user(const User& user) {
    std::cout << "  ID: " << user.id << std::endl;
    std::cout << "  Name: " << user.name << std::endl;
    std::cout << "  Email: " << user.email << std::endl;
    std::cout << "  Age: " << user.age << std::endl;
    std::cout << "  Roles: [";
    for (size_t i = 0; i < user.roles.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << user.roles[i];
    }
    std::cout << "]" << std::endl;
}

// ============================================================================
// Client implementation
// ============================================================================

task<void> run_demo(tcp_rpc_client::ptr client) {
    std::cout << "\n=== Elio RPC Client Demo ===" << std::endl;
    
    // Test 1: Echo
    std::cout << "\n--- Test 1: Echo ---" << std::endl;
    {
        EchoRequest req;
        req.message = "Hello, RPC!";
        req.repeat = 3;
        
        auto result = co_await client->call<Echo>(req);
        if (result.ok()) {
            std::cout << "Echo response (" << result->messages.size() << " messages):" << std::endl;
            for (const auto& msg : result->messages) {
                std::cout << "  - " << msg << std::endl;
            }
        } else {
            std::cerr << "Echo failed: " << result.error_message() << std::endl;
        }
    }
    
    // Test 2: Create users
    std::cout << "\n--- Test 2: Create Users ---" << std::endl;
    std::vector<int32_t> created_ids;
    {
        std::vector<CreateUserRequest> requests = {
            {"Alice", "alice@example.com", 28, {"admin", "user"}},
            {"Bob", "bob@example.com", 35, {"user"}},
            {"Charlie", "charlie@example.com", 42, {"user", "developer"}},
        };
        
        for (const auto& req : requests) {
            auto result = co_await client->call<CreateUser>(req);
            if (result.ok() && result->success) {
                std::cout << "Created user '" << req.name << "' with ID " << result->user_id << std::endl;
                created_ids.push_back(result->user_id);
            } else {
                std::cerr << "Failed to create user: " 
                          << (result.ok() ? result->message : result.error_message()) << std::endl;
            }
        }
    }
    
    // Test 3: Get user
    std::cout << "\n--- Test 3: Get User ---" << std::endl;
    if (!created_ids.empty()) {
        GetUserRequest req;
        req.user_id = created_ids[0];
        
        auto result = co_await client->call<GetUser>(req);
        if (result.ok()) {
            if (result->found && result->user) {
                std::cout << "Found user:" << std::endl;
                print_user(*result->user);
            } else {
                std::cout << "User not found" << std::endl;
            }
        } else {
            std::cerr << "GetUser failed: " << result.error_message() << std::endl;
        }
    }
    
    // Test 4: List users
    std::cout << "\n--- Test 4: List Users ---" << std::endl;
    {
        ListUsersRequest req;
        req.offset = 0;
        req.limit = 10;
        
        auto result = co_await client->call<ListUsers>(req);
        if (result.ok()) {
            std::cout << "Total users: " << result->total_count << std::endl;
            std::cout << "Users:" << std::endl;
            for (const auto& user : result->users) {
                std::cout << "  [" << user.id << "] " << user.name 
                          << " <" << user.email << ">" << std::endl;
            }
        } else {
            std::cerr << "ListUsers failed: " << result.error_message() << std::endl;
        }
    }
    
    // Test 5: Calculate
    std::cout << "\n--- Test 5: Calculate ---" << std::endl;
    {
        std::vector<std::pair<std::string, std::vector<double>>> calcs = {
            {"add", {10, 20, 30}},
            {"subtract", {100, 25, 10}},
            {"multiply", {2, 3, 4}},
            {"divide", {100, 2, 5}},
        };
        
        for (const auto& [op, operands] : calcs) {
            CalculateRequest req;
            req.operation = op;
            req.operands = operands;
            
            auto result = co_await client->call<Calculate>(req);
            if (result.ok() && result->success) {
                std::cout << op << "(";
                for (size_t i = 0; i < operands.size(); ++i) {
                    if (i > 0) std::cout << ", ";
                    std::cout << operands[i];
                }
                std::cout << ") = " << result->result << std::endl;
            } else {
                std::cerr << "Calculate failed: " 
                          << (result.ok() ? result->error : result.error_message()) << std::endl;
            }
        }
    }
    
    // Test 6: Concurrent calls (out-of-order)
    std::cout << "\n--- Test 6: Concurrent Calls ---" << std::endl;
    {
        auto start = std::chrono::steady_clock::now();
        
        // Launch multiple calls concurrently by spawning separate coroutines
        std::atomic<int> completed{0};
        sync::event all_done;
        const int num_calls = 10;
        
        for (int i = 0; i < num_calls; ++i) {
            auto call_task = [&client, i, &completed, &all_done, num_calls]() -> task<void> {
                EchoRequest req;
                req.message = "Concurrent call #" + std::to_string(i);
                req.repeat = 1;
                
                auto result = co_await client->call<Echo>(req);
                if (result.ok()) {
                    std::cout << "  Call #" << i << " completed" << std::endl;
                } else {
                    std::cerr << "  Call #" << i << " failed" << std::endl;
                }
                
                if (++completed == num_calls) {
                    all_done.set();
                }
            };
            
            auto* sched = scheduler::current();
            if (sched) {
                auto t = call_task();
                sched->spawn(t.release());
            }
        }
        
        // Wait for all calls to complete
        co_await all_done.wait();
        
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        std::cout << "All " << num_calls << " concurrent calls completed in " << ms << "ms" << std::endl;
    }
    
    // Test 7: Ping
    std::cout << "\n--- Test 7: Ping ---" << std::endl;
    {
        auto start = std::chrono::steady_clock::now();
        bool alive = co_await client->ping();
        auto elapsed = std::chrono::steady_clock::now() - start;
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
        
        if (alive) {
            std::cout << "Ping successful (round-trip: " << us << "us)" << std::endl;
        } else {
            std::cout << "Ping failed" << std::endl;
        }
    }
    
    // Test 8: Error handling - method not found (invalid method)
    std::cout << "\n--- Test 8: Get non-existent user ---" << std::endl;
    {
        GetUserRequest req;
        req.user_id = 99999;
        
        auto result = co_await client->call<GetUser>(req);
        if (result.ok()) {
            if (result->found) {
                std::cout << "User found (unexpected)" << std::endl;
            } else {
                std::cout << "User not found (expected)" << std::endl;
            }
        } else {
            std::cerr << "Request failed: " << result.error_message() << std::endl;
        }
    }
    
    // Test 9: Timeout handling
    std::cout << "\n--- Test 9: Timeout Test ---" << std::endl;
    {
        EchoRequest req;
        req.message = "Testing timeout";
        req.repeat = 1;
        
        // Use a reasonable timeout
        auto result = co_await client->call<Echo>(req, std::chrono::seconds(5));
        if (result.ok()) {
            std::cout << "Request completed within timeout" << std::endl;
        } else if (result.error() == rpc_error::timeout) {
            std::cout << "Request timed out (expected if server is slow)" << std::endl;
        } else {
            std::cerr << "Request failed: " << result.error_message() << std::endl;
        }
    }
    
    std::cout << "\n=== Demo Complete ===" << std::endl;
}

task<void> client_main(const char* host, uint16_t port) {
    auto& ctx = io::default_io_context();
    
    std::cout << "Connecting to " << host << ":" << port << "..." << std::endl;
    
    auto client = co_await tcp_rpc_client::connect(ctx, host, port);
    if (!client) {
        std::cerr << "Failed to connect to server" << std::endl;
        co_return;
    }
    
    std::cout << "Connected!" << std::endl;
    
    co_await run_demo(*client);
    
    // Close client
    (*client)->close();
}

int main(int argc, char* argv[]) {
    // Parse arguments
    const char* host = "127.0.0.1";
    uint16_t port = 9000;
    
    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = static_cast<uint16_t>(std::stoi(argv[2]));
    }
    
    // Create and start scheduler
    scheduler sched(2);
    sched.set_io_context(&io::default_io_context());
    sched.start();
    
    // Run client
    auto client = client_main(host, port);
    sched.spawn(client.release());
    
    // Wait for completion
    std::this_thread::sleep_for(std::chrono::seconds(5));
    
    // Clean up
    auto& ctx = io::default_io_context();
    for (int i = 0; i < 20 && ctx.has_pending(); ++i) {
        ctx.poll(std::chrono::milliseconds(50));
    }
    
    sched.shutdown();
    
    return 0;
}
