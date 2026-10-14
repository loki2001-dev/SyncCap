#include "catch_amalgamated.hpp"

#include <chrono>
#include <memory>

#include "pipeline/FramePool.h"

TEST_CASE("FramePool hands out at most its capacity") {
    auto pool = FramePool::create(64, 48, 2);
    auto first = pool->acquire(std::chrono::milliseconds(1));
    auto second = pool->acquire(std::chrono::milliseconds(1));
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(first->get_size() == FrameBuffer::computeSize(64, 48));
    REQUIRE_FALSE(pool->acquire(std::chrono::milliseconds(5)));
    REQUIRE(pool->get_exhausted_total() == 1);
}

TEST_CASE("FramePool reuses a released buffer") {
    auto pool = FramePool::create(64, 48, 1);
    const FrameBuffer* address = nullptr;
    {
        auto buffer = pool->acquire(std::chrono::milliseconds(1));
        address = buffer.get();
    }
    auto again = pool->acquire(std::chrono::milliseconds(1));
    REQUIRE(again.get() == address);
    REQUIRE(pool->get_available() == 0);
}

TEST_CASE("FramePool buffer may outlive its pool") {
    std::shared_ptr<FrameBuffer> survivor;
    {
        auto pool = FramePool::create(64, 48, 1);
        survivor = pool->acquire(std::chrono::milliseconds(1));
    }
    // the deleter must free the buffer without touching the destroyed pool
    survivor.reset();
    SUCCEED();
}

TEST_CASE("FramePool close wakes a waiter and refuses further buffers") {
    auto pool = FramePool::create(64, 48, 1);
    auto held = pool->acquire(std::chrono::milliseconds(1));
    pool->close();
    REQUIRE_FALSE(pool->acquire(std::chrono::seconds(5)));
}

TEST_CASE("FrameBuffer lays out the I420 planes contiguously") {
    FrameBuffer buffer(8, 4);
    REQUIRE(buffer.get_size() == 48);
    REQUIRE(buffer.get_plane(1) - buffer.get_plane(0) == 32);
    REQUIRE(buffer.get_plane(2) - buffer.get_plane(1) == 8);
    REQUIRE(buffer.get_stride(0) == 8);
    REQUIRE(buffer.get_stride(2) == 4);
    REQUIRE(buffer.get_plane(3) == nullptr);
}
