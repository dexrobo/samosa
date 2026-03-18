#ifndef DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_MONITOR_IMPL_H
#define DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_MONITOR_IMPL_H

#include <chrono>
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

inline std::chrono::steady_clock::duration SecondsToDuration(double seconds) {
  return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
}

inline timespec DurationToTimespec(const std::chrono::steady_clock::duration duration) {
  const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
  const auto sec = std::chrono::duration_cast<std::chrono::seconds>(nanoseconds);
  const auto remainder = nanoseconds - sec;
  return timespec{.tv_sec = static_cast<time_t>(sec.count()), .tv_nsec = static_cast<long>(remainder.count())};
}

inline bool IsValidBufferSlotId(const uint32_t raw_slot_id) {
  return raw_slot_id >= static_cast<uint32_t>(ToInt(BufferState::BufferA)) &&
         raw_slot_id <= static_cast<uint32_t>(ToInt(BufferState::BufferStateLast));
}

void InvokeMonitor(auto&& monitor_fn, const auto& buffer, const uint32_t sequence)
  requires std::invocable<decltype(monitor_fn), decltype(buffer), uint32_t>
{
  monitor_fn(buffer, sequence);
}

void InvokeMonitor(auto&& monitor_fn, const auto& buffer, uint32_t /*sequence*/) { monitor_fn(buffer); }

}  // namespace detail

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
auto Monitor<Buffer, buffer_size, SharedMemoryBuffer>::GetLatestSnapshot(double timeout_sec, MonitorReadMode read_mode,
                                                                         const uint32_t minimum_sequence)
    -> std::optional<Snapshot> {
  if (!shared_memory_buffer_.IsValid()) {
    return std::nullopt;
  }

  constexpr size_t kMaxValidationAttempts = 8;
  const auto start_time = std::chrono::steady_clock::now();
  const auto timeout_duration = detail::SecondsToDuration(timeout_sec);
  const auto deadline = start_time + timeout_duration;
  size_t attempts = 0;

  while (attempts++ < kMaxValidationAttempts && streaming_control_.get().IsRunning()) {
    const uint32_t initial_packed = shared_memory_buffer_.Get()->sequence_and_writing.load(std::memory_order_acquire);
    if (detail::IsWriting(initial_packed)) {
      if (read_mode == MonitorReadMode::SkipIfBusy) {
        SPDLOG_DEBUG("Monitor: Skipping read because producer write is already active");
        return std::nullopt;
      }
      if (read_mode == MonitorReadMode::WaitForStableSnapshot) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          return std::nullopt;
        }
        const auto remaining = deadline - now;
        const timespec timeout = detail::DurationToTimespec(remaining);
        const auto wait_result = detail::GetDefaultFutex()->Wait(shared_memory_buffer_.Get()->sequence_and_writing,
                                                                 initial_packed, &timeout);
        if (wait_result != detail::WaitResult::Success) {
          SPDLOG_DEBUG("Monitor: Wait returned {} while waiting for producer to finish writing",
                       static_cast<int>(wait_result));
          return std::nullopt;
        }
        continue;
      }
    }

    const uint32_t raw_slot_id = shared_memory_buffer_.Get()->last_written_buffer.load(std::memory_order_acquire);
    if (!detail::IsValidBufferSlotId(raw_slot_id)) {
      if (raw_slot_id != static_cast<uint32_t>(detail::ToInt(detail::BufferState::Unavailable))) {
        SPDLOG_WARN("Monitor: Invalid last_written_buffer value {}", raw_slot_id);
      }
      return std::nullopt;
    }
    if (raw_slot_id == static_cast<uint32_t>(detail::ToInt(detail::BufferState::Unavailable))) {
      return std::nullopt;
    }

    const auto slot = static_cast<detail::BufferState>(raw_slot_id);
    const auto slot_index = detail::ToBufferIndex(slot);
    const uint32_t pre_slot_packed =
        gsl::at(shared_memory_buffer_.Get()->slot_sequence_and_writing, slot_index).load(std::memory_order_acquire);
    if (detail::IsWriting(pre_slot_packed) ||
        detail::GetSequence(pre_slot_packed) == detail::kNoCompletedMonitorSequence) {
      if (read_mode == MonitorReadMode::WaitForStableSnapshot) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          return std::nullopt;
        }
        const auto remaining = deadline - now;
        const timespec timeout = detail::DurationToTimespec(remaining);
        const auto wait_result = detail::GetDefaultFutex()->Wait(shared_memory_buffer_.Get()->sequence_and_writing,
                                                                 initial_packed, &timeout);
        if (wait_result != detail::WaitResult::Success) {
          return std::nullopt;
        }
        continue;
      }
      if (read_mode == MonitorReadMode::SkipIfBusy) {
        return std::nullopt;
      }
    }

    *buffer_cache_ = gsl::at(shared_memory_buffer_.Get()->buffers, slot_index);

    const uint32_t post_slot_packed =
        gsl::at(shared_memory_buffer_.Get()->slot_sequence_and_writing, slot_index).load(std::memory_order_acquire);
    const uint32_t post_slot_id = shared_memory_buffer_.Get()->last_written_buffer.load(std::memory_order_acquire);

    if (pre_slot_packed != post_slot_packed || detail::IsWriting(post_slot_packed) ||
        detail::GetSequence(post_slot_packed) == detail::kNoCompletedMonitorSequence || post_slot_id != raw_slot_id) {
      SPDLOG_DEBUG("Monitor: Discarding suspicious snapshot from slot {}", raw_slot_id);
      if (std::chrono::steady_clock::now() >= deadline) {
        return std::nullopt;
      }
      continue;
    }

    const uint32_t accepted_sequence = detail::GetSequence(post_slot_packed);
    if (accepted_sequence <= minimum_sequence) {
      return std::nullopt;
    }

    return Snapshot{.buffer = std::cref(*buffer_cache_), .sequence = accepted_sequence};
  }

  return std::nullopt;
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
auto Monitor<Buffer, buffer_size, SharedMemoryBuffer>::GetLatestBuffer(double timeout_sec, MonitorReadMode read_mode)
    -> std::optional<std::reference_wrapper<const Buffer>> {
  auto snapshot = GetLatestSnapshot(timeout_sec, read_mode);
  if (!snapshot.has_value()) {
    return std::nullopt;
  }
  return snapshot->buffer;
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
  uint32_t last_observed_sequence = detail::kNoCompletedMonitorSequence;

  // Convert seconds to timespec once
  const timespec timeout = detail::SecondsToTimespec(timeout_sec);

  while (streaming_control_.get().IsRunning() && (max_iterations == 0 || iteration_count < max_iterations)) {
    // Get packed sequence and writing flag
    const uint32_t packed = shared_memory_buffer_.Get()->sequence_and_writing.load(std::memory_order_acquire);
    const uint32_t current_sequence = detail::GetSequence(packed);

    // If there's new data
    if (current_sequence > last_observed_sequence) {
      auto snapshot_opt = GetLatestSnapshot(timeout_sec, read_mode, last_observed_sequence);
      if (snapshot_opt) {
        detail::InvokeMonitor(monitor_fn, snapshot_opt->buffer.get(), snapshot_opt->sequence);
        last_observed_sequence = snapshot_opt->sequence;
        iteration_count++;
      }
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
  }

  SPDLOG_DEBUG("Monitor: Finished monitoring after {} iterations", iteration_count);
}

}  // namespace dex::shared_memory

#endif  // DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_MONITOR_IMPL_H

