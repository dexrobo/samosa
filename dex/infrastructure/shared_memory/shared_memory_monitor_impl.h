#ifndef DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_MONITOR_IMPL_H
#define DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_MONITOR_IMPL_H

#include <cmath>  // For std::modf

#include "spdlog/spdlog.h"

#include "dex/infrastructure/shared_memory/shared_memory_monitor.h"

namespace dex::shared_memory {

namespace detail {

// Helper function to convert float seconds to timespec
inline timespec SecondsToTimespec(double seconds) {
  double integral_part = 0.0;  // Initialize to avoid linter warning
  const auto fractional_part = std::modf(seconds, &integral_part);

  const auto sec = static_cast<time_t>(integral_part);
  const auto nsec = static_cast<int64_t>(fractional_part * 1e9);

  return timespec{.tv_sec = sec, .tv_nsec = nsec};
}

}  // namespace detail

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
auto Monitor<Buffer, buffer_size, SharedMemoryBuffer>::GetLatestBuffer(double timeout_sec, MonitorReadMode read_mode)
    -> std::optional<std::reference_wrapper<const Buffer>> {
  // Handle producer writing state
  if (shared_memory_buffer_.IsValid()) {
    // Get packed sequence and writing flag
    uint32_t packed = shared_memory_buffer_.Get()->sequence_and_writing.load(
        std::memory_order_acquire);  // NOLINT(misc-const-correctness)
    const bool is_writing = detail::IsWriting(packed);

    if (is_writing) {
      switch (read_mode) {
        case MonitorReadMode::SkipDuringProducerWrite:
          SPDLOG_DEBUG("Monitor: Skipping read as producer is writing");
          return std::nullopt;

        case MonitorReadMode::WaitForProducerWriteCompletion: {
          SPDLOG_DEBUG("Monitor: Waiting for producer to finish writing");

          // Convert seconds to timespec
          const timespec timeout = detail::SecondsToTimespec(timeout_sec);

          // Wait for the packed value to change (either sequence or writing flag)
          const auto wait_result =
              detail::GetDefaultFutex()->Wait(shared_memory_buffer_.Get()->sequence_and_writing, packed, &timeout);
          if (wait_result != detail::WaitResult::Success) {
            SPDLOG_DEBUG("Monitor: Wait returned {} while waiting for producer to finish writing",
                         static_cast<int>(wait_result));
            return std::nullopt;
          }

          // Check if still writing after wait.
          packed = shared_memory_buffer_.Get()->sequence_and_writing.load(std::memory_order_acquire);
          if (detail::IsWriting(packed)) {
            SPDLOG_DEBUG("Monitor: Producer is still writing. Will try again next time.");
            return std::nullopt;
          }
          break;
        }

        case MonitorReadMode::ReadDuringProducerWrite:
          SPDLOG_DEBUG("Monitor: Reading while producer is writing");
          break;
      }
    }
  }

  // Get the latest buffer state
  auto latest_buffer = detail::GetLastWrittenBuffer(shared_memory_buffer_);

  // Get a pointer to the buffer
  if (latest_buffer == nullptr) {
    return std::nullopt;
  }

  // Update our buffer cache
  buffer_cache_ = *latest_buffer;  // Dereference the pointer to get the Buffer

  // Return a reference to our cached copy
  return std::cref(buffer_cache_);
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
void Monitor<Buffer, buffer_size, SharedMemoryBuffer>::Run(auto&& monitor_fn, double timeout_sec, uint max_iterations,
                                                           MonitorReadMode read_mode)
  requires detail::MonitorCallback<std::remove_reference_t<decltype(monitor_fn)>, Buffer>
{
  if (!shared_memory_buffer_.IsValid()) {
    SPDLOG_ERROR("Monitor: Invalid shared memory buffer");
    return;
  }

  uint iteration_count = 0;
  uint32_t last_observed_sequence = 0;
  bool first_frame = true;

  // Convert seconds to timespec once
  const timespec timeout = detail::SecondsToTimespec(timeout_sec);

  while (streaming_control_.get().IsRunning() && (max_iterations == 0 || iteration_count < max_iterations)) {
    // Get packed sequence and writing flag
    const uint32_t packed = shared_memory_buffer_.Get()->sequence_and_writing.load(std::memory_order_acquire);
    const uint32_t current_sequence = detail::GetSequence(packed);

    // If there's new data
    if (first_frame || current_sequence != last_observed_sequence) {
      auto buffer_opt = GetLatestBuffer(timeout_sec, read_mode);
      if (buffer_opt) {
        monitor_fn(*buffer_opt, current_sequence);
      }
      // If the producer stopped after running for a while, we need to update the last observed sequence to avoid
      // infinite loop.
      first_frame = false;
      last_observed_sequence = current_sequence;
    } else {
      // Wait for sequence_and_writing to change
      const auto wait_result =
          detail::GetDefaultFutex()->Wait(shared_memory_buffer_.Get()->sequence_and_writing, packed, &timeout);
      if (wait_result == detail::WaitResult::Timeout) {
        SPDLOG_WARN("Monitor: Timed out waiting for new data");
      } else if (wait_result == detail::WaitResult::Interrupted) {
        SPDLOG_DEBUG("Monitor: Interrupted by signal, exiting");
        return;
      } else if (wait_result == detail::WaitResult::Error) {
        SPDLOG_ERROR("Monitor: Fatal error during wait, exiting");
        return;
      }
      // If the producer is writing, the sequence number may not be updated yet. In that case, skip this iteration.
      if (detail::GetSequence(shared_memory_buffer_.Get()->sequence_and_writing.load(std::memory_order_acquire)) ==
          current_sequence) {
        continue;
      }
    }

    iteration_count++;
  }

  SPDLOG_DEBUG("Monitor: Finished monitoring after {} iterations", iteration_count);
}

}  // namespace dex::shared_memory

// work around for clang-tidy warning "Included header xxxxxx is not used directly"
#define SHARED_MEMORY_MONITOR_IMPL_H
#endif  // DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_MONITOR_IMPL_H
