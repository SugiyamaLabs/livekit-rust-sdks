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

#include "amf_h264_decoder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <mutex>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/convert.h"

namespace livekit_ffi {

amf::AMFFactory* AmfH264Decoder::Factory() {
  static amf::AMFFactory* factory = [] {
    amf::AMFFactory* f = nullptr;
    // Kept loaded for the life of the process (decoders hold its objects).
    HMODULE module = LoadLibraryW(AMF_DLL_NAME);
    if (!module)
      return f;
    auto init = reinterpret_cast<AMFInit_Fn>(GetProcAddress(module, AMF_INIT_FUNCTION_NAME));
    if (!init || init(AMF_FULL_VERSION, &f) != AMF_OK) {
      RTC_LOG(LS_WARNING) << "AMF: AMFInit failed";
      return static_cast<amf::AMFFactory*>(nullptr);
    }
    return f;
  }();
  return factory;
}

AmfH264Decoder::AmfH264Decoder() : buffer_pool_(false) {}

AmfH264Decoder::~AmfH264Decoder() {
  Release();
}

webrtc::VideoDecoder::DecoderInfo AmfH264Decoder::GetDecoderInfo() const {
  DecoderInfo info;
  info.implementation_name = "AMD AMF H264";
  info.is_hardware_accelerated = true;
  return info;
}

bool AmfH264Decoder::Open(int width, int height) {
  Close();
  amf::AMFFactory* factory = Factory();
  if (!factory)
    return false;
  if (factory->CreateContext(&context_) != AMF_OK)
    return false;
  // The decoder runs on a D3D11 device; output surfaces are downloaded below.
  if (context_->InitDX11(nullptr) != AMF_OK) {
    RTC_LOG(LS_WARNING) << "AMF: InitDX11 failed";
    Close();
    return false;
  }
  if (factory->CreateComponent(context_, AMFVideoDecoderUVD_H264_AVC, &decoder_) != AMF_OK) {
    RTC_LOG(LS_WARNING) << "AMF: no H.264 decoder component";
    Close();
    return false;
  }
  // No reordering in WebRTC streams: a frame out for every frame in.
  decoder_->SetProperty(AMF_VIDEO_DECODER_REORDER_MODE,
                        static_cast<amf_int64>(AMF_VIDEO_DECODER_MODE_LOW_LATENCY));
  // Timestamps follow decode order, so the RTP timestamp set on the input
  // comes back on the matching output.
  decoder_->SetProperty(AMF_TIMESTAMP_MODE, static_cast<amf_int64>(AMF_TS_DECODE));
  if (decoder_->Init(amf::AMF_SURFACE_NV12, width, height) != AMF_OK) {
    RTC_LOG(LS_WARNING) << "AMF: decoder Init failed for " << width << "x" << height;
    Close();
    return false;
  }
  width_ = width;
  height_ = height;
  return true;
}

void AmfH264Decoder::Close() {
  if (decoder_) {
    decoder_->Terminate();
    decoder_ = nullptr;
  }
  if (context_) {
    context_->Terminate();
    context_ = nullptr;
  }
  width_ = height_ = 0;
}

bool AmfH264Decoder::Configure(const Settings& settings) {
  // Opened on the first SPS, at the stream size.
  return settings.codec_type() == webrtc::kVideoCodecH264 && Factory() != nullptr;
}

int32_t AmfH264Decoder::RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t AmfH264Decoder::Release() {
  Close();
  buffer_pool_.Release();
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t AmfH264Decoder::Decode(const webrtc::EncodedImage& input_image,
                               bool /*missing_frames*/,
                               int64_t /*render_time_ms*/) {
  if (!callback_)
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  if (!input_image.data() || !input_image.size())
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

  parser_.ParseBitstream(input_image);
  if (auto sps = parser_.sps()) {
    const int w = static_cast<int>(sps->width);
    const int h = static_cast<int>(sps->height);
    // First SPS or a new size (comes with a keyframe): (re)open at that size.
    if (!decoder_ || w != width_ || h != height_) {
      if (!Open(w, h))
        return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }
  }
  if (!decoder_)
    return WEBRTC_VIDEO_CODEC_ERROR;  // no SPS yet: ask for a keyframe

  amf::AMFBufferPtr buffer;
  if (context_->AllocBuffer(amf::AMF_MEMORY_HOST, input_image.size(), &buffer) != AMF_OK)
    return WEBRTC_VIDEO_CODEC_ERROR;
  memcpy(buffer->GetNative(), input_image.data(), input_image.size());
  buffer->SetPts(static_cast<amf_pts>(input_image.RtpTimestamp()));

  AMF_RESULT result = decoder_->SubmitInput(buffer);
  if (result == AMF_INPUT_FULL) {
    Drain(input_image);
    result = decoder_->SubmitInput(buffer);
  }
  if (result != AMF_OK && result != AMF_NEED_MORE_INPUT) {
    RTC_LOG(LS_WARNING) << "AMF: SubmitInput failed, " << result;
    return WEBRTC_VIDEO_CODEC_ERROR;  // asks for a keyframe
  }
  Drain(input_image);
  return WEBRTC_VIDEO_CODEC_OK;
}

void AmfH264Decoder::Drain(const webrtc::EncodedImage& input_image) {
  for (;;) {
    amf::AMFDataPtr data;
    if (decoder_->QueryOutput(&data) != AMF_OK || !data)
      return;  // AMF_REPEAT: nothing ready yet
    amf::AMFSurfacePtr surface(data);
    if (!surface || surface->Convert(amf::AMF_MEMORY_HOST) != AMF_OK)
      continue;
    amf::AMFPlane* y = surface->GetPlane(amf::AMF_PLANE_Y);
    amf::AMFPlane* uv = surface->GetPlane(amf::AMF_PLANE_UV);
    if (!y || !uv)
      continue;
    const int w = y->GetWidth();
    const int h = y->GetHeight();
    webrtc::scoped_refptr<webrtc::I420Buffer> i420 = buffer_pool_.CreateI420Buffer(w, h);
    if (!i420)
      continue;
    libyuv::NV12ToI420(static_cast<const uint8_t*>(y->GetNative()), y->GetHPitch(),
                       static_cast<const uint8_t*>(uv->GetNative()), uv->GetHPitch(),
                       i420->MutableDataY(), i420->StrideY(), i420->MutableDataU(),
                       i420->StrideU(), i420->MutableDataV(), i420->StrideV(), w, h);
    auto builder = webrtc::VideoFrame::Builder()
                       .set_video_frame_buffer(i420)
                       .set_timestamp_rtp(static_cast<uint32_t>(surface->GetPts()));
    if (input_image.ColorSpace())
      builder.set_color_space(*input_image.ColorSpace());
    webrtc::VideoFrame frame = builder.build();
    callback_->Decoded(frame, std::nullopt, std::nullopt);
  }
}

}  // namespace livekit_ffi
