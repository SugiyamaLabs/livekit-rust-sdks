/*
 * Copyright 2025 LiveKit, Inc.
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

#include "livekit/apm.h"

#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"

#include <cstdlib>
#include <iostream>
#include <memory>

#ifdef _WIN32
// GetEnvironmentVariableA without windows.h: the CRT getenv() does not see
// variables the host (e.g. .NET) sets after startup.
extern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(
    const char* name, char* buffer, unsigned long size);
#endif

namespace livekit_ffi {

namespace {
// LK_APM_MOBILE_MODE=1: echo cancellation with AECM (lighter, worse at
// double talk) instead of AEC3. Read when each APM is created.
bool MobileModeRequested() {
#ifdef _WIN32
  char value[8];
  return GetEnvironmentVariableA("LK_APM_MOBILE_MODE", value, sizeof(value)) > 0 &&
         value[0] == '1';
#else
  const char* value = std::getenv("LK_APM_MOBILE_MODE");
  return value && value[0] == '1';
#endif
}
}  // namespace

AudioProcessingModule::AudioProcessingModule(
    const AudioProcessingConfig& config) {
  apm_ = webrtc::BuiltinAudioProcessingBuilder()
             .Build(webrtc::CreateEnvironment());

  webrtc::AudioProcessing::Config webrtc_config = config.ToWebrtcConfig();
  if (webrtc_config.echo_canceller.enabled && MobileModeRequested())
    webrtc_config.echo_canceller.mobile_mode = true;
  apm_->ApplyConfig(webrtc_config);
  apm_->Initialize();
}

int AudioProcessingModule::process_stream(const int16_t* src,
                                          size_t src_len,
                                          int16_t* dst,
                                          size_t dst_len,
                                          int sample_rate,
                                          int num_channels) {
  webrtc::StreamConfig stream_cfg(sample_rate, num_channels);
  return apm_->ProcessStream(src, stream_cfg, stream_cfg, dst);
}

int AudioProcessingModule::process_reverse_stream(const int16_t* src,
                                                  size_t src_len,
                                                  int16_t* dst,
                                                  size_t dst_len,
                                                  int sample_rate,
                                                  int num_channels) {
  webrtc::StreamConfig stream_cfg(sample_rate, num_channels);
  return apm_->ProcessReverseStream(src, stream_cfg, stream_cfg, dst);
}

int AudioProcessingModule::set_stream_delay_ms(int delay_ms) {
  return apm_->set_stream_delay_ms(delay_ms);
}

std::unique_ptr<AudioProcessingModule> create_apm(
    bool echo_canceller_enabled,
    bool gain_controller_enabled,
    bool high_pass_filter_enabled,
    bool noise_suppression_enabled) {
  AudioProcessingConfig config;
  config.echo_canceller_enabled = echo_canceller_enabled;
  config.gain_controller_enabled = gain_controller_enabled;
  config.high_pass_filter_enabled = high_pass_filter_enabled;
  config.noise_suppression_enabled = noise_suppression_enabled;
  return std::make_unique<AudioProcessingModule>(config);
}

}  // namespace livekit_ffi
