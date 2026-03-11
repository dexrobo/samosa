#pragma once

#include <functional>
#include <optional>
#include <string_view>

#include "dex/infrastructure/shared_memory/shared_memory_streaming.h"

namespace dex::shared_memory {

namespace detail {

using SequenceNumber = uint64_t;

// Concept for the allowed callback types
template <typename Fn, typename Buffer>
concept MonitorCallback = std::invocable<Fn, const Buffer&> || std::invocable<Fn, const Buffer&, SequenceNumber>;

}  // namespace detail
/**
 * Defines how the Monitor should handle producer writing state.
 */
enum class MonitorReadMode {
  // Skip reading when producer is writing
  SkipDuringProducerWrite,

  // Wait until producer is done writing
  WaitForProducerWriteCompletion,

  // Read even if producer is writing (may get partial updates)
  ReadDuringProducerWrite
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
        streaming_control_{StreamingControl::Instance()} {}

  /**
   * @brief Checks if the monitor is valid and connected to shared memory.
   * @return true if the monitor is valid, false otherwise
   */
  [[nodiscard]] bool IsValid() const { return shared_memory_buffer_.IsValid(); }

  /**
   * @brief Get the latest buffer from shared memory
   * @param writing_mode How to handle the case when producer is writing
   * @param timeout_sec Maximum time to wait in seconds (for WaitForCompletion mode)
   * @return Optional reference to the latest buffer, or nullopt if no valid buffer
   */
  std::optional<std::reference_wrapper<const Buffer>> GetLatestBuffer(
      double timeout_sec = 0.1, MonitorReadMode read_mode = MonitorReadMode::WaitForProducerWriteCompletion);

  /**
   * @brief Run a monitoring loop that calls a function when new data is available
   * @param monitor_fn Function to call with new data
   * @param writing_mode How to handle the case when producer is writing
   * @param timeout_sec Maximum time to wait for new data in seconds
   * @param max_iterations Maximum number of iterations (0 for unlimited)
   */
  void Run(auto&& monitor_fn, double timeout_sec = 0.1, uint max_iterations = 0,
           MonitorReadMode read_mode = MonitorReadMode::ReadDuringProducerWrite)
    requires detail::MonitorCallback<std::remove_reference_t<decltype(monitor_fn)>, Buffer>;

 private:
  SharedMemoryBuffer shared_memory_buffer_;
  std::reference_wrapper<StreamingControl> streaming_control_;

  // Buffer cache to store a copy of the latest buffer
  mutable Buffer buffer_cache_;
};

}  // namespace dex::shared_memory

#include "dex/infrastructure/shared_memory/shared_memory_monitor_impl.h"
