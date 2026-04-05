#include "dex/infrastructure/video_monitor/pipeline.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "spdlog/spdlog.h"

#include "dex/drivers/camera/base/types.h"
#include "dex/infrastructure/shared_memory/shared_memory_monitor.h"
#include "dex/infrastructure/video_monitor/color_convert.h"
#include "dex/infrastructure/video_monitor/encoder.h"
#include "dex/infrastructure/video_monitor/fmp4_muxer.h"
#include "dex/infrastructure/video_monitor/time_base.h"

namespace dex::video_monitor {
namespace {

PixelFormat DetectPixelFormat(const dex::camera::CameraFrameBuffer& frame) {
  // Read the format string from the frame buffer.
  std::string fmt(frame.color_format.data(), strnlen(frame.color_format.data(), frame.color_format.size()));

  if (fmt == "BGR24" || fmt == "BGR" || fmt == "bgr24") return PixelFormat::kBGR24;
  // Default to RGB24 for "RGB24", "RGB", or unknown formats.
  return PixelFormat::kRGB24;
}

}  // namespace

TopicPipeline::TopicPipeline(const TopicConfig& config, FragmentRing& ring) : config_(config), ring_(ring) {}

TopicPipeline::~TopicPipeline() { Stop(); }

void TopicPipeline::Start() {
  if (running_.load()) return;
  stop_requested_.store(false);
  thread_ = std::thread(&TopicPipeline::Run, this);
}

void TopicPipeline::Stop() {
  stop_requested_.store(true);
  if (thread_.joinable()) {
    thread_.join();
  }
  running_.store(false);
}

void TopicPipeline::Run() {
  running_.store(true);
  SPDLOG_INFO("Pipeline starting for topic '{}' (shm: {})", config_.endpoint, config_.shm_name);

  // Retry loop for shared memory connection.
  constexpr int kMaxRetrySec = 30;
  int retry_sec = 1;

  auto& streaming_control = dex::shared_memory::StreamingControl::Instance();

  while (!stop_requested_.load() && streaming_control.IsRunning()) {
    // Monitor is small on the stack — its internal buffer_cache_ is already heap-allocated.
    dex::shared_memory::Monitor<dex::camera::CameraFrameBuffer> monitor(config_.shm_name);
    if (!monitor.IsValid()) {
      stats_.SetState(PipelineState::kWaitingForShm);
      SPDLOG_WARN("Shared memory '{}' not available, retrying in {}s", config_.shm_name, retry_sec);
      for (int i = 0; i < retry_sec * 10 && !stop_requested_.load() && streaming_control.IsRunning(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      retry_sec = std::min(retry_sec * 2, kMaxRetrySec);
      continue;
    }

    SPDLOG_INFO("Connected to shared memory '{}'", config_.shm_name);
    retry_sec = 1;

    stats_.SetState(PipelineState::kWaitingForFrames);

    // Pipeline state — lazily initialized on first valid frame.
    std::unique_ptr<H264Encoder> encoder;
    std::unique_ptr<FMP4Muxer> muxer;
    std::vector<uint8_t> yuv_buf;
    std::vector<uint8_t> downsampled_rgb;  // Scratch buffer for downsampled frames.
    uint64_t frame_count = 0;
    const uint32_t timescale = 90000;

    // H1: Track initialized dimensions/format for change detection.
    uint32_t enc_width = 0;
    uint32_t enc_height = 0;
    PixelFormat enc_format = PixelFormat::kRGB24;

    // Track previous frame timestamp for /status FPS measurement.
    uint64_t prev_timestamp_nanos = 0;

    // Frame rate limiting: skip frames to meet target_fps.
    auto last_encode_time = std::chrono::steady_clock::now();
    const auto min_frame_interval =
        std::chrono::microseconds(config_.target_fps > 0 ? 1000000 / config_.target_fps : 66667);

    // Helper to (re-)initialize encoder and muxer.
    auto init_encoder = [&](uint32_t width, uint32_t height, PixelFormat format) -> bool {
      encoder.reset();
      muxer.reset();

      H264Encoder::Params enc_params{
          .width = width,
          .height = height,
          .fps = config_.target_fps,
          .bitrate_kbps = config_.bitrate_kbps,
          .keyframe_interval = config_.keyframe_interval,
      };
      encoder = std::make_unique<H264Encoder>(enc_params);
      if (!encoder->IsValid()) {
        SPDLOG_ERROR("Failed to initialize encoder for '{}'", config_.endpoint);
        encoder.reset();
        return false;
      }

      yuv_buf.resize(static_cast<size_t>(width) * height * 3 / 2);
      enc_width = width;
      enc_height = height;
      enc_format = format;
      prev_timestamp_nanos = 0;

      stats_.width.store(width, std::memory_order_relaxed);
      stats_.height.store(height, std::memory_order_relaxed);

      SPDLOG_INFO("Encoder initialized for '{}': {}x{} @ {}fps", config_.endpoint, width, height, config_.target_fps);
      return true;
    };

    // Monitor::Run() with lazy encoding via blocking in the callback.
    // When idle (no clients): the callback blocks on WaitForClient(), freezing
    // Run()'s loop — no futex wakes, no 20MB frame copies, near-zero CPU.
    // When active: Run() uses efficient futex-based waits between frames.
    monitor.Run(
        [&](const dex::camera::CameraFrameBuffer& frame) {
          // Lazy encoding: block until a client connects (or shutdown).
          if (ring_.ClientCount() == 0) {
            stats_.SetState(PipelineState::kWaitingForFrames);
            while (ring_.ClientCount() == 0 && !stop_requested_.load() && streaming_control.IsRunning()) {
              ring_.WaitForClient(std::chrono::milliseconds(500));
            }
            return;  // Skip this stale frame — Run() gets a fresh one.
          }

          // Frame rate limiting.
          auto now = std::chrono::steady_clock::now();
          if (now - last_encode_time < min_frame_interval) {
            return;
          }
          last_encode_time = now;

          uint32_t src_width = frame.color_width;
          uint32_t src_height = frame.color_height;

          if (src_width == 0 || src_height == 0 || src_width % 2 != 0 || src_height % 2 != 0) {
            return;
          }

          auto pixel_format = DetectPixelFormat(frame);

          uint32_t width = src_width;
          uint32_t height = src_height;
          bool needs_downsample = false;
          if (config_.max_width > 0 || config_.max_height > 0) {
            ComputeScaledDimensions(src_width, src_height, config_.max_width, config_.max_height, width, height);
            needs_downsample = (width != src_width || height != src_height);
          }

          bool needs_init = !encoder;
          if (encoder && (width != enc_width || height != enc_height || pixel_format != enc_format)) {
            SPDLOG_WARN("Stream '{}' changed: {}x{} -> {}x{}", config_.endpoint, enc_width, enc_height, width, height);
            needs_init = true;
            stats_.encoder_reinits.fetch_add(1, std::memory_order_relaxed);
          }

          if (needs_init && !init_encoder(width, height, pixel_format)) {
            stats_.frames_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
          }

          stats_.SetState(PipelineState::kStreaming);

          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
          const uint8_t* rgb_src = reinterpret_cast<const uint8_t*>(frame.color_image_bytes.data());
          size_t rgb_stride = static_cast<size_t>(src_width) * 3;

          if (needs_downsample) {
            size_t dst_stride = static_cast<size_t>(width) * 3;
            downsampled_rgb.resize(dst_stride * height);
            DownsampleRGB(rgb_src, src_width, src_height, rgb_stride, downsampled_rgb.data(), width, height,
                          dst_stride);
            rgb_src = downsampled_rgb.data();
            rgb_stride = dst_stride;
          }

          ConvertToI420(rgb_src, rgb_stride, yuv_buf.data(), yuv_buf.data() + static_cast<size_t>(width) * height,
                        yuv_buf.data() + static_cast<size_t>(width) * height + (width / 2) * (height / 2), width,
                        height, pixel_format);

          uint64_t timestamp_us = TimeBase::Instance().MicrosecondsSinceStart();
          auto encoded = encoder->Encode(yuv_buf.data(), timestamp_us);
          if (!encoded.has_value()) {
            stats_.frames_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
          }

          if (!muxer && encoded->is_idr) {
            std::vector<uint8_t> sps, pps;
            if (ExtractSPSPPS(encoded->nals, sps, pps)) {
              FMP4Muxer::TrackParams track_params{
                  .width = width,
                  .height = height,
                  .timescale = timescale,
                  .sps = std::move(sps),
                  .pps = std::move(pps),
              };
              muxer = std::make_unique<FMP4Muxer>(track_params);
              ring_.SetInitSegment(muxer->GetInitSegment());
              SPDLOG_INFO("Init segment created for '{}'", config_.endpoint);
            }
          }

          if (!muxer) return;

          const uint32_t duration = timescale / config_.target_fps;

          uint64_t frame_timestamp_nanos = frame.timestamp_nanos;
          if (prev_timestamp_nanos > 0 && frame_timestamp_nanos > prev_timestamp_nanos) {
            uint64_t delta_nanos = frame_timestamp_nanos - prev_timestamp_nanos;
            if (delta_nanos > 0) {
              stats_.measured_fps_x10.store(static_cast<uint32_t>(10000000000ULL / delta_nanos),
                                            std::memory_order_relaxed);
            }
          }
          prev_timestamp_nanos = frame_timestamp_nanos;

          uint64_t decode_time = static_cast<uint64_t>(frame_count) * duration;
          auto fragment_data = muxer->MuxFragment(encoded->nals, decode_time, duration, encoded->is_idr);

          auto shared_data = std::make_shared<const std::vector<uint8_t>>(std::move(fragment_data));
          ring_.Push(Fragment{
              .data = std::move(shared_data),
              .timestamp_us = timestamp_us,
              .contains_idr = encoded->is_idr,
          });

          stats_.frames_encoded.fetch_add(1, std::memory_order_relaxed);
          stats_.last_frame_timestamp_ns.store(
              static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count()),
              std::memory_order_relaxed);
          ++frame_count;
        },
        0.1, dex::shared_memory::MonitorReadMode::Opportunistic);

    if (frame_count > 0) {
      SPDLOG_INFO("Pipeline '{}' encoded {} frames", config_.endpoint, frame_count);
    }
  }

  running_.store(false);
  SPDLOG_INFO("Pipeline stopped for topic '{}'", config_.endpoint);
}

}  // namespace dex::video_monitor
