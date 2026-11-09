/*
 * Copyright 2026 SugiyamaLabs
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "size_switching_decoder.h"

#include "cuda_context.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"

#include <atomic>

namespace webrtc {

namespace {
// NVDEC sessions alive in the process. One at a time: the CUDA context alone
// is ~55 MB of host memory, a second large stream stays on FFmpeg.
std::atomic<int> g_sessions{0};
constexpr int kMaxSessions = 1;
}  // namespace

SizeSwitchingH264Decoder::SizeSwitchingH264Decoder(const Environment& /*env*/)
    : software_(H264Decoder::Create()) {}

SizeSwitchingH264Decoder::~SizeSwitchingH264Decoder() {
  Release();
}

bool SizeSwitchingH264Decoder::Configure(const Settings& settings) {
  settings_ = settings;
  current_ = nullptr;
  if (software_ && software_->Configure(settings))
    current_ = software_.get();
  return current_ != nullptr;
}

int32_t SizeSwitchingH264Decoder::RegisterDecodeCompleteCallback(DecodedImageCallback* callback) {
  callback_ = callback;
  if (software_)
    software_->RegisterDecodeCompleteCallback(callback);
  if (hardware_)
    hardware_->RegisterDecodeCompleteCallback(callback);
  return WEBRTC_VIDEO_CODEC_OK;
}

bool SizeSwitchingH264Decoder::UseHardware() {
  if (hardware_)
    return true;
  if (hardware_failed_)
    return false;
  if (g_sessions.fetch_add(1) >= kMaxSessions) {
    g_sessions.fetch_sub(1);
    return false;  // another large stream already has NVDEC
  }
  session_held_ = true;
  auto* cuda = livekit_ffi::CudaContext::GetInstance();
  if (!cuda->Initialize()) {
    hardware_failed_ = true;
    g_sessions.fetch_sub(1);
    session_held_ = false;
    return false;
  }
  cuda_held_ = true;
  hardware_ = std::make_unique<NvidiaH264DecoderImpl>(cuda->GetContext());
  if (!hardware_->Configure(settings_)) {
    DropHardware();
    hardware_failed_ = true;
    return false;
  }
  hardware_->RegisterDecodeCompleteCallback(callback_);
  return true;
}

void SizeSwitchingH264Decoder::DropHardware() {
  if (hardware_) {
    hardware_->Release();
    hardware_.reset();
  }
  if (cuda_held_) {
    livekit_ffi::CudaContext::GetInstance()->Shutdown();
    cuda_held_ = false;
  }
  if (session_held_) {
    g_sessions.fetch_sub(1);
    session_held_ = false;
  }
}

int32_t SizeSwitchingH264Decoder::Decode(const EncodedImage& input_image,
                                         bool missing_frames,
                                         int64_t render_time_ms) {
  if (!current_)
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

  // Pick the decoder on keyframes, where the SPS (and so the size) can change.
  if (input_image.FrameType() == VideoFrameType::kVideoFrameKey) {
    parser_.ParseBitstream(input_image);
    if (auto sps = parser_.sps()) {
      const bool large = static_cast<int>(sps->width * sps->height) >= kMinHardwarePixels;
      if (large && current_ != hardware_.get() && UseHardware()) {
        RTC_LOG(LS_INFO) << "NVDEC for " << sps->width << "x" << sps->height;
        current_ = hardware_.get();
      } else if (!large && hardware_ && current_ == hardware_.get()) {
        current_ = software_.get();
        DropHardware();  // give the memory back
      }
    }
  }

  int32_t result = current_->Decode(input_image, missing_frames, render_time_ms);
  if (result < 0 && current_ == hardware_.get()) {
    // Driver trouble: back to FFmpeg for good, and ask for a keyframe.
    RTC_LOG(LS_WARNING) << "NVDEC failed (" << result << "), falling back to FFmpeg";
    current_ = software_.get();
    DropHardware();
    hardware_failed_ = true;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  return result;
}

int32_t SizeSwitchingH264Decoder::Release() {
  if (software_)
    software_->Release();
  DropHardware();
  current_ = nullptr;
  return WEBRTC_VIDEO_CODEC_OK;
}

VideoDecoder::DecoderInfo SizeSwitchingH264Decoder::GetDecoderInfo() const {
  if (current_)
    return current_->GetDecoderInfo();
  DecoderInfo info;
  info.implementation_name = "NVDEC/FFmpeg H264";
  return info;
}

}  // namespace webrtc
