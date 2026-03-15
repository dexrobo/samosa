#include <sys/wait.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <sstream>
#include <string>
#include <thread>

#include "absl/strings/match.h"
#include "spdlog/sinks/ostream_sink.h"

#include "dex/infrastructure/shared_memory/futex.h"
#include "dex/infrastructure/shared_memory/shared_memory_streaming_test_private.h"

using dex::shared_memory::test::ArrayBuffer;
using dex::shared_memory::test::CoverageSafeExit;
using dex::shared_memory::test::GetReadIndex;
using dex::shared_memory::test::GetWriteIndex;
using dex::shared_memory::test::LargeBuffer;
using dex::shared_memory::test::LockFreeSharedArrayBuffer;
using dex::shared_memory::test::MockFutex;
using dex::shared_memory::test::SetReadIndex;
using dex::shared_memory::test::SetWriteIndex;
using dex::shared_memory::test::SharedMemStreamingTest;
using dex::shared_memory::test::TestAssert;
using dex::shared_memory::test::WaitForReadIndex;
using dex::shared_memory::test::WaitForWriteIndex;

namespace {

TEST_F(SharedMemStreamingTest, BasicProducerConsumerCommunication) {
  RunProducerConsumer<ArrayBuffer>(
      // Producer lambda
      [](std::string_view shared_memory_name) {
        dex::shared_memory::Producer<ArrayBuffer> producer{shared_memory_name};
        producer.Run([](ArrayBuffer& buffer, uint /*counter*/, int /*buffer_id*/) {
          std::string message = "test message";
          std::memcpy(buffer.data(), message.data(), message.size() + 1);
        });
        CoverageSafeExit(0);
      },
      // Consumer lambda
      [](std::string_view shared_memory_name) {
        const float timeout_seconds = 1.0f;
        const timespec timeout = {
            .tv_sec = static_cast<time_t>(std::floor(timeout_seconds)),
            .tv_nsec = static_cast<int64_t>((timeout_seconds - std::floor(timeout_seconds)) * 1e9)};

        dex::shared_memory::Consumer<ArrayBuffer> consumer{shared_memory_name};
        const auto result = consumer.Run(
            [](const ArrayBuffer& buffer) {
              const std::string message(buffer.data());
              if (message == "test message") CoverageSafeExit(0);
            },
            &timeout);
        // If we reach here, consumer exited without receiving expected message
        CoverageSafeExit(result == dex::shared_memory::RunResult::Timeout ? 1 : 2);
      });
}

TEST_F(SharedMemStreamingTest, ProducerResetByConsumer) {
  std::array<int, 2> pipe_fd = {};
  ASSERT_EQ(pipe(pipe_fd.data()), 0);

  /**
   Producer and consumer has an init state, which corresponds to read and write being Unavailable.

  If producer is already in Unavailable state, then it will wait for read == BufferA / BufferB to resume publishing.
  If producer is already publishing, then read == Unavailable will force producer to init state.
*/

  RunProducerConsumer<ArrayBuffer>(
      // Producer lambda
      [pipe_fd](std::string_view shared_memory_name) {
        close(pipe_fd[0]);  // Close read end

        std::stringstream log_stream;
        auto stream_sink = std::make_shared<spdlog::sinks::ostream_sink_st>(log_stream);
        auto logger = std::make_shared<spdlog::logger>("test_logger", stream_sink);
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::debug);

        auto shared_memory = LockFreeSharedArrayBuffer::Open(shared_memory_name);
        // Initialize publisher's and consumer's state to running
        // Set publisher's initial state to:
        // 1. last published buffer is BufferB
        // 2. publish next frame into BufferA.
        // After publishing to BufferA, it will store into BufferB unless
        // 1. consumer reqeusts BufferA or
        // 2. consumer set read == Unavailable.
        //
        // Producer will think that consumer had requested BufferB previously and that it had already published to
        // BufferB.
        SetWriteIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferB);
        // Producer will think that consumer has requested BufferA and so it will publish to BufferA.
        SetReadIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferA);

        // Start producer
        dex::shared_memory::StreamingControl::Instance().ReconfigureAndReset({.handle_signals = true});
        dex::shared_memory::Producer<ArrayBuffer> producer{shared_memory_name};
        producer.Run([](ArrayBuffer& buffer, uint /*counter*/, int /*buffer_id*/) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          std::string message = "test message";
          std::memcpy(buffer.data(), message.data(), message.size() + 1);
        });

        // Flush the log stream AFTER exiting the producer
        spdlog::default_logger()->flush();
        const std::string log_content = log_stream.str();
        const ssize_t bytes_written = write(pipe_fd[1], log_content.c_str(), log_content.size());
        if (bytes_written == -1) {
          CoverageSafeExit(1);
        }
        CoverageSafeExit(0);
      },

      // Consumer lambda
      [](std::string_view shared_memory_name) {
        const float timeout_seconds = 1.0f;
        const timespec timeout = {
            .tv_sec = static_cast<time_t>(std::floor(timeout_seconds)),
            .tv_nsec = static_cast<int64_t>((timeout_seconds - std::floor(timeout_seconds)) * 1e9)};

        auto shared_memory = LockFreeSharedArrayBuffer::Open(shared_memory_name);

        // Wait for producer to publish a valid frame.
        // Since we set read == BufferA, it will wait for producer to publish to BufferA.
        ASSERT_TRUE(WaitForWriteIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferA));
        ASSERT_EQ(std::strcmp(shared_memory.Get()
                                  ->buffers[dex::shared_memory::detail::ToBufferIndex(
                                      dex::shared_memory::detail::BufferState::BufferA)]
                                  .data(),
                              "test message"),
                  0);

        dex::shared_memory::Consumer<ArrayBuffer> consumer{shared_memory_name};
        const auto result = consumer.Run(
            [](const ArrayBuffer& buffer) {
              const std::string message(buffer.data());
              if (message == "test message") CoverageSafeExit(0);
            },
            &timeout);
        // If we reach here, consumer exited without receiving expected message
        CoverageSafeExit(result == dex::shared_memory::RunResult::Timeout ? 1 : 2);
      });

  // Parent process reads the log content
  close(pipe_fd[1]);  // Close write end
  std::array<char, 4096> buffer = {};
  std::string log_content;
  ssize_t bytes_read = 0;
  while ((bytes_read = read(pipe_fd[0], buffer.data(), buffer.size())) > 0) {
    log_content.append(buffer.data(), bytes_read);
  }
  close(pipe_fd[0]);

  EXPECT_TRUE(absl::StrContains(log_content, "publisher reset"))
      << "Expected 'publisher reset' message not found in logs";  // NOLINT(readability-implicit-bool-conversion)
}

TEST_F(SharedMemStreamingTest, ConsumerTimeout) {
  auto shared_memory = LockFreeSharedArrayBuffer::Create(
      shared_memory_name_,
      dex::shared_memory::InitializeBuffer<ArrayBuffer, 2, dex::shared_memory::LockFreeSharedMemoryBuffer>);
  ASSERT_TRUE(shared_memory.IsValid());

  // Set a short timeout
  const float timeout_seconds = 0.1f;
  const timespec timeout = {.tv_sec = static_cast<time_t>(std::floor(timeout_seconds)),
                            .tv_nsec = static_cast<int64_t>((timeout_seconds - std::floor(timeout_seconds)) * 1e9)};

  // Start consumer with timeout
  dex::shared_memory::Consumer<ArrayBuffer> consumer{shared_memory_name_};
  auto start = std::chrono::steady_clock::now();

  const auto result = consumer.Run([](const ArrayBuffer& /*buffer*/) {}, &timeout);

  auto end = std::chrono::steady_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  // Verify timeout occurred within expected window
  EXPECT_EQ(result, dex::shared_memory::RunResult::Timeout);
  EXPECT_GE(duration.count(), 100);  // At least timeout duration
  EXPECT_LE(duration.count(), 200);  // Allow some overhead but not too much
}

TEST_F(SharedMemStreamingTest, ProducerConsumerAlternateBuffers) {
  RunProducerConsumer<ArrayBuffer>(
      // Producer lambda
      [](std::string_view shared_memory_name) {
        dex::shared_memory::Producer<ArrayBuffer> producer{shared_memory_name};
        for (int i = 0; i < 100; ++i) {
          producer.Run([](ArrayBuffer& /*buffer*/, uint /*counter*/, int /*buffer_id*/) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          });
        }
        CoverageSafeExit(0);
      },
      // Consumer lambda
      [](std::string_view shared_memory_name) {
        dex::shared_memory::Consumer<ArrayBuffer> consumer{shared_memory_name};
        int count = 0;
        int last_buffer_id = -1;
        const auto result =
            consumer.Run([&count, &last_buffer_id](const ArrayBuffer& /*buffer*/, uint /*counter*/, int buffer_id) {
              if (buffer_id != 1 && buffer_id != 2) CoverageSafeExit(2);  // Invalid buffer ID
              if (last_buffer_id != -1 && buffer_id == last_buffer_id)
                CoverageSafeExit(3);  // Buffer IDs didn't alternate
              last_buffer_id = buffer_id;
              count++;
              if (count >= 11) CoverageSafeExit(0);  // Success
              // Continue running if we haven't reached the count
            });
        // If we reach here, consumer exited without reaching count
        CoverageSafeExit(result == dex::shared_memory::RunResult::Stopped ? 1 : 4);
      });
}

TEST_F(SharedMemStreamingTest, ConsumerReceivesFreshFrames) {
  RunProducerConsumer<uint>(
      // Producer lambda
      [](std::string_view shared_memory_name) {
        dex::shared_memory::Producer<uint> producer{shared_memory_name};
        producer.Run([](uint& buffer, uint counter, int /*buffer_id*/) {
          buffer = counter;
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          if (counter >= 100) {
            CoverageSafeExit(0);  // Success
          }
        });
        CoverageSafeExit(0);  // in case consumer kills producer first
      },
      // Consumer lambda
      [](std::string_view shared_memory_name) {
        dex::shared_memory::Consumer<uint> consumer{shared_memory_name};
        uint count = 0;
        uint last_counter = 0;
        const auto result =
            consumer.Run([&count, &last_counter](const uint& buffer, uint /*counter*/, int /*buffer_id*/) {
              if (count > 0 && buffer <= last_counter) {
                CoverageSafeExit(2);  // Frame counter did not increment
              }

              last_counter = buffer;
              count++;
              if (count >= 11) {      // Check consecutive frames
                CoverageSafeExit(0);  // Success
              }
            });
        // If we reach here, consumer exited without reaching count
        CoverageSafeExit(result == dex::shared_memory::RunResult::Stopped ? 1 : 3);
      });
}

TEST_F(SharedMemStreamingTest, ProducerRunningFutexWakeFailure) {
  RunProducerConsumer<ArrayBuffer>(
      // Producer lambda
      [](std::string_view shared_memory_name) {
        MockFutex mock_futex;
        mock_futex.should_fail_wake = true;
        const dex::shared_memory::detail::ScopedFutex scoped_futex(mock_futex);

        auto shared_memory = LockFreeSharedArrayBuffer::Open(shared_memory_name);

        // This will make the producer write to BufferA and publish it on startup.
        SetWriteIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferB);
        SetReadIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferA);

        dex::shared_memory::StreamingControl::Instance().ReconfigureAndReset({.handle_signals = true});
        dex::shared_memory::Producer<ArrayBuffer> producer{shared_memory_name};
        producer.Run([](ArrayBuffer& buffer, uint /*counter*/, int /*buffer_id*/) {
          std::string message = "test message";
          std::memcpy(buffer.data(), message.data(), message.size() + 1);
        });

        // Producer should stop due to FutexWake failure
        ASSERT_EQ(std::strcmp(shared_memory.Get()
                                  ->buffers[dex::shared_memory::detail::ToBufferIndex(
                                      dex::shared_memory::detail::BufferState::BufferB)]
                                  .data(),
                              ""),
                  0);
        ASSERT_EQ(GetWriteIndex(shared_memory), dex::shared_memory::detail::BufferState::BufferA);
        CoverageSafeExit(0);
      },
      // Consumer lambda
      [](std::string_view shared_memory_name) {
        const MockFutex mock_futex;
        const dex::shared_memory::detail::ScopedFutex scoped_futex(mock_futex);

        auto shared_memory = LockFreeSharedArrayBuffer::Open(shared_memory_name);

        // We don't need to run the consumer because we have set the initial states in the shared memory
        // to mimic consumer running.
        ASSERT_TRUE(WaitForWriteIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferA));
        CoverageSafeExit(0);  // Expected timeout
      });
}

TEST_F(SharedMemStreamingTest, ProducerInitFutexWakeFailure) {
  RunProducerConsumer<ArrayBuffer>(
      // Producer lambda
      [](std::string_view shared_memory_name) {
        MockFutex mock_futex;
        mock_futex.should_fail_wake = true;
        const dex::shared_memory::detail::ScopedFutex scoped_futex(mock_futex);

        auto shared_memory = LockFreeSharedArrayBuffer::Open(shared_memory_name);

        // Force consumer process to wait till read index is BufferA.
        SetWriteIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferB);
        SetReadIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferA);
        // Then wait till read index is set to Unavailable when the consumer starts.
        ASSERT_TRUE(WaitForReadIndex(shared_memory, dex::shared_memory::detail::BufferState::Unavailable));

        dex::shared_memory::StreamingControl::Instance().ReconfigureAndReset({.handle_signals = true});
        dex::shared_memory::Producer<ArrayBuffer> producer{shared_memory_name};
        producer.Run([](ArrayBuffer& buffer, uint /*counter*/, int /*buffer_id*/) {
          std::string message = "test message";
          std::memcpy(buffer.data(), message.data(), message.size() + 1);
        });
        ASSERT_EQ(GetWriteIndex(shared_memory), dex::shared_memory::detail::BufferState::Unavailable);
        ASSERT_EQ(std::strcmp(shared_memory.Get()
                                  ->buffers[dex::shared_memory::detail::ToBufferIndex(
                                      dex::shared_memory::detail::BufferState::BufferB)]
                                  .data(),
                              ""),
                  0);
        ASSERT_EQ(std::strcmp(shared_memory.Get()
                                  ->buffers[dex::shared_memory::detail::ToBufferIndex(
                                      dex::shared_memory::detail::BufferState::BufferA)]
                                  .data(),
                              "test message"),
                  0);

        // Producer should stop due to FutexWake failure
        CoverageSafeExit(0);
      },
      // Consumer lambda
      [](std::string_view shared_memory_name) {
        const MockFutex mock_futex;
        const dex::shared_memory::detail::ScopedFutex scoped_futex(mock_futex);

        const float timeout_seconds = 0.1f;
        const timespec timeout = {
            .tv_sec = static_cast<time_t>(std::floor(timeout_seconds)),
            .tv_nsec = static_cast<int64_t>((timeout_seconds - std::floor(timeout_seconds)) * 1e9)};

        auto shared_memory = LockFreeSharedArrayBuffer::Open(shared_memory_name);

        // wait for producer process to start and force write index to BufferA.
        ASSERT_TRUE(WaitForReadIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferA));

        dex::shared_memory::Consumer<ArrayBuffer> consumer{shared_memory_name};
        const auto result = consumer.Run(
            [](const ArrayBuffer& /*buffer*/) {
              CoverageSafeExit(1);  // Should not reach here
            },
            &timeout);
        if (result != dex::shared_memory::RunResult::Timeout) CoverageSafeExit(2);
        CoverageSafeExit(0);  // Expected timeout
      });
}

TEST_F(SharedMemStreamingTest, ConsumerRunningFutexWaitFailure) {
  RunProducerConsumer<ArrayBuffer>(
      // Producer lambda
      [](std::string_view shared_memory_name) {
        const MockFutex mock_futex;
        const dex::shared_memory::detail::ScopedFutex scoped_futex(mock_futex);

        dex::shared_memory::StreamingControl::Instance().ReconfigureAndReset({.handle_signals = true});
        dex::shared_memory::Producer<ArrayBuffer> producer{shared_memory_name};
        producer.Run([](ArrayBuffer& buffer, uint /*counter*/, int /*buffer_id*/) {
          std::string message = "test message";
          std::memcpy(buffer.data(), message.data(), message.size() + 1);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));  // Slow down producer
        });

        auto shared_memory = LockFreeSharedArrayBuffer::Open(shared_memory_name);
        ASSERT_EQ(GetWriteIndex(shared_memory), dex::shared_memory::detail::BufferState::BufferA);

        CoverageSafeExit(0);
      },
      // Consumer lambda
      [](std::string_view shared_memory_name) {
        MockFutex mock_futex;
        mock_futex.should_fail_wait = true;
        const dex::shared_memory::detail::ScopedFutex scoped_futex(mock_futex);

        dex::shared_memory::StreamingControl::Instance().ReconfigureAndReset({.handle_signals = true});
        dex::shared_memory::Consumer<ArrayBuffer> consumer{shared_memory_name};
        const auto result = consumer.Run([](const ArrayBuffer& /*buffer*/) {
          CoverageSafeExit(1);  // Should not reach here
        });
        EXPECT_EQ(result, dex::shared_memory::RunResult::Timeout);

        auto shared_memory = LockFreeSharedArrayBuffer::Open(shared_memory_name);
        ASSERT_EQ(GetReadIndex(shared_memory), dex::shared_memory::detail::BufferState::BufferA);

        // Wait for producer to see the read index update
        ASSERT_TRUE(WaitForWriteIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferA));

        CoverageSafeExit(0);  // Expected to exit due to FutexWait failure
      });
}

TEST_F(SharedMemStreamingTest, ConsumerInitFutexWaitFailure) {
  RunProducerConsumer<ArrayBuffer>(
      // Producer lambda
      [](std::string_view /*shared_memory_name*/) {
        const MockFutex mock_futex;
        const dex::shared_memory::detail::ScopedFutex scoped_futex(mock_futex);

        dex::shared_memory::StreamingControl::Instance().ReconfigureAndReset({.handle_signals = true});
        auto& control = dex::shared_memory::StreamingControl::Instance();
        while (control.IsRunning()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CoverageSafeExit(0);
      },
      // Consumer lambda
      [](std::string_view shared_memory_name) {
        MockFutex mock_futex;
        mock_futex.should_fail_wait = true;
        const dex::shared_memory::detail::ScopedFutex scoped_futex(mock_futex);

        auto shared_memory = LockFreeSharedArrayBuffer::Open(shared_memory_name);
        SetWriteIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferB);
        SetReadIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferA);

        dex::shared_memory::StreamingControl::Instance().ReconfigureAndReset({.handle_signals = true});
        dex::shared_memory::Consumer<ArrayBuffer> consumer{shared_memory_name};
        const auto result = consumer.Run([](const ArrayBuffer& /*buffer*/) {
          CoverageSafeExit(1);  // Should not reach here
        });
        EXPECT_EQ(result, dex::shared_memory::RunResult::Timeout);

        ASSERT_EQ(GetReadIndex(shared_memory), dex::shared_memory::detail::BufferState::Unavailable);

        CoverageSafeExit(0);  // Expected futex failure
      });
}

// Test that signal interruption (EINTR) returns RunResult::Stopped, not RunResult::Timeout
TEST_F(SharedMemStreamingTest, ConsumerInterruptedReturnsStoppedNotTimeout) {
  RunProducerConsumer<ArrayBuffer>(
      // Producer lambda - just wait for consumer to finish
      [](std::string_view /*shared_memory_name*/) {
        const MockFutex mock_futex;
        const dex::shared_memory::detail::ScopedFutex scoped_futex(mock_futex);

        dex::shared_memory::StreamingControl::Instance().ReconfigureAndReset({.handle_signals = true});
        auto& control = dex::shared_memory::StreamingControl::Instance();
        while (control.IsRunning()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CoverageSafeExit(0);
      },
      // Consumer lambda - simulate EINTR by setting wait_failure_result to Interrupted
      [](std::string_view shared_memory_name) {
        MockFutex mock_futex;
        mock_futex.should_fail_wait = true;
        mock_futex.wait_failure_result = dex::shared_memory::detail::WaitResult::Interrupted;
        const dex::shared_memory::detail::ScopedFutex scoped_futex(mock_futex);

        auto shared_memory = LockFreeSharedArrayBuffer::Open(shared_memory_name);
        SetWriteIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferB);
        SetReadIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferA);

        dex::shared_memory::StreamingControl::Instance().ReconfigureAndReset({.handle_signals = true});
        dex::shared_memory::Consumer<ArrayBuffer> consumer{shared_memory_name};
        const auto result = consumer.Run([](const ArrayBuffer& /*buffer*/) {
          CoverageSafeExit(1);  // Should not reach callback
        });

        // Key assertion: interrupted wait should return Stopped, NOT Timeout
        if (result != dex::shared_memory::RunResult::Stopped) {
          CoverageSafeExit(2);  // Wrong result - got Timeout or Error instead of Stopped
        }

        CoverageSafeExit(0);  // Success - got RunResult::Stopped as expected
      });
}

// Test that fatal error (e.g., EFAULT) returns RunResult::Error
TEST_F(SharedMemStreamingTest, ConsumerErrorReturnsError) {
  RunProducerConsumer<ArrayBuffer>(
      // Producer lambda - just wait for consumer to finish
      [](std::string_view /*shared_memory_name*/) {
        const MockFutex mock_futex;
        const dex::shared_memory::detail::ScopedFutex scoped_futex(mock_futex);

        dex::shared_memory::StreamingControl::Instance().ReconfigureAndReset({.handle_signals = true});
        auto& control = dex::shared_memory::StreamingControl::Instance();
        while (control.IsRunning()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CoverageSafeExit(0);
      },
      // Consumer lambda - simulate fatal error by setting wait_failure_result to Error
      [](std::string_view shared_memory_name) {
        MockFutex mock_futex;
        mock_futex.should_fail_wait = true;
        mock_futex.wait_failure_result = dex::shared_memory::detail::WaitResult::Error;
        const dex::shared_memory::detail::ScopedFutex scoped_futex(mock_futex);

        auto shared_memory = LockFreeSharedArrayBuffer::Open(shared_memory_name);
        SetWriteIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferB);
        SetReadIndex(shared_memory, dex::shared_memory::detail::BufferState::BufferA);

        dex::shared_memory::StreamingControl::Instance().ReconfigureAndReset({.handle_signals = true});
        dex::shared_memory::Consumer<ArrayBuffer> consumer{shared_memory_name};
        const auto result = consumer.Run([](const ArrayBuffer& /*buffer*/) {
          CoverageSafeExit(1);  // Should not reach callback
        });

        // Key assertion: error should return Error, NOT Timeout
        if (result != dex::shared_memory::RunResult::Error) {
          CoverageSafeExit(2);  // Wrong result
        }

        CoverageSafeExit(0);  // Success - got RunResult::Error as expected
      });
}

struct SignalHandlingTestParam {
  bool handle_signals;
  int expected_exit_status;
};

class SharedMemStreamingSignalTest : public SharedMemStreamingTest,
                                     public testing::WithParamInterface<SignalHandlingTestParam> {
 protected:
  void SetUp() override {
    // For parameterized tests, we need to include both test name and parameter name
    std::string test_name = std::string(testing::UnitTest::GetInstance()->current_test_info()->test_suite_name()) +
                            "_" + testing::UnitTest::GetInstance()->current_test_info()->name();
    // Replace forward slashes with underscores
    std::ranges::replace(test_name, '/', '_');
    shared_memory_name_ = "test_shared_memory_" + test_name;
    // Reset StreamingControl state before each test
    dex::shared_memory::StreamingControl::Instance().Reset();
  }
};

// NOLINTBEGIN(readability-implicit-bool-conversion)
TEST_P(SharedMemStreamingSignalTest, ProducerInterrupted) {
  const auto& param = GetParam();

  {
    // Create shared memory
    auto shared_memory = LockFreeSharedArrayBuffer::Create(
        shared_memory_name_,
        dex::shared_memory::InitializeBuffer<ArrayBuffer, 2, dex::shared_memory::LockFreeSharedMemoryBuffer>);
    ASSERT_TRUE(shared_memory.IsValid());
  }

  // Fork producer process
  const pid_t producer_pid = fork();
  ASSERT_NE(producer_pid, -1) << "Producer fork failed";

  if (producer_pid == 0) {  // Producer child process
    auto producer_shm = LockFreeSharedArrayBuffer::Open(shared_memory_name_);
    ASSERT_TRUE(producer_shm.IsValid());

    // configuration to handle signals
    dex::shared_memory::StreamingControl::Instance().ReconfigureAndReset({.handle_signals = param.handle_signals});

    dex::shared_memory::Producer<ArrayBuffer> producer{shared_memory_name_};
    producer.Run([](ArrayBuffer& /*buffer*/, uint /*counter*/, int /*buffer_id*/) {
      // Simulate work
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });

    CoverageSafeExit(0);  // Ensure child process exits after running producer
  }

  // Wait for producer to start
  {
    // Open the shared memory in read-write mode
    auto shared_memory = LockFreeSharedArrayBuffer::Open(shared_memory_name_);
    ASSERT_TRUE(shared_memory.IsValid());

    // Verify that write_index is set to Unavailable
    EXPECT_EQ(shared_memory.Get()->write_index.load(std::memory_order_acquire),
              dex::shared_memory::detail::ToInt(dex::shared_memory::detail::BufferState::Unavailable));

    // Store read_index with BufferA
    shared_memory.Get()->read_index.store(
        dex::shared_memory::detail::ToInt(dex::shared_memory::detail::BufferState::BufferA), std::memory_order_release);

    // Wait for write_index to be set to BufferA
    constexpr int kMaxWaitIterations = 100;
    int wait_iterations = 0;
    // Production code uses futex. But for testing, we can use a loop.
    while (shared_memory.Get()->write_index.load(std::memory_order_acquire) !=
           dex::shared_memory::detail::ToInt(dex::shared_memory::detail::BufferState::BufferA)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (++wait_iterations > kMaxWaitIterations) {
        FAIL() << "Timed out waiting for write_index to be BufferA";
      }
    }
  }

  // Send SIGTERM to producer
  ASSERT_EQ(kill(producer_pid, SIGTERM), 0) << "Failed to send SIGTERM to producer";

  // Wait for producer to exit
  int producer_status{};
  ASSERT_NE(waitpid(producer_pid, &producer_status, 0), -1) << "Failed to wait for producer";

  // Check how the process ended
  if (param.handle_signals) {
    // With signal handling, we expect a normal exit with status 0
    EXPECT_TRUE(WIFEXITED(producer_status)) << "Producer did not exit normally";
    EXPECT_EQ(WEXITSTATUS(producer_status), param.expected_exit_status);
    EXPECT_FALSE(WIFSIGNALED(producer_status)) << "Producer was terminated by signal";
    EXPECT_NE(WTERMSIG(producer_status), SIGTERM) << "Producer was terminated by SIGTERM";
  } else {
    // Without signal handling, we expect termination by signal
    EXPECT_FALSE(WIFEXITED(producer_status)) << "Producer exited normally";
    EXPECT_TRUE(WIFSIGNALED(producer_status)) << "Producer was not terminated by signal";
    EXPECT_EQ(WTERMSIG(producer_status), SIGTERM) << "Producer was not terminated by SIGTERM";
  }

  // Ensure that the producer has been terminated
  EXPECT_EQ(kill(producer_pid, 0), -1) << "Producer process still exists after interrupt";
}

TEST_P(SharedMemStreamingSignalTest, ConsumerInterrupted) {
  const auto& param = GetParam();

  {
    // Create shared memory
    auto shared_memory = LockFreeSharedArrayBuffer::Create(
        shared_memory_name_,
        dex::shared_memory::InitializeBuffer<ArrayBuffer, 2, dex::shared_memory::LockFreeSharedMemoryBuffer>);
    ASSERT_TRUE(shared_memory.IsValid());
  }

  // Fork consumer process
  const pid_t consumer_pid = fork();
  ASSERT_NE(consumer_pid, -1) << "Consumer fork failed";

  if (consumer_pid == 0) {  // Consumer child process
    auto consumer_shm = LockFreeSharedArrayBuffer::Open(shared_memory_name_);
    ASSERT_TRUE(consumer_shm.IsValid());

    // configuration to handle signals
    dex::shared_memory::StreamingControl::Instance().ReconfigureAndReset({.handle_signals = param.handle_signals});

    dex::shared_memory::Consumer<ArrayBuffer> consumer{shared_memory_name_};
    // Verify that read_index is set to Unavailable (initial state)
    EXPECT_EQ(consumer_shm.Get()->read_index.load(std::memory_order_acquire),
              dex::shared_memory::detail::ToInt(dex::shared_memory::detail::BufferState::Unavailable));
    const auto result = consumer.Run([](const ArrayBuffer& /*buffer*/, uint /*counter*/, int /*buffer_id*/) {
      // Simulate work
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    EXPECT_EQ(result, dex::shared_memory::RunResult::Stopped);

    CoverageSafeExit(0);  // Ensure child process exits after running consumer
  }

  // Wait for consumer to start
  {
    // Open the shared memory in read-write mode
    auto shared_memory = LockFreeSharedArrayBuffer::Open(shared_memory_name_);
    ASSERT_TRUE(shared_memory.IsValid());

    // Verify that write_index is set to Unavailable (initial state)
    EXPECT_EQ(shared_memory.Get()->write_index.load(std::memory_order_acquire),
              dex::shared_memory::detail::ToInt(dex::shared_memory::detail::BufferState::Unavailable));

    // Wait for read_index to be set to BufferA (Consumers requests BufferA when it sees that write_index is
    // Unavailable)
    constexpr int kMaxWaitIterations = 100;
    int wait_iterations = 0;
    // Production code uses futex. But for testing, we can use a loop.
    while (shared_memory.Get()->read_index.load(std::memory_order_acquire) !=
           dex::shared_memory::detail::ToInt(dex::shared_memory::detail::BufferState::BufferA)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (++wait_iterations > kMaxWaitIterations) {
        FAIL() << "Timed out waiting for read_index to be BufferA";
      }
    }
  }

  // Send SIGTERM to consumer
  ASSERT_EQ(kill(consumer_pid, SIGTERM), 0) << "Failed to send SIGTERM to consumer";

  // Wait for consumer to exit
  int consumer_status{};
  ASSERT_NE(waitpid(consumer_pid, &consumer_status, 0), -1) << "Failed to wait for consumer";

  // Check how the process ended
  if (param.handle_signals) {
    // With signal handling, we expect a normal exit with status 0
    EXPECT_TRUE(WIFEXITED(consumer_status)) << "Consumer did not exit normally";
    EXPECT_EQ(WEXITSTATUS(consumer_status), param.expected_exit_status);
    EXPECT_FALSE(WIFSIGNALED(consumer_status)) << "Consumer was terminated by signal";
    EXPECT_NE(WTERMSIG(consumer_status), SIGTERM) << "Consumer was terminated by SIGTERM";
  } else {
    // Without signal handling, we expect termination by signal
    EXPECT_FALSE(WIFEXITED(consumer_status)) << "Consumer exited normally";
    EXPECT_TRUE(WIFSIGNALED(consumer_status)) << "Consumer was not terminated by signal";
    EXPECT_EQ(WTERMSIG(consumer_status), SIGTERM) << "Consumer was not terminated by SIGTERM";
  }
  // Ensure that the consumer has been terminated
  EXPECT_EQ(kill(consumer_pid, 0), -1) << "Consumer process still exists after interrupt";
}

// NOLINTEND(readability-implicit-bool-conversion)

INSTANTIATE_TEST_SUITE_P(SignalHandling, SharedMemStreamingSignalTest,
                         testing::Values(SignalHandlingTestParam{.handle_signals = true, .expected_exit_status = 0},
                                         SignalHandlingTestParam{.handle_signals = false, .expected_exit_status = 0}
                                         // Exit status not used for signal termination
                                         ),
                         [](const testing::TestParamInfo<SignalHandlingTestParam>& info) {
                           return info.param.handle_signals ? "WithSignalHandling" : "WithoutSignalHandling";
                         });

class StreamingControlTest : public testing::Test {
 protected:
  static void SetUpTestSuite() { testing::GTEST_FLAG(death_test_style) = "threadsafe"; }

  void SetUp() override {
    test_handler_called = 0;  // Reset counter before each test
  }

 public:
  static volatile sig_atomic_t test_handler_called;

  static void TestSignalHandler(int /*sig*/) {
    std::atomic_fetch_add(reinterpret_cast<volatile std::atomic<sig_atomic_t>*>(&test_handler_called), 1);
  }
};

// Define the static member
volatile sig_atomic_t StreamingControlTest::test_handler_called = 0;

TEST_F(StreamingControlTest, ChainHandlers) {
  EXPECT_EXIT(
      {
        // First install our own signal handlers
        struct sigaction test_action{};
        test_action.sa_handler = TestSignalHandler;
        sigaction(SIGINT, &test_action, nullptr);
        sigaction(SIGTERM, &test_action, nullptr);

        struct sigaction previous{};
        sigaction(SIGINT, nullptr, &previous);
        TestAssert(previous.sa_handler == TestSignalHandler, 1);
        sigaction(SIGTERM, nullptr, &previous);
        TestAssert(previous.sa_handler == TestSignalHandler, 2);

        dex::shared_memory::StreamingControl::SetDefaultConfiguration({.handle_signals = true});
        auto& control = dex::shared_memory::StreamingControl::Instance();

        // Test SIGINT with chaining
        test_handler_called = 0;
        TestAssert(control.IsRunning(), 3);  // Should be running initially
        raise(SIGINT);
        TestAssert(test_handler_called == 1, 4);  // Our handler was called
        TestAssert(!control.IsRunning(), 5);      // StreamingControl handler was also called

        // Test that Reset restores running state but keeps handlers
        control.Reset();
        test_handler_called = 0;
        TestAssert(control.IsRunning(), 6);  // Should be running again
        raise(SIGINT);
        TestAssert(test_handler_called == 1, 7);  // Our handler still called
        TestAssert(!control.IsRunning(), 8);      // StreamingControl handler still called

        // Test that ReconfigureAndReset removes StreamingControl handler
        control.ReconfigureAndReset(dex::shared_memory::StreamingControl::Configuration{.handle_signals = false});
        test_handler_called = 0;
        TestAssert(control.IsRunning(), 9);  // Should be running again
        raise(SIGINT);
        TestAssert(test_handler_called == 1, 10);  // Only our handler called
        TestAssert(control.IsRunning(), 11);       // StreamingControl handler not called

        CoverageSafeExit(0);  // Success
      },
      testing::ExitedWithCode(0), "");
}

TEST_F(StreamingControlTest, DefaultBehavior) {
  EXPECT_EXIT(
      {
        auto config = dex::shared_memory::StreamingControl::Configuration{};
        TestAssert(!config.handle_signals, 1);
        TestAssert(config.chain_handlers, 2);
        TestAssert(config.signals.size() == 2, 3);
        TestAssert(config.signals[0] == SIGINT, 4);
        TestAssert(config.signals[1] == SIGTERM, 5);

        struct sigaction previous{};
        sigaction(SIGINT, nullptr, &previous);
        TestAssert(previous.sa_handler == SIG_DFL, 6);
        sigaction(SIGTERM, nullptr, &previous);
        TestAssert(previous.sa_handler == SIG_DFL, 7);

        dex::shared_memory::StreamingControl::Instance();
        sigaction(SIGINT, nullptr, &previous);
        TestAssert(previous.sa_handler == SIG_DFL, 8);
        sigaction(SIGTERM, nullptr, &previous);
        TestAssert(previous.sa_handler == SIG_DFL, 9);

        CoverageSafeExit(0);
      },
      testing::ExitedWithCode(0), "");
}

TEST_F(StreamingControlTest, DefaultConfigurationChanged) {
  EXPECT_EXIT(
      {
        auto config = dex::shared_memory::StreamingControl::Configuration{.handle_signals = true};
        EXPECT_TRUE(config.handle_signals);  // Check that handle_signals is true
        EXPECT_TRUE(config.chain_handlers);
        EXPECT_EQ(config.signals.size(), 2);
        EXPECT_EQ(config.signals[0], SIGINT);
        EXPECT_EQ(config.signals[1], SIGTERM);

        struct sigaction previous{};
        sigaction(SIGINT, nullptr, &previous);
        EXPECT_EQ(previous.sa_handler, SIG_DFL);
        sigaction(SIGTERM, nullptr, &previous);
        EXPECT_EQ(previous.sa_handler, SIG_DFL);

        dex::shared_memory::StreamingControl::SetDefaultConfiguration(config);
        dex::shared_memory::StreamingControl::Instance();
        sigaction(SIGINT, nullptr, &previous);
        EXPECT_NE(previous.sa_handler, SIG_DFL);  // Check that signal handler was installed
        sigaction(SIGTERM, nullptr, &previous);
        EXPECT_NE(previous.sa_handler, SIG_DFL);  // Check that signal handler was installed

        CoverageSafeExit(0);  // Success
      },
      testing::ExitedWithCode(0), "");
}

TEST_F(StreamingControlTest, HandleSignals) {
  EXPECT_EXIT(
      {
        auto config = dex::shared_memory::StreamingControl::Configuration{.handle_signals = true};
        TestAssert(config.handle_signals, 1);
        TestAssert(config.chain_handlers, 2);
        TestAssert(config.signals.size() == 2, 3);
        TestAssert(config.signals[0] == SIGINT, 4);
        TestAssert(config.signals[1] == SIGTERM, 5);

        struct sigaction previous{};
        sigaction(SIGINT, nullptr, &previous);
        TestAssert(previous.sa_handler == SIG_DFL, 6);
        sigaction(SIGTERM, nullptr, &previous);
        TestAssert(previous.sa_handler == SIG_DFL, 7);

        dex::shared_memory::StreamingControl::SetDefaultConfiguration(config);
        dex::shared_memory::StreamingControl::Instance();
        sigaction(SIGINT, nullptr, &previous);
        TestAssert(previous.sa_handler != SIG_DFL, 8);
        sigaction(SIGTERM, nullptr, &previous);
        TestAssert(previous.sa_handler != SIG_DFL, 9);

        CoverageSafeExit(0);
      },
      testing::ExitedWithCode(0), "");
}

TEST(ToBufferIndexTest, ConvertsBufferStatesToIndicesCorrectly) {
  // Test valid states
  EXPECT_EQ(dex::shared_memory::detail::ToBufferIndex(dex::shared_memory::detail::BufferState::BufferA), 0);
  EXPECT_EQ(dex::shared_memory::detail::ToBufferIndex(dex::shared_memory::detail::BufferState::BufferB), 1);

  // Test invalid state
  EXPECT_THROW(dex::shared_memory::detail::ToBufferIndex(dex::shared_memory::detail::BufferState::Unavailable),
               const char*);

  // Compile-time verification
  static_assert(dex::shared_memory::detail::ToBufferIndex(dex::shared_memory::detail::BufferState::BufferA) == 0,
                "BufferA should map to index 0");
  static_assert(dex::shared_memory::detail::ToBufferIndex(dex::shared_memory::detail::BufferState::BufferB) == 1,
                "BufferB should map to index 1");
}

TEST(ToBufferIndexTest, ConvertsBufferStatesFromNumberCorrectly) {
  // Test valid states
  EXPECT_EQ(dex::shared_memory::detail::ToBufferState(0), dex::shared_memory::detail::BufferState::Unavailable);
  EXPECT_EQ(dex::shared_memory::detail::ToBufferState(1), dex::shared_memory::detail::BufferState::BufferA);
  EXPECT_EQ(dex::shared_memory::detail::ToBufferState(2), dex::shared_memory::detail::BufferState::BufferB);

  // Test invalid state
  const auto result = []() { return dex::shared_memory::detail::ToBufferState(3); };
  EXPECT_THROW(result(), const char*);

  // Compile-time verification
  static_assert(dex::shared_memory::detail::ToBufferState(0) == dex::shared_memory::detail::BufferState::Unavailable,
                "BufferA should map to index 0");
  static_assert(dex::shared_memory::detail::ToBufferState(1) == dex::shared_memory::detail::BufferState::BufferA,
                "BufferB should map to index 1");
  static_assert(dex::shared_memory::detail::ToBufferState(2) == dex::shared_memory::detail::BufferState::BufferB,
                "BufferB should map to index 2");
}

TEST(SharedMemoryStreamingTest, ValidateBufferTest) {
  auto shared_memory = dex::shared_memory::SharedMemory<int, 1, dex::shared_memory::LockFreeSharedMemoryBuffer>::Create(
      "test_shared_memory",
      dex::shared_memory::InitializeBuffer<int, 1, dex::shared_memory::LockFreeSharedMemoryBuffer>);
  ASSERT_TRUE(shared_memory.IsValid());

  // Test successful validation
  EXPECT_TRUE(dex::shared_memory::ValidateBuffer(shared_memory.Get()));

  // Test version mismatch
  shared_memory.Get()->version = dex::shared_memory::detail::kSharedMemVersion + 1;
  EXPECT_FALSE(dex::shared_memory::ValidateBuffer(shared_memory.Get()));
}

TEST_F(SharedMemStreamingTest, LargeBufferCommunication) {
  RunProducerConsumer<LargeBuffer>(
      // Producer lambda
      [](std::string_view shared_memory_name) {
        dex::shared_memory::Producer<LargeBuffer> producer{shared_memory_name};
        producer.Run([](LargeBuffer& buffer, uint /*counter*/, int /*buffer_id*/) {
          buffer.a = 0xDEADBEEF;
          std::memset(buffer.data.data(), static_cast<int>(0xAA), buffer.data.size());
        });
        CoverageSafeExit(0);
      },
      // Consumer lambda
      [](std::string_view shared_memory_name) {
        const float timeout_seconds = 1.0f;
        const timespec timeout = {
            .tv_sec = static_cast<time_t>(std::floor(timeout_seconds)),
            .tv_nsec = static_cast<int64_t>((timeout_seconds - std::floor(timeout_seconds)) * 1e9)};

        dex::shared_memory::Consumer<LargeBuffer> consumer{shared_memory_name};
        const auto result = consumer.Run(
            [](const LargeBuffer& buffer) {
              if (buffer.a == 0xDEADBEEF && buffer.data[0] == static_cast<std::byte>(0xAA)) CoverageSafeExit(0);
            },
            &timeout);
        CoverageSafeExit(result == dex::shared_memory::RunResult::Timeout ? 1 : 2);
      });
}

}  // namespace

