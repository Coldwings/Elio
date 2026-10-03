#include <catch2/catch_test_macros.hpp>
#include <elio/http/detail/route_connection.hpp>
#include <elio/http/detail/route_idle_pool.hpp>

#include <array>
#include <fcntl.h>

using namespace elio::http::detail;

namespace {

struct socket_pair {
    route_connection client;
    elio::net::tcp_stream peer{-1};
    int descriptor;
    socket_pair() {
        std::array<int, 2> descriptors{};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                         0, descriptors.data()) != 0)
            throw std::runtime_error("pool fixture socketpair failed");
        descriptor = descriptors[0];
        client = route_connection(elio::net::stream(elio::net::tcp_stream{descriptor}));
        peer = elio::net::tcp_stream{descriptors[1]};
    }
};

struct collision_guard {
    collision_guard() { force_connection_key_collision_for_test.store(true); }
    ~collision_guard() { force_connection_key_collision_for_test.store(false); }
};

connection_key proxy_route() {
    connection_key key;
    key.mode = route_mode::connect_tunnel;
    key.target = route_endpoint::from("origin.example", 443);
    key.target_secure = true;
    key.hops.push_back({route_endpoint::from("proxy.example", 8080), 0, 1});
    return key;
}

} // namespace

TEST_CASE("Private proxy idle pooling isolates complete routes despite hash collisions",
          "[http][proxy][pool][issue-1249]") {
    collision_guard collision;
    route_idle_pool<route_connection> pool(1, std::chrono::seconds(60));
    auto first = proxy_route();
    auto other_auth = first;
    other_auth.hops[0].auth_domain = 2;
    auto other_target = first;
    other_target.target = route_endpoint::from("other.example", 443);
    socket_pair one, two, three;
    REQUIRE(pool.retain(first, one.client));
    REQUIRE(pool.retain(other_auth, two.client));
    REQUIRE(pool.retain(other_target, three.client));
    REQUIRE(one.client.fd() == -1);
    auto selected_two = pool.take(other_auth);
    auto selected_one = pool.take(first);
    auto selected_three = pool.take(other_target);
    REQUIRE(selected_two);
    REQUIRE(selected_one);
    REQUIRE(selected_three);
    CHECK(selected_two->fd() == two.descriptor);
    CHECK(selected_one->fd() == one.descriptor);
    CHECK(selected_three->fd() == three.descriptor);
    REQUIRE_FALSE(pool.take(first));
}

TEST_CASE("Private proxy idle cap rejects retention without taking caller ownership",
          "[http][proxy][pool][issue-1249]") {
    route_idle_pool<route_connection> pool(1, std::chrono::seconds(60));
    socket_pair one, two;
    const auto key = proxy_route();
    REQUIRE(pool.retain(key, one.client));
    REQUIRE_FALSE(pool.retain(key, two.client));
    REQUIRE(two.client.fd() == two.descriptor);
    {
        auto detached = pool.detach();
        REQUIRE_FALSE(pool.take(key));
        REQUIRE(::fcntl(one.descriptor, F_GETFD) >= 0);
    }
    REQUIRE(::fcntl(one.descriptor, F_GETFD) == -1);
    REQUIRE(::fcntl(two.descriptor, F_GETFD) >= 0);
    route_idle_pool<route_connection> no_idle(0, std::chrono::seconds(60));
    REQUIRE_FALSE(no_idle.retain(key, two.client));
    REQUIRE(two.client.fd() == two.descriptor);
}

TEST_CASE("Private proxy idle expiry closes retired streams before returning a miss",
          "[http][proxy][pool][issue-1249]") {
    route_idle_pool<route_connection> pool(1, std::chrono::seconds(0));
    socket_pair one;
    const auto key = proxy_route();
    REQUIRE(pool.retain(key, one.client));
    REQUIRE_FALSE(pool.take(key));
    REQUIRE(::fcntl(one.descriptor, F_GETFD) == -1);
}
