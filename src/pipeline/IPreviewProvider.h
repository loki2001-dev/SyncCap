#pragma once

#include <cstdint>
#include <memory>

#include "model/PreviewSet.h"

// the latest preview sets of the video pipeline. thread-safe; returns immutable snapshots
class IPreviewProvider {
public:
    virtual ~IPreviewProvider() = default;

    // nullptr until the first preview set of the session
    virtual std::shared_ptr<const preview_set> get_latest_preview() const = 0;
    // one of the recently kept sets, or nullptr when it has been replaced
    virtual std::shared_ptr<const preview_set> find_preview(uint64_t tick) const = 0;
};
