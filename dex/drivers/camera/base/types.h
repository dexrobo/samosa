#ifndef DEX_DRIVERS_CAMERA_BASE_TYPES_H
#define DEX_DRIVERS_CAMERA_BASE_TYPES_H

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "gsl/gsl"

namespace dex::camera {

constexpr std::size_t kNameLength = 64;
constexpr std::size_t kSerialNumberLength = 128;

constexpr std::size_t kMaxWidth = 1920;
constexpr std::size_t kMaxHeight = 1080;

constexpr std::size_t kFormatStringLength = 8;
constexpr std::size_t kIntrinsicMatrixSize = 9;
constexpr std::size_t kDistortionCoeffsSize = 8;
constexpr std::size_t kExtrinsicMatrixSize = 12;

// PNG header size: 8 bytes signature + 25 bytes IHDR + 12 bytes IEND + padding
// Plus extra space for potential additional chunks (tEXt, iTXt, etc.)
constexpr std::size_t kHeaderSize = 8192;

/**
 * @brief Converts a string to a null-terminated character array.
 */
template <std::size_t num_elements>
inline void StringToArray(const std::string& str, std::array<char, num_elements>& arr) {
  const int64_t copy_len = std::min(static_cast<std::size_t>(str.size()), arr.size() - 1);
  std::copy(str.begin(), str.begin() + copy_len, arr.begin());
  gsl::at(arr, static_cast<gsl::index>(copy_len)) = '\0';  // null termination
}

/**
 * @brief Container for raw camera frames and associated metadata.
 *
 * @note CONVENTION: All extrinsics, transformations, and reconstructions are reported with respect
 * to the primary camera (the left camera for stereo setups, or the color camera for monocular/RGB-D).
 * The color_stereo_right image is intended for debugging and verification only.
 */
struct CameraFrameBuffer {
  // ===== Encoded Image Fields =====
  // Color image encoding metadata
  uint32_t color_width{};
  uint32_t color_height{};
  uint32_t color_bytes_per_pixel{};
  uint32_t color_image_size{};
  std::array<char, kFormatStringLength> color_format{};
  uint32_t color_stride{};

  // Depth image encoding metadata
  uint32_t depth_width{};
  uint32_t depth_height{};
  uint32_t depth_bytes_per_pixel{};
  uint32_t depth_image_size{};
  std::array<char, kFormatStringLength> depth_format{};
  uint32_t depth_stride{};

  // Stereo pair image encoding metadata (e.g., left image for ZED)
  uint32_t color_stereo_right_width{};
  uint32_t color_stereo_right_height{};
  uint32_t color_stereo_right_bytes_per_pixel{};
  uint32_t color_stereo_right_image_size{};
  std::array<char, kFormatStringLength> color_stereo_right_format{};

  // Image data buffers
  std::array<std::byte, (kMaxWidth * kMaxHeight * 3) + kHeaderSize> color_image_bytes{};
  std::array<std::byte, (kMaxWidth * kMaxHeight * 2) + kHeaderSize> depth_image_bytes{};
  std::array<std::byte, (kMaxWidth * kMaxHeight * 3) + kHeaderSize> color_stereo_right_image_bytes{};

  // ===== Camera Calibration Fields (Used for Point Cloud Generation) =====
  std::array<double, kIntrinsicMatrixSize> color_intrinsics_k{};
  std::array<double, kDistortionCoeffsSize> color_intrinsics_d{};
  std::array<double, kIntrinsicMatrixSize> depth_intrinsics_k{};
  std::array<double, kDistortionCoeffsSize> depth_intrinsics_d{};
  std::array<double, kExtrinsicMatrixSize> depth_to_rgb_extrinsics{};
  double stereo_baseline_meters{};
  float depth_scale{};
  bool cam_to_world_extrinsics_set{};
  std::array<double, kExtrinsicMatrixSize> cam_to_world_extrinsics{};

  // ===== Common Fields =====
  std::array<char, kSerialNumberLength> serial_number{};
  std::array<char, kNameLength> camera_name{};
  uint64_t timestamp_nanos{};
  uint64_t frame_id{};
};

}  // namespace dex::camera

#endif  // DEX_DRIVERS_CAMERA_BASE_TYPES_H

