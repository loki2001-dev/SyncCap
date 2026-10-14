#include "catch_amalgamated.hpp"

#include <optional>

#include "core/BoundedQueue.h"

TEST_CASE("BoundedQueue accepts up to its capacity") {
    BoundedQueue<int> queue(3, overflow_policy::reject);
    REQUIRE(queue.push(1) == push_result::accepted);
    REQUIRE(queue.push(2) == push_result::accepted);
    REQUIRE(queue.push(3) == push_result::accepted);
    REQUIRE(queue.get_size() == 3);
    REQUIRE(queue.get_dropped_total() == 0);
}

TEST_CASE("BoundedQueue rejects when full under the reject policy") {
    BoundedQueue<int> queue(2, overflow_policy::reject);
    queue.push(1);
    queue.push(2);
    REQUIRE(queue.push(3) == push_result::rejected);
    REQUIRE(queue.push(4) == push_result::rejected);
    REQUIRE(queue.get_dropped_total() == 2);
    REQUIRE(*queue.pop() == 1);
    REQUIRE(*queue.pop() == 2);
    REQUIRE_FALSE(queue.pop().has_value());
}

TEST_CASE("BoundedQueue drops the oldest when full under the drop_oldest policy") {
    BoundedQueue<int> queue(2, overflow_policy::drop_oldest);
    queue.push(1);
    queue.push(2);
    std::optional<int> evicted;
    REQUIRE(queue.push(3, evicted) == push_result::dropped_oldest);
    REQUIRE(evicted == 1);
    REQUIRE(queue.get_dropped_total() == 1);
    REQUIRE(*queue.pop() == 2);
    REQUIRE(*queue.pop() == 3);
}

TEST_CASE("BoundedQueue treats capacity zero as one") {
    BoundedQueue<int> queue(0, overflow_policy::reject);
    REQUIRE(queue.get_capacity() == 1);
    REQUIRE(queue.push(1) == push_result::accepted);
    REQUIRE(queue.push(2) == push_result::rejected);
}

TEST_CASE("BoundedQueue takeAll empties the queue") {
    BoundedQueue<int> queue(4, overflow_policy::reject);
    queue.push(1);
    queue.push(2);
    const auto items = queue.takeAll();
    REQUIRE(items.size() == 2);
    REQUIRE(queue.get_size() == 0);
}
