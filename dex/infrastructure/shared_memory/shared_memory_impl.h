#pragma once

// System headers
#include <fcntl.h>     // for O_RDWR, O_CREAT
#include <sys/mman.h>  // for mmap, munmap
#include <sys/stat.h>  // for shm_open, shm_unlink
#include <unistd.h>    // for close

#include <experimental/memory>
#include <string>

#include "spdlog/spdlog.h"

#include "dex/infrastructure/shared_memory/shared_memory.h"
#include "dex/infrastructure/shared_memory/shared_memory_private.h"

namespace dex::shared_memory {

using std::experimental::make_observer;
using std::experimental::observer_ptr;

namespace detail {

enum class BufferState : int {
  Unavailable = 0,           // No frame published yet
  BufferA = 1,               // Buffer A is published
  BufferB = 2,               // Buffer B is published
  BufferStateLast = BufferB  // sentinel value to check if the state is valid
};

[[nodiscard]] constexpr int ToInt(const BufferState state) { return static_cast<int>(state); }

// Helper functions for safe conversion
[[nodiscard]] constexpr BufferState ToBufferState(const int value) {
  constexpr auto min_value = static_cast<std::underlying_type_t<BufferState>>(BufferState::Unavailable);
  constexpr auto max_value = static_cast<std::underlying_type_t<BufferState>>(BufferState::BufferStateLast);

  if (value < min_value || value > max_value) {
    throw "Invalid buffer state value";  // Compile-time error
  }
  return static_cast<BufferState>(value);
}

// Convert BufferState to zero-based array index
constexpr size_t ToBufferIndex(const BufferState state) {
  if (state == BufferState::Unavailable) {
    throw "Cannot get buffer index for Unavailable state";  // Compile-time error
  }
  return ToInt(state) - 1;
}

// Helper function to get a buffer reference from a SharedMemory object based on buffer state
template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::SharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
[[nodiscard]] inline observer_ptr<const Buffer> GetBufferByState(
    const SharedMemory<Buffer, buffer_size, SharedMemoryBuffer>& shared_memory, const BufferState state) {
  if (!shared_memory.IsValid()) {
    return observer_ptr<const Buffer>(nullptr);
  }

  if (state == BufferState::Unavailable) {
    return observer_ptr<const Buffer>(nullptr);
  }

  // Return an observer_ptr to the buffer using make_observer
  return make_observer(&shared_memory.Get()->buffers[ToBufferIndex(state)]);
}

// Buffer access within shared memory
template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::SharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
[[nodiscard]] std::span<Buffer> GetBuffers(const SharedMemory<Buffer, buffer_size, SharedMemoryBuffer>& shared_memory) {
  return std::span(shared_memory.Get()->buffers);
}

}  // namespace detail

////////////////////////////////////////////////////////////////////////////////
// Shared memory implementation
////////////////////////////////////////////////////////////////////////////////
template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::SharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
SharedMemory<Buffer, buffer_size, SharedMemoryBuffer>::SharedMemory(const std::string_view name, const bool create,
                                                                    detail::BufferCallback<BufferType> init,
                                                                    detail::BufferCallback<BufferType> validate)
    : state_{.name = std::string(name)} {
  if (!OpenFile(create)) {
    Cleanup();
    return;
  }

  if (!create) {
    struct stat st = {};
    if (fstat(*state_.file_descriptor, &st) != 0) {
      SPDLOG_ERROR("fstat error: {}", detail::FormatSystemError("fstat error", errno));
      Cleanup();
      return;
    }

    if (static_cast<size_t>(st.st_size) != state_.size) {
      SPDLOG_ERROR(
          "Shared memory size mismatch for {}: expected {} bytes, got {} bytes. This indicates a "
          "version mismatch between producer and consumer. Please ensure both are running the same version.",
          state_.name, state_.size, st.st_size);
      Cleanup();
      return;
    }
  }

  if (create && !InitializeSize()) {
    Cleanup();
    shm_unlink(state_.name.c_str());
    return;
  }

  if (!MapMemory()) {
    Cleanup();
    if (create) shm_unlink(state_.name.c_str());
    return;
  }

  // Close fd after successful mmap
  state_.file_descriptor.reset();

  if (create) {
    if (!InitializeBuffer(init)) {
      Cleanup();
      shm_unlink(state_.name.c_str());
      return;
    }
  } else {
    if (!ValidateBuffer(validate)) {
      Cleanup();
      return;
    }
  }

  state_.initialized = true;
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::SharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
bool SharedMemory<Buffer, buffer_size, SharedMemoryBuffer>::OpenFile(const bool create) {
  const int file_descriptor = shm_open(state_.name.c_str(), O_RDWR | (create ? O_CREAT : 0), 0666);
  if (file_descriptor < 0) {
    SPDLOG_ERROR("shm_open error: {}", detail::FormatSystemError("shm_open error", errno));
    return false;
  }
  state_.file_descriptor.reset(new int{file_descriptor});
  return true;
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::SharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
bool SharedMemory<Buffer, buffer_size, SharedMemoryBuffer>::InitializeSize() {
  if (ftruncate(*state_.file_descriptor, state_.size) != 0) {
    SPDLOG_ERROR("ftruncate error: {}", detail::FormatSystemError("ftruncate error", errno));
    return false;
  }
  return true;
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::SharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
bool SharedMemory<Buffer, buffer_size, SharedMemoryBuffer>::MapMemory() {
  state_.memory_address = mmap(nullptr, state_.size, PROT_READ | PROT_WRITE, MAP_SHARED, *state_.file_descriptor, 0);
  if (state_.memory_address == MAP_FAILED) {
    SPDLOG_ERROR("mmap error: {}", detail::FormatSystemError("mmap error", errno));
    return false;
  }
  state_.buffer = static_cast<SharedMemoryBuffer<Buffer, buffer_size>*>(state_.memory_address);
  return true;
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::SharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
void SharedMemory<Buffer, buffer_size, SharedMemoryBuffer>::Cleanup() {
  if (state_.buffer != nullptr && state_.memory_address != nullptr) {
    munmap(state_.memory_address, state_.size);
    state_.buffer = nullptr;
    state_.memory_address = nullptr;
  }
  state_.file_descriptor.reset();
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::SharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
bool SharedMemory<Buffer, buffer_size, SharedMemoryBuffer>::Destroy(const std::string_view name) {
  if (shm_unlink(std::string(name).c_str()) != 0) {
    SPDLOG_ERROR("shm_unlink error: {}", detail::FormatSystemError("shm_unlink error", errno));
    return false;
  }
  return true;
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::SharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
bool SharedMemory<Buffer, buffer_size, SharedMemoryBuffer>::InitializeBuffer(
    detail::BufferCallback<BufferType> callback) {
  if (!state_.buffer) return false;

  // Use NullCallback if nullptr is passed
  auto initialization_function = callback ? callback : NullCallback<BufferType>;
  return initialization_function(state_.buffer);
}

template <typename Buffer, size_t buffer_size, template <typename, size_t> typename SharedMemoryBuffer>
  requires detail::SharedMemoryBufferType<Buffer, buffer_size, SharedMemoryBuffer>
bool SharedMemory<Buffer, buffer_size, SharedMemoryBuffer>::ValidateBuffer(
    detail::BufferCallback<BufferType> callback) {
  if (!state_.buffer) return false;

  // Use NullCallback if nullptr is passed
  auto validation_function = callback ? callback : NullCallback<BufferType>;
  return validation_function(state_.buffer);
}

}  // namespace dex::shared_memory
