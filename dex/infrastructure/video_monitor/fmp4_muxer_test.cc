#include "dex/infrastructure/video_monitor/fmp4_muxer.h"

#include <cstdint>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace dex::video_monitor {
namespace {

// Minimal SPS/PPS for Baseline profile, 320x240.
const std::vector<uint8_t> kTestSPS = {0x67, 0x42, 0x00, 0x0A, 0xF8, 0x41, 0xA2};
const std::vector<uint8_t> kTestPPS = {0x68, 0xCE, 0x38, 0x80};

// Read a big-endian uint32 from a buffer.
uint32_t ReadU32BE(const std::vector<uint8_t>& buf, size_t offset) {
  return (static_cast<uint32_t>(buf[offset]) << 24) | (static_cast<uint32_t>(buf[offset + 1]) << 16) |
         (static_cast<uint32_t>(buf[offset + 2]) << 8) | static_cast<uint32_t>(buf[offset + 3]);
}

// Read a 4CC type string from a buffer.
std::string ReadType(const std::vector<uint8_t>& buf, size_t offset) {
  return std::string(reinterpret_cast<const char*>(buf.data() + offset), 4);
}

// Find a box of the given type starting at `start` within `end`.
// Returns the offset of the box, or end if not found.
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

TEST(FMP4MuxerTest, InitSegmentStartsWithFtyp) {
  FMP4Muxer::TrackParams params{.width = 320, .height = 240, .timescale = 90000, .sps = kTestSPS, .pps = kTestPPS};
  FMP4Muxer muxer(params);

  auto init = muxer.GetInitSegment();
  ASSERT_GE(init.size(), 8u);
  EXPECT_EQ(ReadType(init, 4), "ftyp");
}

TEST(FMP4MuxerTest, InitSegmentContainsMoov) {
  FMP4Muxer::TrackParams params{.width = 320, .height = 240, .timescale = 90000, .sps = kTestSPS, .pps = kTestPPS};
  FMP4Muxer muxer(params);

  auto init = muxer.GetInitSegment();

  // Find moov box after ftyp.
  uint32_t ftyp_size = ReadU32BE(init, 0);
  size_t moov_pos = FindBox(init, "moov", ftyp_size, init.size());
  EXPECT_LT(moov_pos, init.size()) << "moov box not found in init segment";
}

TEST(FMP4MuxerTest, InitSegmentContainsMvex) {
  FMP4Muxer::TrackParams params{.width = 320, .height = 240, .timescale = 90000, .sps = kTestSPS, .pps = kTestPPS};
  FMP4Muxer muxer(params);

  auto init = muxer.GetInitSegment();

  // Find moov then mvex inside it.
  uint32_t ftyp_size = ReadU32BE(init, 0);
  size_t moov_pos = FindBox(init, "moov", ftyp_size, init.size());
  ASSERT_LT(moov_pos, init.size());

  uint32_t moov_size = ReadU32BE(init, moov_pos);
  size_t mvex_pos = FindBox(init, "mvex", moov_pos + 8, moov_pos + moov_size);
  EXPECT_LT(mvex_pos, moov_pos + moov_size) << "mvex box not found in moov";
}

TEST(FMP4MuxerTest, FragmentContainsMoofAndMdat) {
  FMP4Muxer::TrackParams params{.width = 320, .height = 240, .timescale = 90000, .sps = kTestSPS, .pps = kTestPPS};
  FMP4Muxer muxer(params);

  // Fake Annex B NAL: start code + some data.
  std::vector<uint8_t> annex_b = {0x00, 0x00, 0x00, 0x01, 0x65, 0xAA, 0xBB, 0xCC};

  auto fragment = muxer.MuxFragment(annex_b, 0, 3000, true);
  ASSERT_GE(fragment.size(), 16u);

  // First box should be moof.
  EXPECT_EQ(ReadType(fragment, 4), "moof");

  // After moof should be mdat.
  uint32_t moof_size = ReadU32BE(fragment, 0);
  size_t mdat_pos = FindBox(fragment, "mdat", moof_size, fragment.size());
  EXPECT_LT(mdat_pos, fragment.size()) << "mdat box not found after moof";
}

TEST(FMP4MuxerTest, SequenceNumberIncrements) {
  FMP4Muxer::TrackParams params{.width = 320, .height = 240, .timescale = 90000, .sps = kTestSPS, .pps = kTestPPS};
  FMP4Muxer muxer(params);

  std::vector<uint8_t> annex_b = {0x00, 0x00, 0x00, 0x01, 0x65, 0xAA};

  auto frag1 = muxer.MuxFragment(annex_b, 0, 3000, true);
  auto frag2 = muxer.MuxFragment(annex_b, 3000, 3000, false);

  // mfhd is inside moof. moof header (8) + mfhd header (12) + sequence_number (4).
  // mfhd starts at offset 8 in moof: size(4) + "mfhd"(4) + version+flags(4) = 16 bytes from moof start.
  // sequence_number is at moof_start + 8 + 8 + 4 = moof_start + 20.
  uint32_t seq1 = ReadU32BE(frag1, 20);
  uint32_t seq2 = ReadU32BE(frag2, 20);
  EXPECT_EQ(seq1, 1u);
  EXPECT_EQ(seq2, 2u);
}

TEST(ExtractSPSPPSTest, ExtractsFromAnnexB) {
  // Construct Annex B with SPS (type 7) and PPS (type 8).
  std::vector<uint8_t> annex_b;
  // SPS: 00 00 00 01 [67 42 00 0A F8 41 A2]
  annex_b.insert(annex_b.end(), {0x00, 0x00, 0x00, 0x01});
  annex_b.insert(annex_b.end(), kTestSPS.begin(), kTestSPS.end());
  // PPS: 00 00 00 01 [68 CE 38 80]
  annex_b.insert(annex_b.end(), {0x00, 0x00, 0x00, 0x01});
  annex_b.insert(annex_b.end(), kTestPPS.begin(), kTestPPS.end());

  std::vector<uint8_t> sps, pps;
  EXPECT_TRUE(ExtractSPSPPS(annex_b, sps, pps));
  EXPECT_EQ(sps, kTestSPS);
  EXPECT_EQ(pps, kTestPPS);
}

TEST(AnnexBToLengthPrefixedTest, ConvertsCorrectly) {
  std::vector<uint8_t> annex_b = {0x00, 0x00, 0x00, 0x01, 0x65, 0xAA, 0xBB};
  auto result = AnnexBToLengthPrefixed(annex_b);

  // Should be: 4-byte length (00 00 00 03) + payload (65 AA BB).
  ASSERT_EQ(result.size(), 7u);
  EXPECT_EQ(ReadU32BE(result, 0), 3u);
  EXPECT_EQ(result[4], 0x65);
  EXPECT_EQ(result[5], 0xAA);
  EXPECT_EQ(result[6], 0xBB);
}

TEST(AnnexBToLengthPrefixedTest, HandlesMultipleNALs) {
  std::vector<uint8_t> annex_b = {0x00, 0x00, 0x00, 0x01, 0x67, 0xAA, 0x00, 0x00, 0x01, 0x68, 0xBB, 0xCC};
  auto result = AnnexBToLengthPrefixed(annex_b);

  // First NAL: length 2 (67 AA), second NAL: length 2 (68 BB CC) — wait.
  // 67 AA is followed by 00 00 01 so NAL is [67 AA], length=2.
  // 68 BB CC is the rest, length=3.
  ASSERT_EQ(result.size(), 4 + 2 + 4 + 3u);  // Two length-prefixed NALs.
  EXPECT_EQ(ReadU32BE(result, 0), 2u);
  EXPECT_EQ(result[4], 0x67);
  EXPECT_EQ(ReadU32BE(result, 6), 3u);
  EXPECT_EQ(result[10], 0x68);
}

}  // namespace
}  // namespace dex::video_monitor
