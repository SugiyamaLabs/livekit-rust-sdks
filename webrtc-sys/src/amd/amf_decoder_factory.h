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

#ifndef LIVEKIT_AMF_DECODER_FACTORY_H_
#define LIVEKIT_AMF_DECODER_FACTORY_H_

#include <memory>
#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder_factory.h"

namespace livekit_ffi {

// Hardware H.264 decoding on AMD GPUs (Windows) through AMF.
// Set LK_DISABLE_AMFDEC to turn it off.
class AmfVideoDecoderFactory : public webrtc::VideoDecoderFactory {
 public:
  // amfrt64.dll present and an H.264 decoder opens; probed once.
  static bool IsSupported();

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override;
  std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env,
                                               const webrtc::SdpVideoFormat& format) override;
};

}  // namespace livekit_ffi

#endif  // LIVEKIT_AMF_DECODER_FACTORY_H_
