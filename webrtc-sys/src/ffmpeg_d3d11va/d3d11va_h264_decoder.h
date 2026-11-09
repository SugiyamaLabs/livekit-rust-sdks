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

#ifndef LIVEKIT_D3D11VA_H264_DECODER_H_
#define LIVEKIT_D3D11VA_H264_DECODER_H_

#include <memory>
#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "common_video/include/video_frame_buffer_pool.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace livekit_ffi {

// H.264 decoded on the GPU through D3D11VA (any vendor), using a minimal
// FFmpeg build (avcodec/avutil, LGPL) loaded at runtime from next to
// livekit_ffi.dll. One shared D3D11 device: no CUDA context, so memory stays
// close to the software decoder while decoding leaves the CPU.
class D3d11vaH264Decoder : public webrtc::VideoDecoder {
 public:
  // The FFmpeg DLLs load and a D3D11VA device opens (probed once).
  static bool IsAvailable();

  D3d11vaH264Decoder();
  ~D3d11vaH264Decoder() override;

  bool Configure(const Settings& settings) override;
  int32_t Decode(const webrtc::EncodedImage& input_image,
                 bool missing_frames,
                 int64_t render_time_ms) override;
  int32_t RegisterDecodeCompleteCallback(
      webrtc::DecodedImageCallback* callback) override;
  int32_t Release() override;
  DecoderInfo GetDecoderInfo() const override;

 private:
  void Deliver(AVFrame* frame, const webrtc::EncodedImage& input_image);

  AVCodecContext* context_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVFrame* download_ = nullptr;
  webrtc::DecodedImageCallback* callback_ = nullptr;
  webrtc::VideoFrameBufferPool buffer_pool_;
};

// Set LK_DISABLE_D3D11VA to turn it off.
class D3d11vaVideoDecoderFactory : public webrtc::VideoDecoderFactory {
 public:
  static bool IsSupported();
  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;
  std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env,
                                               const webrtc::SdpVideoFormat& format) override;
};

}  // namespace livekit_ffi

#endif  // LIVEKIT_D3D11VA_H264_DECODER_H_
