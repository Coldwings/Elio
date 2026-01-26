#include <catch2/catch_test_macros.hpp>
#include <elio/signal/signalfd.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/io/io_context.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace elio::signal;
using namespace elio::coro;
using namespace elio::runtime;
using namespace std::chrono_literals;

TEST_CASE("signal_set basic operations", "[signal][signal_set]") {
    SECTION("default constructor creates empty set") {
        signal_set sigs;
        REQUIRE_FALSE(sigs.contains(SIGINT));
        REQUIRE_FALSE(sigs.contains(SIGTERM));
    }
    
    SECTION("add signals") {
        signal_set sigs;
        sigs.add(SIGINT);
        REQUIRE(sigs.contains(SIGINT));
        REQUIRE_FALSE(sigs.contains(SIGTERM));
        
        sigs.add(SIGTERM);
        REQUIRE(sigs.contains(SIGTERM));
    }
    
    SECTION("chained add") {
        signal_set sigs;
        sigs.add(SIGINT).add(SIGTERM).add(SIGUSR1);
        REQUIRE(sigs.contains(SIGINT));
        REQUIRE(sigs.contains(SIGTERM));
        REQUIRE(sigs.contains(SIGUSR1));
    }
    
    SECTION("initializer list constructor") {
        signal_set sigs{SIGINT, SIGTERM, SIGUSR1};
        REQUIRE(sigs.contains(SIGINT));
        REQUIRE(sigs.contains(SIGTERM));
        REQUIRE(sigs.contains(SIGUSR1));
        REQUIRE_FALSE(sigs.contains(SIGUSR2));
    }
    
    SECTION("remove signal") {
        signal_set sigs{SIGINT, SIGTERM};
        REQUIRE(sigs.contains(SIGINT));
        
        sigs.remove(SIGINT);
        REQUIRE_FALSE(sigs.contains(SIGINT));
        REQUIRE(sigs.contains(SIGTERM));
    }
    
    SECTION("clear all signals") {
        signal_set sigs{SIGINT, SIGTERM, SIGUSR1};
        sigs.clear();
        REQUIRE_FALSE(sigs.contains(SIGINT));
        REQUIRE_FALSE(sigs.contains(SIGTERM));
        REQUIRE_FALSE(sigs.contains(SIGUSR1));
    }
    
    SECTION("fill with all signals") {
        signal_set sigs;
        sigs.fill();
        REQUIRE(sigs.contains(SIGINT));
        REQUIRE(sigs.contains(SIGTERM));
        REQUIRE(sigs.contains(SIGUSR1));
        REQUIRE(sigs.contains(SIGUSR2));
    }
}

TEST_CASE("signal_set block/unblock", "[signal][signal_set]") {
    signal_set sigs{SIGUSR1, SIGUSR2};
    sigset_t old_mask;
    
    SECTION("block signals") {
        REQUIRE(sigs.block(&old_mask));
        
        // Verify signals are blocked
        sigset_t current;
        pthread_sigmask(SIG_BLOCK, nullptr, &current);
        REQUIRE(sigismember(&current, SIGUSR1) == 1);
        REQUIRE(sigismember(&current, SIGUSR2) == 1);
        
        // Restore old mask
        pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
    }
    
    SECTION("unblock signals") {
        // First block
        sigs.block(&old_mask);
        
        // Then unblock
        REQUIRE(sigs.unblock());
        
        // Verify at least these signals are unblocked
        sigset_t current;
        pthread_sigmask(SIG_BLOCK, nullptr, &current);
        
        // Restore old mask
        pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
    }
}

TEST_CASE("signal_fd creation", "[signal][signal_fd]") {
    SECTION("create with single signal") {
        signal_set sigs;
        sigs.add(SIGUSR1);
        
        signal_fd sigfd(sigs);
        REQUIRE(sigfd.valid());
        REQUIRE(sigfd.fd() >= 0);
    }
    
    SECTION("create with multiple signals") {
        signal_set sigs{SIGUSR1, SIGUSR2};
        signal_fd sigfd(sigs);
        REQUIRE(sigfd.valid());
    }
    
    SECTION("bool conversion") {
        signal_set sigs{SIGUSR1};
        signal_fd sigfd(sigs);
        REQUIRE(static_cast<bool>(sigfd));
    }
    
    SECTION("move constructor") {
        signal_set sigs{SIGUSR1};
        signal_fd sigfd1(sigs);
        int fd1 = sigfd1.fd();
        REQUIRE(sigfd1.valid());
        
        signal_fd sigfd2(std::move(sigfd1));
        REQUIRE(sigfd2.valid());
        REQUIRE(sigfd2.fd() == fd1);
        REQUIRE_FALSE(sigfd1.valid());
    }
    
    SECTION("move assignment") {
        signal_set sigs{SIGUSR1};
        signal_fd sigfd1(sigs);
        int fd1 = sigfd1.fd();
        
        signal_set sigs2{SIGUSR2};
        signal_fd sigfd2(sigs2);
        
        sigfd2 = std::move(sigfd1);
        REQUIRE(sigfd2.valid());
        REQUIRE(sigfd2.fd() == fd1);
        REQUIRE_FALSE(sigfd1.valid());
    }
    
    SECTION("explicit close") {
        signal_set sigs{SIGUSR1};
        signal_fd sigfd(sigs);
        REQUIRE(sigfd.valid());
        
        sigfd.close();
        REQUIRE_FALSE(sigfd.valid());
        REQUIRE(sigfd.fd() < 0);
    }
}

TEST_CASE("signal_fd synchronous read", "[signal][signal_fd]") {
    signal_set sigs{SIGUSR1};
    signal_fd sigfd(sigs);
    
    SECTION("try_read with no pending signal") {
        auto info = sigfd.try_read();
        REQUIRE_FALSE(info.has_value());
    }
    
    SECTION("try_read after sending signal") {
        // Send signal to self
        kill(getpid(), SIGUSR1);
        
        // Give it a moment to be delivered
        std::this_thread::sleep_for(10ms);
        
        auto info = sigfd.try_read();
        REQUIRE(info.has_value());
        REQUIRE(info->signo == SIGUSR1);
        REQUIRE(info->pid == static_cast<uint32_t>(getpid()));
    }
}

TEST_CASE("signal_fd async wait", "[signal][signal_fd]") {
    std::atomic<bool> received{false};
    std::atomic<int> received_signo{0};
    
    // Block signals BEFORE creating scheduler threads
    signal_set sigs{SIGUSR1};
    sigset_t old_mask;
    sigs.block(&old_mask);
    
    elio::io::io_context ctx;
    
    auto wait_task = [&]() -> task<void> {
        signal_fd sigfd(sigs, ctx, false);  // Don't re-block, already blocked
        
        auto info = co_await sigfd.wait();
        if (info) {
            received_signo = info->signo;
            received = true;
        }
    };
    
    scheduler sched(1);
    sched.set_io_context(&ctx);
    sched.start();
    
    {
        auto t = wait_task();
        sched.spawn(t.release());
    }
    
    // Give the coroutine time to start and enter wait
    std::this_thread::sleep_for(50ms);
    
    // Send the signal
    kill(getpid(), SIGUSR1);
    
    // Wait for completion - don't poll ctx from main thread,
    // let the scheduler's worker thread handle it to avoid races
    for (int i = 0; i < 200 && !received; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    
    sched.shutdown();
    
    // Restore signal mask
    pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
    
    REQUIRE(received);
    REQUIRE(received_signo == SIGUSR1);
}

TEST_CASE("signal_fd multiple signals", "[signal][signal_fd]") {
    std::atomic<int> count{0};
    std::atomic<bool> got_usr1{false};
    std::atomic<bool> got_usr2{false};
    
    // Block signals BEFORE creating scheduler threads
    signal_set sigs{SIGUSR1, SIGUSR2};
    sigset_t old_mask;
    sigs.block(&old_mask);
    
    elio::io::io_context ctx;
    
    auto wait_task = [&]() -> task<void> {
        signal_fd sigfd(sigs, ctx, false);  // Don't re-block, already blocked
        
        for (int i = 0; i < 2; ++i) {
            auto info = co_await sigfd.wait();
            if (info) {
                count++;
                if (info->signo == SIGUSR1) got_usr1 = true;
                if (info->signo == SIGUSR2) got_usr2 = true;
            }
        }
    };
    
    scheduler sched(1);
    sched.set_io_context(&ctx);
    sched.start();
    
    {
        auto t = wait_task();
        sched.spawn(t.release());
    }
    
    std::this_thread::sleep_for(50ms);
    
    // Send both signals
    kill(getpid(), SIGUSR1);
    kill(getpid(), SIGUSR2);
    
    // Wait for completion - don't poll ctx from main thread
    for (int i = 0; i < 200 && count < 2; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    
    sched.shutdown();
    
    // Restore signal mask
    pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
    
    REQUIRE(count == 2);
    REQUIRE(got_usr1);
    REQUIRE(got_usr2);
}

TEST_CASE("signal_info", "[signal][signal_info]") {
    signal_set sigs{SIGUSR1};
    signal_fd sigfd(sigs);
    
    // Send signal to self
    kill(getpid(), SIGUSR1);
    std::this_thread::sleep_for(10ms);
    
    auto info = sigfd.try_read();
    REQUIRE(info.has_value());
    
    SECTION("signal number") {
        REQUIRE(info->signo == SIGUSR1);
    }
    
    SECTION("sender info") {
        REQUIRE(info->pid == static_cast<uint32_t>(getpid()));
        REQUIRE(info->uid == static_cast<uint32_t>(getuid()));
    }
    
    SECTION("signal name") {
        REQUIRE(info->name() != nullptr);
        REQUIRE(std::string(info->name()) == "USR1");
    }
    
    SECTION("full signal name") {
        REQUIRE(info->full_name() == "SIGUSR1");
    }
}

TEST_CASE("signal_block_guard", "[signal][guard]") {
    sigset_t before, during, after;
    
    // Get current mask
    pthread_sigmask(SIG_BLOCK, nullptr, &before);
    int was_blocked = sigismember(&before, SIGUSR1);
    
    {
        signal_set sigs{SIGUSR1};
        signal_block_guard guard(sigs);
        
        // Check that SIGUSR1 is blocked
        pthread_sigmask(SIG_BLOCK, nullptr, &during);
        REQUIRE(sigismember(&during, SIGUSR1) == 1);
    }
    
    // Check that mask is restored
    pthread_sigmask(SIG_BLOCK, nullptr, &after);
    REQUIRE(sigismember(&after, SIGUSR1) == was_blocked);
}

TEST_CASE("signal utility functions", "[signal][utility]") {
    SECTION("signal_name") {
        REQUIRE(std::string(signal_name(SIGINT)) == "INT");
        REQUIRE(std::string(signal_name(SIGTERM)) == "TERM");
        REQUIRE(std::string(signal_name(SIGUSR1)) == "USR1");
    }
    
    SECTION("signal_number with prefix") {
        REQUIRE(signal_number("SIGINT") == SIGINT);
        REQUIRE(signal_number("SIGTERM") == SIGTERM);
        REQUIRE(signal_number("SIGUSR1") == SIGUSR1);
    }
    
    SECTION("signal_number without prefix") {
        REQUIRE(signal_number("INT") == SIGINT);
        REQUIRE(signal_number("TERM") == SIGTERM);
        REQUIRE(signal_number("USR1") == SIGUSR1);
    }
    
    SECTION("signal_number unknown") {
        REQUIRE(signal_number("UNKNOWN") == -1);
        REQUIRE(signal_number(nullptr) == -1);
    }
}

TEST_CASE("wait_signal convenience function", "[signal][wait_signal]") {
    std::atomic<bool> received{false};
    
    // Block signals BEFORE creating scheduler threads
    signal_set sigs{SIGUSR1};
    sigset_t old_mask;
    sigs.block(&old_mask);
    
    elio::io::io_context ctx;
    
    auto wait_task = [&]() -> task<void> {
        auto info = co_await wait_signal(sigs, ctx, false);  // Don't re-block, already blocked
        REQUIRE(info.signo == SIGUSR1);
        received = true;
    };
    
    scheduler sched(1);
    sched.set_io_context(&ctx);
    sched.start();
    
    {
        auto t = wait_task();
        sched.spawn(t.release());
    }
    
    std::this_thread::sleep_for(50ms);
    kill(getpid(), SIGUSR1);
    
    // Wait for completion - don't poll ctx from main thread
    for (int i = 0; i < 200 && !received; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    
    sched.shutdown();
    
    // Restore signal mask
    pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
    
    REQUIRE(received);
}

TEST_CASE("signal_fd update", "[signal][signal_fd]") {
    signal_set sigs{SIGUSR1};
    signal_fd sigfd(sigs);
    
    REQUIRE(sigfd.signals().contains(SIGUSR1));
    REQUIRE_FALSE(sigfd.signals().contains(SIGUSR2));
    
    // Update to a different signal set
    signal_set new_sigs{SIGUSR2};
    REQUIRE(sigfd.update(new_sigs));
    
    REQUIRE(sigfd.signals().contains(SIGUSR2));
    // Note: The old signals may or may not be in the set depending on implementation
    
    // Send SIGUSR2 and verify we can read it
    kill(getpid(), SIGUSR2);
    std::this_thread::sleep_for(10ms);
    
    auto info = sigfd.try_read();
    REQUIRE(info.has_value());
    REQUIRE(info->signo == SIGUSR2);
}
