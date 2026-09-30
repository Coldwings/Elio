// Watchdog that aborts the test binary if a single run exceeds a configurable
// wall-clock budget (default 600 s). This is intentionally a process-wide,
// constructor-time install — Catch2 owns main() so we can't gate it on
// runtime config. The goal is purely diagnostic: a hung test process should
// report the timeout and exit fast, not silently consume an entire CI runner slot
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
//   - raises SIGABRT for normal process-abort diagnostics, and
//   - falls back to _exit(124) (the standard "timeout" exit code) in
//     case SIGABRT is interposed.
//
// Linked into elio_tests / elio_tests_asan / elio_tests_tsan via
// tests/CMakeLists.txt.

#include <signal.h>
#include <unistd.h>

#include <cstdlib>

namespace {

constexpr int kDefaultWatchdogSecs = 600;

void elio_test_watchdog_alarm(int /*sig*/) {
    // Stack symbolization can allocate and is not async-signal-safe.
    static const char msg[] =
        "\n##[error] elio_tests watchdog: wall-clock timeout reached, "
        "aborting (set ELIO_TEST_WATCHDOG_SECS=0 to disable)\n";
    [[maybe_unused]] ssize_t written = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
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
