#include "catch_amalgamated.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "core/BlockingQueue.h"

TEST_CASE("BlockingQueue pop returns false after close and drain") {
    BlockingQueue<int> queue(4, overflow_policy::reject);
    queue.push(7);
    queue.close();
    int value = 0;
    REQUIRE(queue.pop(value));
    REQUIRE(value == 7);
    REQUIRE_FALSE(queue.pop(value));
}

TEST_CASE("BlockingQueue rejects pushes after close") {
    BlockingQueue<int> queue(4, overflow_policy::drop_oldest);
    queue.close();
    REQUIRE(queue.push(1) == push_result::rejected);
    REQUIRE(queue.get_dropped_total() == 1);
}

TEST_CASE("BlockingQueue close wakes a blocked consumer") {
    BlockingQueue<int> queue(4, overflow_policy::reject);
    std::atomic<bool> returned{false};
    std::thread consumer([&queue, &returned]() {
        int value = 0;
        queue.pop(value);
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE_FALSE(returned);
    queue.close();
    consumer.join();
    REQUIRE(returned);
}

TEST_CASE("BlockingQueue popFor times out on an empty queue") {
    BlockingQueue<int> queue(4, overflow_policy::reject);
    int value = 0;
    const auto begin = std::chrono::steady_clock::now();
    REQUIRE_FALSE(queue.popFor(value, std::chrono::milliseconds(20)));
    REQUIRE(std::chrono::steady_clock::now() - begin >= std::chrono::milliseconds(15));
}

TEST_CASE("BlockingQueue drop_oldest keeps the newest items and counts drops") {
    BlockingQueue<int> queue(3, overflow_policy::drop_oldest);
    for (int value = 0; value < 10; ++value) {
        queue.push(value);
    }
    REQUIRE(queue.get_dropped_total() == 7);
    REQUIRE(queue.get_high_water() == 3);
    int value = 0;
    REQUIRE(queue.tryPop(value));
    REQUIRE(value == 7);
    REQUIRE(queue.clear() == 2);
    REQUIRE(queue.get_size() == 0);
}

TEST_CASE("BlockingQueue loses nothing under concurrent producers") {
    constexpr int PRODUCERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 20000;
    BlockingQueue<int> queue(64, overflow_policy::reject);
    std::atomic<long long> rejected{0};
    std::atomic<long long> consumed{0};
    std::atomic<long long> sum{0};

    std::thread consumer([&]() {
        int value = 0;
        while (queue.pop(value)) {
            consumed += 1;
            sum += value;
        }
    });
    std::vector<std::thread> producers;
    for (int producer = 0; producer < PRODUCERS; ++producer) {
        producers.emplace_back([&]() {
            for (int item = 1; item <= ITEMS_PER_PRODUCER; ++item) {
                // retry until accepted: every item must arrive exactly once
                while (queue.push(item) == push_result::rejected) {
                    rejected += 1;
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& thread : producers) {
        thread.join();
    }
    queue.close();
    consumer.join();

    const long long expectedSum = static_cast<long long>(PRODUCERS) * ITEMS_PER_PRODUCER * (ITEMS_PER_PRODUCER + 1) / 2;
    REQUIRE(consumed == static_cast<long long>(PRODUCERS) * ITEMS_PER_PRODUCER);
    REQUIRE(sum == expectedSum);
    REQUIRE(static_cast<long long>(queue.get_dropped_total()) == rejected);
}
