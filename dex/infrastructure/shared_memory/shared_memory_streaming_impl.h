#ifndef DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_STREAMING_IMPL_H
#define DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_STREAMING_IMPL_H

#include "spdlog/spdlog.h"

#include "dex/infrastructure/shared_memory/futex.h"
#include "dex/infrastructure/shared_memory/shared_memory_streaming.h"

namespace dex::shared_memory {

namespace detail {

// InvokeProducer overloads
void InvokeProducer(auto&& producer_fn, auto& buffer, const uint frame_count, const int buffer_id)
  requires std::invocable<decltype(producer_fn), decltype(buffer), uint, int>
{
  producer_fn(buffer, frame_count, buffer_id);
}

void InvokeProducer(auto&& producer_fn, auto& buffer, const uint frame_count, int /*buffer_id*/)
  requires std::invocable<decltype(producer_fn), decltype(buffer), uint>
{
  producer_fn(buffer, frame_count);
}

// Fallback overload
void InvokeProducer(auto&& producer_fn, auto& buffer, uint /*frame_count*/, int /*buffer_id*/) { producer_fn(buffer); }

// InvokeConsumer overloads
void InvokeConsumer(auto&& consumer_fn, const auto& buffer, const uint frame_count, const int buffer_id)
  requires std::invocable<decltype(consumer_fn), decltype(buffer), uint, int>
{
  consumer_fn(buffer, frame_count, buffer_id);
}

void InvokeConsumer(auto&& consumer_fn, const auto& buffer, const uint frame_count, int /*buffer_id*/)
  requires std::invocable<decltype(consumer_fn), decltype(buffer), uint>
{
  consumer_fn(buffer, frame_count);
}

// Fallback overload
void InvokeConsumer(auto&& consumer_fn, const auto& buffer, uint /*frame_count*/, int /*buffer_id*/) {
  consumer_fn(buffer);
}

}  // namespace detail

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename StreamingSharedMemoryBuffer>
bool InitializeBuffer(StreamingSharedMemoryBuffer<Buffer, buffer_size>* buffer) {
  buffer->version = detail::kSharedMemVersion;
  buffer->write_index.store(ToInt(detail::BufferState::Unavailable), std::memory_order_relaxed);
  buffer->read_index.store(ToInt(detail::BufferState::Unavailable), std::memory_order_relaxed);
  if constexpr (requires { buffer->sequence_and_writing; }) {
    buffer->sequence_and_writing.store(0, std::memory_order_relaxed);  // Sequence 0, not writing
  }
  std::memset(buffer->buffers.data(), 0, sizeof(buffer->buffers));
  return true;
}

////////////////////////////////////////////////////////////////////////////////
// Producer implementation
// Provides non-blocking snapshot streaming to shared memory.
////////////////////////////////////////////////////////////////////////////////
template <typename Buffer, size_t buffer_size, template <typename, size_t> typename StreamingSharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, StreamingSharedMemoryBuffer>
void Producer<Buffer, buffer_size, StreamingSharedMemoryBuffer>::Run(auto&& produce)
  requires detail::ProducerFunction<std::remove_reference_t<decltype(produce)>, Buffer>
{
  uint counter = 0;
  while (streaming_control_.get().IsRunning()) {
    ProduceFrame(counter, produce);
    ++counter;
  }
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename StreamingSharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, StreamingSharedMemoryBuffer>
void Producer<Buffer, buffer_size, StreamingSharedMemoryBuffer>::ProduceFrame(const uint frame_count, auto&& produce)
  requires detail::ProducerFunction<std::remove_reference_t<decltype(produce)>, Buffer>
{
  const auto last_write =
      detail::ToBufferState(shared_memory_buffer_.Get()->write_index.load(std::memory_order_relaxed));
  auto next_write =
      (last_write == detail::BufferState::BufferA) ? detail::BufferState::BufferB : detail::BufferState::BufferA;

  auto next_read = detail::ToBufferState(shared_memory_buffer_.Get()->read_index.load(std::memory_order_acquire));
  if (next_read == detail::BufferState::Unavailable) {
    next_write = detail::BufferState::BufferA;
  }

  // Set the writing flag while preserving the sequence
  if constexpr (requires { shared_memory_buffer_.Get()->sequence_and_writing; }) {
    shared_memory_buffer_.Get()->sequence_and_writing.fetch_or(detail::kWritingBitMask, std::memory_order_release);
  }

  SPDLOG_DEBUG("[{}, {}] producing frame...", frame_count, ToInt(next_write));
  detail::InvokeProducer(std::forward<decltype(produce)>(produce),
                         gsl::at(shared_memory_buffer_.Get()->buffers, detail::ToBufferIndex(next_write)), frame_count,
                         detail::ToInt(next_write));
  SPDLOG_DEBUG("[{}, {}] producing frame... done", frame_count, ToInt(next_write));

  // Update last written buffer
  shared_memory_buffer_.Get()->last_written_buffer.store(detail::ToInt(next_write), std::memory_order_relaxed);

  if constexpr (requires { shared_memory_buffer_.Get()->sequence_and_writing; }) {
    // Update sequence and clear writing flag atomically. This will only store LSB 31 bits of the frame count.
    // We are OK with this because `sequence_and_writing` is only used for monitoring, not for synchronization.
    shared_memory_buffer_.Get()->sequence_and_writing.store(frame_count & detail::kSequenceMask,
                                                            std::memory_order_release);

    // Wake any waiting monitors
    if (!detail::GetDefaultFutex()->Wake(shared_memory_buffer_.Get()->sequence_and_writing, INT_MAX)) {
      SPDLOG_WARN("Failed to wake monitors");
      // But, this is not critical for normal operation. So, ignore.
    }
  }

  if (next_read == detail::BufferState::Unavailable) {
    SPDLOG_DEBUG("publisher reset");
    shared_memory_buffer_.Get()->write_index.store(detail::ToInt(detail::BufferState::Unavailable),
                                                   std::memory_order_release);
    if (!detail::GetDefaultFutex()->Wake(shared_memory_buffer_.Get()->write_index, 1)) {
      SPDLOG_WARN("Failed to wake on publisher reset");
      streaming_control_.get().Stop();
      return;
    }
    return;
  }

  SPDLOG_DEBUG("[{}, {}] storing frame...", frame_count, ToInt(next_write));
  next_read = detail::ToBufferState(shared_memory_buffer_.Get()->read_index.load(std::memory_order_acquire));
  if (next_read == next_write) {
    shared_memory_buffer_.Get()->write_index.store(detail::ToInt(next_write), std::memory_order_release);
    SPDLOG_DEBUG("[{}, {}] publishing frame... done", frame_count, ToInt(next_write));
  }
  SPDLOG_DEBUG("[{}, {}] storing frame... done", frame_count, ToInt(next_write));
  if (!detail::GetDefaultFutex()->Wake(shared_memory_buffer_.Get()->write_index, 1)) {
    SPDLOG_WARN("Failed to wake on publishing frame");
    streaming_control_.get().Stop();
    return;
  }
}

////////////////////////////////////////////////////////////////////////////////
// Consumer implementation
// Provides blocking consumption of the latest available snapshot.
////////////////////////////////////////////////////////////////////////////////
template <typename Buffer, size_t buffer_size, template <typename, size_t> typename StreamingSharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, StreamingSharedMemoryBuffer>
RunResult Consumer<Buffer, buffer_size, StreamingSharedMemoryBuffer>::HandleWaitResult(
    const detail::WaitResult wait_result) {
  switch (wait_result) {
    case detail::WaitResult::Success:
      return RunResult::Success;
    case detail::WaitResult::Timeout:
      streaming_control_.get().Stop();
      return RunResult::Timeout;
    case detail::WaitResult::Interrupted:
      return RunResult::Stopped;
    default:
      streaming_control_.get().Stop();
      return RunResult::Error;
  }
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename StreamingSharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, StreamingSharedMemoryBuffer>
RunResult Consumer<Buffer, buffer_size, StreamingSharedMemoryBuffer>::Run(auto&& consume, const timespec* timeout)
  requires detail::ConsumerFunction<std::remove_reference_t<decltype(consume)>, Buffer>
{
  shared_memory_buffer_.Get()->read_index.store(detail::ToInt(detail::BufferState::Unavailable),
                                                std::memory_order_relaxed);
  auto current_write = detail::ToBufferState(shared_memory_buffer_.Get()->write_index.load(std::memory_order_acquire));

  while (streaming_control_.get().IsRunning() && current_write != detail::BufferState::Unavailable) {
    const auto wait_result = detail::GetDefaultFutex()->Wait(shared_memory_buffer_.Get()->write_index,
                                                             detail::ToInt(current_write), timeout);
    auto result = HandleWaitResult(wait_result);
    if (result != RunResult::Success) {
      return result;
    }
    current_write = detail::ToBufferState(shared_memory_buffer_.Get()->write_index.load(std::memory_order_acquire));
  }
  SPDLOG_DEBUG("producer has reset. begin consuming...");

  uint counter = 0;
  while (streaming_control_.get().IsRunning()) {
    SPDLOG_DEBUG("[{}, {}] pre-capture: ", counter, counter);
    const auto result = ConsumeFrame(counter, consume, timeout);
    if (result != RunResult::Success) {
      return result;
    }
    SPDLOG_DEBUG("[{}, {}] post-capture: ", counter, counter);
    ++counter;
  }
  return RunResult::Stopped;
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename StreamingSharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, StreamingSharedMemoryBuffer>
RunResult Consumer<Buffer, buffer_size, StreamingSharedMemoryBuffer>::ConsumeFrame(const uint frame_count,
                                                                                   auto&& consume,
                                                                                   const timespec* timeout)
  requires detail::ConsumerFunction<std::remove_reference_t<decltype(consume)>, Buffer>
{
  const auto last_read = detail::ToBufferState(shared_memory_buffer_.Get()->read_index.load(std::memory_order_relaxed));
  const auto next_read =
      (last_read == detail::BufferState::BufferA) ? detail::BufferState::BufferB : detail::BufferState::BufferA;

  shared_memory_buffer_.Get()->read_index.store(detail::ToInt(next_read), std::memory_order_release);
  auto current_write = detail::ToBufferState(shared_memory_buffer_.Get()->write_index.load(std::memory_order_acquire));

  // Wait until a new frame is published
  while (streaming_control_.get().IsRunning() && next_read != current_write) {
    SPDLOG_DEBUG("[{}, {}] waiting for frame...", frame_count, ToInt(current_write));
    const auto wait_result = detail::GetDefaultFutex()->Wait(shared_memory_buffer_.Get()->write_index,
                                                             detail::ToInt(current_write), timeout);
    auto result = HandleWaitResult(wait_result);
    if (result != RunResult::Success) {
      return result;
    }
    current_write = detail::ToBufferState(shared_memory_buffer_.Get()->write_index.load(std::memory_order_acquire));
    SPDLOG_DEBUG("[{}, {}] waiting for frame... done", frame_count, ToInt(current_write));
  }

  SPDLOG_DEBUG("[{}, {}] frame available", frame_count, ToInt(current_write));

  // Only process the frame if we're still running
  if (streaming_control_.get().IsRunning()) {
    SPDLOG_DEBUG("[{}, {}] processing frame...", frame_count, ToInt(current_write));
    detail::InvokeConsumer(std::forward<decltype(consume)>(consume),
                           gsl::at(shared_memory_buffer_.Get()->buffers, detail::ToBufferIndex(current_write)),
                           frame_count, detail::ToInt(current_write));
    SPDLOG_DEBUG("[{}, {}] processing frame... done", frame_count, ToInt(current_write));
    return RunResult::Success;
  }
  return RunResult::Stopped;
}

}  // namespace dex::shared_memory

#endif  // DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_STREAMING_IMPL_H

