#ifndef DEX_INFRASTRUCTURE_VIDEO_MONITOR_TIME_BASE_H
#define DEX_INFRASTRUCTURE_VIDEO_MONITOR_TIME_BASE_H

#include <chrono>
#include <cstdint>

namespace dex::video_monitor {

/// Shared monotonic time base for all pipelines.
/// All PTS values are relative to this epoch, ensuring time-synchronized streams.
class TimeBase {
 public:
  static TimeBase& Instance() {
    static TimeBase instance;
    return instance;
  }

  /// Microseconds since the time base was created.
  [[nodiscard]] uint64_t MicrosecondsSinceStart() const {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - epoch_).count());
  }

  TimeBase(const TimeBase&) = delete;
  TimeBase& operator=(const TimeBase&) = delete;

 private:
  TimeBase() : epoch_(std::chrono::steady_clock::now()) {}

  std::chrono::steady_clock::time_point epoch_;
};

}  // namespace dex::video_monitor

#endif  // DEX_INFRASTRUCTURE_VIDEO_MONITOR_TIME_BASE_H
