#pragma once

#include <cstdint>

/**
 * @brief the shared frame trigger (software FSYNC). immutable, so every capture thread reads it without a lock
 *
 * tick k is due at epoch + k * period
 */
class FrameClock {
public:
    FrameClock();
    FrameClock(int64_t epochNs, int fps);

    int64_t targetOf(uint64_t tick) const;
    // the first tick due at or after timeNs
    uint64_t nextTickAt(int64_t timeNs) const;
    int64_t get_period_ns() const;
    int64_t get_epoch_ns() const;

private:
    int64_t _epochNs;
    int64_t _periodNs;
    int _fps;
};
