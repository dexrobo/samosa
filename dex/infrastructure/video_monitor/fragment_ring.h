#ifndef DEX_INFRASTRUCTURE_VIDEO_MONITOR_FRAGMENT_RING_H
#define DEX_INFRASTRUCTURE_VIDEO_MONITOR_FRAGMENT_RING_H

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace dex::video_monitor {

/// A single fMP4 media segment (moof + mdat).
struct Fragment {
  std::shared_ptr<const std::vector<uint8_t>> data;
  uint64_t sequence{0};
  uint64_t timestamp_us{0};
  bool contains_idr{false};
};

/// Thread-safe broadcast ring buffer for fMP4 fragments.
///
/// Single writer (pipeline thread) pushes fragments. Multiple readers (HTTP handlers)
/// independently read at their own pace. Slow readers skip to the latest IDR.
class FragmentRing {
 public:
  explicit FragmentRing(size_t capacity);

  // --- Writer interface (single writer only) ---

  /// Push a new fragment. Thread-safe but assumes a single writer.
  void Push(Fragment fragment);

  /// Set the init segment (ftyp + moov). Called once when the first IDR is encoded.
  void SetInitSegment(std::vector<uint8_t> init);

  // --- Reader interface (multiple concurrent readers) ---

  struct ReadResult {
    /// Init segment, present on first read by a new client.
    std::optional<std::vector<uint8_t>> init_segment;
    /// Fragments available since after_sequence.
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> fragments;
    /// Sequence number of the last returned fragment (or after_sequence if none).
    uint64_t last_sequence{0};
  };

  /// Read all fragments newer than `after_sequence`.
  /// If `after_sequence` is 0 or too old, returns from the latest IDR.
  /// Includes the init segment if `after_sequence` is 0.
  [[nodiscard]] ReadResult ReadFrom(uint64_t after_sequence) const;

  /// Block until a fragment newer than `after_sequence` is available, or timeout.
  /// Returns true if new data is available, false on timeout or shutdown.
  bool WaitForNew(uint64_t after_sequence, std::chrono::milliseconds timeout) const;

  /// Wake all waiting readers (used during shutdown).
  void NotifyAll();

  /// Current head sequence number.
  [[nodiscard]] uint64_t HeadSequence() const;

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;

  std::vector<Fragment> ring_;
  size_t capacity_;
  uint64_t head_sequence_{0};
  uint64_t latest_idr_sequence_{0};
  std::vector<uint8_t> init_segment_;
  bool shutdown_{false};
};

}  // namespace dex::video_monitor

#endif  // DEX_INFRASTRUCTURE_VIDEO_MONITOR_FRAGMENT_RING_H
