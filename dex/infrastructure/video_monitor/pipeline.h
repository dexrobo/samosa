#ifndef DEX_INFRASTRUCTURE_VIDEO_MONITOR_PIPELINE_H
#define DEX_INFRASTRUCTURE_VIDEO_MONITOR_PIPELINE_H

#include <atomic>
#include <thread>

#include "dex/infrastructure/video_monitor/config.h"
#include "dex/infrastructure/video_monitor/fragment_ring.h"
#include "dex/infrastructure/video_monitor/pipeline_stats.h"

namespace dex::video_monitor {

/// Reads frames from a shared memory topic, encodes to H.264, muxes to fMP4,
/// and pushes fragments to a FragmentRing for HTTP fan-out.
///
/// Each pipeline runs on its own dedicated thread and owns all its resources
/// (monitor, frame buffer, YUV buffer, encoder, muxer).
class TopicPipeline {
 public:
  TopicPipeline(TopicConfig config, FragmentRing& ring);
  ~TopicPipeline();

  TopicPipeline(const TopicPipeline&) = delete;
  TopicPipeline& operator=(const TopicPipeline&) = delete;
  TopicPipeline(TopicPipeline&&) = delete;
  TopicPipeline& operator=(TopicPipeline&&) = delete;

  /// Start the pipeline thread. Non-blocking.
  void Start();

  /// Signal stop and join the thread.
  void Stop();

  [[nodiscard]] bool IsRunning() const { return running_.load(std::memory_order_relaxed); }

  /// Access pipeline stats (lock-free reads safe from any thread).
  [[nodiscard]] const PipelineStats& Stats() const { return stats_; }

 private:
  void Run();

  TopicConfig config_;
  FragmentRing& ring_;
  PipelineStats stats_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::thread thread_;
};

}  // namespace dex::video_monitor

#endif  // DEX_INFRASTRUCTURE_VIDEO_MONITOR_PIPELINE_H
