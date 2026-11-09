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

#ifndef LIVEKIT_AMF_H264_DECODER_H_
#define LIVEKIT_AMF_H264_DECODER_H_

#include "AMF/public/include/components/VideoDecoderUVD.h"
#include "AMF/public/include/core/Factory.h"
#include "api/video_codecs/video_decoder.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "common_video/include/video_frame_buffer_pool.h"

namespace livekit_ffi {

// H.264 decoding on AMD GPUs through AMF (amfrt64.dll, installed with the AMD
// driver and loaded at runtime). Decoded NV12 surfaces are downloaded and
// handed to WebRTC as I420, like the NVDEC decoder does.
class AmfH264Decoder : public webrtc::VideoDecoder {
 public:
  // The AMF factory from amfrt64.dll, loaded once; null without an AMD driver.
  static amf::AMFFactory* Factory();

  AmfH264Decoder();
  ~AmfH264Decoder() override;

  // Opens a context and an H.264 decoder at this size. Also the factory probe.
  bool Open(int width, int height);

  bool Configure(const Settings& settings) override;
  int32_t Decode(const webrtc::EncodedImage& input_image,
                 bool missing_frames,
                 int64_t render_time_ms) override;
  int32_t RegisterDecodeCompleteCallback(
      webrtc::DecodedImageCallback* callback) override;
  int32_t Release() override;
  DecoderInfo GetDecoderInfo() const override;

 private:
  // Exposes the last SPS (the decoder is sized from it).
  class Parser : public webrtc::H264BitstreamParser {
   public:
    std::optional<webrtc::SpsParser::SpsState> sps() const { return sps_; }
  };

  void Close();
  void Drain(const webrtc::EncodedImage& input_image);

  amf::AMFContextPtr context_;
  amf::AMFComponentPtr decoder_;
  int width_ = 0, height_ = 0;
  Parser parser_;
  webrtc::DecodedImageCallback* callback_ = nullptr;
  webrtc::VideoFrameBufferPool buffer_pool_;
};

}  // namespace livekit_ffi

#endif  // LIVEKIT_AMF_H264_DECODER_H_
