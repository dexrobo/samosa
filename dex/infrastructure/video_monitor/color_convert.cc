#include "dex/infrastructure/video_monitor/color_convert.h"

#include <algorithm>

namespace dex::video_monitor {

void ConvertToI420(const uint8_t* src, size_t src_stride, uint8_t* dst_y, uint8_t* dst_u, uint8_t* dst_v,
                   uint32_t width, uint32_t height, PixelFormat format) {
  // BT.601 coefficients (scaled by 256 for fixed-point integer math).
  // Y  =  66*R + 129*G +  25*B + 128 >> 8 + 16
  // Cb = -38*R -  74*G + 112*B + 128 >> 8 + 128
  // Cr = 112*R -  94*G -  18*B + 128 >> 8 + 128

  // Channel offsets: RGB24 → R=0,G=1,B=2; BGR24 → R=2,G=1,B=0
  const int r_idx = (format == PixelFormat::kRGB24) ? 0 : 2;
  const int b_idx = (format == PixelFormat::kRGB24) ? 2 : 0;

  const uint32_t uv_width = width / 2;

  // Process two rows at a time for chroma subsampling (2x2 block → 1 U, 1 V).
  for (uint32_t row = 0; row < height; row += 2) {
    const uint8_t* row0 = src + row * src_stride;
    const uint8_t* row1 = src + (row + 1) * src_stride;
    uint8_t* y0 = dst_y + row * width;
    uint8_t* y1 = dst_y + (row + 1) * width;
    uint8_t* u = dst_u + (row / 2) * uv_width;
    uint8_t* v = dst_v + (row / 2) * uv_width;

    for (uint32_t col = 0; col < width; col += 2) {
      // Read the 2x2 pixel block.
      const uint8_t* p00 = row0 + col * 3;
      const uint8_t* p01 = row0 + (col + 1) * 3;
      const uint8_t* p10 = row1 + col * 3;
      const uint8_t* p11 = row1 + (col + 1) * 3;

      int r00 = p00[r_idx], g00 = p00[1], b00 = p00[b_idx];
      int r01 = p01[r_idx], g01 = p01[1], b01 = p01[b_idx];
      int r10 = p10[r_idx], g10 = p10[1], b10 = p10[b_idx];
      int r11 = p11[r_idx], g11 = p11[1], b11 = p11[b_idx];

      // Luma for all 4 pixels.
      y0[col] = static_cast<uint8_t>(std::clamp(((66 * r00 + 129 * g00 + 25 * b00 + 128) >> 8) + 16, 0, 255));
      y0[col + 1] = static_cast<uint8_t>(std::clamp(((66 * r01 + 129 * g01 + 25 * b01 + 128) >> 8) + 16, 0, 255));
      y1[col] = static_cast<uint8_t>(std::clamp(((66 * r10 + 129 * g10 + 25 * b10 + 128) >> 8) + 16, 0, 255));
      y1[col + 1] = static_cast<uint8_t>(std::clamp(((66 * r11 + 129 * g11 + 25 * b11 + 128) >> 8) + 16, 0, 255));

      // Average the 2x2 block for chroma.
      int avg_r = (r00 + r01 + r10 + r11 + 2) / 4;
      int avg_g = (g00 + g01 + g10 + g11 + 2) / 4;
      int avg_b = (b00 + b01 + b10 + b11 + 2) / 4;

      u[col / 2] =
          static_cast<uint8_t>(std::clamp(((-38 * avg_r - 74 * avg_g + 112 * avg_b + 128) >> 8) + 128, 0, 255));
      v[col / 2] = static_cast<uint8_t>(std::clamp(((112 * avg_r - 94 * avg_g - 18 * avg_b + 128) >> 8) + 128, 0, 255));
    }
  }
}

void ComputeScaledDimensions(uint32_t src_width, uint32_t src_height, uint32_t max_width, uint32_t max_height,
                             uint32_t& out_width, uint32_t& out_height) {
  // No limit or already fits.
  if ((max_width == 0 && max_height == 0) || (src_width <= max_width && src_height <= max_height)) {
    out_width = src_width;
    out_height = src_height;
    return;
  }

  // Scale to fit within bounds while preserving aspect ratio.
  double scale_w = (max_width > 0) ? static_cast<double>(max_width) / src_width : 1e9;
  double scale_h = (max_height > 0) ? static_cast<double>(max_height) / src_height : 1e9;
  double scale = std::min(scale_w, scale_h);

  out_width = static_cast<uint32_t>(src_width * scale);
  out_height = static_cast<uint32_t>(src_height * scale);

  // Ensure even dimensions (required for I420 and H.264).
  out_width &= ~1U;
  out_height &= ~1U;

  // Safety: minimum 2x2.
  if (out_width < 2) out_width = 2;
  if (out_height < 2) out_height = 2;
}

void DownsampleRGB(const uint8_t* src, uint32_t src_width, uint32_t src_height, size_t src_stride, uint8_t* dst,
                   uint32_t dst_width, uint32_t dst_height, size_t dst_stride) {
  // Bilinear interpolation for smooth downsampling.
  for (uint32_t dst_row = 0; dst_row < dst_height; ++dst_row) {
    // Map destination row to fractional source position.
    float src_y =
        (static_cast<float>(dst_row) + 0.5f) * static_cast<float>(src_height) / static_cast<float>(dst_height) - 0.5f;
    if (src_y < 0) src_y = 0;
    auto sy0 = static_cast<uint32_t>(src_y);
    uint32_t sy1 = std::min(sy0 + 1, src_height - 1);
    float fy = src_y - static_cast<float>(sy0);

    const uint8_t* row0 = src + sy0 * src_stride;
    const uint8_t* row1 = src + sy1 * src_stride;
    uint8_t* dst_line = dst + dst_row * dst_stride;

    for (uint32_t dst_col = 0; dst_col < dst_width; ++dst_col) {
      float src_x =
          (static_cast<float>(dst_col) + 0.5f) * static_cast<float>(src_width) / static_cast<float>(dst_width) - 0.5f;
      if (src_x < 0) src_x = 0;
      auto sx0 = static_cast<uint32_t>(src_x);
      uint32_t sx1 = std::min(sx0 + 1, src_width - 1);
      float fx = src_x - static_cast<float>(sx0);

      // Four source pixels.
      const uint8_t* p00 = row0 + sx0 * 3;
      const uint8_t* p10 = row0 + sx1 * 3;
      const uint8_t* p01 = row1 + sx0 * 3;
      const uint8_t* p11 = row1 + sx1 * 3;

      uint8_t* dp = dst_line + dst_col * 3;
      for (int ch = 0; ch < 3; ++ch) {
        float top = static_cast<float>(p00[ch]) * (1 - fx) + static_cast<float>(p10[ch]) * fx;
        float bot = static_cast<float>(p01[ch]) * (1 - fx) + static_cast<float>(p11[ch]) * fx;
        dp[ch] = static_cast<uint8_t>(top * (1 - fy) + bot * fy + 0.5f);
      }
    }
  }
}

}  // namespace dex::video_monitor
