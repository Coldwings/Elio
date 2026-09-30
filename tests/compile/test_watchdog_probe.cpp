#include <csignal>
#include <cstring>
#include <unistd.h>

namespace {
void report_abort(int) {
    // Make the watchdog's SIGABRT path observable without producing a core.
    ::_exit(86);
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (std::strcmp(argv[1], "disabled") == 0) {
        struct sigaction installed{};
        if (::sigaction(SIGALRM, nullptr, &installed) != 0) return 3;
        return installed.sa_handler == SIG_DFL ? 0 : 4;
    }

    struct sigaction abort_action{};
    abort_action.sa_handler = report_abort;
    ::sigemptyset(&abort_action.sa_mask);
    if (::sigaction(SIGABRT, &abort_action, nullptr) != 0) return 5;

    sigset_t abort_signal;
    ::sigemptyset(&abort_signal);
    ::sigaddset(&abort_signal, SIGABRT);
    if (::sigprocmask(SIG_UNBLOCK, &abort_signal, nullptr) != 0) return 6;
    if (std::strcmp(argv[1], "fallback") == 0) {
        if (::sigprocmask(SIG_BLOCK, &abort_signal, nullptr) != 0) return 7;
        ::raise(SIGALRM);
        return 8;
    }
    if (std::strcmp(argv[1], "wait") != 0) return 9;
    for (;;) ::pause();
}
