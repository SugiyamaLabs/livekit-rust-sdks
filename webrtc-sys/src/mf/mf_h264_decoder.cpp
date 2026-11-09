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

#include "mf_h264_decoder.h"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/convert.h"

using Microsoft::WRL::ComPtr;

namespace livekit_ffi {

namespace {

// CODECAPI_AVLowLatencyMode, spelled out so no extra GUID library is needed.
constexpr GUID kLowLatencyMode = {
    0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};

// CODECAPI_AVDecVideoAcceleration_H264: lets the MFT use DXVA (on by default
// on most systems, set explicitly so a policy/registry default does not win).
constexpr GUID kH264Acceleration = {
    0xf7db8a2f, 0x4f48, 0x4ee8, {0xae, 0x31, 0x8b, 0x6e, 0xbe, 0x55, 0x8a, 0xe2}};

// LK_MFDEC_LOG=<file>: plain-text trace of the decoder setup and of the first
// frames (whether DXVA is on, whether output samples are GPU surfaces).
void Trace(const char* fmt, ...) {
  char path[260];
  if (GetEnvironmentVariableA("LK_MFDEC_LOG", path, sizeof(path)) == 0)
    return;
  FILE* f = nullptr;
  if (fopen_s(&f, path, "a") != 0 || !f)
    return;
  va_list args;
  va_start(args, fmt);
  vfprintf(f, fmt, args);
  va_end(args);
  fputc('\n', f);
  fclose(f);
}

// WebRTC calls Configure and Decode from its own threads: COM must be up on
// each one. RPC_E_CHANGED_MODE (already STA) is fine for an in-proc MFT.
void EnsureCom() {
  thread_local bool done = false;
  if (!done) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    done = true;
  }
}

}  // namespace

MfH264Decoder::MfH264Decoder() : buffer_pool_(false) {}

MfH264Decoder::~MfH264Decoder() {
  Release();
}

webrtc::VideoDecoder::DecoderInfo MfH264Decoder::GetDecoderInfo() const {
  DecoderInfo info;
  info.implementation_name = "MediaFoundation H264 (D3D11)";
  info.is_hardware_accelerated = true;
  return info;
}

bool MfH264Decoder::Init() {
  EnsureCom();
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
    RTC_LOG(LS_WARNING) << "MF decoder: MFStartup failed";
    return false;
  }
  mf_started_ = true;

  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                 ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr, nullptr);
  if (hr == E_INVALIDARG)  // runtimes without 11_1
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels + 1,
                           ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION, &device_, nullptr, nullptr);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MF decoder: no D3D11 video device, hr=" << hr;
    return false;
  }
  ComPtr<ID3D10Multithread> mt;
  if (SUCCEEDED(device_.As(&mt)))
    mt->SetMultithreadProtected(TRUE);

  if (FAILED(MFCreateDXGIDeviceManager(&reset_token_, &manager_)) ||
      FAILED(manager_->ResetDevice(device_.Get(), reset_token_))) {
    RTC_LOG(LS_WARNING) << "MF decoder: DXGI device manager failed";
    return false;
  }

  hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(&mft_));
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MF decoder: H.264 MFT unavailable, hr=" << hr;
    return false;
  }
  ComPtr<IMFAttributes> attrs;
  UINT32 aware = 0;
  if (FAILED(mft_->GetAttributes(&attrs)) ||
      FAILED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware)) || !aware) {
    RTC_LOG(LS_WARNING) << "MF decoder: MFT is not D3D11 aware";
    return false;
  }
  attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
  ComPtr<ICodecAPI> codec_api;
  if (SUCCEEDED(mft_.As(&codec_api))) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = 1;
    HRESULT low = codec_api->SetValue(&kLowLatencyMode, &v);
    HRESULT accel = codec_api->SetValue(&kH264Acceleration, &v);
    Trace("init: low latency hr=0x%08lx, h264 acceleration hr=0x%08lx", low, accel);
  } else {
    Trace("init: no ICodecAPI");
  }
  // Without DXVA (no GPU decoder for H.264) this fails: the software MFT would
  // not beat FFmpeg by much, so the factory falls back instead.
  hr = mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                            reinterpret_cast<ULONG_PTR>(manager_.Get()));
  Trace("init: SET_D3D_MANAGER hr=0x%08lx", hr);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MF decoder: DXVA refused, hr=" << hr;
    return false;
  }

  ComPtr<IMFMediaType> in;
  MFCreateMediaType(&in);
  in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  if (hint_width_ > 0 && hint_height_ > 0)
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, hint_width_, hint_height_);
  hr = mft_->SetInputType(0, in.Get(), 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MF decoder: SetInputType failed, hr=" << hr;
    return false;
  }
  if (!SetOutputNv12())
    return false;
  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  return true;
}

bool MfH264Decoder::SetOutputNv12() {
  for (DWORD i = 0;; i++) {
    ComPtr<IMFMediaType> type;
    if (FAILED(mft_->GetOutputAvailableType(0, i, &type)))
      break;
    GUID sub{};
    type->GetGUID(MF_MT_SUBTYPE, &sub);
    if (sub != MFVideoFormat_NV12)
      continue;
    if (FAILED(mft_->SetOutputType(0, type.Get(), 0)))
      return false;
    MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &coded_width_, &coded_height_);
    width_ = coded_width_;
    height_ = coded_height_;
    MFVideoArea area{};
    if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area),
                                sizeof(area), nullptr)) &&
        area.Area.cx > 0 && area.Area.cy > 0) {
      width_ = static_cast<UINT32>(area.Area.cx);
      height_ = static_cast<UINT32>(area.Area.cy);
    }
    MFT_OUTPUT_STREAM_INFO info{};
    mft_->GetOutputStreamInfo(0, &info);
    provides_samples_ = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                         MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    output_size_ = info.cbSize;
    Trace("output NV12: coded %ux%u, visible %ux%u, provides samples %d, flags 0x%lx", coded_width_,
          coded_height_, width_, height_, provides_samples_ ? 1 : 0, info.dwFlags);
    return true;
  }
  // Before the first SPS the MFT may not offer NV12 yet; the stream change
  // after the first frames sets it.
  return true;
}

bool MfH264Decoder::Configure(const Settings& settings) {
  if (settings.codec_type() != webrtc::kVideoCodecH264)
    return false;
  if (settings.max_render_resolution().Valid()) {
    hint_width_ = static_cast<UINT32>(settings.max_render_resolution().Width());
    hint_height_ = static_cast<UINT32>(settings.max_render_resolution().Height());
  }
  if (mft_)
    return true;
  return Init();
}

int32_t MfH264Decoder::RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MfH264Decoder::Release() {
  if (mft_) {
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    mft_.Reset();
  }
  manager_.Reset();
  device_.Reset();
  buffer_pool_.Release();
  if (mf_started_) {
    MFShutdown();
    mf_started_ = false;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MfH264Decoder::Decode(const webrtc::EncodedImage& input_image,
                              bool /*missing_frames*/,
                              int64_t /*render_time_ms*/) {
  if (!mft_ || !callback_)
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  if (!input_image.data() || !input_image.size())
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  EnsureCom();

  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(input_image.size()), &buffer)))
    return WEBRTC_VIDEO_CODEC_ERROR;
  BYTE* dst = nullptr;
  buffer->Lock(&dst, nullptr, nullptr);
  memcpy(dst, input_image.data(), input_image.size());
  buffer->Unlock();
  buffer->SetCurrentLength(static_cast<DWORD>(input_image.size()));
  ComPtr<IMFSample> sample;
  MFCreateSample(&sample);
  sample->AddBuffer(buffer.Get());
  // The RTP timestamp rides through the MFT as the sample time.
  sample->SetSampleTime(static_cast<LONGLONG>(input_image.RtpTimestamp()));

  HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
  if (hr == MF_E_NOTACCEPTING) {
    if (!Drain(input_image))
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    hr = mft_->ProcessInput(0, sample.Get(), 0);
  }
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MF decoder: ProcessInput failed, hr=" << hr;
    return WEBRTC_VIDEO_CODEC_ERROR;  // asks for a keyframe
  }
  return Drain(input_image) ? WEBRTC_VIDEO_CODEC_OK : WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
}

bool MfH264Decoder::Drain(const webrtc::EncodedImage& input_image) {
  for (;;) {
    MFT_OUTPUT_DATA_BUFFER out{};
    ComPtr<IMFSample> own;
    if (!provides_samples_) {
      ComPtr<IMFMediaBuffer> b;
      MFCreateMemoryBuffer(output_size_, &b);
      MFCreateSample(&own);
      own->AddBuffer(b.Get());
      out.pSample = own.Get();
    }
    DWORD status = 0;
    HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
    if (out.pEvents)
      out.pEvents->Release();
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
      return true;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      if (!SetOutputNv12())
        return false;
      continue;
    }
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "MF decoder: ProcessOutput failed, hr=" << hr;
      return false;
    }
    if (out.pSample) {
      Deliver(out.pSample, input_image);
      if (provides_samples_)
        out.pSample->Release();
    }
  }
}

void MfH264Decoder::Deliver(IMFSample* sample, const webrtc::EncodedImage& input_image) {
  if (!width_ || !height_)
    return;
  LONGLONG time = 0;
  sample->GetSampleTime(&time);
  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(sample->GetBufferByIndex(0, &buffer)))
    return;

  if (delivered_ < 3) {
    ComPtr<IMFDXGIBuffer> dxgi;
    Trace("frame %d: %ux%u, GPU surface %d", delivered_, width_, height_,
          SUCCEEDED(buffer.As(&dxgi)) ? 1 : 0);
  }
  delivered_++;

  // On a DXGI buffer Lock2D reads the GPU surface back to memory.
  BYTE* data = nullptr;
  LONG pitch = 0;
  ComPtr<IMF2DBuffer> buffer2d;
  bool locked2d = SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&data, &pitch));
  DWORD length = 0;
  if (!locked2d) {
    if (FAILED(buffer->Lock(&data, nullptr, &length)))
      return;
    pitch = static_cast<LONG>(coded_width_);
  }

  webrtc::scoped_refptr<webrtc::I420Buffer> i420 =
      buffer_pool_.CreateI420Buffer(static_cast<int>(width_), static_cast<int>(height_));
  if (i420) {
    const uint8_t* uv = data + static_cast<size_t>(pitch) * coded_height_;
    libyuv::NV12ToI420(data, pitch, uv, pitch, i420->MutableDataY(), i420->StrideY(),
                       i420->MutableDataU(), i420->StrideU(), i420->MutableDataV(),
                       i420->StrideV(), static_cast<int>(width_), static_cast<int>(height_));
  }
  if (locked2d)
    buffer2d->Unlock2D();
  else
    buffer->Unlock();
  if (!i420)
    return;

  auto builder = webrtc::VideoFrame::Builder()
                     .set_video_frame_buffer(i420)
                     .set_timestamp_rtp(static_cast<uint32_t>(time));
  if (input_image.ColorSpace())
    builder.set_color_space(*input_image.ColorSpace());
  webrtc::VideoFrame frame = builder.build();
  callback_->Decoded(frame, std::nullopt, std::nullopt);
}

}  // namespace livekit_ffi
