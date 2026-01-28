#pragma once

/// @file signalfd.hpp
/// @brief Async signal handling using signalfd
///
/// This module provides coroutine-friendly signal handling using Linux signalfd.
/// Traditional signal handlers can interrupt at any time, making it unsafe to
/// modify coroutine state or scheduler internals. Using signalfd, signals become
/// normal I/O events that can be handled in a regular coroutine context.
///
/// Usage:
/// @code
/// // Block signals and create signalfd
/// elio::signal::signal_set sigs;
/// sigs.add(SIGINT).add(SIGTERM);
/// elio::signal::signal_fd sigfd(sigs);
///
/// // Wait for signals in a coroutine
/// while (true) {
///     auto info = co_await sigfd.wait();
///     if (info.signo == SIGINT) {
///         // Handle SIGINT
///     }
/// }
/// @endcode

#include <elio/io/io_context.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/log/macros.hpp>
#include <elio/coro/task.hpp>

#include <csignal>
#include <cerrno>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <sys/signalfd.h>
#include <unistd.h>

namespace elio::signal {

/// Information about a received signal
/// This is a simplified wrapper around signalfd_siginfo
struct signal_info {
    int signo;              ///< Signal number
    int32_t errno_value;    ///< Error number (if applicable)
    int32_t code;           ///< Signal code (SI_USER, SI_KERNEL, etc.)
    uint32_t pid;           ///< PID of sending process
    uint32_t uid;           ///< UID of sending process
    int32_t status;         ///< Exit status or signal (for SIGCHLD)
    
    /// Construct from signalfd_siginfo
    explicit signal_info(const signalfd_siginfo& ssi) noexcept
        : signo(static_cast<int>(ssi.ssi_signo))
        , errno_value(static_cast<int32_t>(ssi.ssi_errno))
        , code(static_cast<int32_t>(ssi.ssi_code))
        , pid(ssi.ssi_pid)
        , uid(ssi.ssi_uid)
        , status(static_cast<int32_t>(ssi.ssi_status)) {}
    
    /// Default constructor
    signal_info() noexcept
        : signo(0), errno_value(0), code(0), pid(0), uid(0), status(0) {}
    
    /// Get signal name
    const char* name() const noexcept {
        return sigabbrev_np(signo);
    }
    
    /// Get full signal name (e.g., "SIGINT")
    std::string full_name() const {
        const char* abbrev = sigabbrev_np(signo);
        if (abbrev) {
            return std::string("SIG") + abbrev;
        }
        return "SIG" + std::to_string(signo);
    }
};

/// Set of signals to handle via signalfd
/// This class manages a sigset_t and provides convenient methods for building signal sets.
class signal_set {
public:
    /// Default constructor - creates an empty signal set
    signal_set() noexcept {
        sigemptyset(&mask_);
    }
    
    /// Construct from initializer list of signal numbers
    signal_set(std::initializer_list<int> signals) noexcept {
        sigemptyset(&mask_);
        for (int sig : signals) {
            sigaddset(&mask_, sig);
        }
    }
    
    /// Add a signal to the set
    /// @param signo Signal number (e.g., SIGINT, SIGTERM)
    /// @return Reference to this for chaining
    signal_set& add(int signo) noexcept {
        sigaddset(&mask_, signo);
        return *this;
    }
    
    /// Remove a signal from the set
    signal_set& remove(int signo) noexcept {
        sigdelset(&mask_, signo);
        return *this;
    }
    
    /// Clear all signals from the set
    signal_set& clear() noexcept {
        sigemptyset(&mask_);
        return *this;
    }
    
    /// Fill the set with all signals
    signal_set& fill() noexcept {
        sigfillset(&mask_);
        return *this;
    }
    
    /// Check if a signal is in the set
    bool contains(int signo) const noexcept {
        return sigismember(&mask_, signo) == 1;
    }
    
    /// Get the underlying sigset_t
    const sigset_t& mask() const noexcept {
        return mask_;
    }
    
    /// Get mutable reference to underlying sigset_t
    sigset_t& mask() noexcept {
        return mask_;
    }
    
    /// Block these signals for the current thread
    /// Returns the old signal mask
    /// @param old_mask Optional pointer to receive the previous mask
    /// @return true on success
    bool block(sigset_t* old_mask = nullptr) const noexcept {
        return pthread_sigmask(SIG_BLOCK, &mask_, old_mask) == 0;
    }
    
    /// Unblock these signals for the current thread
    bool unblock() const noexcept {
        return pthread_sigmask(SIG_UNBLOCK, &mask_, nullptr) == 0;
    }
    
    /// Set the signal mask (replacing the current mask)
    bool set_mask(sigset_t* old_mask = nullptr) const noexcept {
        return pthread_sigmask(SIG_SETMASK, &mask_, old_mask) == 0;
    }
    
    /// Block signals for all threads (process-wide)
    /// This should be called early in main() before spawning threads
    bool block_all_threads() const noexcept {
        return sigprocmask(SIG_BLOCK, &mask_, nullptr) == 0;
    }

private:
    sigset_t mask_;
};

/// Awaitable for reading from signalfd
class signal_wait_awaitable {
public:
    signal_wait_awaitable(io::io_context& ctx, int fd) noexcept
        : ctx_(ctx), fd_(fd) {}
    
    bool await_ready() const noexcept {
        return false;
    }
    
    void await_suspend(std::coroutine_handle<> awaiter) {
        io::io_request req{};
        req.op = io::io_op::read;
        req.fd = fd_;
        req.buffer = &siginfo_;
        req.length = sizeof(siginfo_);
        req.offset = -1;
        req.awaiter = awaiter;
        
        if (!ctx_.prepare(req)) {
            result_ = io::io_result{-EAGAIN, 0};
            awaiter.resume();
            return;
        }
        ctx_.submit();
    }
    
    std::optional<signal_info> await_resume() {
        result_ = io::io_context::get_last_result();
        if (result_.result == static_cast<int>(sizeof(signalfd_siginfo))) {
            return signal_info(siginfo_);
        }
        return std::nullopt;
    }
    
private:
    io::io_context& ctx_;
    int fd_;
    signalfd_siginfo siginfo_{};
    io::io_result result_{};
};

/// Async signal file descriptor
/// Creates a signalfd that can be used to wait for signals in coroutines.
/// The signals in the set are automatically blocked when the signal_fd is created.
class signal_fd {
public:
    /// Construct a signal_fd for the given signal set
    /// @param signals The set of signals to handle
    /// @param ctx Optional I/O context (defaults to the current worker's context)
    /// @param auto_block If true (default), automatically block the signals
    /// @throws std::system_error if signalfd creation fails
    explicit signal_fd(const signal_set& signals, 
                       io::io_context& ctx = io::current_io_context(),
                       bool auto_block = true)
        : ctx_(ctx)
        , signals_(signals)
        , old_mask_saved_(false) {
        
        // Block the signals before creating signalfd
        if (auto_block) {
            if (!signals_.block(&old_mask_)) {
                throw std::system_error(errno, std::system_category(),
                                       "failed to block signals");
            }
            old_mask_saved_ = true;
        }
        
        // Create the signalfd
        fd_ = signalfd(-1, &signals_.mask(), SFD_NONBLOCK | SFD_CLOEXEC);
        if (fd_ < 0) {
            int saved_errno = errno;
            if (old_mask_saved_) {
                pthread_sigmask(SIG_SETMASK, &old_mask_, nullptr);
            }
            throw std::system_error(saved_errno, std::system_category(),
                                   "signalfd creation failed");
        }
        
        ELIO_LOG_DEBUG("signal_fd created with fd={}", fd_);
    }
    
    /// Destructor - closes the fd and optionally restores signal mask
    ~signal_fd() {
        if (fd_ >= 0) {
            ::close(fd_);
            ELIO_LOG_DEBUG("signal_fd closed fd={}", fd_);
        }
        // Note: We don't restore the old mask by default in destructor
        // because the user might want signals to stay blocked.
        // Call restore_mask() explicitly if needed.
    }
    
    // Non-copyable
    signal_fd(const signal_fd&) = delete;
    signal_fd& operator=(const signal_fd&) = delete;
    
    // Movable
    signal_fd(signal_fd&& other) noexcept
        : ctx_(other.ctx_)
        , fd_(std::exchange(other.fd_, -1))
        , signals_(std::move(other.signals_))
        , old_mask_(other.old_mask_)
        , old_mask_saved_(std::exchange(other.old_mask_saved_, false)) {}
    
    signal_fd& operator=(signal_fd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            // Note: ctx_ is a reference, cannot be reassigned
            fd_ = std::exchange(other.fd_, -1);
            signals_ = std::move(other.signals_);
            old_mask_ = other.old_mask_;
            old_mask_saved_ = std::exchange(other.old_mask_saved_, false);
        }
        return *this;
    }
    
    /// Get the file descriptor
    int fd() const noexcept { return fd_; }
    
    /// Check if the fd is valid
    bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return valid(); }
    
    /// Get the signal set
    const signal_set& signals() const noexcept { return signals_; }
    
    /// Wait for a signal asynchronously
    /// @return Awaitable that yields signal_info when a signal is received
    auto wait() {
        return signal_wait_awaitable(ctx_, fd_);
    }
    
    /// Read a signal synchronously (non-blocking)
    /// @return signal_info if a signal is available, nullopt otherwise
    std::optional<signal_info> try_read() {
        signalfd_siginfo siginfo;
        ssize_t n = ::read(fd_, &siginfo, sizeof(siginfo));
        if (n == sizeof(siginfo)) {
            return signal_info(siginfo);
        }
        return std::nullopt;
    }
    
    /// Update the signal set
    /// @param new_signals The new signal set
    /// @param block If true, block the new signals before updating
    /// @return true on success
    bool update(const signal_set& new_signals, bool block = true) {
        if (block) {
            if (!new_signals.block()) {
                return false;
            }
        }
        
        int result = signalfd(fd_, &new_signals.mask(), 0);
        if (result < 0) {
            return false;
        }
        
        signals_ = new_signals;
        return true;
    }
    
    /// Restore the original signal mask (before this signal_fd was created)
    /// This is useful when you want to restore signal handling to the previous state
    bool restore_mask() noexcept {
        if (old_mask_saved_) {
            return pthread_sigmask(SIG_SETMASK, &old_mask_, nullptr) == 0;
        }
        return true;
    }
    
    /// Close the signalfd explicitly
    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    io::io_context& ctx_;
    int fd_ = -1;
    signal_set signals_;
    sigset_t old_mask_{};
    bool old_mask_saved_;
};

/// RAII guard to block signals for the scope
/// Useful for temporarily blocking signals during initialization
class signal_block_guard {
public:
    explicit signal_block_guard(const signal_set& signals)
        : signals_(signals) {
        signals_.block(&old_mask_);
    }
    
    ~signal_block_guard() {
        pthread_sigmask(SIG_SETMASK, &old_mask_, nullptr);
    }
    
    // Non-copyable, non-movable
    signal_block_guard(const signal_block_guard&) = delete;
    signal_block_guard& operator=(const signal_block_guard&) = delete;
    signal_block_guard(signal_block_guard&&) = delete;
    signal_block_guard& operator=(signal_block_guard&&) = delete;

private:
    signal_set signals_;
    sigset_t old_mask_{};
};

/// Create a coroutine that waits for any of the specified signals
/// This is a convenience function that creates a signal_fd and waits for signals
/// @param signals The signals to wait for
/// @param ctx Optional I/O context
/// @param auto_block If true (default), automatically block the signals
/// @return task that yields signal_info when a signal is received
inline coro::task<signal_info> wait_signal(const signal_set& signals,
                                           io::io_context& ctx = io::current_io_context(),
                                           bool auto_block = true) {
    signal_fd sigfd(signals, ctx, auto_block);
    auto info = co_await sigfd.wait();
    if (info) {
        co_return *info;
    }
    throw std::runtime_error("signal_fd read failed");
}

/// Wait for a single signal
/// @param signo The signal to wait for
/// @param ctx Optional I/O context
/// @return task that yields signal_info when the signal is received
inline coro::task<signal_info> wait_signal(int signo,
                                           io::io_context& ctx = io::current_io_context()) {
    signal_set signals;
    signals.add(signo);
    co_return co_await wait_signal(signals, ctx);
}

/// Utility function to get signal name from number
inline const char* signal_name(int signo) {
    return sigabbrev_np(signo);
}

/// Utility function to get signal number from name
/// @param name Signal name (with or without "SIG" prefix)
/// @return Signal number, or -1 if not found
inline int signal_number(const char* name) {
    // Skip "SIG" prefix if present
    if (name && std::strncmp(name, "SIG", 3) == 0) {
        name += 3;
    }
    
    // Linear search through known signals
    static const struct {
        const char* name;
        int number;
    } signals[] = {
        {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"ILL", SIGILL},
        {"TRAP", SIGTRAP}, {"ABRT", SIGABRT}, {"BUS", SIGBUS}, {"FPE", SIGFPE},
        {"KILL", SIGKILL}, {"USR1", SIGUSR1}, {"SEGV", SIGSEGV}, {"USR2", SIGUSR2},
        {"PIPE", SIGPIPE}, {"ALRM", SIGALRM}, {"TERM", SIGTERM}, {"CHLD", SIGCHLD},
        {"CONT", SIGCONT}, {"STOP", SIGSTOP}, {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN},
        {"TTOU", SIGTTOU}, {"URG", SIGURG}, {"XCPU", SIGXCPU}, {"XFSZ", SIGXFSZ},
        {"VTALRM", SIGVTALRM}, {"PROF", SIGPROF}, {"WINCH", SIGWINCH},
        {"IO", SIGIO}, {"PWR", SIGPWR}, {"SYS", SIGSYS},
    };
    
    if (name) {
        for (const auto& sig : signals) {
            if (std::strcmp(name, sig.name) == 0) {
                return sig.number;
            }
        }
    }
    return -1;
}

} // namespace elio::signal
