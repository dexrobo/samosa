#include "dex/infrastructure/video_monitor/fragment_ring.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace dex::video_monitor {
namespace {

Fragment MakeFragment(bool is_idr = false) {
  auto data = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0xDE, 0xAD});
  return Fragment{.data = std::move(data), .timestamp_us = 0, .contains_idr = is_idr};
}

TEST(FragmentRingTest, EmptyRingReturnsNothing) {
  FragmentRing ring(10);
  auto result = ring.ReadFrom(0);
  EXPECT_TRUE(result.fragments.empty());
  EXPECT_FALSE(result.init_segment.has_value());
}

TEST(FragmentRingTest, PushAndRead) {
  FragmentRing ring(10);
  ring.Push(MakeFragment(true));

  auto result = ring.ReadFrom(0);
  ASSERT_EQ(result.fragments.size(), 1u);
  EXPECT_EQ(result.last_sequence, 1u);
}

TEST(FragmentRingTest, InitSegmentIncludedOnFirstRead) {
  FragmentRing ring(10);
  ring.SetInitSegment({0x01, 0x02, 0x03});
  ring.Push(MakeFragment(true));

  auto result = ring.ReadFrom(0);
  ASSERT_TRUE(result.init_segment.has_value());
  EXPECT_EQ(result.init_segment->size(), 3u);
}

TEST(FragmentRingTest, InitSegmentNotIncludedOnSubsequentRead) {
  FragmentRing ring(10);
  ring.SetInitSegment({0x01, 0x02, 0x03});
  ring.Push(MakeFragment(true));

  auto result = ring.ReadFrom(1);
  EXPECT_FALSE(result.init_segment.has_value());
}

TEST(FragmentRingTest, ReadFromReturnsOnlyNewFragments) {
  FragmentRing ring(10);
  ring.Push(MakeFragment(true));
  ring.Push(MakeFragment());
  ring.Push(MakeFragment());

  auto result = ring.ReadFrom(1);
  EXPECT_EQ(result.fragments.size(), 2u);
  EXPECT_EQ(result.last_sequence, 3u);
}

TEST(FragmentRingTest, SlowReaderSkipsToIDR) {
  FragmentRing ring(4);  // Small ring.

  // Fill the ring past capacity.
  ring.Push(MakeFragment(true));  // seq 1 (IDR)
  ring.Push(MakeFragment());      // seq 2
  ring.Push(MakeFragment());      // seq 3
  ring.Push(MakeFragment(true));  // seq 4 (IDR)
  ring.Push(MakeFragment());      // seq 5 — overwrites seq 1

  // Reader asking for seq 1 is too old. Should skip to latest IDR (seq 4).
  auto result = ring.ReadFrom(0);
  EXPECT_GE(result.fragments.size(), 1u);
  EXPECT_GE(result.last_sequence, 4u);
}

TEST(FragmentRingTest, WaitForNewBlocksUntilData) {
  FragmentRing ring(10);

  std::thread writer([&ring] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ring.Push(MakeFragment(true));
  });

  bool got_data = ring.WaitForNew(0, std::chrono::milliseconds(2000));
  EXPECT_TRUE(got_data);

  writer.join();
}

TEST(FragmentRingTest, WaitForNewTimesOut) {
  FragmentRing ring(10);
  bool got_data = ring.WaitForNew(0, std::chrono::milliseconds(10));
  EXPECT_FALSE(got_data);
}

TEST(FragmentRingTest, NotifyAllWakesWaiters) {
  FragmentRing ring(10);

  std::thread waiter([&ring] { ring.WaitForNew(0, std::chrono::milliseconds(5000)); });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ring.NotifyAll();

  waiter.join();  // Should return promptly.
}

TEST(FragmentRingTest, HeadSequenceTracksCorrectly) {
  FragmentRing ring(10);
  EXPECT_EQ(ring.HeadSequence(), 0u);

  ring.Push(MakeFragment(true));
  EXPECT_EQ(ring.HeadSequence(), 1u);

  ring.Push(MakeFragment());
  ring.Push(MakeFragment());
  EXPECT_EQ(ring.HeadSequence(), 3u);
}

TEST(FragmentRingTest, MultipleReadersIndependent) {
  FragmentRing ring(10);
  ring.SetInitSegment({0xAA});
  ring.Push(MakeFragment(true));
  ring.Push(MakeFragment());
  ring.Push(MakeFragment());

  // Reader A reads from start.
  auto result_a = ring.ReadFrom(0);
  EXPECT_TRUE(result_a.init_segment.has_value());
  EXPECT_GE(result_a.fragments.size(), 1u);

  // Reader B reads from sequence 2.
  auto result_b = ring.ReadFrom(2);
  EXPECT_FALSE(result_b.init_segment.has_value());
  EXPECT_EQ(result_b.fragments.size(), 1u);
  EXPECT_EQ(result_b.last_sequence, 3u);
}

// --- Client tracking tests ---

TEST(FragmentRingTest, ClientCountStartsAtZero) {
  FragmentRing ring(10);
  EXPECT_EQ(ring.ClientCount(), 0u);
}

TEST(FragmentRingTest, AddRemoveClientTracksCorrectly) {
  FragmentRing ring(10);
  ring.AddClient();
  EXPECT_EQ(ring.ClientCount(), 1u);
  ring.AddClient();
  EXPECT_EQ(ring.ClientCount(), 2u);
  ring.RemoveClient();
  EXPECT_EQ(ring.ClientCount(), 1u);
  ring.RemoveClient();
  EXPECT_EQ(ring.ClientCount(), 0u);
}

TEST(FragmentRingTest, WaitForClientReturnsTrueWhenClientPresent) {
  FragmentRing ring(10);
  ring.AddClient();
  bool result = ring.WaitForClient(std::chrono::milliseconds(10));
  EXPECT_TRUE(result);
}

TEST(FragmentRingTest, WaitForClientTimesOutWhenNoClient) {
  FragmentRing ring(10);
  bool result = ring.WaitForClient(std::chrono::milliseconds(10));
  EXPECT_FALSE(result);
}

TEST(FragmentRingTest, WaitForClientWakesOnAddClient) {
  FragmentRing ring(10);

  std::thread adder([&ring] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ring.AddClient();
  });

  bool result = ring.WaitForClient(std::chrono::milliseconds(2000));
  EXPECT_TRUE(result);
  EXPECT_EQ(ring.ClientCount(), 1u);

  adder.join();
}

TEST(FragmentRingTest, WaitForClientWakesOnShutdown) {
  FragmentRing ring(10);

  std::thread stopper([&ring] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ring.NotifyAll();
  });

  bool result = ring.WaitForClient(std::chrono::milliseconds(2000));
  // Returns true because shutdown_ is true (predicate satisfied).
  EXPECT_TRUE(result);

  stopper.join();
}

TEST(FragmentRingTest, WaitForClientDoesNotBlockReadFrom) {
  // Verify that WaitForClient releases the mutex so ReadFrom can proceed.
  FragmentRing ring(10);
  ring.SetInitSegment({0xAA});
  ring.Push(MakeFragment(true));

  std::atomic<bool> read_done{false};

  // Start a thread that waits for client.
  std::thread waiter([&ring] { ring.WaitForClient(std::chrono::milliseconds(500)); });

  // Give the waiter time to enter wait_for (which releases the mutex).
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // ReadFrom should succeed — not blocked by the waiter.
  auto result = ring.ReadFrom(0);
  read_done.store(true);
  EXPECT_TRUE(result.init_segment.has_value());

  // Clean up — add a client to wake the waiter.
  ring.AddClient();
  waiter.join();
  EXPECT_TRUE(read_done.load());
}

TEST(FragmentRingTest, IdleToActiveTransition) {
  // Simulate: pipeline idle → client connects → pipeline encodes.
  FragmentRing ring(10);

  // Pipeline checks: no clients, enters wait.
  EXPECT_EQ(ring.ClientCount(), 0u);

  // Client connects in another thread.
  std::thread client([&ring] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ring.AddClient();
  });

  // Pipeline waits for client.
  bool got_client = ring.WaitForClient(std::chrono::milliseconds(2000));
  EXPECT_TRUE(got_client);
  EXPECT_EQ(ring.ClientCount(), 1u);

  // Pipeline encodes and pushes.
  ring.Push(MakeFragment(true));
  EXPECT_EQ(ring.HeadSequence(), 1u);

  // Client disconnects.
  ring.RemoveClient();
  EXPECT_EQ(ring.ClientCount(), 0u);

  client.join();
}

TEST(FragmentRingTest, RapidConnectDisconnect) {
  FragmentRing ring(10);

  // Rapid add/remove cycles.
  for (int i = 0; i < 100; ++i) {
    ring.AddClient();
    EXPECT_EQ(ring.ClientCount(), 1u);
    ring.RemoveClient();
    EXPECT_EQ(ring.ClientCount(), 0u);
  }
}

}  // namespace
}  // namespace dex::video_monitor
