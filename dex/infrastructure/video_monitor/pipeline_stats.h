#ifndef DEX_INFRASTRUCTURE_VIDEO_MONITOR_PIPELINE_STATS_H
#define DEX_INFRASTRUCTURE_VIDEO_MONITOR_PIPELINE_STATS_H

#include <atomic>
#include <cstdint>
#include <string>

namespace dex::video_monitor {

enum class PipelineState : int {
  kWaitingForShm = 0,
  kWaitingForFrames = 1,
  kStreaming = 2,
  kStale = 3,
};

inline const char* PipelineStateToString(PipelineState state) {
  switch (state) {
    case PipelineState::kWaitingForShm:
      return "waiting_for_shm";
    case PipelineState::kWaitingForFrames:
      return "waiting_for_frames";
    case PipelineState::kStreaming:
      return "streaming";
    case PipelineState::kStale:
      return "stale";
  }
  return "unknown";
}

/// Lock-free pipeline statistics. Written by the pipeline thread (sole writer),
/// read by the HTTP handler on demand. No synchronization needed.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct PipelineStats {
  std::atomic<uint64_t> frames_encoded{0};
  std::atomic<uint64_t> frames_dropped{0};
  std::atomic<uint64_t> last_frame_timestamp_ns{0};
  std::atomic<uint32_t> width{0};
  std::atomic<uint32_t> height{0};
  std::atomic<uint32_t> encoder_reinits{0};
  std::atomic<uint32_t> clients_connected{0};
  std::atomic<int> state{static_cast<int>(PipelineState::kWaitingForShm)};
  std::atomic<uint32_t> measured_fps_x10{0};  // FPS * 10 for one decimal place without floats.

  void SetState(PipelineState new_state) { state.store(static_cast<int>(new_state), std::memory_order_relaxed); }

  [[nodiscard]] PipelineState GetState() const {
    return static_cast<PipelineState>(state.load(std::memory_order_relaxed));
  }
};

// NOLINTEND(misc-non-private-member-variables-in-classes)

}  // namespace dex::video_monitor

#endif  // DEX_INFRASTRUCTURE_VIDEO_MONITOR_PIPELINE_STATS_H
