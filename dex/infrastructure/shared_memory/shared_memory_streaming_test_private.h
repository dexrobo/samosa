#pragma once

#include <array>

#include "gtest/gtest.h"
#include "spdlog/sinks/stdout_sinks.h"
#include "spdlog/spdlog.h"

#include "dex/infrastructure/shared_memory/shared_memory_streaming.h"

namespace dex::shared_memory::test {

constexpr size_t kFrameSize = 64;
using ArrayBuffer = std::array<char, kFrameSize>;
using LockFreeSharedArrayBuffer =
    shared_memory::SharedMemory<ArrayBuffer, 2, shared_memory::LockFreeSharedMemoryBuffer>;
using LockFreeSharedUnsignedIntBuffer = shared_memory::SharedMemory<uint, 2, shared_memory::LockFreeSharedMemoryBuffer>;

inline void CoverageSafeExit(int code) {
  // while `exit()` is not thread-safe, we need to use this instead of `_exit()`
  // because we need to flush the coverage data.
  exit(code);  // NOLINT(concurrency-mt-unsafe)
}

// Mock futex operations for testing
class MockFutex : public detail::Futex {
 public:
  bool should_fail_wait = false;
  bool should_fail_wake = false;
  detail::WaitResult wait_failure_result = detail::WaitResult::Timeout;

  [[nodiscard]] detail::WaitResult Wait(const std::atomic<uint32_t>& futex_address, const int expected,
                                        const timespec* timeout = nullptr) const override {
    if (should_fail_wait) return wait_failure_result;
    return detail::DefaultFutex().Wait(futex_address, expected, timeout);
  }

  [[nodiscard]] bool Wake(const std::atomic<uint32_t>& futex_address, const int count) const override {
    if (should_fail_wake) return false;
    return detail::DefaultFutex().Wake(futex_address, count);
  }
};

template <typename T>
constexpr void TestAssert(T condition, int code) {
  if (!condition) CoverageSafeExit(code);
}

template <typename Buffer>
void SetWriteIndex(Buffer& shared_memory, detail::BufferState state) {
  shared_memory.Get()->write_index.store(detail::ToInt(state), std::memory_order_release);
}

/**
 * Sets the read index of the shared memory buffer to the specified state.
 * @param shared_memory The shared memory buffer
 * @param state The buffer state to set
 */
template <typename Buffer>
void SetReadIndex(Buffer& shared_memory, detail::BufferState state) {
  shared_memory.Get()->read_index.store(detail::ToInt(state), std::memory_order_release);
}

/**
 * Gets the current write index state from the shared memory buffer.
 * @param shared_memory The shared memory buffer
 * @return The current write index buffer state
 */
template <typename Buffer>
detail::BufferState GetWriteIndex(Buffer& shared_memory) {
  return static_cast<detail::BufferState>(shared_memory.Get()->write_index.load(std::memory_order_acquire));
}

/**
 * Gets the current read index state from the shared memory buffer.
 * @param shared_memory The shared memory buffer
 * @return The current read index buffer state
 */
template <typename Buffer>
detail::BufferState GetReadIndex(Buffer& shared_memory) {
  return static_cast<detail::BufferState>(shared_memory.Get()->read_index.load(std::memory_order_acquire));
}

/**
 * Waits for the write index to reach an expected state.
 * @param shared_memory The shared memory buffer
 * @param expected_state The buffer state to wait for
 * @param max_wait_ms Maximum time to wait in milliseconds
 * @return true if the expected state was reached, false if timed out
 */
template <typename Buffer>
bool WaitForWriteIndex(Buffer& shared_memory, detail::BufferState expected_state, int max_wait_ms = 1000) {
  int waited_ms = 0;
  while (GetWriteIndex(shared_memory) != expected_state) {
    if (waited_ms >= max_wait_ms) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    waited_ms++;
  }
  return true;
}

/**
 * Waits for the read index to reach an expected state.
 * @param shared_memory The shared memory buffer
 * @param expected_state The buffer state to wait for
 * @param max_wait_ms Maximum time to wait in milliseconds
 * @return true if the expected state was reached, false if timed out
 */
template <typename Buffer>
bool WaitForReadIndex(Buffer& shared_memory, detail::BufferState expected_state, int max_wait_ms = 1000) {
  int waited_ms = 0;
  while (GetReadIndex(shared_memory) != expected_state) {
    if (waited_ms >= max_wait_ms) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    waited_ms++;
  }
  return true;
}

class SharedMemStreamingTest : public testing::Test {
 public:
  static void SetUpTestSuite() {
    // Configure spdlog to log to stderr for test output capture
    auto logger = spdlog::get("streaming_stderr_logger");
    if (logger == nullptr) {
      auto stderr_logger = spdlog::stderr_logger_mt("streaming_stderr_logger");
      spdlog::set_default_logger(stderr_logger);
    }
  }

  // Helper method to run producer and consumer processes
  template <typename Buffer, size_t buffer_size = 2>
  void RunProducerConsumer(const std::function<void(std::string_view)>& producer_func,
                           const std::function<void(std::string_view)>& consumer_func) {
    {
      // Create shared memory
      auto shared_memory =  // NOLINT(readability-isolate-declaration)
          SharedMemory<Buffer, buffer_size,
                       shared_memory::LockFreeSharedMemoryBuffer>::Create(  // NOLINT(readability-identifier-naming)
              shared_memory_name_, InitializeBuffer<Buffer>);
      ASSERT_TRUE(shared_memory.IsValid());
    }

    // Fork consumer process
    const pid_t consumer_pid = fork();
    ASSERT_NE(consumer_pid, -1) << "Consumer fork failed";

    if (consumer_pid == 0) {  // Consumer child process
      consumer_func(shared_memory_name_);
    }

    // Fork producer process
    const pid_t producer_pid = fork();
    ASSERT_NE(producer_pid, -1) << "Producer fork failed";

    if (producer_pid == 0) {  // Producer child process
      StreamingControl::Instance().ReconfigureAndReset({.handle_signals = true});
      producer_func(shared_memory_name_);
    }

    // Parent process: wait for consumer to finish
    int consumer_status{};
    ASSERT_NE(waitpid(consumer_pid, &consumer_status, 0), -1) << "Failed to wait for consumer";

    // Kill producer once consumer is done - ignore if already exited
    if (kill(producer_pid, SIGTERM) == -1 && errno != ESRCH) {
      std::array<char, 256> err_buf = {};
#ifdef __APPLE__
      strerror_r(errno, err_buf.data(), err_buf.size());
#else
      [[maybe_unused]] auto* err_str = strerror_r(errno, err_buf.data(), err_buf.size());
#endif
      FAIL() << "Failed to send SIGTERM to producer: " << err_buf.data();
    }

    // Wait for producer to finish
    int producer_status{};
    ASSERT_NE(waitpid(producer_pid, &producer_status, 0), -1) << "Failed to wait for producer";

    // Verify results
    EXPECT_TRUE(WIFEXITED(consumer_status)) << "Consumer process did not exit normally";
    EXPECT_EQ(WEXITSTATUS(consumer_status), 0) << "Consumer process indicated failure";
    EXPECT_TRUE(WIFEXITED(producer_status)) << "Producer process did not exit normally";
    EXPECT_EQ(WEXITSTATUS(producer_status), 0) << "Producer process indicated failure";
  }

 protected:
  void SetUp() override {
    shared_memory_name_ =
        std::string("test_shared_memory_") + testing::UnitTest::GetInstance()->current_test_info()->name();

    // Reset StreamingControl state before each test
    StreamingControl::Instance().Reset();
  }

  void TearDown() override {
    [[maybe_unused]] const bool result =  // NOLINT(cppcoreguidelines-init-variables)
        SharedMemory<ArrayBuffer, 2, shared_memory::LockFreeSharedMemoryBuffer>::Destroy(shared_memory_name_);
  }

  std::string shared_memory_name_;
};

}  // namespace dex::shared_memory::test
