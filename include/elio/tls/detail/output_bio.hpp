#pragma once

#include <openssl/bio.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>

namespace elio::tls::detail {

/// Ciphertext-only output BIO storage. All calls require the owning SSL mutex.
/// Keep this nonmovable object alive until its BIO and external drain are gone.
/// The budget charges payload allocations, not queue metadata or OpenSSL state.
class output_bio_state {
public:
    explicit output_bio_state(int fd, size_t budget) noexcept
        : fd_(fd), budget_(budget) {}
    output_bio_state(const output_bio_state&) = delete;
    output_bio_state& operator=(const output_bio_state&) = delete;

    // Iterative destruction avoids recursion proportional to queued blocks.
    ~output_bio_state() {
        while (head_) {
            auto retired = std::move(head_);
            head_ = std::move(retired->next);
        }
    }

    /// Caller transfers the returned BIO to SSL; it does not own this object/fd.
    BIO* make_bio() noexcept {
        if (error_) return nullptr;
        const auto* method = bio_method();
        if (!method) { fail(ENOMEM); return nullptr; }
        BIO* bio = BIO_new(method);
        if (!bio) { fail(ENOMEM); return nullptr; }
        BIO_set_data(bio, this);
        BIO_set_init(bio, 1);
        return bio;
    }

    int error() const noexcept { return error_; }
    void fail(int error) noexcept { if (!error_) error_ = error > 0 ? error : EIO; }
    size_t budget() const noexcept { return budget_; }
    size_t retained_bytes() const noexcept { return retained_; }
    size_t pending_bytes() const noexcept { return pending_; }
    uint64_t accepted_bytes() const noexcept { return accepted_; }
    uint64_t drained_bytes() const noexcept { return drained_; }

    /// Prefix remains stable across append. Only one drainer may lease it.
    /// Do not consume its block until the external send and cleanup return.
    /// After failure no new drain may start; an existing lease stays valid.
    std::span<const std::byte> pending() const noexcept {
        if (error_ || !head_) return {};
        return {head_->bytes.get() + head_->offset, head_->size - head_->offset};
    }

    /// Retire externally sent bytes after releasing their I/O lease. A partial
    /// block continues charging its entire allocation, including its prefix.
    void consume(size_t count) noexcept {
        if (count > pending_) { fail(EINVAL); return; }
        pending_ -= count;
        drained_ += count;
        while (count) {
            const auto step = std::min(count, head_->size - head_->offset);
            head_->offset += step;
            count -= step;
            if (head_->offset == head_->size) {
                retained_ -= head_->size;
                auto retired = std::move(head_);
                head_ = std::move(retired->next);
                if (!head_) tail_ = nullptr;
            }
        }
    }

#ifdef ELIO_RUNTIME_TEST_HOOKS
    struct test_hooks {
        void* context = nullptr;
        // Returns send(2) semantics and sets errno on failure.
        ssize_t (*send)(void*, int, const void*, size_t, int) = nullptr;
        // May throw; invoked before metadata (false) or payload (true) allocation.
        void (*allocate)(void*, bool) = nullptr;
    };
    void set_test_hooks(test_hooks hooks) noexcept { hooks_ = hooks; }
#endif

private:
    struct block {
        std::unique_ptr<std::byte[]> bytes;
        size_t size = 0;
        size_t offset = 0;
        std::unique_ptr<block> next;
    };

    int accept(const char* data, int length) noexcept {
        if (error_) { errno = error_; return -1; }
        if (length < 0 || (!data && length)) { fail(EINVAL); errno = error_; return -1; }
        if (!length) return 0;
        const auto size = static_cast<size_t>(length);
        if (size > std::numeric_limits<uint64_t>::max() - accepted_) {
            fail(EOVERFLOW); errno = error_; return -1;
        }
        try {
            size_t sent = 0;
            // Never bypass a retained prefix, including one leased by a drain.
            if (!head_) {
                ssize_t result;
#ifdef ELIO_RUNTIME_TEST_HOOKS
                if (hooks_.send) result = hooks_.send(hooks_.context, fd_, data, size, MSG_DONTWAIT | MSG_NOSIGNAL);
                else
#endif
                result = ::send(fd_, data, size, MSG_DONTWAIT | MSG_NOSIGNAL);
                if (result < 0) {
                    const int error = errno;
                    if (error != EAGAIN && error != EWOULDBLOCK && error != EINTR) {
                        fail(error); errno = error_; return -1;
                    }
                } else {
                    sent = static_cast<size_t>(result);
                    if (sent > size) { fail(EIO); errno = error_; return -1; }
                    if (!sent) { fail(EPIPE); errno = error_; return -1; }
                    // Preserve real socket side effects even if suffix admission fails.
                    accepted_ += sent;
                    drained_ += sent;
                }
            }
            const auto remaining = size - sent;
            if (remaining) {
                if (remaining > budget_ - retained_) {
                    fail(ENOBUFS); errno = error_; return -1;
                }
#ifdef ELIO_RUNTIME_TEST_HOOKS
                if (hooks_.allocate) hooks_.allocate(hooks_.context, false);
#endif
                auto queued = std::make_unique<block>();
#ifdef ELIO_RUNTIME_TEST_HOOKS
                if (hooks_.allocate) hooks_.allocate(hooks_.context, true);
#endif
                queued->bytes = std::make_unique_for_overwrite<std::byte[]>(remaining);
                queued->size = remaining;
                std::memcpy(queued->bytes.get(), data + sent, remaining);
                auto* last = queued.get();
                if (tail_) tail_->next = std::move(queued);
                else head_ = std::move(queued);
                tail_ = last;
                retained_ += remaining;
                pending_ += remaining;
                accepted_ += remaining;
            }
            return length;
        } catch (const std::bad_alloc&) {
            fail(ENOMEM);
        } catch (...) {
            fail(EIO);
        }
        errno = error_;
        return -1;
    }

    static int write_callback(BIO* bio, const char* data, int length) noexcept {
        BIO_clear_retry_flags(bio);
        auto* state = static_cast<output_bio_state*>(BIO_get_data(bio));
        if (!state) { errno = EINVAL; return -1; }
        return state->accept(data, length);
    }
    static int create_callback(BIO* bio) noexcept {
        BIO_set_init(bio, 0);
        BIO_set_data(bio, nullptr);
        BIO_set_shutdown(bio, BIO_NOCLOSE);
        return 1;
    }
    static int destroy_callback(BIO* bio) noexcept {
        if (!bio) return 0;
        BIO_set_data(bio, nullptr);
        BIO_set_init(bio, 0);
        return 1;
    }
    static long control_callback(BIO* bio, int command, long, void*) noexcept {
        const auto* state = static_cast<output_bio_state*>(BIO_get_data(bio));
        switch (command) {
            case BIO_CTRL_FLUSH: return state && !state->error() ? 1 : 0;
            case BIO_CTRL_WPENDING:
                return state ? static_cast<long>(std::min<size_t>(state->pending_bytes(), LONG_MAX)) : 0;
            case BIO_CTRL_GET_CLOSE: return BIO_NOCLOSE;
            case BIO_CTRL_SET_CLOSE: return 1; // Socket lifetime always belongs to tls_stream.
            default: return 0;
        }
    }
    static const BIO_METHOD* bio_method() noexcept {
        struct method_owner {
            BIO_METHOD* value = nullptr;
            method_owner() noexcept {
                value = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "Elio bounded ciphertext output");
                if (value && (!BIO_meth_set_write(value, write_callback) ||
                              !BIO_meth_set_ctrl(value, control_callback) ||
                              !BIO_meth_set_create(value, create_callback) ||
                              !BIO_meth_set_destroy(value, destroy_callback))) {
                    BIO_meth_free(value);
                    value = nullptr;
                }
            }
            ~method_owner() { BIO_meth_free(value); }
        };
        static const method_owner method;
        return method.value;
    }

    int fd_;
    size_t budget_;
    size_t retained_ = 0;
    size_t pending_ = 0;
    uint64_t accepted_ = 0;
    uint64_t drained_ = 0;
    int error_ = 0;
    std::unique_ptr<block> head_;
    block* tail_ = nullptr;
#ifdef ELIO_RUNTIME_TEST_HOOKS
    test_hooks hooks_;
#endif
};

} // namespace elio::tls::detail
