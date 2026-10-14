#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

// fixed-length history of one metric for a plot. oldest value first, the newest replaces the oldest when full
class MetricSeries {
public:
    explicit MetricSeries(size_t capacity = DEFAULT_CAPACITY)
        : _capacity(capacity == 0 ? 1 : capacity) {
        _values.reserve(_capacity);
    }

    void push(float value) {
        if (_values.size() < _capacity) {
            _values.push_back(value);
            return;
        }
        _values[_next] = value;
        _next = (_next + 1) % _capacity;
    }

    // values in time order
    std::vector<float> get_ordered() const {
        std::vector<float> ordered;
        ordered.reserve(_values.size());
        for (size_t index = 0; index < _values.size(); ++index) {
            ordered.push_back(_values[(_next + index) % _values.size()]);
        }
        return ordered;
    }

    float get_max() const {
        return _values.empty() ? 0.0F : *std::max_element(_values.begin(), _values.end());
    }

    float get_last() const {
        if (_values.empty()) {
            return 0.0F;
        }
        return _values[(_next + _values.size() - 1) % _values.size()];
    }

    size_t get_size() const {
        return _values.size();
    }

    size_t get_capacity() const {
        return _capacity;
    }

    static constexpr size_t DEFAULT_CAPACITY = 300;

private:
    size_t _capacity;
    size_t _next = 0;
    std::vector<float> _values;
};
