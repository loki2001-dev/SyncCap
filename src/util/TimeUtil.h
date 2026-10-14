#pragma once

#include <chrono>
#include <cstdint>
#include <string>

using steady_time_t = std::chrono::steady_clock::time_point;

// clock helpers. steady_clock for every interval and timestamp, system_clock only for names and reports
class TimeUtil {
public:
    static int64_t toNanoseconds(steady_time_t time);
    static int64_t steadyNowNs();

    // 2026-10-14T13:00:25.120Z
    static std::string formatIso8601Utc(std::chrono::system_clock::time_point time);
    // 20261014_130025 in local time, used in directory names
    static std::string formatCompactLocal(std::chrono::system_clock::time_point time);
    // 13:00:25 in local time, used in on-screen event lists
    static std::string formatClockLocal(std::chrono::system_clock::time_point time);
};
