#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "dex/infrastructure/video_monitor/encoder.h"
#include "dex/infrastructure/video_monitor/fmp4_muxer.h"

namespace dex::video_monitor {
namespace {

std::vector<uint8_t> MakeSyntheticI420(uint32_t width, uint32_t height, uint8_t luma = 128) {
  const size_t y_size = width * height;
  const size_t uv_size = (width / 2) * (height / 2);
  std::vector<uint8_t> frame(y_size + 2 * uv_size);
  std::memset(frame.data(), luma, y_size);
  std::memset(frame.data() + y_size, 128, uv_size);
  std::memset(frame.data() + y_size + uv_size, 128, uv_size);
  return frame;
}

uint32_t ReadU32BE(const std::vector<uint8_t>& buf, size_t offset) {
  return (static_cast<uint32_t>(buf[offset]) << 24) | (static_cast<uint32_t>(buf[offset + 1]) << 16) |
         (static_cast<uint32_t>(buf[offset + 2]) << 8) | static_cast<uint32_t>(buf[offset + 3]);
}

std::string ReadType(const std::vector<uint8_t>& buf, size_t offset) {
  return std::string(reinterpret_cast<const char*>(buf.data() + offset), 4);
}

size_t FindBox(const std::vector<uint8_t>& buf, const std::string& type, size_t start, size_t end) {
  size_t pos = start;
  while (pos + 8 <= end) {
    uint32_t box_size = ReadU32BE(buf, pos);
    if (box_size < 8) break;
    if (ReadType(buf, pos + 4) == type) return pos;
    pos += box_size;
  }
  return end;
}

// Encode N frames, extract SPS/PPS from first IDR, create muxer, produce full fMP4 output.
TEST(EncoderMuxerIntegration, ProducesValidFMP4Stream) {
  constexpr uint32_t kWidth = 320;
  constexpr uint32_t kHeight = 240;
  constexpr uint32_t kFps = 30;
  constexpr uint32_t kTimescale = 90000;
  constexpr int kNumFrames = 10;

  H264Encoder::Params enc_params{
      .width = kWidth, .height = kHeight, .fps = kFps, .bitrate_kbps = 500, .keyframe_interval = 30};
  H264Encoder encoder(enc_params);
  ASSERT_TRUE(encoder.IsValid());

  std::unique_ptr<FMP4Muxer> muxer;
  std::vector<uint8_t> init_segment;
  std::vector<std::vector<uint8_t>> fragments;

  for (int i = 0; i < kNumFrames; ++i) {
    // Vary luma slightly per frame so encoder produces non-trivial output.
    auto frame = MakeSyntheticI420(kWidth, kHeight, static_cast<uint8_t>(100 + i * 5));
    uint64_t timestamp_us = static_cast<uint64_t>(i) * 1000000 / kFps;

    auto encoded = encoder.Encode(frame.data(), timestamp_us);
    if (!encoded.has_value()) continue;

    // On first IDR, create the muxer.
    if (!muxer && encoded->is_idr) {
      std::vector<uint8_t> sps, pps;
      ASSERT_TRUE(ExtractSPSPPS(encoded->nals, sps, pps)) << "Failed to extract SPS/PPS from first IDR";
      EXPECT_FALSE(sps.empty());
      EXPECT_FALSE(pps.empty());

      FMP4Muxer::TrackParams track_params{
          .width = kWidth, .height = kHeight, .timescale = kTimescale, .sps = std::move(sps), .pps = std::move(pps)};
      muxer = std::make_unique<FMP4Muxer>(track_params);
      init_segment = muxer->GetInitSegment();
    }

    if (!muxer) continue;

    uint64_t decode_time = timestamp_us * kTimescale / 1000000;
    uint32_t duration = kTimescale / kFps;
    auto fragment = muxer->MuxFragment(encoded->nals, decode_time, duration, encoded->is_idr);
    fragments.push_back(std::move(fragment));
  }

  // Validate init segment.
  ASSERT_FALSE(init_segment.empty());
  EXPECT_EQ(ReadType(init_segment, 4), "ftyp");

  uint32_t ftyp_size = ReadU32BE(init_segment, 0);
  size_t moov_pos = FindBox(init_segment, "moov", ftyp_size, init_segment.size());
  EXPECT_LT(moov_pos, init_segment.size()) << "moov box not found";

  // Validate mvex exists (signals fragmented MP4).
  uint32_t moov_size = ReadU32BE(init_segment, moov_pos);
  size_t mvex_pos = FindBox(init_segment, "mvex", moov_pos + 8, moov_pos + moov_size);
  EXPECT_LT(mvex_pos, moov_pos + moov_size) << "mvex box not found";

  // Validate we got multiple fragments.
  ASSERT_GE(fragments.size(), 5u) << "Expected at least 5 fragments from 10 frames";

  // Validate each fragment has moof + mdat.
  for (size_t i = 0; i < fragments.size(); ++i) {
    const auto& frag = fragments[i];
    ASSERT_GE(frag.size(), 16u) << "Fragment " << i << " too small";
    EXPECT_EQ(ReadType(frag, 4), "moof") << "Fragment " << i << " doesn't start with moof";

    uint32_t moof_size = ReadU32BE(frag, 0);
    size_t mdat_pos = FindBox(frag, "mdat", moof_size, frag.size());
    EXPECT_LT(mdat_pos, frag.size()) << "mdat not found in fragment " << i;
  }

  // Validate the full concatenated stream is parseable as a sequence of boxes.
  std::vector<uint8_t> full_stream;
  full_stream.insert(full_stream.end(), init_segment.begin(), init_segment.end());
  for (const auto& frag : fragments) {
    full_stream.insert(full_stream.end(), frag.begin(), frag.end());
  }

  // Walk through all boxes and verify they don't overlap or have zero sizes.
  size_t pos = 0;
  int box_count = 0;
  while (pos + 8 <= full_stream.size()) {
    uint32_t size = ReadU32BE(full_stream, pos);
    ASSERT_GE(size, 8u) << "Invalid box size at offset " << pos;
    ASSERT_LE(pos + size, full_stream.size()) << "Box overflows stream at offset " << pos;
    pos += size;
    ++box_count;
  }
  EXPECT_EQ(pos, full_stream.size()) << "Trailing bytes after last box";
  EXPECT_GE(box_count, 12) << "Expected ftyp + moov + N*(moof+mdat), got " << box_count << " boxes";
}

}  // namespace
}  // namespace dex::video_monitor
