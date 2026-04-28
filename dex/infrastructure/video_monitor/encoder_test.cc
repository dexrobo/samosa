#include "dex/infrastructure/video_monitor/encoder.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "gtest/gtest.h"

namespace dex::video_monitor {
namespace {

// Generate a synthetic I420 frame (solid gray).
std::vector<uint8_t> MakeSyntheticI420(uint32_t width, uint32_t height) {
  const size_t y_size = width * height;
  const size_t uv_size = (width / 2) * (height / 2);
  std::vector<uint8_t> frame(y_size + 2 * uv_size);
  std::memset(frame.data(), 128, y_size);                      // Y = 128 (mid-gray)
  std::memset(frame.data() + y_size, 128, uv_size);            // U = 128
  std::memset(frame.data() + y_size + uv_size, 128, uv_size);  // V = 128
  return frame;
}

TEST(H264EncoderTest, InitializesSuccessfully) {
  H264Encoder::Params params{.width = 320, .height = 240, .fps = 30, .bitrate_kbps = 500, .keyframe_interval = 30};
  H264Encoder encoder(params);
  ASSERT_TRUE(encoder.IsValid());
}

TEST(H264EncoderTest, EncodesFirstFrameAsIDR) {
  H264Encoder::Params params{.width = 320, .height = 240, .fps = 30, .bitrate_kbps = 500, .keyframe_interval = 30};
  H264Encoder encoder(params);
  ASSERT_TRUE(encoder.IsValid());

  auto frame = MakeSyntheticI420(320, 240);
  auto result = encoder.Encode(frame.data(), 0);

  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->is_idr);
  EXPECT_FALSE(result->nals.empty());
  EXPECT_EQ(result->pts, 0u);
}

TEST(H264EncoderTest, ProducesNonIDRAfterFirst) {
  H264Encoder::Params params{.width = 320, .height = 240, .fps = 30, .bitrate_kbps = 500, .keyframe_interval = 30};
  H264Encoder encoder(params);
  ASSERT_TRUE(encoder.IsValid());

  auto frame = MakeSyntheticI420(320, 240);

  // First frame is IDR.
  auto first = encoder.Encode(frame.data(), 0);
  ASSERT_TRUE(first.has_value());
  EXPECT_TRUE(first->is_idr);

  // Second frame should not be IDR.
  auto second = encoder.Encode(frame.data(), 33333);
  ASSERT_TRUE(second.has_value());
  EXPECT_FALSE(second->is_idr);
}

TEST(H264EncoderTest, OutputContainsAnnexBStartCodes) {
  H264Encoder::Params params{.width = 320, .height = 240, .fps = 30, .bitrate_kbps = 500, .keyframe_interval = 30};
  H264Encoder encoder(params);
  ASSERT_TRUE(encoder.IsValid());

  auto frame = MakeSyntheticI420(320, 240);
  auto result = encoder.Encode(frame.data(), 0);
  ASSERT_TRUE(result.has_value());

  // Annex B streams start with 0x00 0x00 0x00 0x01 or 0x00 0x00 0x01.
  ASSERT_GE(result->nals.size(), 4u);
  EXPECT_EQ(result->nals[0], 0x00);
  EXPECT_EQ(result->nals[1], 0x00);
  // Either 3-byte or 4-byte start code.
  bool has_start_code = (result->nals[2] == 0x01) || (result->nals[2] == 0x00 && result->nals[3] == 0x01);
  EXPECT_TRUE(has_start_code);
}

TEST(H264EncoderTest, MoveConstructor) {
  H264Encoder::Params params{.width = 320, .height = 240, .fps = 30, .bitrate_kbps = 500, .keyframe_interval = 30};
  H264Encoder encoder1(params);
  ASSERT_TRUE(encoder1.IsValid());

  H264Encoder encoder2(std::move(encoder1));
  EXPECT_TRUE(encoder2.IsValid());
  // NOLINTNEXTLINE(bugprone-use-after-move)
  EXPECT_FALSE(encoder1.IsValid());

  auto frame = MakeSyntheticI420(320, 240);
  auto result = encoder2.Encode(frame.data(), 0);
  ASSERT_TRUE(result.has_value());
}

}  // namespace
}  // namespace dex::video_monitor
