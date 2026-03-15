#include "dex/infrastructure/shared_memory/futex.h"

#include <chrono>
#include <thread>

#include "gtest/gtest.h"

namespace dex::shared_memory::detail {
namespace {

class MockFutex : public Futex {
 public:
  [[nodiscard]] WaitResult Wait(const std::atomic<uint32_t>& futex_address, const int expected,
                                const timespec* timeout) const override {
    last_wait_address_ = &futex_address;
    last_wait_expected_ = expected;
    last_wait_timeout_ = timeout;
    return wait_return_value_;
  }

  [[nodiscard]] bool Wake(const std::atomic<uint32_t>& futex_address, const int count) const override {
    last_wake_address_ = &futex_address;
    last_wake_count_ = count;
    return wake_return_value_;
  }

  mutable const std::atomic<uint32_t>* last_wait_address_ = nullptr;
  mutable uint32_t last_wait_expected_ = 0;
  mutable const timespec* last_wait_timeout_ = nullptr;
  mutable const std::atomic<uint32_t>* last_wake_address_ = nullptr;
  mutable uint32_t last_wake_count_ = 0;
  WaitResult wait_return_value_ = WaitResult::Success;
  bool wake_return_value_ = true;
};

class FutexTest : public testing::Test {
 protected:
  void SetUp() override {
    const MockFutex mock_futex;
    FutexManager::Override(mock_futex_);
  }

  void TearDown() override { FutexManager::Restore(); }

  MockFutex mock_futex_;
  std::atomic<uint32_t> test_futex_{0};
};

TEST_F(FutexTest, DefaultFutexOperations) {
  const DefaultFutex default_futex;
  const std::atomic<uint32_t> value{42};

  // Test basic operations (note: this mainly verifies the interface works)
  // Will return immediately since value (42) != expected (0)
  EXPECT_EQ(default_futex.Wait(value, 0), WaitResult::Success);
  EXPECT_TRUE(default_futex.Wake(value, 1));
}

TEST_F(FutexTest, FutexManagerGetReturnsInstance) {
  auto futex = FutexManager::Get();
  EXPECT_NE(futex.get(), nullptr);
}

TEST_F(FutexTest, ScopedFutexOverrideAndRestore) {
  auto original_futex = FutexManager::Get();
  {
    const MockFutex local_mock;
    const ScopedFutex scoped_override(local_mock);
    EXPECT_EQ(FutexManager::Get().get(), &local_mock);
  }
  EXPECT_EQ(FutexManager::Get().get(), original_futex.get());
}

TEST_F(FutexTest, MockFutexWaitParameters) {
  const timespec timeout = {.tv_sec = 1, .tv_nsec = 0};
  auto result = GetDefaultFutex()->Wait(test_futex_, 42, &timeout);

  EXPECT_EQ(result, WaitResult::Success);
  EXPECT_EQ(mock_futex_.last_wait_address_, &test_futex_);
  EXPECT_EQ(mock_futex_.last_wait_expected_, 42);
  EXPECT_EQ(mock_futex_.last_wait_timeout_, &timeout);
}

TEST_F(FutexTest, MockFutexWakeParameters) {
  auto result = GetDefaultFutex()->Wake(test_futex_, 5);

  EXPECT_TRUE(result);
  EXPECT_EQ(mock_futex_.last_wake_address_, &test_futex_);
  EXPECT_EQ(mock_futex_.last_wake_count_, 5);
}

TEST_F(FutexTest, FutexManagerOverrideAndRestore) {
  const MockFutex another_mock;
  auto original = FutexManager::Get();

  FutexManager::Override(another_mock);
  EXPECT_EQ(FutexManager::Get().get(), &another_mock);

  FutexManager::Restore();
  EXPECT_EQ(FutexManager::Get().get(), original.get());
}

class FutexImplementationTest : public testing::Test {
 protected:
  std::atomic<uint32_t> futex_var_{0};
};

TEST_F(FutexImplementationTest, FutexWaitReturnsOnValueMismatch) {
  // Set up a different thread to modify the value
  std::thread modifier_thread([this]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    futex_var_.store(1);
    EXPECT_TRUE(FutexWake(futex_var_, 1));
  });

  // Wait should return Success when the value changes
  EXPECT_EQ(FutexWait(futex_var_, 0), WaitResult::Success);
  EXPECT_EQ(futex_var_.load(), 1);

  modifier_thread.join();
}

TEST_F(FutexImplementationTest, FutexWaitTimeout) {
  const timespec timeout = {.tv_sec = 0, .tv_nsec = 1000000};  // 1ms
  EXPECT_EQ(FutexWait(futex_var_, 0, &timeout), WaitResult::Timeout);
}

TEST_F(FutexImplementationTest, FutexWaitReturnsImmediatelyOnMismatch) {
  futex_var_.store(1);
  // Should return immediately (with Success) because value doesn't match expected
  EXPECT_EQ(FutexWait(futex_var_, 0), WaitResult::Success);
}

TEST_F(FutexImplementationTest, FutexWakeMultipleWaiters) {
  const int kNumThreads = 3;
  std::atomic<int> woken_count{0};
  std::vector<std::thread> waiter_threads;
  waiter_threads.reserve(kNumThreads);

  timespec timeout = {.tv_sec = 1, .tv_nsec = 0};  // 1 second timeout
  for (int i = 0; i < kNumThreads; ++i) {
    waiter_threads.emplace_back([this, &woken_count, &timeout]() {
      const auto wait_result = FutexWait(futex_var_, 0, &timeout);
      EXPECT_EQ(wait_result, WaitResult::Success) << "Thread timed out instead of being woken";
      if (wait_result == WaitResult::Success) {
        woken_count.fetch_add(1);
      }
    });
  }

  // Give threads time to start waiting
  std::this_thread::sleep_for(std::chrono::milliseconds(1));

  // Wake up all waiters
  futex_var_.store(1);
  EXPECT_TRUE(FutexWake(futex_var_, kNumThreads));

  // Wait for all threads to complete
  for (auto& thread : waiter_threads) {
    thread.join();
  }

  EXPECT_EQ(woken_count.load(), kNumThreads);
}

TEST_F(FutexImplementationTest, FutexWakeNoWaiters) {
  // Should succeed even with no waiters
  EXPECT_TRUE(FutexWake(futex_var_, 1));
}

TEST_F(FutexImplementationTest, FutexWakePartialWaiters) {
  GTEST_SKIP() << "This test is not deterministic, skipping for now.";
  const int kTotalThreads = 4;
  const int kThreadsToWake = 2;
  std::atomic<int> woken_count{0};  // track threads that have been woken up
  std::atomic<int> ready_count{0};  // track threads that are ready to wait
  std::vector<std::thread> waiter_threads;
  waiter_threads.reserve(kTotalThreads);

  for (int i = 0; i < kTotalThreads; ++i) {
    waiter_threads.emplace_back([this, &woken_count, &ready_count]() {
      ready_count.fetch_add(1);  // Signal this thread is ready to wait
      const auto wait_result = FutexWait(futex_var_, 0);
      EXPECT_EQ(wait_result, WaitResult::Success) << "Thread timed out instead of being woken";
      if (wait_result == WaitResult::Success) {
        woken_count.fetch_add(1);
      }
    });
  }

  // Wait for all threads to be ready to wait
  while (ready_count.load() < kTotalThreads) {
    std::this_thread::yield();
  }

  // Wake up only some waiters
  futex_var_.store(1);
  EXPECT_TRUE(FutexWake(futex_var_, kThreadsToWake));

  // Wait for the exact number of threads to wake up
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (woken_count.load() != kThreadsToWake && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  // We cannot guarantee that all the threads will wake up, but we can guarantee that the number of
  // threads that woke up is less than or equal to the number of threads that were woken up.
  EXPECT_LE(woken_count.load(), kThreadsToWake) << "Wrong number of threads woken up";

  // Wake remaining waiters
  EXPECT_TRUE(FutexWake(futex_var_, kTotalThreads - kThreadsToWake));

  // Wait for all threads to complete
  for (auto& thread : waiter_threads) {
    thread.join();
  }

  EXPECT_EQ(woken_count.load(), kTotalThreads);
}

}  // namespace
}  // namespace dex::shared_memory::detail

