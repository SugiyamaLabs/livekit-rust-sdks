#include "nvidia_decoder_factory.h"

#include <modules/video_coding/codecs/h264/include/h264.h>

#include <cstdlib>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <memory>
#include <mutex>

#include "cuda_context.h"
#include "h264_decoder_impl.h"
#include "h265_decoder_impl.h"
#include "rtc_base/logging.h"
#if defined(_WIN32)
#include "size_switching_decoder.h"
#endif

namespace webrtc {

namespace {

constexpr char kDisableNvdecEnvVar[] = "LK_DISABLE_NVDEC";

bool IsNvdecDisabledByEnv() {
  return std::getenv(kDisableNvdecEnvVar) != nullptr;
}

void LogNvdecDisabledByEnv() {
  static std::once_flag log_once;
  std::call_once(log_once, [] {
    RTC_LOG(LS_INFO) << "NVIDIA NVDEC disabled because " << kDisableNvdecEnvVar
                     << " is set.";
  });
}

}  // namespace

constexpr char kSdpKeyNameCodecImpl[] = "implementation_name";
constexpr char kCodecName[] = "NvCodec";
#if defined(_WIN32)
constexpr char kNvdecRuntimeLibrary[] = "nvcuvid.dll";
#else
constexpr char kNvdecRuntimeLibrary[] = "libnvcuvid.so.1";
#endif

namespace {

bool IsNvdecRuntimeAvailable() {
#if defined(_WIN32)
  // Comes with the NVIDIA driver; the import itself is delay-loaded.
  HMODULE hModule = LoadLibraryA(kNvdecRuntimeLibrary);
#else
  void* hModule = dlopen(kNvdecRuntimeLibrary, RTLD_LAZY | RTLD_LOCAL);
#endif
  if (!hModule) {
    RTC_LOG(LS_WARNING) << "NVDEC runtime library (" << kNvdecRuntimeLibrary
                        << ") not found, hardware decoding unavailable.";
    return false;
  }

#if defined(_WIN32)
  FreeLibrary(hModule);
#else
  dlclose(hModule);
#endif
  return true;
}

}  // namespace

static int GetCudaDeviceCapabilityMajorVersion(CUcontext context) {
  cuCtxSetCurrent(context);

  CUdevice device;
  cuCtxGetDevice(&device);

  int major;
  cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR,
                       device);

  return major;
}

std::vector<SdpVideoFormat> SupportedNvDecoderCodecs(CUcontext context) {
  std::vector<SdpVideoFormat> supportedFormats;

  // HardwareGeneration Kepler is 3.x
  // https://docs.nvidia.com/deploy/cuda-compatibility/index.html#faq
  // Kepler support h264 profile Main, Highprofile up to Level4.1
  // https://docs.nvidia.com/video-technologies/video-codec-sdk/nvdec-video-decoder-api-prog-guide/index.html#video-decoder-capabilities__table_o3x_fms_3lb
  if (GetCudaDeviceCapabilityMajorVersion(context) <= 3) {
    supportedFormats = {
        CreateH264Format(webrtc::H264Profile::kProfileHigh,
                         webrtc::H264Level::kLevel4_1, "1"),
        CreateH264Format(webrtc::H264Profile::kProfileMain,
                         webrtc::H264Level::kLevel4_1, "1"),
    };
  } else {
    supportedFormats = {
        // Constrained Baseline Profile does not support NvDecoder, but WebRTC
        // uses this profile by default,
        // so it must be returned in this method.
        CreateH264Format(webrtc::H264Profile::kProfileConstrainedBaseline,
                         webrtc::H264Level::kLevel5_1, "1"),
        CreateH264Format(webrtc::H264Profile::kProfileBaseline,
                         webrtc::H264Level::kLevel5_1, "1"),
        CreateH264Format(webrtc::H264Profile::kProfileHigh,
                         webrtc::H264Level::kLevel5_1, "1"),
        CreateH264Format(webrtc::H264Profile::kProfileMain,
                         webrtc::H264Level::kLevel5_1, "1"),
        SdpVideoFormat("H265"),
        SdpVideoFormat("HEVC"),
    };
  }

  for (auto& format : supportedFormats) {
    format.parameters.emplace(kSdpKeyNameCodecImpl, kCodecName);
  }

  return supportedFormats;
}

NvidiaVideoDecoderFactory::NvidiaVideoDecoderFactory()
    : cu_context_(nullptr) {
  if (IsNvdecDisabledByEnv()) {
    LogNvdecDisabledByEnv();
    return;
  }

  if (!IsNvdecRuntimeAvailable()) {
    RTC_LOG(LS_INFO) << "NvidiaVideoDecoderFactory created without NVDEC "
                        "support because the runtime library is unavailable.";
    return;
  }

#if defined(_WIN32)
  // CUDA is initialized lazily, by the first stream large enough to use NVDEC
  // (SizeSwitchingH264Decoder): a call with only thumbnails never loads it.
  for (auto profile : {webrtc::H264Profile::kProfileConstrainedBaseline, webrtc::H264Profile::kProfileBaseline,
                       webrtc::H264Profile::kProfileHigh, webrtc::H264Profile::kProfileMain}) {
    auto format = CreateH264Format(profile, webrtc::H264Level::kLevel5_1, "1");
    format.parameters.emplace(kSdpKeyNameCodecImpl, kCodecName);
    supported_formats_.push_back(format);
  }
  return;
#endif
  cu_context_ = livekit_ffi::CudaContext::GetInstance();
  if (cu_context_->Initialize()) {
    supported_formats_ = SupportedNvDecoderCodecs(cu_context_->GetContext());
  } else {
    RTC_LOG(LS_ERROR) << "Failed to initialize CUDA context.";
    cu_context_ = nullptr;
  }
  RTC_LOG(LS_INFO) << "NvidiaVideoDecoderFactory created with "
                   << supported_formats_.size() << " supported formats.";
}

NvidiaVideoDecoderFactory::~NvidiaVideoDecoderFactory() {
  if (cu_context_) {
    // This ownership model relies on WebRTC releasing decoders before their
    // owning factory, so the factory lifetime bounds this context reference.
    RTC_LOG(LS_INFO) << "NvidiaVideoDecoderFactory destroyed; releasing CUDA "
                        "context.";
    cu_context_->Shutdown();
    cu_context_ = nullptr;
  }
}

bool NvidiaVideoDecoderFactory::IsSupported() {
  if (IsNvdecDisabledByEnv()) {
    LogNvdecDisabledByEnv();
    return false;
  }

#if defined(_WIN32)
  // Opt-in on Windows: the CUDA context costs ~60-70 MB of host memory, more
  // than the CPU it saves is worth by default (D3D11VA zero-copy is the default).
  if (!std::getenv("LK_ENABLE_NVDEC"))
    return false;
  // Only check that the driver libraries exist; cuInit waits for a large stream.
  if (HMODULE cuda = LoadLibraryA("nvcuda.dll")) {
    FreeLibrary(cuda);
  } else {
    return false;
  }
#else
  if (!livekit_ffi::CudaContext::IsAvailable()) {
    RTC_LOG(LS_WARNING) << "Cuda Context is not available.";
    return false;
  }
#endif

  if (!IsNvdecRuntimeAvailable()) {
    return false;
  }

  std::cout << "Nvidia Decoder is supported." << std::endl;
  return true;
}

std::unique_ptr<VideoDecoder> NvidiaVideoDecoderFactory::Create(
    const Environment& env,
    const SdpVideoFormat& format) {
#if defined(_WIN32)
  for (const auto& supported_format : supported_formats_) {
    if (format.IsSameCodec(supported_format) && format.name == "H264")
      return std::make_unique<SizeSwitchingH264Decoder>(env);
  }
  return nullptr;
#endif
  // Check if the requested format is supported.
  for (const auto& supported_format : supported_formats_) {
    if (format.IsSameCodec(supported_format)) {
      // If the format is supported, create and return the decoder.
      if (!cu_context_) {
        auto* ctx = livekit_ffi::CudaContext::GetInstance();
        if (!ctx->Initialize()) {
          RTC_LOG(LS_ERROR) << "Failed to initialize CUDA context.";
          return nullptr;
        }
        cu_context_ = ctx;
      }
      if (format.name == "H264") {
        RTC_LOG(LS_INFO) << "Using NVIDIA HW decoder (NVDEC) for H264";
        return std::make_unique<NvidiaH264DecoderImpl>(cu_context_->GetContext());
      }
      if (format.name == "H265" || format.name == "HEVC") {
        RTC_LOG(LS_INFO) << "Using NVIDIA HW decoder (NVDEC) for H265/HEVC";
        return std::make_unique<NvidiaH265DecoderImpl>(cu_context_->GetContext());
      }
    }
  }
  return nullptr;
}

std::vector<SdpVideoFormat> NvidiaVideoDecoderFactory::GetSupportedFormats()
    const {
  return supported_formats_;
}

}  // namespace webrtc
