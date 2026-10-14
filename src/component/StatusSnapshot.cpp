#include "component/StatusSnapshot.h"

#include "util/TimeUtil.h"

namespace {

constexpr double NANOS_PER_SECOND = 1e9;

}  // namespace

system_status StatusSnapshot::build(const IStateReader& reader, const status_identity& identity) {
    system_status status;
    status.deviceId = identity.deviceId;
    status.timestamp = TimeUtil::formatIso8601Utc(std::chrono::system_clock::now());
    status.uptimeS = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - identity.startedAt)
                                             .count()) /
                     NANOS_PER_SECOND;
    status.acquisition = *reader.get_acquisition_state();
    const auto video = reader.get_video_state();
    status.channels = video->channels;
    status.sync = video->sync;
    status.sensor = *reader.get_sensor_state();
    status.storage = *reader.get_storage_state();
    return status;
}
