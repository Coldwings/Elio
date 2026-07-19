#include <elio/coro/task.hpp>
#include <elio/runtime/async_main.hpp>
#include <elio/runtime/scheduler.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <future>
#include <memory>
#include <thread>

namespace {

using namespace std::chrono_literals;

constexpr auto child_timeout = 20s;

int wait_for_child(pid_t pid) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + child_timeout;
    for (;;) {
        int status = 0;
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            if (WIFEXITED(status)) {
                return WEXITSTATUS(status);
            }
            if (WIFSIGNALED(status)) {
                return 128 + WTERMSIG(status);
            }
            return 252;
        }
        if (result < 0 && errno != EINTR) {
            return 253;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            (void)::kill(pid, SIGKILL);
            do {
                status = 0;
            } while (::waitpid(pid, &status, 0) < 0 && errno == EINTR);
            return 254;
        }
        std::this_thread::sleep_for(1ms);
    }
}

int run_fresh_runtime() noexcept {
    try {
        elio::runtime::run_config config;
        config.num_threads = 1;
        config.blocking_threads = 1;
        config.shutdown_timeout = 5s;
        const int result = elio::run(
            []() -> elio::coro::task<int> { co_return 42; }, config);
        return result == 42 ? 0 : 1;
    } catch (...) {
        return 2;
    }
}

int fail(const char* step, int detail, int exit_code) noexcept {
    std::fprintf(stderr, "%s failed (detail=%d)\n", step, detail);
    return exit_code;
}

} // namespace

int main() {
    // Fork before any Elio runtime exists. The child may create, run, and
    // normally tear down an independent runtime.
    pid_t pid = ::fork();
    if (pid < 0) {
        return fail("fork-before-runtime", errno, 10);
    }
    if (pid == 0) {
        return run_fresh_runtime();
    }
    if (const int result = wait_for_child(pid); result != 0) {
        return fail("fresh child runtime", result, 11);
    }

    elio::runtime::scheduler scheduler(
        2, elio::runtime::wait_strategy::blocking(), 1);
    scheduler.start();

    // Once runtime threads exist, the child takes only the immediate _exit
    // path and never touches or destroys inherited Elio state.
    pid = ::fork();
    if (pid < 0) {
        return fail("active-runtime fork for _exit", errno, 12);
    }
    if (pid == 0) {
        ::_exit(0);
    }
    if (const int result = wait_for_child(pid); result != 0) {
        return fail("active-runtime child _exit", result, 13);
    }

    // execve and _exit are async-signal-safe. No Elio or C++ runtime cleanup
    // runs in the inherited child image.
    pid = ::fork();
    if (pid < 0) {
        return fail("active-runtime fork for exec", errno, 14);
    }
    if (pid == 0) {
        char path[] = "/bin/true";
        char arg0[] = "true";
        char* argv[] = {arg0, nullptr};
        char* envp[] = {nullptr};
        ::execve(path, argv, envp);
        ::_exit(127);
    }
    if (const int result = wait_for_child(pid); result != 0) {
        return fail("active-runtime child exec", result, 15);
    }

    // Forking a child that immediately execs or exits must not invalidate the
    // parent's original runtime.
    auto completion = std::make_shared<std::promise<int>>();
    auto future = completion->get_future();
    scheduler.go([completion]() -> elio::coro::task<void> {
        completion->set_value(42);
        co_return;
    });
    if (future.wait_for(5s) != std::future_status::ready) {
        return fail("parent runtime continuation", 1, 16);
    }
    if (future.get() != 42) {
        return fail("parent runtime result", 1, 17);
    }
    if (!scheduler.shutdown(5s)) {
        return fail("parent runtime shutdown", 1, 18);
    }

    return 0;
}
