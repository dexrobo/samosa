/**
 * @file shared_memory.h
 * @brief Core primitives for lock-free snapshot streaming via shared memory.
 *
 * This library is optimized for streaming snapshots of POD types between processes.
 * It is not a general-purpose IPC queue; consumers always jump to the latest state.
 */

#ifndef DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_H
#define DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_H

#include <unistd.h>  // for close

#include <concepts>  // for std::same_as
#include <cstddef>   // for std::size_t
#include <gsl/gsl>   // Include the GSL header
#include <span>      // Add include for std::span
#include <string>    // for std::string

namespace dex::shared_memory {

namespace detail {

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
concept SharedMemoryBufferType = requires(SharedMemoryBuffer<Buffer, buffer_size>* shared_memory_buffer) {
  { std::span<Buffer, buffer_size>(shared_memory_buffer->buffers) } -> std::same_as<std::span<Buffer, buffer_size>>;
};

// Replace the lambda with a named struct
struct FileDescriptorDeleter {
  void operator()(gsl::owner<const int*> file_descriptor) const {
    if (file_descriptor != nullptr && *file_descriptor >= 0) {
      close(*file_descriptor);
      delete file_descriptor;  // Ensure the pointer is deleted
    }
  }
};

using UniqueFileDescriptor = ::std::unique_ptr<int, FileDescriptorDeleter>;

// Define the callback type explicitly
template <typename Buffer>
using BufferCallback = bool (*)(Buffer*);

}  // namespace detail

// An empty callback to pass to the SharedMemory constructor when no callback is needed.
template <typename Buffer>
bool NullCallback(Buffer* /*buffer*/) {
  return true;
}

////////////////////////////////////////////////////////////////////////////////
/**
 * RAII wrapper for shared memory management
 *
 * Handles the lifecycle of shared memory mapping, ensuring proper cleanup
 * on destruction. Prevents resource leaks by automatically unmapping memory
 * when the object goes out of scope.
 *
 * @tparam SharedMemoryBuffer Template class defining the shared memory buffer structure
 * @tparam Buffer The type of data to be stored in the buffer
 * @tparam buffer_size The number of buffers in the shared memory segment
 */
////////////////////////////////////////////////////////////////////////////////
template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::SharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
class SharedMemory {
 public:
  using BufferType = SharedMemoryBuffer<Buffer, buffer_size>;

  /**
   * @brief Creates a new shared memory segment.
   * @param name Name of the shared memory segment.
   * @param init_callback Callback function to initialize the buffer.
   * @return A new SharedMemory instance.
   */
  [[nodiscard]] static SharedMemory Create(const std::string_view name,
                                           detail::BufferCallback<BufferType> init = &NullCallback<BufferType>) {
    return SharedMemory(name, true, init, nullptr);
  }

  /**
   * @brief Opens an existing shared memory segment.
   * @param name Name of the shared memory segment.
   * @param validate_callback Callback function to validate the buffer.
   * @return A new SharedMemory instance.
   */
  [[nodiscard]] static SharedMemory Open(const std::string_view name,
                                         detail::BufferCallback<BufferType> validate = &NullCallback<BufferType>) {
    return SharedMemory(name, false, nullptr, validate);
  }

  ~SharedMemory() { Cleanup(); }

  /**
   * @brief Destroys the shared memory segment associated with the given name.
   * @param name Name of the shared memory segment to destroy.
   * @return true if the shared memory was successfully unlinked, false otherwise.
   */
  [[nodiscard]] static bool Destroy(std::string_view name);

  /**
   * @brief Retrieves a pointer to the mapped lock-free shared memory buffer.
   * @return Pointer to the shared memory buffer if initialization was successful; otherwise, nullptr.
   */
  [[nodiscard]] SharedMemoryBuffer<Buffer, buffer_size>* Get() const {
    return state_.initialized ? state_.buffer : nullptr;
  }

  /**
   * @brief Checks whether the shared memory was properly initialized.
   * @return true if the shared memory is valid; false otherwise.
   */
  [[nodiscard]] bool IsValid() const { return state_.initialized; }

  // Prevent copying and moving
  SharedMemory(const SharedMemory&) = delete;
  SharedMemory& operator=(const SharedMemory&) = delete;
  SharedMemory(SharedMemory&&) = delete;
  SharedMemory& operator=(SharedMemory&&) = delete;

 private:
  /**
   * @brief Constructs a new SharedMemory object.
   * @param name Name of the shared memory segment.
   * @param create If true, attempts to create a new shared memory segment; otherwise, opens an existing one.
   * @param init_callback Callback function to initialize the buffer (only used when create is true)
   * @param validate_callback Callback function to validate the buffer (only used when create is false)
   */
  explicit SharedMemory(
      std::string_view name, bool create,
      detail::BufferCallback<BufferType> init = nullptr,  // NOLINT(bugprone-easily-swappable-parameters)
      detail::BufferCallback<BufferType> validate = nullptr);

  struct State {
    std::string name;
    detail::UniqueFileDescriptor file_descriptor{nullptr, detail::FileDescriptorDeleter{}};
    void* memory_address = nullptr;
    SharedMemoryBuffer<Buffer, buffer_size>* buffer = nullptr;
    size_t size = sizeof(SharedMemoryBuffer<Buffer, buffer_size>);
    bool initialized = false;
  } state_;

  // State transition methods
  [[nodiscard]] bool OpenFile(bool create);
  [[nodiscard]] bool InitializeSize();
  [[nodiscard]] bool MapMemory();
  [[nodiscard]] bool InitializeBuffer(detail::BufferCallback<BufferType> callback);
  [[nodiscard]] bool ValidateBuffer(detail::BufferCallback<BufferType> callback);
  void Cleanup();
};

}  // namespace dex::shared_memory

#include "dex/infrastructure/shared_memory/shared_memory_impl.h"

#endif  // DEX_INFRASTRUCTURE_SHARED_MEMORY_SHARED_MEMORY_H

