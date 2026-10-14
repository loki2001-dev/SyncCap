#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "model/RecordItem.h"

// one channel of a preview set: a downscaled JPEG of the latched picture
struct preview_image {
    int index = 0;  // position in the configured channel list
    int channelId = 0;
    std::string sourceType;
    int64_t timestampNs = 0;  // latch time, steady_clock
    int width = 0;
    int height = 0;
    std::shared_ptr<const std::vector<uint8_t>> jpeg;
};

// the pictures of one locked tick, all channels together
struct preview_set {
    uint64_t tick = 0;
    int64_t targetNs = 0;
    int64_t offsetNs = 0;
    sync_verdict verdict = sync_verdict::locked;
    std::vector<preview_image> images;
};
