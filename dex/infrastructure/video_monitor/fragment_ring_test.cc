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

}  // namespace
}  // namespace dex::video_monitor
