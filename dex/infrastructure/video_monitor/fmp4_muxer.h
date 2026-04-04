#ifndef DEX_INFRASTRUCTURE_VIDEO_MONITOR_FMP4_MUXER_H
#define DEX_INFRASTRUCTURE_VIDEO_MONITOR_FMP4_MUXER_H

#include <cstdint>
#include <vector>

namespace dex::video_monitor {

/// Minimal fragmented MP4 (ISO BMFF) muxer for H.264 video.
///
/// Produces CMAF-compatible output:
///   - Init segment: ftyp + moov (with avcC containing SPS/PPS)
///   - Media segments: moof + mdat (one fragment per call)
///
/// The output is playable in browsers via Media Source Extensions (MSE)
/// or directly with a `<video>` tag when served with Content-Type: video/mp4.
class FMP4Muxer {
 public:
  struct TrackParams {
    uint32_t width{};
    uint32_t height{};
    uint32_t timescale{90000};  // 90kHz is standard for H.264 in MPEG-TS/MP4.
    std::vector<uint8_t> sps;   // Sequence Parameter Set (raw NALU, no start code).
    std::vector<uint8_t> pps;   // Picture Parameter Set (raw NALU, no start code).
  };

  explicit FMP4Muxer(const TrackParams& params);

  /// Generate the init segment (ftyp + moov). Send once per client connection.
  [[nodiscard]] std::vector<uint8_t> GetInitSegment() const;

  /// Mux one frame's encoded NALs into a media segment (moof + mdat).
  /// @param annex_b_nals Annex B formatted NALs (with 00 00 00 01 start codes).
  /// @param decode_time  Decode timestamp in timescale units.
  /// @param duration     Frame duration in timescale units.
  /// @param is_idr       Whether this fragment contains an IDR (keyframe).
  /// @return fMP4 media segment bytes (moof + mdat).
  std::vector<uint8_t> MuxFragment(const std::vector<uint8_t>& annex_b_nals, uint64_t decode_time, uint32_t duration,
                                   bool is_idr);

 private:
  TrackParams params_;
  uint32_t sequence_number_{0};
};

/// Extract SPS and PPS NALUs from an Annex B bitstream (typically from the first IDR).
/// Returns true if both SPS and PPS were found.
bool ExtractSPSPPS(const std::vector<uint8_t>& annex_b, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps);

/// Convert Annex B NALs (start code delimited) to length-prefixed NALs (ISO BMFF format).
/// Uses 4-byte big-endian length prefixes.
std::vector<uint8_t> AnnexBToLengthPrefixed(const std::vector<uint8_t>& annex_b);

}  // namespace dex::video_monitor

#endif  // DEX_INFRASTRUCTURE_VIDEO_MONITOR_FMP4_MUXER_H
