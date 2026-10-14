#include "catch_amalgamated.hpp"

#include <string>
#include <vector>

#include "core/MessageBus.h"
#include "core/UvLoop.h"

namespace {

struct text_message {
    std::string text;
};

struct number_message {
    int value = 0;
};

}  // namespace

TEST_CASE("MessageBus delivers by type on drain") {
    UvLoop loop;
    MessageBus bus(loop, 16);
    std::vector<std::string> texts;
    int numbers = 0;
    bus.subscribe<text_message>([&texts](const text_message& message) {
        texts.push_back(message.text);
    });
    bus.subscribe<number_message>([&numbers](const number_message& message) {
        numbers += message.value;
    });
    bus.publish(text_message{"a"});
    bus.publish(number_message{5});
    REQUIRE(texts.empty());
    bus.drain();
    REQUIRE(texts == std::vector<std::string>{"a"});
    REQUIRE(numbers == 5);
}

TEST_CASE("MessageBus stops delivering after unsubscribe") {
    UvLoop loop;
    MessageBus bus(loop, 16);
    int count = 0;
    const subscription_id_t id = bus.subscribe<number_message>([&count](const number_message&) {
        count += 1;
    });
    bus.publish(number_message{});
    bus.drain();
    bus.unsubscribe(id);
    bus.publish(number_message{});
    bus.drain();
    REQUIRE(count == 1);
}

TEST_CASE("MessageBus drops the oldest message on overflow and counts it") {
    UvLoop loop;
    MessageBus bus(loop, 2);
    std::vector<int> values;
    bus.subscribe<number_message>([&values](const number_message& message) {
        values.push_back(message.value);
    });
    bus.publish(number_message{1});
    bus.publish(number_message{2});
    bus.publish(number_message{3});
    bus.drain();
    REQUIRE(values == std::vector<int>{2, 3});
    REQUIRE(bus.get_dropped_total() == 1);
}

TEST_CASE("MessageBus delivers a message published inside a handler after the current one") {
    UvLoop loop;
    MessageBus bus(loop, 16);
    std::vector<std::string> order;
    bus.subscribe<number_message>([&](const number_message&) {
        order.push_back("number");
        bus.publish(text_message{"from handler"});
    });
    bus.subscribe<text_message>([&](const text_message& message) {
        order.push_back(message.text);
    });
    bus.publish(number_message{});
    bus.drain();
    REQUIRE(order == std::vector<std::string>{"number", "from handler"});
}
