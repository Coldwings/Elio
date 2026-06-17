#include <catch2/catch_test_macros.hpp>
#include <elio/sync/channel.hpp>
#include <iostream>

TEST_CASE("channel close debug", "[sync][channel]") {
    elio::sync::channel<int> c(10);

    std::cout << "Before send: size = " << c.size() << std::endl;
    c.try_send(1);
    std::cout << "After send(1): size = " << c.size() << std::endl;
    c.try_send(2);
    std::cout << "After send(2): size = " << c.size() << std::endl;

    c.close();
    std::cout << "After close: size = " << c.size() << std::endl;

    auto v = c.try_recv();
    std::cout << "After try_recv: has_value = " << v.has_value() << std::endl;
    if (v.has_value()) {
        std::cout << "Value = " << *v << std::endl;
    }

    REQUIRE(v.has_value());
    REQUIRE(*v == 1);
}
