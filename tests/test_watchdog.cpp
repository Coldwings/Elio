// Watchdog that aborts the test binary if a single run exceeds a configurable
// wall-clock budget (default 600 s). This is intentionally a process-wide,
// constructor-time install — Catch2 owns main() so we can't gate it on
// runtime config. The goal is purely diagnostic: a hung test process should
// dump a stack and exit fast, not silently consume an entire CI runner slot
// while producing zero log output (as happened on PR #88's arm64-Debug ASAN
// run, which sat hung for 31 minutes before being cancelled manually).
//
// Configuration:
//   ELIO_TEST_WATCHDOG_SECS=N   override timeout (seconds); N=0 disables
//   ELIO_TEST_WATCHDOG_DISABLE=1 disable entirely (interactive debugging)
//
// On fire, the handler:
//   - writes a marker line to stderr (visible in the GitHub Actions log
//     even after the process is killed),
//   - if compiled with AddressSanitizer/ThreadSanitizer, asks the
//     sanitizer to print the calling thread's stack trace,
//   - raises SIGABRT so the sanitizer/libc abort handler can dump
//     additional state, and
//   - falls back to _exit(124) (the standard "timeout" exit code) in
//     case SIGABRT is interposed.
//
// Linked into elio_tests / elio_tests_asan / elio_tests_tsan via
// tests/CMakeLists.txt.

#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

#if defined(__SANITIZE_ADDRESS__)
#  define ELIO_TEST_WATCHDOG_HAS_SANITIZER 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define ELIO_TEST_WATCHDOG_HAS_SANITIZER 1
#  else
#    define ELIO_TEST_WATCHDOG_HAS_SANITIZER 0
#  endif
#elif defined(__SANITIZE_THREAD__)
#  define ELIO_TEST_WATCHDOG_HAS_SANITIZER 1
#else
#  define ELIO_TEST_WATCHDOG_HAS_SANITIZER 0
#endif

#if ELIO_TEST_WATCHDOG_HAS_SANITIZER
extern "C" void __sanitizer_print_stack_trace();
#endif

namespace {

constexpr int kDefaultWatchdogSecs = 600;

void elio_test_watchdog_alarm(int /*sig*/) {
    // async-signal-safe: only ::write, no fmt/iostream.
    static const char msg[] =
        "\n##[error] elio_tests watchdog: wall-clock timeout reached, "
        "dumping stack and aborting (set ELIO_TEST_WATCHDOG_SECS=0 to disable)\n";
    [[maybe_unused]] ssize_t written = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
#if ELIO_TEST_WATCHDOG_HAS_SANITIZER
    __sanitizer_print_stack_trace();
#endif
    // Try SIGABRT so sanitizer's abort handler can attach more context.
    ::raise(SIGABRT);
    // Fallback in case SIGABRT was masked or interposed.
    ::_exit(124);
}

__attribute__((constructor))
void elio_install_test_watchdog() {
    if (const char* disabled = ::getenv("ELIO_TEST_WATCHDOG_DISABLE");
        disabled != nullptr && disabled[0] == '1' && disabled[1] == '\0') {
        return;
    }

    int timeout = kDefaultWatchdogSecs;
    if (const char* env = ::getenv("ELIO_TEST_WATCHDOG_SECS"); env != nullptr) {
        const int parsed = ::atoi(env);
        if (parsed == 0) {
            return;  // explicit disable
        }
        if (parsed > 0) {
            timeout = parsed;
        }
    }

    struct sigaction sa{};
    sa.sa_handler = &elio_test_watchdog_alarm;
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGALRM, &sa, nullptr);

    ::alarm(static_cast<unsigned int>(timeout));
}

}  // namespace
