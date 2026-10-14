#include "util/TimeUtil.h"

#include <cstdio>
#include <ctime>

#include <array>

namespace {

constexpr int MILLIS_PER_SECOND = 1000;
constexpr size_t TIME_TEXT_SIZE = 32;

std::tm toTm(std::time_t time, bool utc) {
    std::tm result{};
#ifdef _WIN32
    if (utc) {
        gmtime_s(&result, &time);
    } else {
        localtime_s(&result, &time);
    }
#else
    if (utc) {
        gmtime_r(&time, &result);
    } else {
        localtime_r(&time, &result);
    }
#endif
    return result;
}

}  // namespace

int64_t TimeUtil::toNanoseconds(steady_time_t time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

int64_t TimeUtil::steadyNowNs() {
    return toNanoseconds(std::chrono::steady_clock::now());
}

std::string TimeUtil::formatIso8601Utc(std::chrono::system_clock::time_point time) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count() % MILLIS_PER_SECOND;
    const std::tm parts = toTm(seconds, true);
    std::array<char, TIME_TEXT_SIZE> text{};
    const size_t length = std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%S", &parts);
    std::array<char, TIME_TEXT_SIZE> withMillis{};
    std::snprintf(withMillis.data(), withMillis.size(), "%.*s.%03dZ", static_cast<int>(length), text.data(),
                  static_cast<int>(millis));
    return withMillis.data();
}

std::string TimeUtil::formatCompactLocal(std::chrono::system_clock::time_point time) {
    const std::tm parts = toTm(std::chrono::system_clock::to_time_t(time), false);
    std::array<char, TIME_TEXT_SIZE> text{};
    const size_t length = std::strftime(text.data(), text.size(), "%Y%m%d_%H%M%S", &parts);
    return std::string(text.data(), length);
}

std::string TimeUtil::formatClockLocal(std::chrono::system_clock::time_point time) {
    const std::tm parts = toTm(std::chrono::system_clock::to_time_t(time), false);
    std::array<char, TIME_TEXT_SIZE> text{};
    const size_t length = std::strftime(text.data(), text.size(), "%H:%M:%S", &parts);
    return std::string(text.data(), length);
}
