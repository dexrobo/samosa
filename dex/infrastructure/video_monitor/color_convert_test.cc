#include "dex/infrastructure/video_monitor/color_convert.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include "gtest/gtest.h"

namespace dex::video_monitor {
namespace {

// BT.601 reference conversion (floating point) for verification.
void ReferenceRGBtoYUV(uint8_t r, uint8_t g, uint8_t b, uint8_t& y, uint8_t& u, uint8_t& v) {
  double yd = 0.257 * r + 0.504 * g + 0.098 * b + 16.0;
  double ud = -0.148 * r - 0.291 * g + 0.439 * b + 128.0;
  double vd = 0.439 * r - 0.368 * g - 0.071 * b + 128.0;
  y = static_cast<uint8_t>(std::clamp(std::round(yd), 0.0, 255.0));
  u = static_cast<uint8_t>(std::clamp(std::round(ud), 0.0, 255.0));
  v = static_cast<uint8_t>(std::clamp(std::round(vd), 0.0, 255.0));
}

TEST(ColorConvertTest, PureWhiteRGB) {
  // 2x2 white pixels.
  const uint32_t w = 2, h = 2;
  std::vector<uint8_t> rgb(w * h * 3, 255);
  std::vector<uint8_t> y(w * h), u(w / 2 * h / 2), v(w / 2 * h / 2);

  ConvertToI420(rgb.data(), w * 3, y.data(), u.data(), v.data(), w, h, PixelFormat::kRGB24);

  // White in BT.601: Y=235, U=128, V=128 (approximately).
  for (auto yv : y) EXPECT_NEAR(yv, 235, 2);
  EXPECT_NEAR(u[0], 128, 2);
  EXPECT_NEAR(v[0], 128, 2);
}

TEST(ColorConvertTest, PureBlackRGB) {
  const uint32_t w = 2, h = 2;
  std::vector<uint8_t> rgb(w * h * 3, 0);
  std::vector<uint8_t> y(w * h), u(w / 2 * h / 2), v(w / 2 * h / 2);

  ConvertToI420(rgb.data(), w * 3, y.data(), u.data(), v.data(), w, h, PixelFormat::kRGB24);

  // Black in BT.601: Y=16, U=128, V=128.
  for (auto yv : y) EXPECT_NEAR(yv, 16, 1);
  EXPECT_NEAR(u[0], 128, 1);
  EXPECT_NEAR(v[0], 128, 1);
}

TEST(ColorConvertTest, PureRedRGB) {
  const uint32_t w = 2, h = 2;
  std::vector<uint8_t> rgb(w * h * 3, 0);
  for (uint32_t i = 0; i < w * h; ++i) rgb[i * 3] = 255;  // R=255, G=0, B=0

  std::vector<uint8_t> y(w * h), u(w / 2 * h / 2), v(w / 2 * h / 2);
  ConvertToI420(rgb.data(), w * 3, y.data(), u.data(), v.data(), w, h, PixelFormat::kRGB24);

  uint8_t ref_y{}, ref_u{}, ref_v{};
  ReferenceRGBtoYUV(255, 0, 0, ref_y, ref_u, ref_v);

  for (auto yv : y) EXPECT_NEAR(yv, ref_y, 2);
  EXPECT_NEAR(u[0], ref_u, 2);
  EXPECT_NEAR(v[0], ref_v, 2);
}

TEST(ColorConvertTest, BGRFormatSwapsChannels) {
  const uint32_t w = 2, h = 2;
  // BGR with B=0, G=0, R=255 → same as RGB red.
  std::vector<uint8_t> bgr(w * h * 3, 0);
  for (uint32_t i = 0; i < w * h; ++i) bgr[i * 3 + 2] = 255;  // B=0, G=0, R=255 in BGR layout

  std::vector<uint8_t> y_bgr(w * h), u_bgr(1), v_bgr(1);
  ConvertToI420(bgr.data(), w * 3, y_bgr.data(), u_bgr.data(), v_bgr.data(), w, h, PixelFormat::kBGR24);

  // Should match RGB red.
  std::vector<uint8_t> rgb(w * h * 3, 0);
  for (uint32_t i = 0; i < w * h; ++i) rgb[i * 3] = 255;

  std::vector<uint8_t> y_rgb(w * h), u_rgb(1), v_rgb(1);
  ConvertToI420(rgb.data(), w * 3, y_rgb.data(), u_rgb.data(), v_rgb.data(), w, h, PixelFormat::kRGB24);

  EXPECT_EQ(y_bgr, y_rgb);
  EXPECT_EQ(u_bgr, u_rgb);
  EXPECT_EQ(v_bgr, v_rgb);
}

TEST(ColorConvertTest, LargerImage4x4) {
  const uint32_t w = 4, h = 4;
  std::vector<uint8_t> rgb(w * h * 3);

  // Fill with a gradient pattern.
  for (uint32_t i = 0; i < w * h; ++i) {
    rgb[i * 3 + 0] = static_cast<uint8_t>(i * 16);        // R
    rgb[i * 3 + 1] = static_cast<uint8_t>(255 - i * 16);  // G
    rgb[i * 3 + 2] = static_cast<uint8_t>(i * 8);         // B
  }

  std::vector<uint8_t> y(w * h), u(w / 2 * h / 2), v(w / 2 * h / 2);
  ConvertToI420(rgb.data(), w * 3, y.data(), u.data(), v.data(), w, h, PixelFormat::kRGB24);

  // Verify output sizes are correct and values are in valid range.
  ASSERT_EQ(y.size(), 16u);
  ASSERT_EQ(u.size(), 4u);
  ASSERT_EQ(v.size(), 4u);

  for (auto yv : y) {
    EXPECT_GE(yv, 16);
    EXPECT_LE(yv, 235);
  }
}

}  // namespace
}  // namespace dex::video_monitor
