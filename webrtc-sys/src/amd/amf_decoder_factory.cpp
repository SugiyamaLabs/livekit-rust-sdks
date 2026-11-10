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

#include "amf_decoder_factory.h"

#include <cstdlib>

#include "amf_h264_decoder.h"
#include "api/video_codecs/h264_profile_level_id.h"
#include "api/video_codecs/video_decoder_software_fallback_wrapper.h"
#include "media/base/media_constants.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "rtc_base/logging.h"

namespace livekit_ffi {

bool AmfVideoDecoderFactory::IsSupported() {
  static bool supported = [] {
    // Opt-in until tested on AMD hardware; D3D11VA already covers AMD.
    if (!std::getenv("LK_ENABLE_AMFDEC"))
      return false;
    if (!AmfH264Decoder::Factory())
      return false;  // no AMD driver
    AmfH264Decoder probe;
    bool ok = probe.Open(1280, 720);
    RTC_LOG(LS_INFO) << "AMF hardware H.264 decoder " << (ok ? "available" : "unavailable");
    return ok;
  }();
  return supported;
}

std::vector<webrtc::SdpVideoFormat> AmfVideoDecoderFactory::GetSupportedFormats() const {
  return {
      webrtc::CreateH264Format(webrtc::H264Profile::kProfileConstrainedBaseline,
                               webrtc::H264Level::kLevel5_1, "1"),
      webrtc::CreateH264Format(webrtc::H264Profile::kProfileBaseline,
                               webrtc::H264Level::kLevel5_1, "1"),
      webrtc::CreateH264Format(webrtc::H264Profile::kProfileHigh,
                               webrtc::H264Level::kLevel5_1, "1"),
      webrtc::CreateH264Format(webrtc::H264Profile::kProfileMain,
                               webrtc::H264Level::kLevel5_1, "1"),
  };
}

std::unique_ptr<webrtc::VideoDecoder> AmfVideoDecoderFactory::Create(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format) {
  if (format.name != webrtc::kH264CodecName)
    return nullptr;
  RTC_LOG(LS_INFO) << "Using AMD AMF HW decoder for H264";
  auto hw = std::make_unique<AmfH264Decoder>();
  // Driver trouble mid-call drops to FFmpeg instead of a black tile.
  if (auto sw = webrtc::H264Decoder::Create())
    return webrtc::CreateVideoDecoderSoftwareFallbackWrapper(env, std::move(sw), std::move(hw));
  return hw;
}

}  // namespace livekit_ffi
