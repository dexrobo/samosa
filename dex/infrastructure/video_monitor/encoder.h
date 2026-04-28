#ifndef DEX_INFRASTRUCTURE_VIDEO_MONITOR_ENCODER_H
#define DEX_INFRASTRUCTURE_VIDEO_MONITOR_ENCODER_H

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

// Forward-declare OpenH264 type to keep the header clean.
class ISVCEncoder;

namespace dex::video_monitor {

class H264Encoder {
 public:
  struct Params {
    uint32_t width{};
    uint32_t height{};
    uint32_t fps{30};                // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    uint32_t bitrate_kbps{2000};     // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    uint32_t keyframe_interval{60};  // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
  };

  struct EncodedFrame {
    std::vector<uint8_t> nals;  // Annex B NAL units (with start codes).
    bool is_idr{false};
    uint64_t pts{0};
  };

  explicit H264Encoder(const Params& params);
  ~H264Encoder();

  H264Encoder(const H264Encoder&) = delete;
  H264Encoder& operator=(const H264Encoder&) = delete;
  H264Encoder(H264Encoder&& other) noexcept;
  H264Encoder& operator=(H264Encoder&& other) noexcept;

  /// Encode a single I420 frame.
  /// @param yuv_i420 Pointer to I420 planar data (Y: w*h, U: w/2*h/2, V: w/2*h/2).
  /// @param timestamp_us Presentation timestamp in microseconds.
  /// @return Encoded frame, or nullopt if the encoder dropped the frame.
  std::optional<EncodedFrame> Encode(const uint8_t* yuv_i420, uint64_t timestamp_us);

  /// Returns true if the encoder was initialized successfully.
  [[nodiscard]] bool IsValid() const { return encoder_ != nullptr; }

 private:
  ISVCEncoder* encoder_{nullptr};
  Params params_;
};

}  // namespace dex::video_monitor

#endif  // DEX_INFRASTRUCTURE_VIDEO_MONITOR_ENCODER_H
