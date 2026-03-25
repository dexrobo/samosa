#ifndef DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_MONITOR_IMPL_H
#define DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_MONITOR_IMPL_H

#include <chrono>
#include <cmath>  // For std::modf
#include <memory>

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
  return timespec{.tv_sec = static_cast<time_t>(sec.count()), .tv_nsec = static_cast<int64_t>(remainder.count())};
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
bool Monitor<Buffer, buffer_size, SharedMemoryBuffer>::WaitForMonitorStateChange(
    const uint32_t expected_packed, const std::chrono::steady_clock::time_point& deadline) const {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return false;
  }

  const auto remaining = deadline - now;
  const timespec timeout = detail::DurationToTimespec(remaining);
  const auto wait_result =
      detail::GetDefaultFutex()->Wait(shared_memory_buffer_.Get()->sequence_and_writing, expected_packed, &timeout);
  if (wait_result != detail::WaitResult::Success) {
    SPDLOG_DEBUG("Monitor: Wait returned {} while waiting for producer state change", static_cast<int>(wait_result));
    return false;
  }
  return true;
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
auto Monitor<Buffer, buffer_size, SharedMemoryBuffer>::HandleInitialState(
    const MonitorReadMode read_mode, const uint32_t initial_packed,
    const std::chrono::steady_clock::time_point& deadline) const -> InitialStateAction {
  if (!detail::IsWriting(initial_packed)) {
    return InitialStateAction::Continue;
  }

  if (read_mode == MonitorReadMode::SkipIfBusy) {
    SPDLOG_DEBUG("Monitor: Skipping read because producer write is already active");
    return InitialStateAction::ReturnNone;
  }

  if (read_mode != MonitorReadMode::WaitForStableSnapshot) {
    return InitialStateAction::Continue;
  }

  if (!WaitForMonitorStateChange(initial_packed, deadline)) {
    return InitialStateAction::ReturnNone;
  }
  return InitialStateAction::Retry;
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
auto Monitor<Buffer, buffer_size, SharedMemoryBuffer>::SelectCandidateSlot(
    const MonitorReadMode read_mode, const uint32_t initial_packed,
    const std::chrono::steady_clock::time_point& deadline) const -> std::optional<CandidateSlot> {
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
  const bool slot_is_unavailable =
      detail::IsWriting(pre_slot_packed) || detail::GetSequence(pre_slot_packed) == detail::kNoCompletedMonitorSequence;
  if (!slot_is_unavailable) {
    return CandidateSlot{.raw_slot_id = raw_slot_id, .slot_index = slot_index, .pre_slot_packed = pre_slot_packed};
  }

  if (read_mode == MonitorReadMode::SkipIfBusy) {
    return std::nullopt;
  }
  if (read_mode == MonitorReadMode::WaitForStableSnapshot && WaitForMonitorStateChange(initial_packed, deadline)) {
    return CandidateSlot{.raw_slot_id = static_cast<uint32_t>(detail::ToInt(detail::BufferState::Unavailable)),
                         .slot_index = 0,
                         .pre_slot_packed = detail::kNoCompletedMonitorSequence};
  }
  return std::nullopt;
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
bool Monitor<Buffer, buffer_size, SharedMemoryBuffer>::IsAcceptedSnapshot(const CandidateSlot& candidate) const {
  const uint32_t post_slot_packed =
      gsl::at(shared_memory_buffer_.Get()->slot_sequence_and_writing, candidate.slot_index)
          .load(std::memory_order_acquire);
  const uint32_t post_slot_id = shared_memory_buffer_.Get()->last_written_buffer.load(std::memory_order_acquire);
  return candidate.pre_slot_packed == post_slot_packed && !detail::IsWriting(post_slot_packed) &&
         detail::GetSequence(post_slot_packed) != detail::kNoCompletedMonitorSequence &&
         post_slot_id == candidate.raw_slot_id;
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
auto Monitor<Buffer, buffer_size, SharedMemoryBuffer>::CopyLatestSnapshotInto(Buffer& destination, double timeout_sec,
                                                                              MonitorReadMode read_mode,
                                                                              const uint32_t minimum_sequence) const
    -> std::optional<detail::SequenceNumber> {
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
    const auto initial_state_action = HandleInitialState(read_mode, initial_packed, deadline);
    if (initial_state_action == InitialStateAction::ReturnNone) {
      return std::nullopt;
    }
    if (initial_state_action == InitialStateAction::Retry) {
      continue;
    }

    auto candidate_opt = SelectCandidateSlot(read_mode, initial_packed, deadline);
    if (!candidate_opt.has_value()) {
      return std::nullopt;
    }
    const CandidateSlot& candidate = *candidate_opt;
    if (candidate.raw_slot_id == static_cast<uint32_t>(detail::ToInt(detail::BufferState::Unavailable))) {
      continue;
    }

    destination = gsl::at(shared_memory_buffer_.Get()->buffers, candidate.slot_index);

    if (!IsAcceptedSnapshot(candidate)) {
      SPDLOG_DEBUG("Monitor: Discarding suspicious snapshot from slot {}", candidate.raw_slot_id);
      if (std::chrono::steady_clock::now() >= deadline) {
        return std::nullopt;
      }
      continue;
    }

    const uint32_t accepted_sequence =
        detail::GetSequence(gsl::at(shared_memory_buffer_.Get()->slot_sequence_and_writing, candidate.slot_index)
                                .load(std::memory_order_acquire));
    if (!detail::IsNewerSequence(accepted_sequence, minimum_sequence)) {
      return std::nullopt;
    }

    return accepted_sequence;
  }

  return std::nullopt;
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
auto Monitor<Buffer, buffer_size, SharedMemoryBuffer>::GetLatestBuffer(double timeout_sec, MonitorReadMode read_mode)
    -> std::optional<std::reference_wrapper<const Buffer>> {
  if (!ReadInto(*buffer_cache_, timeout_sec, read_mode)) {
    return std::nullopt;
  }
  return std::cref(*buffer_cache_);
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
bool Monitor<Buffer, buffer_size, SharedMemoryBuffer>::ReadInto(Buffer& destination, double timeout_sec,
                                                                MonitorReadMode read_mode,
                                                                detail::SequenceNumber* sequence) const {
  auto accepted_sequence = CopyLatestSnapshotInto(destination, timeout_sec, read_mode);
  if (!accepted_sequence.has_value()) {
    return false;
  }
  if (sequence != nullptr) {
    *sequence = *accepted_sequence;
  }
  return true;
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
void Monitor<Buffer, buffer_size, SharedMemoryBuffer>::Run(auto&& monitor_fn, double timeout_sec,
                                                           MonitorReadMode read_mode, uint max_iterations)
  requires detail::MonitorCallback<std::remove_reference_t<decltype(monitor_fn)>, Buffer>
{
  if (!shared_memory_buffer_.IsValid()) {
    SPDLOG_ERROR("Monitor: Invalid shared memory buffer");
    return;
  }

  auto buffer_cache = std::make_unique<Buffer>();
  uint iteration_count = 0;
  uint32_t last_observed_sequence = detail::kNoCompletedMonitorSequence;

  // Convert seconds to timespec once
  const timespec timeout = detail::SecondsToTimespec(timeout_sec);

  while (streaming_control_.get().IsRunning() && (max_iterations == 0 || iteration_count < max_iterations)) {
    // Get packed sequence and writing flag
    const uint32_t packed = shared_memory_buffer_.Get()->sequence_and_writing.load(std::memory_order_acquire);
    const uint32_t current_sequence = detail::GetSequence(packed);

    // If there's new data
    if (detail::IsNewerSequence(current_sequence, last_observed_sequence)) {
      auto accepted_sequence = CopyLatestSnapshotInto(*buffer_cache, timeout_sec, read_mode, last_observed_sequence);
      if (accepted_sequence) {
        detail::InvokeMonitor(monitor_fn, *buffer_cache, *accepted_sequence);
        last_observed_sequence = *accepted_sequence;
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

