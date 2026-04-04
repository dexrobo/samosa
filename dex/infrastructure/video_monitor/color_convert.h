#ifndef DEX_INFRASTRUCTURE_VIDEO_MONITOR_COLOR_CONVERT_H
#define DEX_INFRASTRUCTURE_VIDEO_MONITOR_COLOR_CONVERT_H

#include <cstddef>
#include <cstdint>

namespace dex::video_monitor {

enum class PixelFormat {
  kRGB24,
  kBGR24,
};

/// Convert RGB24 or BGR24 interleaved pixels to I420 (YUV 4:2:0 planar).
///
/// Output layout: width*height Y bytes, then (width/2)*(height/2) U bytes,
/// then (width/2)*(height/2) V bytes. Uses BT.601 coefficients.
///
/// @param src        Source RGB24/BGR24 pixel data (3 bytes per pixel, row-major).
/// @param src_stride Bytes per row in source (typically width * 3).
/// @param dst_y      Destination Y plane (width * height bytes).
/// @param dst_u      Destination U (Cb) plane (width/2 * height/2 bytes).
/// @param dst_v      Destination V (Cr) plane (width/2 * height/2 bytes).
/// @param width      Image width in pixels (must be even).
/// @param height     Image height in pixels (must be even).
/// @param format     Source pixel format (RGB24 or BGR24).
void ConvertToI420(const uint8_t* src, size_t src_stride, uint8_t* dst_y, uint8_t* dst_u, uint8_t* dst_v,
                   uint32_t width, uint32_t height, PixelFormat format);

}  // namespace dex::video_monitor

#endif  // DEX_INFRASTRUCTURE_VIDEO_MONITOR_COLOR_CONVERT_H
