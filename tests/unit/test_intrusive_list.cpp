#include <catch2/catch_test_macros.hpp>
#include <elio/detail/intrusive_list.hpp>

using namespace elio::detail;

// Test node type
struct test_node : intrusive_list_node<test_node> {
    int value;
    explicit test_node(int v = 0) : value(v) {}
};

TEST_CASE("intrusive_list: empty list", "[intrusive_list]") {
    intrusive_list<test_node> list;

    REQUIRE(list.empty());
    REQUIRE(list.size() == 0);
    REQUIRE(list.front() == nullptr);
    REQUIRE(list.back() == nullptr);
    REQUIRE(list.pop_front() == nullptr);
    REQUIRE(list.pop_back() == nullptr);
}

TEST_CASE("intrusive_list: push_back and pop_front", "[intrusive_list]") {
    intrusive_list<test_node> list;
    test_node a(1), b(2), c(3);

    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);

    REQUIRE(list.size() == 3);
    REQUIRE(list.front()->value == 1);
    REQUIRE(list.back()->value == 3);

    auto* n1 = list.pop_front();
    REQUIRE(n1->value == 1);
    REQUIRE_FALSE(n1->is_linked());

    auto* n2 = list.pop_front();
    REQUIRE(n2->value == 2);

    auto* n3 = list.pop_front();
    REQUIRE(n3->value == 3);

    REQUIRE(list.empty());
}

TEST_CASE("intrusive_list: push_front and pop_back", "[intrusive_list]") {
    intrusive_list<test_node> list;
    test_node a(1), b(2), c(3);

    list.push_front(&a);
    list.push_front(&b);
    list.push_front(&c);

    REQUIRE(list.front()->value == 3);
    REQUIRE(list.back()->value == 1);

    auto* n1 = list.pop_back();
    REQUIRE(n1->value == 1);

    auto* n2 = list.pop_back();
    REQUIRE(n2->value == 2);

    auto* n3 = list.pop_back();
    REQUIRE(n3->value == 3);

    REQUIRE(list.empty());
}

TEST_CASE("intrusive_list: remove from middle", "[intrusive_list]") {
    intrusive_list<test_node> list;
    test_node a(1), b(2), c(3);

    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);

    list.remove(&b);
    REQUIRE_FALSE(b.is_linked());
    REQUIRE(list.size() == 2);
    REQUIRE(list.front()->value == 1);
    REQUIRE(list.back()->value == 3);

    // Remove head
    list.remove(&a);
    REQUIRE(list.size() == 1);
    REQUIRE(list.front()->value == 3);

    // Remove tail (only element)
    list.remove(&c);
    REQUIRE(list.empty());
}

TEST_CASE("intrusive_list: remove unlinked node is safe", "[intrusive_list]") {
    intrusive_list<test_node> list;
    test_node a(1);

    // Removing a node that was never added should be a no-op
    list.remove(&a);
    REQUIRE_FALSE(a.is_linked());
    REQUIRE(list.empty());
}

TEST_CASE("intrusive_list: is_linked tracks state", "[intrusive_list]") {
    intrusive_list<test_node> list;
    test_node a(1);

    REQUIRE_FALSE(a.is_linked());
    list.push_back(&a);
    REQUIRE(a.is_linked());
    list.remove(&a);
    REQUIRE_FALSE(a.is_linked());
}

TEST_CASE("intrusive_list: contains", "[intrusive_list]") {
    intrusive_list<test_node> list;
    test_node a(1), b(2), c(3);

    list.push_back(&a);
    list.push_back(&b);

    REQUIRE(list.contains(&a));
    REQUIRE(list.contains(&b));
    REQUIRE_FALSE(list.contains(&c));

    // Clean up before nodes go out of scope
    list.clear();
}

TEST_CASE("intrusive_list: clear unlinks all nodes", "[intrusive_list]") {
    intrusive_list<test_node> list;
    test_node a(1), b(2), c(3);

    list.push_back(&a);
    list.push_back(&b);
    list.push_back(&c);

    list.clear();
    REQUIRE(list.empty());
    REQUIRE_FALSE(a.is_linked());
    REQUIRE_FALSE(b.is_linked());
    REQUIRE_FALSE(c.is_linked());
}

TEST_CASE("intrusive_list: single element operations", "[intrusive_list]") {
    intrusive_list<test_node> list;
    test_node a(42);

    list.push_back(&a);
    REQUIRE(list.size() == 1);
    REQUIRE(list.front() == &a);
    REQUIRE(list.back() == &a);

    list.remove(&a);
    REQUIRE(list.empty());
}

TEST_CASE("intrusive_list: move constructor", "[intrusive_list]") {
    intrusive_list<test_node> list;
    test_node a(1), b(2);
    list.push_back(&a);
    list.push_back(&b);

    intrusive_list<test_node> moved(std::move(list));
    REQUIRE(moved.size() == 2);
    REQUIRE(moved.front()->value == 1);
    REQUIRE(list.empty());

    moved.clear();
}
