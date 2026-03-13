#include "dex/infrastructure/shared_memory/shared_memory_monitor.h"

#include <array>
#include <atomic>
#include <chrono>
#include <regex>
#include <string>
#include <thread>

#include "gtest/gtest.h"

#include "dex/infrastructure/shared_memory/shared_memory_streaming_test_private.h"
#include "dex/infrastructure/shared_memory/streaming_control.h"

// TODO: make sure that Run() correctly waits for data changes and reads the data

namespace dex::shared_memory {

// Constants for test
constexpr uint32_t kWritingFlag = uint32_t{1} << 31;  // Bit 31 for writing flag

class SharedMemoryMonitorTest : public testing::Test {
 protected:
  void SetUp() override {
    // Create a unique name for each test
    shared_memory_name_ = std::string("test_monitor_") + testing::UnitTest::GetInstance()->current_test_info()->name();

    // Reset StreamingControl state before each test
    StreamingControl::Instance().Reset();
  }

  void TearDown() override {
    // Clean up shared memory - handle the nodiscard return value
    [[maybe_unused]] const bool result =  // NOLINT(cppcoreguidelines-init-variables)
        SharedMemory<test::ArrayBuffer, 2, LockFreeSharedMemoryBuffer>::Destroy(shared_memory_name_);
  }

  std::string shared_memory_name_;
};

// Test that Monitor correctly handles invalid shared memory
TEST_F(SharedMemoryMonitorTest, InvalidSharedMemory) {
  // Create a monitor with a non-existent shared memory name
  Monitor<test::ArrayBuffer> monitor{"non_existent_shared_memory"};
  EXPECT_FALSE(monitor.IsValid());

  // Try to get the latest buffer
  auto buffer_opt = monitor.GetLatestBuffer(0.1, MonitorReadMode::SkipDuringProducerWrite);
  EXPECT_FALSE(buffer_opt.has_value());
}

// Test that Monitor correctly handles the case when there's no valid buffer
TEST_F(SharedMemoryMonitorTest, NoValidBuffer) {
  // Create shared memory
  auto shared_memory = SharedMemory<test::ArrayBuffer, 2, LockFreeSharedMemoryBuffer>::Create(
      shared_memory_name_, InitializeBuffer<test::ArrayBuffer>);
  ASSERT_TRUE(shared_memory.IsValid());

  // Set the last written buffer to Unavailable
  shared_memory.Get()->last_written_buffer.store(detail::ToInt(detail::BufferState::Unavailable),
                                                 std::memory_order_release);

  // Create a monitor
  Monitor<test::ArrayBuffer> monitor{shared_memory_name_};
  ASSERT_TRUE(monitor.IsValid());

  // Try to get the latest buffer
  auto buffer_opt = monitor.GetLatestBuffer(0.1, MonitorReadMode::SkipDuringProducerWrite);
  EXPECT_FALSE(buffer_opt.has_value());
}

// Test that Monitor can read data independent of producer-consumer handshaking
TEST_F(SharedMemoryMonitorTest, IndependentReading) {
  // Create shared memory
  auto shared_memory = SharedMemory<test::ArrayBuffer, 2, LockFreeSharedMemoryBuffer>::Create(
      shared_memory_name_, InitializeBuffer<test::ArrayBuffer>);
  ASSERT_TRUE(shared_memory.IsValid());

  // Set up producer and consumer indices to simulate active handshaking
  shared_memory.Get()->write_index.store(detail::ToInt(detail::BufferState::BufferA), std::memory_order_release);

  shared_memory.Get()->read_index.store(detail::ToInt(detail::BufferState::BufferB), std::memory_order_release);

  // Write different data to both buffers
  const std::string message_a = "Buffer A Data";
  const std::string message_b = "Buffer B Data";

  std::memcpy(shared_memory.Get()->buffers[0].data(), message_a.data(), message_a.size() + 1);
  std::memcpy(shared_memory.Get()->buffers[1].data(), message_b.data(), message_b.size() + 1);

  // Set the last written buffer to BufferA
  shared_memory.Get()->last_written_buffer.store(detail::ToInt(detail::BufferState::BufferA),
                                                 std::memory_order_release);

  // Create a monitor
  Monitor<test::ArrayBuffer> monitor{shared_memory_name_};
  ASSERT_TRUE(monitor.IsValid());

  // Monitor should read from BufferA (last written) regardless of read/write indices
  auto buffer_opt = monitor.GetLatestBuffer(0.1, MonitorReadMode::ReadDuringProducerWrite);
  ASSERT_TRUE(buffer_opt.has_value());
  EXPECT_EQ(std::string(buffer_opt->get().data()), message_a);

  // Set the writing flag
  shared_memory.Get()->sequence_and_writing.store(kWritingFlag, std::memory_order_release);

  // Change last written buffer to BufferB
  shared_memory.Get()->last_written_buffer.store(detail::ToInt(detail::BufferState::BufferB),
                                                 std::memory_order_release);
  // Monitor should now read from BufferB
  buffer_opt = monitor.GetLatestBuffer(0.1, MonitorReadMode::ReadDuringProducerWrite);
  ASSERT_TRUE(buffer_opt.has_value());
  EXPECT_EQ(std::string(buffer_opt->get().data()), message_b);
}

TEST_F(SharedMemoryMonitorTest, SkipDuringWrite) {
  // Create shared memory
  auto shared_memory = SharedMemory<test::ArrayBuffer, 2, LockFreeSharedMemoryBuffer>::Create(
      shared_memory_name_, InitializeBuffer<test::ArrayBuffer>);
  ASSERT_TRUE(shared_memory.IsValid());

  // Set up initial state
  shared_memory.Get()->last_written_buffer.store(detail::ToInt(detail::BufferState::BufferA),
                                                 std::memory_order_release);

  // Set the writing flag
  shared_memory.Get()->sequence_and_writing.store(kWritingFlag, std::memory_order_release);

  // Create a monitor
  Monitor<test::ArrayBuffer> monitor{shared_memory_name_};
  ASSERT_TRUE(monitor.IsValid());

  // Should skip reading while producer is writing
  auto buffer_opt = monitor.GetLatestBuffer(0.1, MonitorReadMode::SkipDuringProducerWrite);
  EXPECT_FALSE(buffer_opt.has_value());
}

TEST_F(SharedMemoryMonitorTest, WaitForCompletion) {
  // Create shared memory
  auto shared_memory = SharedMemory<test::ArrayBuffer, 2, LockFreeSharedMemoryBuffer>::Create(
      shared_memory_name_, InitializeBuffer<test::ArrayBuffer>);
  ASSERT_TRUE(shared_memory.IsValid());

  // Set up initial state
  shared_memory.Get()->last_written_buffer.store(detail::ToInt(detail::BufferState::BufferA),
                                                 std::memory_order_release);

  // Write test data
  const std::string message = "Test Data";
  std::memcpy(shared_memory.Get()->buffers[0].data(), message.data(), message.size() + 1);

  // Set the writing flag
  shared_memory.Get()->sequence_and_writing.store(kWritingFlag, std::memory_order_release);

  // Create a monitor
  Monitor<test::ArrayBuffer> monitor{shared_memory_name_};
  ASSERT_TRUE(monitor.IsValid());

  // Synchronization flags
  std::atomic<bool> thread_started_sleeping{false};
  std::atomic<bool> get_latest_buffer_started{false};
  constexpr auto kSleepDuration = std::chrono::milliseconds(50);

  // Start a thread that will clear the writing flag
  std::thread clear_writing_flag([&]() {
    // Signal that we're ready to sleep
    thread_started_sleeping.store(true, std::memory_order_release);

    // Wait for acknowledgment that GetLatestBuffer is ready to be called
    while (!get_latest_buffer_started.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }

    std::this_thread::sleep_for(kSleepDuration);

    // Clear the writing flag and wake up waiting threads
    shared_memory.Get()->sequence_and_writing.store(0, std::memory_order_release);
    const bool result = detail::GetDefaultFutex()->Wake(shared_memory.Get()->sequence_and_writing, 1);
    ASSERT_TRUE(result);
  });

  // Wait until the thread signals it's ready to sleep
  while (!thread_started_sleeping.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  // Record start time
  auto start_time = std::chrono::steady_clock::now();

  // Signal that we're about to call GetLatestBuffer
  get_latest_buffer_started.store(true, std::memory_order_release);

  // Should wait for producer to finish writing
  auto buffer_opt = monitor.GetLatestBuffer(0.1, MonitorReadMode::WaitForProducerWriteCompletion);

  // Record end time and calculate duration
  auto elapsed_time = std::chrono::steady_clock::now() - start_time;

  ASSERT_TRUE(buffer_opt.has_value());
  EXPECT_EQ(std::string(buffer_opt->get().data()), message);

  // Now we can safely verify the waiting time
  EXPECT_GE(elapsed_time, kSleepDuration);
  EXPECT_LE(elapsed_time, kSleepDuration * 2);

  clear_writing_flag.join();
}

TEST_F(SharedMemoryMonitorTest, WaitForCompletionTimeout) {
  // Create shared memory
  auto shared_memory = SharedMemory<test::ArrayBuffer, 2, LockFreeSharedMemoryBuffer>::Create(
      shared_memory_name_, InitializeBuffer<test::ArrayBuffer>);
  ASSERT_TRUE(shared_memory.IsValid());

  // Set up initial state
  shared_memory.Get()->last_written_buffer.store(detail::ToInt(detail::BufferState::BufferA),
                                                 std::memory_order_release);

  // Set the writing flag
  shared_memory.Get()->sequence_and_writing.store(kWritingFlag, std::memory_order_release);

  // Create a monitor
  Monitor<test::ArrayBuffer> monitor{shared_memory_name_};
  ASSERT_TRUE(monitor.IsValid());

  // Record start time
  auto start_time = std::chrono::steady_clock::now();

  // Should timeout waiting for producer
  auto buffer_opt = monitor.GetLatestBuffer(0.1, MonitorReadMode::WaitForProducerWriteCompletion);

  // Record end time and calculate duration
  auto elapsed_time = std::chrono::steady_clock::now() - start_time;

  EXPECT_FALSE(buffer_opt.has_value());
  // Verify we waited at least the timeout duration
  EXPECT_GE(elapsed_time, std::chrono::milliseconds(10));
  // Verify the writing flag is still set (indicating we timed out rather than the flag being cleared)
  EXPECT_TRUE(detail::IsWriting(shared_memory.Get()->sequence_and_writing.load(std::memory_order_acquire)));
}

TEST_F(SharedMemoryMonitorTest, RunWithCallback) {
  // Create shared memory
  auto shared_memory = SharedMemory<test::ArrayBuffer, 2, LockFreeSharedMemoryBuffer>::Create(
      shared_memory_name_, InitializeBuffer<test::ArrayBuffer>);
  ASSERT_TRUE(shared_memory.IsValid());

  // Set up initial state
  shared_memory.Get()->last_written_buffer.store(detail::ToInt(detail::BufferState::BufferA),
                                                 std::memory_order_release);

  // Write test data
  const std::string message = "Test Data";
  std::memcpy(shared_memory.Get()->buffers[0].data(), message.data(), message.size() + 1);

  // Create a monitor
  Monitor<test::ArrayBuffer> monitor{shared_memory_name_};
  ASSERT_TRUE(monitor.IsValid());

  // Track callback invocations
  size_t callback_count = 0;
  uint64_t last_sequence = 0;

  // Start a thread that will update the sequence number
  std::thread update_sequence([&shared_memory]() {
    for (uint64_t i = 1; i <= 3; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      shared_memory.Get()->sequence_and_writing.store(i, std::memory_order_release);
    }
  });

  // Run the monitor with a callback
  monitor.Run(
      [&](const test::ArrayBuffer& buffer, uint64_t sequence) {
        callback_count++;
        if (callback_count > 1) {
          // Only check sequence increment after the first call
          EXPECT_GT(sequence, last_sequence);
        }
        last_sequence = sequence;
        EXPECT_EQ(std::string(buffer.data()), message);
      },
      0.1, 5);

  update_sequence.join();

  // Should have received callbacks for sequence changes
  EXPECT_GT(callback_count, 0);
}

// Producer-consumer handshaking cycle:
// 1. read_index == Unavailable -> Producer is reset -> write_index == Unavailable, next_write = BufferA
// 2. Keep writing to BufferA -> sequence_and_writing increases
// 3. read_index = BufferA -> write_index = BufferA, last_written_buffer = BufferA -> next_write = BufferB
// 4. Keep writing to BufferB -> sequence_and_writing increases
// 5. read_index = BufferB -> write_index = BufferB, last_written_buffer = BufferB -> next_write = BufferA,
// sequence_and_writing increases
// 6. read_index = BufferA -> write_index = BufferA, last_written_buffer = BufferA -> next_write = BufferB,
// sequence_and_writing increases
// 7. read_index = BufferB -> write_index = BufferB, last_written_buffer = BufferB -> next_write = BufferA,
// sequence_and_writing increases
// 8. read_index == Unavailable -> Producer is reset -> write_index == Unavailable, next_write = BufferA
// 9. Keep writing to BufferA -> sequence_and_writing increases
TEST_F(SharedMemoryMonitorTest, RunWithCallbackFastProducerFromInit) {
  // 1. read_index == Unavailable -> Producer is reset -> write_index == Unavailable, next_write = BufferA
  // 2. Keep writing to BufferA -> sequence_and_writing increases
  // 3. read_index = BufferA -> write_index = BufferA, last_written_buffer = BufferA -> next_write = BufferB
  auto shared_memory = SharedMemory<test::ArrayBuffer, 2, LockFreeSharedMemoryBuffer>::Create(
      shared_memory_name_, InitializeBuffer<test::ArrayBuffer>);
  ASSERT_TRUE(shared_memory.IsValid());

  Producer<test::ArrayBuffer, 2, LockFreeSharedMemoryBuffer> producer{shared_memory_name_};
  ASSERT_TRUE(producer.IsValid());

  Monitor<test::ArrayBuffer> monitor{shared_memory_name_};
  ASSERT_TRUE(monitor.IsValid());

  std::memcpy(shared_memory.Get()->buffers[0].data(), "A invalid\0", 10);
  std::memcpy(shared_memory.Get()->buffers[1].data(), "B invalid\0", 10);

  std::regex format_regex(R"(buffer: \d+, frame: \d+)");  // NOLINT(bugprone-unused-local-non-trivial-variable)  //
                                                          // Regex pattern for the expected format

  const uint max_iterations = 5;

  std::atomic<bool> producer_started{false};
  std::atomic<uint64_t> producer_sequence{0};

  std::thread producer_thread([&producer, &producer_started]() {
    producer_started.store(true, std::memory_order_release);
    producer.Run([&](test::ArrayBuffer& buffer, uint64_t frame_count, int buffer_id) {
      const std::string content = "buffer: " + std::to_string(buffer_id) + ", frame: " + std::to_string(frame_count);
      std::memcpy(buffer.data(), content.c_str(), content.size() + 1);
      std::this_thread::yield();
    });
  });

  std::thread monitor_thread([&monitor, &format_regex, &producer_sequence]() {
    uint64_t last_sequence_number = 0;
    monitor.Run(
        [&](const test::ArrayBuffer& buffer, uint64_t sequence_number) {
          if (sequence_number == 0) {
            return;
          }
          EXPECT_GT(sequence_number, last_sequence_number);
          EXPECT_TRUE(std::regex_match(buffer.data(), format_regex))
              << "Buffer content does not match the expected format.";
          last_sequence_number = sequence_number;
          producer_sequence.store(sequence_number, std::memory_order_relaxed);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        },
        0.1, max_iterations);
  });

  while (!producer_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  monitor_thread.join();
  StreamingControl::Instance().Stop();
  producer_thread.join();

  // Check if the content matches the expected format
  const std::string buffer_a_content = std::string(shared_memory.Get()->buffers[0].data());
  const std::string buffer_b_content = std::string(shared_memory.Get()->buffers[1].data());
  EXPECT_TRUE(std::regex_match(buffer_a_content, format_regex))
      << "Buffer A content does not match the expected format.";  // NOLINT(readability-implicit-bool-conversion)
  EXPECT_EQ(buffer_b_content, "B invalid")
      << "Buffer B content does not match the expected format.";  // NOLINT(readability-implicit-bool-conversion)
  EXPECT_GE(producer_sequence.load(std::memory_order_relaxed), max_iterations);
}

TEST_F(SharedMemoryMonitorTest, RunStopsWhenStreamingControlStopped) {
  // Create shared memory
  auto shared_memory = SharedMemory<test::ArrayBuffer, 2, LockFreeSharedMemoryBuffer>::Create(
      shared_memory_name_, InitializeBuffer<test::ArrayBuffer>);
  ASSERT_TRUE(shared_memory.IsValid());

  // Set up initial state
  shared_memory.Get()->last_written_buffer.store(detail::ToInt(detail::BufferState::BufferA),
                                                 std::memory_order_release);

  // Create a monitor
  Monitor<test::ArrayBuffer> monitor{shared_memory_name_};
  ASSERT_TRUE(monitor.IsValid());

  // Track callback invocations
  std::atomic<int> callback_count = 0;

  // Start a thread that will stop streaming control
  std::thread stop_streaming([&]() {
    while (callback_count.load(std::memory_order_relaxed) == 0) std::this_thread::yield();
    StreamingControl::Instance().Stop();
  });

  // Run the monitor with a callback
  monitor.Run(
      [&](const test::ArrayBuffer& /*buffer*/, uint64_t) { callback_count.fetch_add(1, std::memory_order_relaxed); },
      0.1, 1000);

  stop_streaming.join();

  // After the first call of monitor_fn, Run() would just keep timing out because the data hasn't changed
  EXPECT_EQ(callback_count, 1);
}

TEST_F(SharedMemoryMonitorTest, RunCallsCallbackImmediatelyOnFirstFrame) {
  // Create shared memory
  auto shared_memory = SharedMemory<test::ArrayBuffer, 2, LockFreeSharedMemoryBuffer>::Create(
      shared_memory_name_, InitializeBuffer<test::ArrayBuffer>);
  ASSERT_TRUE(shared_memory.IsValid());

  // Set up initial state
  shared_memory.Get()->last_written_buffer.store(detail::ToInt(detail::BufferState::BufferA),
                                                 std::memory_order_release);

  // Write test data
  const std::string message = "Initial Data";
  std::memcpy(shared_memory.Get()->buffers[0].data(), message.data(), message.size() + 1);

  // Set initial sequence number
  shared_memory.Get()->sequence_and_writing.store(42, std::memory_order_release);

  // Create a monitor
  Monitor<test::ArrayBuffer> monitor{shared_memory_name_};
  ASSERT_TRUE(monitor.IsValid());

  // Track callback invocations
  std::atomic<bool> callback_called{false};
  uint64_t received_sequence = 0;

  // Start a thread that will stop streaming control after a short delay
  std::thread stop_streaming([&]() {
    // Wait for callback to be called
    while (!callback_called.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    // Stop after a short delay to ensure Run() exits
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    StreamingControl::Instance().Stop();
  });

  // Run the monitor with a callback - should call immediately due to first_frame
  monitor.Run(
      [&](const test::ArrayBuffer& buffer, uint64_t sequence) {
        callback_called.store(true, std::memory_order_release);
        received_sequence = sequence;
        EXPECT_EQ(std::string(buffer.data()), message);
      },
      0.1, 5);

  stop_streaming.join();

  // Verify callback was called immediately with the initial sequence
  EXPECT_TRUE(callback_called.load());
  EXPECT_EQ(received_sequence, 42);
}

}  // namespace dex::shared_memory
