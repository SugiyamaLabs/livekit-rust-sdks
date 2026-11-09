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

#include "d3d11va_h264_decoder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
#include "libavutil/log.h"
}

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/h264_profile_level_id.h"
#include "api/video_codecs/video_decoder_software_fallback_wrapper.h"
#include "media/base/media_constants.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/convert.h"

namespace livekit_ffi {

namespace {

// LK_D3D11VA_LOG=<file>: plain-text trace (loading, device, chosen format).
void Trace(const char* fmt, ...) {
  char path[260];
  if (GetEnvironmentVariableA("LK_D3D11VA_LOG", path, sizeof(path)) == 0)
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

// The FFmpeg entry points used here, resolved from avcodec-61.dll /
// avutil-59.dll (FFmpeg 7.1) next to livekit_ffi.dll. Nothing is linked, so
// these never clash with the FFmpeg inside libwebrtc.
struct Ff {
  decltype(&avcodec_find_decoder) find_decoder;
  decltype(&avcodec_alloc_context3) alloc_context;
  decltype(&avcodec_open2) open;
  decltype(&avcodec_free_context) free_context;
  decltype(&avcodec_send_packet) send_packet;
  decltype(&avcodec_receive_frame) receive_frame;
  decltype(&av_packet_alloc) packet_alloc;
  decltype(&av_packet_free) packet_free;
  decltype(&av_frame_alloc) frame_alloc;
  decltype(&av_frame_free) frame_free;
  decltype(&av_frame_unref) frame_unref;
  decltype(&av_hwframe_transfer_data) transfer;
  decltype(&av_hwdevice_ctx_create) device_create;
  decltype(&av_buffer_ref) buffer_ref;
  decltype(&av_log_set_callback) log_set_callback;
  decltype(&av_log_set_level) log_set_level;
  // One D3D11VA device shared by every decoder.
  AVBufferRef* device = nullptr;
};

HMODULE LoadNextToUs(const wchar_t* name) {
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&LoadNextToUs), &self))
    return nullptr;
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
  std::wstring dir(path, n);
  dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
  return LoadLibraryExW((dir + name).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

template <typename T>
bool Resolve(HMODULE module, const char* name, T& out) {
  out = reinterpret_cast<T>(GetProcAddress(module, name));
  return out != nullptr;
}

// Loaded once, kept for the life of the process; null when unavailable.
Ff* Api() {
  static Ff* api = []() -> Ff* {
    if (std::getenv("LK_DISABLE_D3D11VA"))
      return nullptr;
    HMODULE util = LoadNextToUs(L"avutil-59.dll");
    HMODULE codec = util ? LoadNextToUs(L"avcodec-61.dll") : nullptr;
    Trace("load: avutil %p, avcodec %p (error %lu)", (void*)util, (void*)codec, GetLastError());
    if (!codec) {
      RTC_LOG(LS_INFO) << "D3D11VA: FFmpeg DLLs not found next to livekit_ffi.dll";
      return nullptr;
    }
    static Ff f;
    bool ok = Resolve(codec, "avcodec_find_decoder", f.find_decoder) &&
              Resolve(codec, "avcodec_alloc_context3", f.alloc_context) &&
              Resolve(codec, "avcodec_open2", f.open) &&
              Resolve(codec, "avcodec_free_context", f.free_context) &&
              Resolve(codec, "avcodec_send_packet", f.send_packet) &&
              Resolve(codec, "avcodec_receive_frame", f.receive_frame) &&
              Resolve(codec, "av_packet_alloc", f.packet_alloc) &&
              Resolve(codec, "av_packet_free", f.packet_free) &&
              Resolve(util, "av_frame_alloc", f.frame_alloc) &&
              Resolve(util, "av_frame_free", f.frame_free) &&
              Resolve(util, "av_frame_unref", f.frame_unref) &&
              Resolve(util, "av_hwframe_transfer_data", f.transfer) &&
              Resolve(util, "av_hwdevice_ctx_create", f.device_create) &&
              Resolve(util, "av_buffer_ref", f.buffer_ref) &&
              Resolve(util, "av_log_set_callback", f.log_set_callback) &&
              Resolve(util, "av_log_set_level", f.log_set_level);
    if (!ok) {
      RTC_LOG(LS_WARNING) << "D3D11VA: FFmpeg DLLs missing an entry point";
      return nullptr;
    }
    // FFmpeg's own messages go to the same trace (and nowhere else).
    f.log_set_level(AV_LOG_WARNING);
    f.log_set_callback([](void*, int level, const char* format, va_list args) {
      if (level > AV_LOG_WARNING)
        return;
      char line[512];
      vsnprintf(line, sizeof(line), format, args);
      Trace("ffmpeg[%d]: %s", level, line);
    });
    const int created = f.device_create(&f.device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
    Trace("entry points %d, device_create %d", ok ? 1 : 0, created);
    if (created < 0) {
      RTC_LOG(LS_INFO) << "D3D11VA: no D3D11 video device";
      return nullptr;
    }
    return &f;
  }();
  return api;
}

AVPixelFormat PickD3d11(AVCodecContext*, const AVPixelFormat* formats) {
  for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; f++) {
    if (*f == AV_PIX_FMT_D3D11) {
      Trace("get_format: D3D11");
      return *f;
    }
  }
  Trace("get_format: no D3D11 offered, first is %d", formats[0]);
  // No D3D11VA profile for this stream: decode in software inside FFmpeg; the
  // output path below handles YUV420P too.
  return formats[0];
}

}  // namespace

bool D3d11vaH264Decoder::IsAvailable() {
  return Api() != nullptr;
}

D3d11vaH264Decoder::D3d11vaH264Decoder() : buffer_pool_(false) {}

D3d11vaH264Decoder::~D3d11vaH264Decoder() {
  Release();
}

webrtc::VideoDecoder::DecoderInfo D3d11vaH264Decoder::GetDecoderInfo() const {
  DecoderInfo info;
  info.implementation_name = "FFmpeg D3D11VA H264";
  info.is_hardware_accelerated = true;
  return info;
}

bool D3d11vaH264Decoder::Configure(const Settings& settings) {
  Ff* ff = Api();
  if (!ff || settings.codec_type() != webrtc::kVideoCodecH264)
    return false;
  Release();
  const AVCodec* codec = ff->find_decoder(AV_CODEC_ID_H264);
  if (!codec)
    return false;
  context_ = ff->alloc_context(codec);
  if (!context_)
    return false;
  context_->hw_device_ctx = ff->buffer_ref(ff->device);
  context_->get_format = PickD3d11;
  context_->thread_count = 1;  // the GPU decodes; no CPU slice threads
  context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
  const int opened = ff->open(context_, codec, nullptr);
  Trace("configure: open %d", opened);
  if (opened < 0) {
    RTC_LOG(LS_WARNING) << "D3D11VA: avcodec_open2 failed";
    Release();
    return false;
  }
  packet_ = ff->packet_alloc();
  frame_ = ff->frame_alloc();
  download_ = ff->frame_alloc();
  return packet_ && frame_ && download_;
}

int32_t D3d11vaH264Decoder::RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) {
  callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t D3d11vaH264Decoder::Release() {
  Ff* ff = Api();
  if (ff) {
    if (frame_)
      ff->frame_free(&frame_);
    if (download_)
      ff->frame_free(&download_);
    if (packet_)
      ff->packet_free(&packet_);
    if (context_)
      ff->free_context(&context_);  // also drops its hw_device_ctx reference
  }
  buffer_pool_.Release();
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t D3d11vaH264Decoder::Decode(const webrtc::EncodedImage& input_image,
                                   bool /*missing_frames*/,
                                   int64_t /*render_time_ms*/) {
  Ff* ff = Api();
  if (!ff || !context_ || !callback_)
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  if (!input_image.data() || !input_image.size())
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;

  // FFmpeg reads up to AV_INPUT_BUFFER_PADDING_SIZE past the end: copy into a
  // padded buffer.
  thread_local std::vector<uint8_t> padded;
  padded.assign(input_image.data(), input_image.data() + input_image.size());
  padded.resize(input_image.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
  packet_->data = padded.data();
  packet_->size = static_cast<int>(input_image.size());
  packet_->pts = static_cast<int64_t>(input_image.RtpTimestamp());

  int ret = ff->send_packet(context_, packet_);
  packet_->data = nullptr;
  packet_->size = 0;
  if (ret < 0 && ret != AVERROR(EAGAIN)) {
    RTC_LOG(LS_WARNING) << "D3D11VA: send_packet failed, " << ret;
    Trace("send_packet %d (size %zu)", ret, input_image.size());
    return WEBRTC_VIDEO_CODEC_ERROR;  // asks for a keyframe
  }
  for (;;) {
    ret = ff->receive_frame(context_, frame_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      break;
    if (ret < 0) {
      RTC_LOG(LS_WARNING) << "D3D11VA: receive_frame failed, " << ret;
      Trace("receive_frame %d", ret);
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (traced_frames_ < 3) {
      Trace("frame %d: format %d, %dx%d", traced_frames_, frame_->format, frame_->width, frame_->height);
      traced_frames_++;
    }
    if (frame_->format == AV_PIX_FMT_D3D11) {
      // GPU surface → NV12 in memory (FFmpeg keeps one staging texture).
      if (ff->transfer(download_, frame_, 0) >= 0) {
        download_->pts = frame_->pts;
        Deliver(download_, input_image);
      }
      ff->frame_unref(download_);
    } else {
      Deliver(frame_, input_image);
    }
    ff->frame_unref(frame_);
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

void D3d11vaH264Decoder::Deliver(AVFrame* frame, const webrtc::EncodedImage& input_image) {
  const int w = frame->width;
  const int h = frame->height;
  if (w <= 0 || h <= 0)
    return;
  webrtc::scoped_refptr<webrtc::I420Buffer> i420 = buffer_pool_.CreateI420Buffer(w, h);
  if (!i420)
    return;
  if (frame->format == AV_PIX_FMT_NV12) {
    libyuv::NV12ToI420(frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1],
                       i420->MutableDataY(), i420->StrideY(), i420->MutableDataU(), i420->StrideU(),
                       i420->MutableDataV(), i420->StrideV(), w, h);
  } else if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
    libyuv::I420Copy(frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1],
                     frame->data[2], frame->linesize[2], i420->MutableDataY(), i420->StrideY(),
                     i420->MutableDataU(), i420->StrideU(), i420->MutableDataV(), i420->StrideV(), w, h);
  } else {
    return;
  }
  auto builder = webrtc::VideoFrame::Builder()
                     .set_video_frame_buffer(i420)
                     .set_timestamp_rtp(static_cast<uint32_t>(frame->pts));
  if (input_image.ColorSpace())
    builder.set_color_space(*input_image.ColorSpace());
  webrtc::VideoFrame out = builder.build();
  callback_->Decoded(out, std::nullopt, std::nullopt);
}

bool D3d11vaVideoDecoderFactory::IsSupported() {
  static bool supported = D3d11vaH264Decoder::IsAvailable();
  Trace("factory: supported %d", supported ? 1 : 0);
  return supported;
}

std::vector<webrtc::SdpVideoFormat> D3d11vaVideoDecoderFactory::GetSupportedFormats() const {
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

std::unique_ptr<webrtc::VideoDecoder> D3d11vaVideoDecoderFactory::Create(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format) {
  if (format.name != webrtc::kH264CodecName)
    return nullptr;
  auto hw = std::make_unique<D3d11vaH264Decoder>();
  if (auto sw = webrtc::H264Decoder::Create())
    return webrtc::CreateVideoDecoderSoftwareFallbackWrapper(env, std::move(sw), std::move(hw));
  return hw;
}

}  // namespace livekit_ffi
