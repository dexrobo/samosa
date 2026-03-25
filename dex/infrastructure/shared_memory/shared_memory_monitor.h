#ifndef DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_MONITOR_H
#define DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_MONITOR_H

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "dex/infrastructure/shared_memory/shared_memory_streaming.h"

namespace dex::shared_memory {

namespace detail {

using SequenceNumber = uint64_t;
inline constexpr double kDefaultMonitorTimeoutSec = 0.1;

// Concept for the allowed callback types
template <typename Fn, typename Buffer>
concept MonitorCallback = std::invocable<Fn, const Buffer&> || std::invocable<Fn, const Buffer&, SequenceNumber>;

}  // namespace detail
/**
 * Defines how the Monitor should handle producer writing state.
 */
enum class MonitorReadMode {
  // Do not begin from an already-active producer write episode. If validation detects overlap while
  // copying, the monitor discards or retries within the caller's timeout budget.
  SkipIfBusy,

  // If the producer is already writing, wait for `sequence_and_writing` to change within the timeout
  // budget. Only validated snapshots are returned.
  WaitForStableSnapshot,

  // Best-effort mode. The monitor may sample while a producer write is in flight, but still performs
  // post-copy validation and discards detected overlap when possible.
  Opportunistic
};

/**
 * Monitor for passively reading shared memory content.
 *
 * This class allows monitoring the content of a shared memory segment
 * without participating in the producer-consumer protocol. It's useful
 * for debugging, monitoring, or diagnostic purposes.
 *
 * Unlike Consumer, the Monitor:
 * - Does not update the read_index
 * - Does not block the Producer
 * - Can observe data without affecting the normal Producer-Consumer flow
 * - Samples producer-written state (`last_written_buffer` and monitor metadata), not the consumer
 *   publication handshake (`read_index` / `write_index`)
 *
 * Guarantees:
 * - Best-effort and lossy: samples may be skipped
 * - Passive: no writes to producer/consumer ownership state
 * - Returns only snapshots that pass monitor-side validation
 *
 * @tparam Buffer The buffer type to be monitored
 * @tparam buffer_size The number of buffers (default=2 for double buffering)
 * @tparam StreamingSharedMemoryBuffer The type of the shared memory buffer (default=LockFreeSharedMemoryBuffer)
 */
template <typename Buffer, size_t buffer_size = 2,
          template <typename, size_t> typename StreamingSharedMemoryBuffer = LockFreeSharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, StreamingSharedMemoryBuffer>
class Monitor {
  using SharedMemoryBuffer = SharedMemory<Buffer, buffer_size, StreamingSharedMemoryBuffer>;

 public:
  /**
   * @brief Constructs a new Monitor object.
   * @param shared_memory_name name of the shared memory segment to monitor
   */
  explicit Monitor(const std::string_view shared_memory_name)
      : shared_memory_buffer_(SharedMemoryBuffer::Open(
            shared_memory_name, ValidateBuffer<Buffer, buffer_size, StreamingSharedMemoryBuffer>)),
        streaming_control_{StreamingControl::Instance()},
        buffer_cache_(std::make_unique<Buffer>()) {}

  /**
   * @brief Checks if the monitor is valid and connected to shared memory.
   * @return true if the monitor is valid, false otherwise
   */
  [[nodiscard]] bool IsValid() const { return shared_memory_buffer_.IsValid(); }

  /**
   * @brief Get the latest validated buffer as a convenience reference.
   * @param timeout_sec Maximum time to wait in seconds
   * @param read_mode How to handle the case when producer is writing
   * @return Optional reference to the latest buffer, or nullopt if no valid buffer
   *
   * Prefer ReadInto() for repeated or concurrent reads. This convenience API returns a reference backed by
   * internal scratch storage rather than caller-owned memory.
   */
  std::optional<std::reference_wrapper<const Buffer>> GetLatestBuffer(
      double timeout_sec = detail::kDefaultMonitorTimeoutSec,
      MonitorReadMode read_mode = MonitorReadMode::WaitForStableSnapshot);

  /**
   * @brief Copy the latest validated snapshot into caller-owned storage.
   * @param destination Buffer to overwrite with the accepted snapshot
   * @param timeout_sec Maximum time to wait in seconds
   * @param read_mode How to handle the case when producer is writing
   * @param sequence Optional out-parameter for the accepted monitor sequence
   * @return true if a validated snapshot was copied, false otherwise
   *
   * This is the preferred API for repeated reads because it reuses caller-owned storage and avoids
   * per-read allocations.
   */
  [[nodiscard]] bool ReadInto(Buffer& destination, double timeout_sec = detail::kDefaultMonitorTimeoutSec,
                              MonitorReadMode read_mode = MonitorReadMode::WaitForStableSnapshot,
                              detail::SequenceNumber* sequence = nullptr) const;

  /**
   * @brief Run a monitoring loop that calls a function when new data is available
   * @param monitor_fn Function to call with new data
   * @param timeout_sec Maximum time to wait for new data in seconds
   * @param max_iterations Maximum number of iterations (0 for unlimited)
   * @param writing_mode How to handle the case when producer is writing
   *
   * TODO: Support monitor_fn with 1 argument. Current implementation only supports 2.
   */
  void Run(auto&& monitor_fn, double timeout_sec = detail::kDefaultMonitorTimeoutSec,
           MonitorReadMode read_mode = MonitorReadMode::WaitForStableSnapshot, uint max_iterations = 0)
    requires detail::MonitorCallback<std::remove_reference_t<decltype(monitor_fn)>, Buffer>;

 private:
  struct CandidateSlot {
    uint32_t raw_slot_id;
    size_t slot_index;
    uint32_t pre_slot_packed;
  };

  enum class InitialStateAction {
    Continue,
    Retry,
    ReturnNone,
  };

  [[nodiscard]] bool WaitForMonitorStateChange(uint32_t expected_packed,
                                               const std::chrono::steady_clock::time_point& deadline) const;
  [[nodiscard]] InitialStateAction HandleInitialState(MonitorReadMode read_mode, uint32_t initial_packed,
                                                      const std::chrono::steady_clock::time_point& deadline) const;
  [[nodiscard]] std::optional<CandidateSlot> SelectCandidateSlot(
      MonitorReadMode read_mode, uint32_t initial_packed, const std::chrono::steady_clock::time_point& deadline) const;
  [[nodiscard]] bool IsAcceptedSnapshot(const CandidateSlot& candidate) const;
  [[nodiscard]] std::optional<detail::SequenceNumber> CopyLatestSnapshotInto(
      Buffer& destination, double timeout_sec, MonitorReadMode read_mode,
      uint32_t minimum_sequence = detail::kNoCompletedMonitorSequence) const;

  SharedMemoryBuffer shared_memory_buffer_;
  std::reference_wrapper<StreamingControl> streaming_control_;
  mutable std::unique_ptr<Buffer> buffer_cache_;
};

}  // namespace dex::shared_memory

#include "dex/infrastructure/shared_memory/shared_memory_monitor_impl.h"

#endif  // DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_MONITOR_H

