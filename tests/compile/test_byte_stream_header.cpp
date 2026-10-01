#include <elio/net/byte_stream.hpp>

namespace {

using elio::coro::cancel_token;
using elio::coro::task;
using elio::io::io_result;
using elio::net::close_scope;
using elio::net::publishing_byte_stream;
using elio::net::publishing_byte_stream_contract;
using elio::net::write_finish_result;

struct syntax_operations {
    using byte_stream_contract = publishing_byte_stream_contract;

    task<io_result> read(void*, size_t, cancel_token);
    task<io_result> write(const void*, size_t, cancel_token);
    task<write_finish_result> finish_write(cancel_token, std::chrono::milliseconds);
    close_scope read_end_scope() const noexcept;
    task<void> abort_and_settle();
};

struct syntax_stream : syntax_operations {
    syntax_stream() = default;
    syntax_stream(syntax_stream&&) = default;
    syntax_stream(const syntax_stream&) = delete;
};

struct copy_assignable_stream : syntax_stream {
    copy_assignable_stream() = default;
    copy_assignable_stream(copy_assignable_stream&&) = default;
    copy_assignable_stream(const copy_assignable_stream&) = delete;
    copy_assignable_stream& operator=(const copy_assignable_stream&);
};

struct immovable_stream : syntax_stream {
    immovable_stream() = default;
    immovable_stream(immovable_stream&&) = delete;
    immovable_stream(const immovable_stream&) = delete;
};

struct untagged_stream {
    untagged_stream() = default;
    untagged_stream(untagged_stream&&) = default;
    untagged_stream(const untagged_stream&) = delete;

    task<io_result> read(void*, size_t, cancel_token);
    task<io_result> write(const void*, size_t, cancel_token);
    task<write_finish_result> finish_write(cancel_token, std::chrono::milliseconds);
    close_scope read_end_scope() const noexcept;
    task<void> abort_and_settle();
};

struct accepted_only_contract {};
struct accepted_only_stream : syntax_stream {
    using byte_stream_contract = accepted_only_contract;
};

struct wrong_read_result : syntax_stream {
    task<int> read(void*, size_t, cancel_token);
};

struct missing_read_token : syntax_stream {
    task<io_result> read(void*, size_t);
};

struct wrong_write_buffer : syntax_stream {
    task<io_result> write(void*, size_t, cancel_token);
};

struct wrong_write_result : syntax_stream {
    task<int> write(const void*, size_t, cancel_token);
};

struct missing_write_token : syntax_stream {
    task<io_result> write(const void*, size_t);
};

struct wrong_finish_result : syntax_stream {
    task<void> finish_write(cancel_token, std::chrono::milliseconds);
};

struct missing_finish_budget : syntax_stream {
    task<write_finish_result> finish_write(cancel_token);
};

struct wrong_eof_scope : syntax_stream {
    int read_end_scope() const noexcept;
};

struct throwing_eof_scope : syntax_stream {
    close_scope read_end_scope() const;
};

struct mutable_eof_scope : syntax_stream {
    close_scope read_end_scope() noexcept;
};

struct missing_abort : syntax_stream {
    void abort_and_settle() = delete;
};

struct synchronous_abort : syntax_stream {
    void abort_and_settle();
};

static_assert(publishing_byte_stream<syntax_stream>);
static_assert(!publishing_byte_stream<syntax_operations>);
static_assert(!publishing_byte_stream<copy_assignable_stream>);
static_assert(!publishing_byte_stream<immovable_stream>);
static_assert(!publishing_byte_stream<untagged_stream>);
static_assert(!publishing_byte_stream<accepted_only_stream>);
static_assert(!publishing_byte_stream<wrong_read_result>);
static_assert(!publishing_byte_stream<missing_read_token>);
static_assert(!publishing_byte_stream<wrong_write_buffer>);
static_assert(!publishing_byte_stream<wrong_write_result>);
static_assert(!publishing_byte_stream<missing_write_token>);
static_assert(!publishing_byte_stream<wrong_finish_result>);
static_assert(!publishing_byte_stream<missing_finish_budget>);
static_assert(!publishing_byte_stream<wrong_eof_scope>);
static_assert(!publishing_byte_stream<throwing_eof_scope>);
static_assert(!publishing_byte_stream<mutable_eof_scope>);
static_assert(!publishing_byte_stream<missing_abort>);
static_assert(!publishing_byte_stream<synchronous_abort>);
static_assert(!publishing_byte_stream<const syntax_stream>);
static_assert(!publishing_byte_stream<syntax_stream&>);
static_assert(!publishing_byte_stream<int>);

} // namespace

int main() {}
