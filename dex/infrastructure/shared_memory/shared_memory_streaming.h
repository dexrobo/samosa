#pragma once

// System headers
#include <csignal>  // for signal, SIGINT, SIGTERM
#include <experimental/memory>

#include "dex/infrastructure/shared_memory/futex.h"
#include "dex/infrastructure/shared_memory/shared_memory.h"
#include "dex/infrastructure/shared_memory/streaming_control.h"

namespace dex::shared_memory {

/// Result of Consumer::Run indicating why the loop exited
enum class RunResult {
  Success,  /// Normal continuous operation
  Stopped,  /// Normal shutdown (stop requested or interrupted)
  Timeout,  /// Futex wait timed out
  Error,    /// Catch-all: Fatal system error (EFAULT, etc.)
};

namespace detail {

static constexpr uint32_t kSharedMemVersion = 1;

// Concept for shared memory used in streaming applications
template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
concept StreamingSharedMemoryBufferType =
    // First, require that the type fulfills the basic SharedMemoryBufferType constraints.
    SharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer> &&
    // Then add the additional constraints specific to streaming.
    requires(SharedMemoryBuffer<Buffer, buffer_size>* shared_memory_buffer) {
      { shared_memory_buffer->read_index } -> ::std::same_as<::std::atomic<uint32_t>&>;
      { shared_memory_buffer->write_index } -> ::std::same_as<::std::atomic<uint32_t>&>;
      { shared_memory_buffer->version } -> ::std::same_as<uint32_t&>;
    };

// Concept for producer functions - allows multiple signatures
template <typename F, typename Buffer>
concept ProducerFunction =
    ::std::invocable<F, Buffer&> || ::std::invocable<F, Buffer&, uint> || ::std::invocable<F, Buffer&, uint, int>;

// Concept for consumer functions - allows multiple signatures
template <typename F, typename Buffer>
concept ConsumerFunction = ::std::invocable<F, const Buffer&> || ::std::invocable<F, const Buffer&, uint> ||
                           ::std::invocable<F, const Buffer&, uint, int>;

constexpr uint32_t kWritingBitMask = 1U << 31;        // Highest bit indicates writing
constexpr uint32_t kSequenceMask = ~kWritingBitMask;  // All bits except highest (31 bits for sequence)

// Extract sequence from packed value
inline uint32_t GetSequence(const uint32_t packed_value) { return packed_value & kSequenceMask; }

// Extract writing flag from packed value
inline bool IsWriting(const uint32_t packed_value) { return (packed_value & kWritingBitMask) != 0; }

// Create packed value from sequence and writing flag
inline uint32_t PackSequenceAndWriting(const uint32_t sequence, const bool is_writing) {
  return (sequence & kSequenceMask) | (is_writing ? kWritingBitMask : 0);
}

}  // namespace detail

using std::experimental::observer_ptr;

/**
 * Buffer mechanism for lock-free IPC communication.
 *
 * Uses two buffers to allow the producer to write to one buffer while the
 * consumer reads from the other. Synchronization is achieved through atomic
 * indices and futexes for efficient waiting.
 *
 * @tparam Buffer The type of data to be stored in the buffers
 * @tparam buffer_size The number of buffers (default=2 for double buffering)
 *
 * Buffer States:
 * - Unavailable (0): Initial state, no valid data
 * - BufferA (1): Buffer A contains the latest data
 * - BufferB (2): Buffer B contains the latest data
 */
template <typename Buffer, size_t buffer_size = 2>
struct LockFreeSharedMemoryBuffer {
  std::atomic<uint32_t> sequence_and_writing;  // Bit 31 = is_writing, Bits 0-30 = sequence (31 bits)
                                               // at 60fps, the sequence will overflow ~ 1 year
  std::atomic<uint32_t> read_index;            // Current buffer being requested by consumer
  std::atomic<uint32_t> write_index;           // Current buffer available for consumer to read
  std::atomic<uint32_t> last_written_buffer;   // Which buffer was last written to
  uint32_t version{};                          // Shared memory format version
  std::array<Buffer, buffer_size> buffers;     // Array of shared memory buffers
};

template <typename Buffer, size_t buffer_size = 2,
          template <typename, size_t> typename StreamingSharedMemoryBuffer = LockFreeSharedMemoryBuffer>
bool InitializeBuffer(StreamingSharedMemoryBuffer<Buffer, buffer_size>* buffer);

template <typename Buffer, size_t buffer_size = 2,
          template <typename, size_t> typename StreamingSharedMemoryBuffer = LockFreeSharedMemoryBuffer>
bool ValidateBuffer(StreamingSharedMemoryBuffer<Buffer, buffer_size>* buffer) {
  if (buffer->version != detail::kSharedMemVersion) {
    SPDLOG_ERROR("Version mismatch: expected {} but got {}", detail::kSharedMemVersion, buffer->version);
    return false;
  }
  return true;
}

namespace detail {

// Helper to get the latest buffer state from shared memory
template <typename Buffer, size_t buffer_size = 2,
          template <typename, size_t> typename StreamingSharedMemoryBuffer = LockFreeSharedMemoryBuffer>
[[nodiscard]] inline observer_ptr<const Buffer> GetLastWrittenBuffer(
    const SharedMemory<Buffer, buffer_size, StreamingSharedMemoryBuffer>& shared_memory) {
  if (!shared_memory.IsValid()) {
    return nullptr;
  }

  auto latest_buffer_id = ToBufferState(shared_memory.Get()->last_written_buffer.load(std::memory_order_acquire));
  return GetBufferByState(shared_memory, latest_buffer_id);
}

}  // namespace detail

/**
 * Producer for double-buffered IPC communication
 *
 * Manages the producer side of the shared memory communication:
 * - Writes data to the inactive buffer while consumer reads from active buffer
 * - Alternates between two buffers (BufferA and BufferB)
 * - Uses atomic operations and futexes for synchronization
 * - Supports different producer function signatures for flexibility
 *
 * Thread Safety:
 * - Thread-safe for single producer
 * - Multiple producers should not share the same shared memory segment
 *
 * @tparam Buffer The buffer type to be used for the producer and consumer
 * @tparam buffer_size The number of buffers (default=2 for double buffering)
 *
 * Example usage:
 * ```cpp
 * auto producer = Producer{shared_mem_buffer};
 * producer.Run([](auto& buffer, uint count, int id) {
 *     // Write data to buffer
 * });
 * ```
 */
template <typename Buffer, size_t buffer_size = 2,
          template <typename, size_t> typename StreamingSharedMemoryBuffer = LockFreeSharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, StreamingSharedMemoryBuffer>
class Producer {
  using SharedMemoryBuffer = SharedMemory<Buffer, buffer_size, StreamingSharedMemoryBuffer>;

 public:
  /**
   * @brief Constructs a new Producer object.
   * @param shared_memory_name name of the shared memory segment
   */
  explicit Producer(const std::string_view shared_memory_name)
      : shared_memory_buffer_(SharedMemoryBuffer::Open(
            shared_memory_name, ValidateBuffer<Buffer, buffer_size, StreamingSharedMemoryBuffer>)),
        streaming_control_{StreamingControl::Instance()} {}

  /**
   * @brief Checks if the producer is valid and connected to shared memory.
   * @return true if the producer is valid, false otherwise
   */
  [[nodiscard]] bool IsValid() const { return shared_memory_buffer_.IsValid(); }

  /**
   * @brief Runs the production loop.
   * Continuously invokes the provided produce_fn function to write frames
   * to the shared memory buffer until a termination signal is received.
   * @tparam Buffer The buffer type to be used for the producer and consumer.
   * @param produce_fn The function used to produce a new frame.
   */
  void Run(auto&& produce_fn)
    requires detail::ProducerFunction<std::remove_reference_t<decltype(produce_fn)>, Buffer>;

 private:
  void ProduceFrame(const uint frame_count, auto&& produce)
    requires detail::ProducerFunction<std::remove_reference_t<decltype(produce)>, Buffer>;

  SharedMemoryBuffer shared_memory_buffer_;
  std::reference_wrapper<StreamingControl> streaming_control_;
};

////////////////////////////////////////////////////////////////////////////////
/**
 * Consumer for double-buffered IPC communication
 *
 * Manages the consumer side of the shared memory communication:
 * - Reads data from the active buffer while producer writes to inactive buffer
 * - Alternates between two buffers (BufferA and BufferB)
 * - Uses atomic operations and futexes for synchronization
 * - Supports different consumer function signatures for flexibility
 * - Optional timeout for blocking operations
 *
 * Thread Safety:
 * - Thread-safe for single consumer
 * - Multiple consumers should not share the same shared memory segment
 *
 * @tparam Buffer The buffer type to be used for the producer and consumer
 * @tparam buffer_size The number of buffers (default=2 for double buffering)
 *
 * Example usage:
 * ```cpp
 * auto consumer = Consumer{shared_mem_buffer};
 * consumer.Run([](const auto& buffer) {
 *     // Process data from buffer
 * });
 * ```
 */
////////////////////////////////////////////////////////////////////////////////
template <typename Buffer, size_t buffer_size = 2,
          template <typename, size_t> typename StreamingSharedMemoryBuffer = LockFreeSharedMemoryBuffer>
  requires detail::StreamingSharedMemoryBufferType<Buffer, buffer_size, StreamingSharedMemoryBuffer>
class Consumer {
  using SharedMemoryBuffer = SharedMemory<Buffer, buffer_size, StreamingSharedMemoryBuffer>;

 public:
  /**
   * @brief Constructs a new Consumer object.
   * @param shared_memory_name name of the shared memory segment
   */
  explicit Consumer(const std::string_view shared_memory_name)
      : shared_memory_buffer_(SharedMemoryBuffer::Open(
            shared_memory_name, ValidateBuffer<Buffer, buffer_size, StreamingSharedMemoryBuffer>)),
        streaming_control_{StreamingControl::Instance()} {}

  /**
   * @brief Checks if the consumer is valid and connected to shared memory.
   * @return true if the consumer is valid, false otherwise
   */
  [[nodiscard]] bool IsValid() const { return shared_memory_buffer_.IsValid(); }

  /**
   * @brief Runs the consumption loop.
   * Continuously invokes the provided consume_fn function to process frames
   * from the shared memory buffer. The loop continues until a termination signal
   * is received or an optional timeout occurs.
   * @tparam Buffer The buffer type to be used for the producer and consumer.
   * @param consume_fn The function used to process a frame.
   * @param timeout Optional timeout for blocking operations; pass nullptr for an indefinite wait.
   * @return RunResult indicating why the loop exited.
   */
  [[nodiscard]] RunResult Run(auto&& consume_fn, const timespec* timeout = nullptr)
    requires detail::ConsumerFunction<std::remove_reference_t<decltype(consume_fn)>, Buffer>;

 private:
  /// @return RunResult
  [[nodiscard]] RunResult ConsumeFrame(const uint frame_count, auto&& consume, const timespec* timeout = nullptr)
    requires detail::ConsumerFunction<std::remove_reference_t<decltype(consume)>, Buffer>;

  /// @return RunResult
  [[nodiscard]] RunResult HandleWaitResult(const detail::WaitResult wait_result);

  SharedMemoryBuffer shared_memory_buffer_;
  // To get around: cppcoreguidelines-avoid-const-or-ref-data-members
  std::reference_wrapper<StreamingControl> streaming_control_;
};

}  // namespace dex::shared_memory

#include "dex/infrastructure/shared_memory/shared_memory_streaming_impl.h"
