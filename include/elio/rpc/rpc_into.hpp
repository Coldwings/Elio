#pragma once

#include "rpc_protocol.hpp"
#include <elio/sync/event.hpp>

#include <atomic>
#include <cstddef>
#include <span>
#include <tuple>
#include <type_traits>
#include <vector>

namespace elio::rpc {
namespace detail {

struct blob_target {
    std::span<uint8_t> contiguous;
    std::vector<iovec> scatter;
    size_t* written = nullptr;
    size_t length = 0;

    bool fits(size_t n) const noexcept {
        if (!written) return n <= contiguous.size();
        for (const auto& v : scatter) {
            if (n <= v.iov_len) return true;
            n -= v.iov_len;
        }
        return n == 0;
    }
};

template<bool Scatter, auto... Members>
struct field_bindings {
    static constexpr bool has_scatter = Scatter;
    std::array<blob_target, sizeof...(Members)> targets;
};

template<class T> struct is_bindings : std::false_type {};
template<bool S, auto... M> struct is_bindings<field_bindings<S, M...>> : std::true_type {};
template<class T, class R> concept bindings_for = is_bindings<T>::value &&
    (!T::has_scatter || std::is_nothrow_move_constructible_v<R>);

template<class T> struct blob_member {};
template<class R> struct blob_member<buffer_ref R::*> { using response = R; };
template<auto M> concept blob_member_pointer = requires { typename blob_member<decltype(M)>::response; };

} // namespace detail

/// Bind a top-level response blob to writable caller storage. The target
/// remains exclusively borrowed until the call returns, including on error.
template<auto Member> requires detail::blob_member_pointer<Member>
auto into(std::span<uint8_t> dst) {
    return detail::field_bindings<false, Member>{{detail::blob_target{dst, {}, nullptr, 0}}};
}

template<auto Member> requires detail::blob_member_pointer<Member>
auto into(std::span<std::byte> dst) {
    return into<Member>(std::span<uint8_t>{reinterpret_cast<uint8_t*>(dst.data()), dst.size()});
}

/// Scatter one logical blob over the supplied segments. Descriptors are
/// copied, not the pages. On success written is the actual length and the
/// corresponding (contiguous-only) buffer_ref is empty. Result transfer
/// must not throw after publishing the external count.
template<auto Member> requires detail::blob_member_pointer<Member>
auto into(std::span<const iovec> dst, size_t& written) {
    for (const auto& v : dst) {
        if (v.iov_len && !v.iov_base) throw std::invalid_argument("null RPC receive segment");
    }
    return detail::field_bindings<true, Member>{{detail::blob_target{
        {}, std::vector<iovec>(dst.begin(), dst.end()), &written, 0}}};
}

template<bool... S, auto... M>
requires (sizeof...(M) >= 2)
auto bind_fields(detail::field_bindings<S, M>... field) {
    return detail::field_bindings<(S || ...), M...>{{std::move(field.targets[0])...}};
}

namespace detail {

// Only one connection reader can use a destination at a time. Sealing
// prevents new reads. An admitted read holds the lease through its CRC update.
class into_gate {
    enum phase { idle, writing, sealed, sealing };
    std::atomic<phase> phase_{idle};
    // Exactly one call waits for both terminal publication and the final
    // admitted read. Its wake state lives in the plan before admission.
    sync::detail::wake_state settled_;
    void signal() noexcept {
        if (settled_.claim_notification() == sync::detail::wake_action::schedule)
            settled_.schedule_claimed();
    }
public:
#ifdef ELIO_RUNTIME_TEST_HOOKS
    static inline std::atomic<sync::event*> quiescence_wait_for_test{nullptr};
#endif
    bool enter() noexcept {
        phase expected = idle;
        return phase_.compare_exchange_strong(expected, writing, std::memory_order_acq_rel);
    }
    void leave() noexcept {
        phase expected = writing;
        if (!phase_.compare_exchange_strong(expected, idle, std::memory_order_acq_rel)) {
            phase_.store(sealed, std::memory_order_release);
            signal();
        }
    }
    void seal() noexcept {
        auto p = phase_.load(std::memory_order_acquire);
        while (p == idle || p == writing) {
            if (phase_.compare_exchange_weak(p, p == idle ? sealed : sealing,
                                             std::memory_order_acq_rel)) {
                if (p == idle) signal();
#ifdef ELIO_RUNTIME_TEST_HOOKS
                else if (auto* notice = quiescence_wait_for_test.load(std::memory_order_acquire))
                    notice->set();
#endif
                return;
            }
        }
    }
    struct wait {
        sync::detail::wake_state& state;
        bool await_ready() const noexcept { return state.notification_was_selected(); }
        bool await_suspend(std::coroutine_handle<> h) const noexcept {
            return state.set_handle_blocked(h) && state.unblock_after_publish();
        }
        void await_resume() const noexcept {}
        ~wait() { state.abandon(); }
    };
    wait wait_settled() noexcept { return {settled_}; }
};

struct frame_io_failure {};
struct target_too_small {};
struct target_sealed {};

// The compact payload holds ordinary fields and a zero-length placeholder
// for each bound blob. Ordinary deserialization can therefore retain unbound
// buffer_refs exactly as in a regular call, without staging the bound bytes.
class field_input {
public:
    field_input(uint32_t size, bool checksum, std::span<const uint8_t> header)
        : remaining(size), checksummed(checksum),
          crc(checksum ? crc32_update(header.data(), header.size(), 0xffffffffu) : 0) {}
    virtual ~field_input() = default;
    uint32_t remaining;
    bool checksummed;
    uint32_t crc;
    message_buffer compact;

    virtual coro::task<bool> read_exact(void* dst, size_t n) = 0;

    void check(size_t n) const {
        if (n > remaining) throw serialization_error("RPC field exceeds frame");
    }
    coro::task<void> take(void* dst, size_t n) {
        check(n);
        if (n) {
            const bool read = co_await read_exact(dst, n);
            if (!read) throw frame_io_failure{};
            if (checksummed) crc = crc32_update(dst, n, crc);
            remaining -= static_cast<uint32_t>(n);
        }
    }
    coro::task<void> copy(size_t n) {
        check(n);
        if (!n) co_return;
        auto pos = compact.size();
        compact.resize(pos + n);
        co_await take(compact.data() + pos, n);
    }
    template<class T> coro::task<T> prefix() {
        auto pos = compact.size();
        co_await copy(sizeof(T));
        co_return endian::read_le<T>(compact.data() + pos);
    }
    coro::task<void> drain() {
        std::array<uint8_t, 4096> scratch;
        while (remaining) co_await take(scratch.data(), std::min<size_t>(remaining, scratch.size()));
    }
    coro::task<void> receive(blob_target& dst, into_gate& gate) {
        auto pos = compact.size();
        auto n = co_await prefix<uint32_t>();
        check(n);
        if (!dst.fits(n)) throw target_too_small{};
        endian::write_le<uint32_t>(compact.data() + pos, 0);
        dst.length = n;
        auto part = [&](void* ptr, size_t len) -> coro::task<void> {
            if (!gate.enter()) throw target_sealed{};
            struct release { into_gate& gate; ~release() { gate.leave(); } } lease{gate};
            co_await take(ptr, len);
        };
        if (!dst.written) {
            if (n) co_await part(dst.contiguous.data(), n);
        } else {
            for (const auto& v : dst.scatter) {
                if (!n) break;
                auto len = std::min<size_t>(n, v.iov_len);
                if (len) co_await part(v.iov_base, len);
                n -= static_cast<uint32_t>(len);
            }
        }
    }
};

template<class Stream>
class stream_field_input final : public field_input {
    Stream& stream_;
public:
    stream_field_input(Stream& stream, const frame_header& h, std::span<const uint8_t> bytes)
        : field_input(h.payload_length, has_flag(h.flags, message_flags::has_checksum), bytes),
          stream_(stream) {}
    coro::task<bool> read_exact(void* dst, size_t n) override {
        auto r = co_await stream_.read_exactly(dst, n);
        co_return r.result >= 0 && static_cast<size_t>(r.result) == n;
    }
};

class bound_receive {
public:
    into_gate gate;
    rpc_error decode_error = rpc_error::success;
    virtual ~bound_receive() = default;
    virtual coro::task<void> decode(field_input&) = 0;

    template<class Stream>
    coro::task<frame_result> read_body(Stream& s, const frame_header& h,
                                       std::span<const uint8_t> bytes) {
        stream_field_input<Stream> in(s, h, bytes);
        try {
            try {
                co_await decode(in);
            } catch (const target_too_small&) {
                decode_error = rpc_error::resource_exhausted;
            } catch (const target_sealed&) {
                decode_error = rpc_error::cancelled;
            } catch (const serialization_error&) {
                decode_error = rpc_error::serialization_error;
            }
            if (in.remaining) co_await in.drain();
            if (in.checksummed) {
                std::array<uint8_t, checksum_size> trailer;
                const bool read = co_await in.read_exact(trailer.data(), trailer.size());
                if (!read ||
                    endian::read_le<uint32_t>(trailer.data()) != crc32_finalize(in.crc))
                    co_return std::nullopt;
            }
        } catch (const frame_io_failure&) {
            co_return std::nullopt;
        } catch (const std::bad_alloc&) {
            // Cannot maintain framing if metadata storage cannot be obtained.
            co_return std::nullopt;
        }
        co_return std::make_pair(h, std::move(in.compact));
    }
};

// Locate variable-width metadata preceding the last bound blob without
// looking ahead into a destination. Everything after it is copied in one
// read and can use the ordinary synchronous decoder, including custom codecs
// which consume sequentially and do not inspect the entire original frame.
template<class T>
coro::task<void> copy_wire_field(field_input& in);

template<class R, size_t I = 0>
coro::task<void> copy_wire_members(field_input& in) {
    constexpr auto fields = R::_elio_rpc_get_fields();
    if constexpr (I < std::tuple_size_v<decltype(fields)>) {
        using F = std::remove_cvref_t<decltype(std::get<I>(fields))>;
        co_await copy_wire_field<typename F::value_type>(in);
        co_await copy_wire_members<R, I + 1>(in);
    }
}

template<class T>
coro::task<void> copy_wire_field(field_input& in) {
    if constexpr (is_primitive_v<T>) {
        co_await in.copy(sizeof(T));
    } else if constexpr (is_string_type_v<T> || is_buffer_ref_v<T>) {
        auto len = co_await in.prefix<uint32_t>();
        co_await in.copy(len);
    } else if constexpr (is_std_array_v<T> || is_vector_v<T>) {
        using E = element_type_t<T>;
        size_t count;
        if constexpr (is_std_array_v<T>) {
            count = std::tuple_size_v<T>;
        } else {
            auto n = co_await in.prefix<uint32_t>();
            check_container_count<min_wire_size_v<E>>(n, in.remaining);
            count = n;
        }
        if constexpr (is_primitive_v<E>) {
            if (count > in.remaining / sizeof(E)) throw serialization_error("RPC array exceeds frame");
            co_await in.copy(count * sizeof(E));
        } else {
            for (size_t i = 0; i < count; ++i) co_await copy_wire_field<E>(in);
        }
    } else if constexpr (is_optional_v<T>) {
        const auto present = co_await in.prefix<uint8_t>();
        if (present) co_await copy_wire_field<element_type_t<T>>(in);
    } else if constexpr (is_map_type_v<T>) {
        using K = typename map_types<T>::key_type;
        using V = typename map_types<T>::value_type;
        auto count = co_await in.prefix<uint32_t>();
        check_container_count<min_wire_size_v<K> + min_wire_size_v<V>>(count, in.remaining);
        for (uint32_t i = 0; i < count; ++i) {
            co_await copy_wire_field<K>(in);
            co_await copy_wire_field<V>(in);
        }
    } else if constexpr (has_rpc_fields_v<T>) {
        co_await copy_wire_members<T>(in);
    } else {
        static_assert(sizeof(T) == 0, "RPC into needs a known wire layout before a bound field");
    }
}

template<class R>
class typed_receive : public bound_receive {
public:
    virtual R parse(buffer_view&) = 0;
    virtual void commit(R&) = 0;
};

template<class R, auto M, size_t I = 0>
consteval size_t field_index() {
    constexpr auto fields = R::_elio_rpc_get_fields();
    if constexpr (I == std::tuple_size_v<decltype(fields)>) {
        static_assert(I < std::tuple_size_v<decltype(fields)>,
                      "RPC into must bind a registered top-level response buffer_ref");
    } else {
        constexpr auto pointer = std::get<I>(fields).ptr;
        if constexpr (std::is_same_v<std::remove_cv_t<decltype(pointer)>, decltype(M)>) {
            if constexpr (pointer == M) return I;
            else return field_index<R, M, I + 1>();
        } else return field_index<R, M, I + 1>();
    }
}

template<class R, bool S, auto... M>
class receive_plan final : public typed_receive<R> {
    static constexpr std::array indices{field_index<R, M>()...};
    static_assert([] {
        for (size_t i = 0; i < indices.size(); ++i)
            for (size_t j = 0; j < i; ++j)
                if (indices[i] == indices[j]) return false;
        return true;
    }(), "RPC into cannot bind the same field twice");
    field_bindings<S, M...> bindings_;

    template<size_t I>
    coro::task<void> walk(field_input& in) {
        if constexpr (I > *std::max_element(indices.begin(), indices.end())) {
            co_await in.copy(in.remaining);
        } else {
            constexpr size_t slot = std::find(indices.begin(), indices.end(), I) - indices.begin();
            if constexpr (slot < indices.size()) {
                co_await in.receive(bindings_.targets[slot], this->gate);
            } else {
                constexpr auto fields = R::_elio_rpc_get_fields();
                using F = std::remove_cvref_t<decltype(std::get<I>(fields))>;
                co_await copy_wire_field<typename F::value_type>(in);
            }
            co_await walk<I + 1>(in);
        }
    }
public:
    explicit receive_plan(field_bindings<S, M...> bindings) : bindings_(std::move(bindings)) {}
    coro::task<void> decode(field_input& in) override { return walk<0>(in); }
    R parse(buffer_view& view) override {
        R result;
        size_t credit = 0;
        for (const auto& target : bindings_.targets) credit += target.length;
        size_t index = 0;
        auto read_member = [&](auto member) {
            auto slot = std::find(indices.begin(), indices.end(), index++);
            if (slot != indices.end()) credit -= bindings_.targets[slot - indices.begin()].length;
            decode_field(view, member.get(result), credit);
        };
        std::apply([&](auto... member) { (read_member(member), ...); }, R::_elio_rpc_get_fields());
        require_typed_payload_consumed(view);
        return result;
    }
    void commit(R& response) override {
        size_t i = 0;
        auto install = [&](buffer_ref& field) {
            const auto& t = bindings_.targets[i++];
            if (t.written) {
                field = {};
                *t.written = t.length;
            } else field = buffer_ref(t.contiguous.data(), t.length);
        };
        (install(response.*M), ...);
    }
};

template<class R, bool S, auto... M>
auto make_receive_plan(field_bindings<S, M...> bindings) {
    return std::make_shared<receive_plan<R, S, M...>>(std::move(bindings));
}

} // namespace detail
} // namespace elio::rpc
