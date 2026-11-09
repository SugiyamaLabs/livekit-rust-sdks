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

#ifndef WEBRTC_NVIDIA_SIZE_SWITCHING_DECODER_H_
#define WEBRTC_NVIDIA_SIZE_SWITCHING_DECODER_H_

#include <memory>

#include "api/environment/environment.h"
#include "api/video_codecs/video_decoder.h"
#include "h264_decoder_impl.h"

namespace webrtc {

// H.264 decoder that uses NVDEC only for large streams. Each NVDEC session
// costs ~20 MB of host memory (frame buffers, CUDA), which for a thumbnail
// camera is more than FFmpeg spends decoding it; for 720p and up NVDEC saves
// real CPU. Switches on keyframes (a size change always comes with one), and
// CUDA is only initialized when the first large stream shows up.
class SizeSwitchingH264Decoder : public VideoDecoder {
 public:
  explicit SizeSwitchingH264Decoder(const Environment& env);
  ~SizeSwitchingH264Decoder() override;

  bool Configure(const Settings& settings) override;
  int32_t Decode(const EncodedImage& input_image,
                 bool missing_frames,
                 int64_t render_time_ms) override;
  int32_t RegisterDecodeCompleteCallback(DecodedImageCallback* callback) override;
  int32_t Release() override;
  DecoderInfo GetDecoderInfo() const override;

  // Width x height from which NVDEC is used.
  static constexpr int kMinHardwarePixels = 1280 * 720;

 private:
  bool UseHardware();
  void DropHardware();

  std::unique_ptr<VideoDecoder> software_;
  std::unique_ptr<NvidiaH264DecoderImpl> hardware_;
  VideoDecoder* current_ = nullptr;
  bool cuda_held_ = false;
  bool session_held_ = false;
  bool hardware_failed_ = false;
  Settings settings_;
  DecodedImageCallback* callback_ = nullptr;
  H264BitstreamParserEx parser_;
};

}  // namespace webrtc

#endif  // WEBRTC_NVIDIA_SIZE_SWITCHING_DECODER_H_
