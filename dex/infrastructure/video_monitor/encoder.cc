#include "dex/infrastructure/video_monitor/encoder.h"

#include <cstdint>
#include <cstring>
#include <utility>

#include "codec_api.h"
#include "codec_app_def.h"
#include "codec_def.h"
#include "spdlog/spdlog.h"

namespace dex::video_monitor {

H264Encoder::H264Encoder(const Params& params) : params_(params) {
  int ret = WelsCreateSVCEncoder(&encoder_);
  if (ret != 0 || encoder_ == nullptr) {
    SPDLOG_ERROR("WelsCreateSVCEncoder failed: {}", ret);
    encoder_ = nullptr;
    return;
  }

  SEncParamExt ext_params;
  std::memset(&ext_params, 0, sizeof(ext_params));
  encoder_->GetDefaultParams(&ext_params);

  ext_params.iUsageType = CAMERA_VIDEO_REAL_TIME;
  ext_params.iPicWidth = static_cast<int>(params_.width);
  ext_params.iPicHeight = static_cast<int>(params_.height);
  ext_params.fMaxFrameRate = static_cast<float>(params_.fps);
  ext_params.iTargetBitrate = static_cast<int>(params_.bitrate_kbps) *
                              1000;  // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
  ext_params.iMaxBitrate = ext_params.iTargetBitrate * 2;
  ext_params.iRCMode = RC_BITRATE_MODE;
  ext_params.uiIntraPeriod = params_.keyframe_interval;
  ext_params.iTemporalLayerNum = 1;
  ext_params.iSpatialLayerNum = 1;
  ext_params.iMultipleThreadIdc = 1;  // Single thread -- minimize CPU for production co-residency.
  ext_params.bEnableFrameSkip = false;
  ext_params.bEnableDenoise = false;
  ext_params.bEnableBackgroundDetection = false;
  ext_params.bEnableAdaptiveQuant = false;
  ext_params.bEnableSceneChangeDetect = false;
  ext_params.iEntropyCodingModeFlag = 0;  // CAVLC (faster than CABAC).
  ext_params.iComplexityMode = LOW_COMPLEXITY;

  // Single spatial layer matching the input dimensions.
  auto& layer = ext_params.sSpatialLayers[0];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
  layer.iVideoWidth = ext_params.iPicWidth;
  layer.iVideoHeight = ext_params.iPicHeight;
  layer.fFrameRate = ext_params.fMaxFrameRate;
  layer.iSpatialBitrate = ext_params.iTargetBitrate;
  layer.iMaxSpatialBitrate = ext_params.iMaxBitrate;
  layer.uiProfileIdc = PRO_BASELINE;

  ret = encoder_->InitializeExt(&ext_params);
  if (ret != 0) {
    SPDLOG_ERROR("OpenH264 InitializeExt failed: {}", ret);
    WelsDestroySVCEncoder(encoder_);
    encoder_ = nullptr;
    return;
  }

  int video_format = videoFormatI420;
  encoder_->SetOption(ENCODER_OPTION_DATAFORMAT, &video_format);

  SPDLOG_INFO("OpenH264 encoder initialized: {}x{} @ {}fps, {}kbps, IDR every {} frames", params_.width, params_.height,
              params_.fps, params_.bitrate_kbps, params_.keyframe_interval);
}

H264Encoder::~H264Encoder() {
  if (encoder_ != nullptr) {
    encoder_->Uninitialize();
    WelsDestroySVCEncoder(encoder_);
  }
}

H264Encoder::H264Encoder(H264Encoder&& other) noexcept : encoder_(other.encoder_), params_(other.params_) {
  other.encoder_ = nullptr;
}

H264Encoder& H264Encoder::operator=(H264Encoder&& other) noexcept {
  if (this != &other) {
    if (encoder_ != nullptr) {
      encoder_->Uninitialize();
      WelsDestroySVCEncoder(encoder_);
    }
    encoder_ = other.encoder_;
    params_ = other.params_;
    other.encoder_ = nullptr;
  }
  return *this;
}

std::optional<H264Encoder::EncodedFrame> H264Encoder::Encode(const uint8_t* yuv_i420, uint64_t timestamp_us) {
  if (encoder_ == nullptr) {
    return std::nullopt;
  }

  SSourcePicture pic;
  std::memset(&pic, 0, sizeof(pic));
  pic.iColorFormat = videoFormatI420;
  pic.iPicWidth = static_cast<int>(params_.width);
  pic.iPicHeight = static_cast<int>(params_.height);
  pic.uiTimeStamp = static_cast<int64_t>(
      timestamp_us /
      1000);  // NOLINT(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) OpenH264 expects ms.

  const auto width = static_cast<int>(params_.width);
  const auto height = static_cast<int>(params_.height);

  // I420 layout: Y plane, then U plane (w/2 * h/2), then V plane (w/2 * h/2).
  // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  // NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result,readability-math-missing-parentheses)
  pic.pData[0] = const_cast<unsigned char*>(yuv_i420);
  pic.pData[1] = const_cast<unsigned char*>(yuv_i420 + (width * height));
  pic.pData[2] = const_cast<unsigned char*>(yuv_i420 + (width * height) + ((width / 2) * (height / 2)));
  // NOLINTEND(bugprone-implicit-widening-of-multiplication-result,readability-math-missing-parentheses)
  // NOLINTEND(cppcoreguidelines-pro-type-const-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
  pic.iStride[0] = width;
  pic.iStride[1] = width / 2;
  pic.iStride[2] = width / 2;

  SFrameBSInfo info;
  std::memset(&info, 0, sizeof(info));

  int ret = encoder_->EncodeFrame(&pic, &info);
  if (ret != 0) {
    SPDLOG_WARN("EncodeFrame failed: {}", ret);
    return std::nullopt;
  }

  if (info.eFrameType == videoFrameTypeSkip) {
    return std::nullopt;
  }

  // Collect all NAL units into a single Annex B byte stream.
  EncodedFrame frame;
  frame.pts = timestamp_us;
  frame.is_idr = (info.eFrameType == videoFrameTypeIDR);

  for (int layer_idx = 0; layer_idx < info.iLayerNum; ++layer_idx) {
    const auto& layer_info = info.sLayerInfo[layer_idx];  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    const uint8_t* buf = layer_info.pBsBuf;
    for (int nal_idx = 0; nal_idx < layer_info.iNalCount; ++nal_idx) {
      const int nal_len =
          layer_info.pNalLengthInByte[nal_idx];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      frame.nals.insert(frame.nals.end(), buf, buf + nal_len);
      buf += nal_len;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
  }

  return frame;
}

}  // namespace dex::video_monitor
