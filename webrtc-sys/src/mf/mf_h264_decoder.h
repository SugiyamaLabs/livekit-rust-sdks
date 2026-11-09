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

#ifndef LIVEKIT_MF_H264_DECODER_H_
#define LIVEKIT_MF_H264_DECODER_H_

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include "api/video_codecs/video_decoder.h"
#include "common_video/include/video_frame_buffer_pool.h"

namespace livekit_ffi {

// H.264 decoder on Windows backed by the Media Foundation H.264 decoder MFT
// with D3D11 (DXVA) acceleration, so decoding runs on the GPU (NVIDIA, AMD or
// Intel) instead of FFmpeg on one CPU thread. Decoded NV12 surfaces are read
// back and handed to WebRTC as I420, like the NVDEC decoder does on Linux.
class MfH264Decoder : public webrtc::VideoDecoder {
 public:
  MfH264Decoder();
  ~MfH264Decoder() override;

  // Creates the device, the MFT and checks that DXVA is accepted. Used both by
  // Configure() and by the factory probe.
  bool Init();

  bool Configure(const Settings& settings) override;
  int32_t Decode(const webrtc::EncodedImage& input_image,
                 bool missing_frames,
                 int64_t render_time_ms) override;
  int32_t RegisterDecodeCompleteCallback(
      webrtc::DecodedImageCallback* callback) override;
  int32_t Release() override;
  DecoderInfo GetDecoderInfo() const override;

 private:
  bool SetOutputNv12();
  // Pulls every frame the MFT has ready. false on a hard error.
  bool Drain(const webrtc::EncodedImage& input_image);
  void Deliver(IMFSample* sample, const webrtc::EncodedImage& input_image);

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager_;
  Microsoft::WRL::ComPtr<IMFTransform> mft_;
  UINT reset_token_ = 0;
  bool mf_started_ = false;
  bool provides_samples_ = true;
  DWORD output_size_ = 0;
  // Coded size (rows of the NV12 surface, e.g. 1088) and the visible size.
  UINT32 coded_width_ = 0, coded_height_ = 0;
  UINT32 width_ = 0, height_ = 0;
  // Size from the receiver settings, given to the MFT up front (some drivers
  // only take the DXVA path once they know the resolution).
  UINT32 hint_width_ = 0, hint_height_ = 0;
  int delivered_ = 0;
  webrtc::DecodedImageCallback* callback_ = nullptr;
  webrtc::VideoFrameBufferPool buffer_pool_;
};

}  // namespace livekit_ffi

#endif  // LIVEKIT_MF_H264_DECODER_H_
