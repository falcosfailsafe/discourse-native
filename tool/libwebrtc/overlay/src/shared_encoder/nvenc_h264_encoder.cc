#include "nvenc_h264_encoder.h"

#include <cstring>

#include "ffnvcodec/nvEncodeAPI.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace discourse {
namespace shared_encoder {
namespace {

using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
using MaxVersionFn = NVENCSTATUS(NVENCAPI*)(uint32_t*);

std::string Status(const char* what, NVENCSTATUS s) {
  return std::string(what) + " failed (NVENCSTATUS " +
         std::to_string(static_cast<int>(s)) + ")";
}

// The NVENC runtime, loaded once per process and shared by every probe and
// session. The module is never unloaded (one reference for the whole process,
// not one per toggle) and the function table is stateless.
struct Runtime {
  void* module = nullptr;
  NV_ENCODE_API_FUNCTION_LIST fn = {};
  bool loaded = false;
  std::string error;

  Runtime() { loaded = Load(&error); }

  bool Load(std::string* out_error) {
#ifdef _WIN32
    // The driver installs the DLL in System32. Search only there so a DLL
    // planted in the current directory or next to the app is never loaded.
    module = reinterpret_cast<void*>(LoadLibraryExW(
        L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    const char* name = "nvEncodeAPI64.dll";
#else
    module = dlopen("libnvidia-encode.so.1", RTLD_NOW);
    const char* name = "libnvidia-encode.so.1";
#endif
    if (!module) {
      *out_error = std::string(name) + " not found (no NVIDIA driver with NVENC)";
      return false;
    }
    auto sym = [&](const char* s) -> void* {
#ifdef _WIN32
      return reinterpret_cast<void*>(
          GetProcAddress(reinterpret_cast<HMODULE>(module), s));
#else
      return dlsym(module, s);
#endif
    };
    auto max_version =
        reinterpret_cast<MaxVersionFn>(sym("NvEncodeAPIGetMaxSupportedVersion"));
    auto create =
        reinterpret_cast<CreateInstanceFn>(sym("NvEncodeAPICreateInstance"));
    if (!max_version || !create) {
      *out_error = "NVENC runtime lacks the expected entry points";
      return false;
    }
    uint32_t supported = 0;
    if (max_version(&supported) != NV_ENC_SUCCESS) {
      *out_error = "NvEncodeAPIGetMaxSupportedVersion failed";
      return false;
    }
    const uint32_t ours =
        (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (supported < ours) {
      *out_error = "NVIDIA driver is too old for NVENC API " +
               std::to_string(NVENCAPI_MAJOR_VERSION) + "." +
               std::to_string(NVENCAPI_MINOR_VERSION);
      return false;
    }
    std::memset(&fn, 0, sizeof fn);
    fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    const NVENCSTATUS s = create(&fn);
    if (s != NV_ENC_SUCCESS) {
      *out_error = Status("NvEncodeAPICreateInstance", s);
      return false;
    }
    return true;
  }
};

// Thread-safe (C++11 magic static); loads on first use only.
const Runtime& SharedRuntime() {
  static const Runtime runtime;
  return runtime;
}

// One NVENC session on a D3D11 device. Shared by the probe and the encoder.
struct Session {
  const Runtime& rt = SharedRuntime();
  void* encoder = nullptr;

  bool Open(void* d3d11_device, std::string* error) {
    if (!d3d11_device) {
      *error = "no D3D11 device";
      return false;
    }
    if (!rt.loaded) {
      *error = rt.error;
      return false;
    }
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS p = {};
    p.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    p.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    p.device = d3d11_device;
    p.apiVersion = NVENCAPI_VERSION;
    const NVENCSTATUS s = rt.fn.nvEncOpenEncodeSessionEx(&p, &encoder);
    if (s != NV_ENC_SUCCESS) {
      encoder = nullptr;
      *error = Status("nvEncOpenEncodeSessionEx", s);
      return false;
    }
    return true;
  }

  bool SupportsH264(std::string* error) {
    uint32_t count = 0;
    if (rt.fn.nvEncGetEncodeGUIDCount(encoder, &count) != NV_ENC_SUCCESS ||
        count == 0) {
      *error = "nvEncGetEncodeGUIDCount failed";
      return false;
    }
    std::vector<GUID> guids(count);
    uint32_t got = 0;
    if (rt.fn.nvEncGetEncodeGUIDs(encoder, guids.data(), count, &got) !=
        NV_ENC_SUCCESS) {
      *error = "nvEncGetEncodeGUIDs failed";
      return false;
    }
    for (uint32_t i = 0; i < got; ++i) {
      if (std::memcmp(&guids[i], &NV_ENC_CODEC_H264_GUID, sizeof(GUID)) == 0) {
        return true;
      }
    }
    *error = "GPU has no NVENC H.264 encoder";
    return false;
  }

  void Close() {
    if (encoder) {
      rt.fn.nvEncDestroyEncoder(encoder);
      encoder = nullptr;
    }
  }
  ~Session() { Close(); }
};

}  // namespace

struct NvencH264Encoder::Impl {
  Session session;
  SharedScreenPreset preset;
  int width = 0, height = 0;
  void* bitstream = nullptr;
  struct Registered {
    void* texture = nullptr;
    NV_ENC_REGISTERED_PTR handle = nullptr;
  };
  std::vector<Registered> inputs;
  bool force_idr_first = true;

  const NV_ENCODE_API_FUNCTION_LIST& fn() { return session.rt.fn; }

  ~Impl() {
    if (session.encoder) {
      for (auto& r : inputs) {
        if (r.handle) fn().nvEncUnregisterResource(session.encoder, r.handle);
      }
      if (bitstream) {
        fn().nvEncDestroyBitstreamBuffer(session.encoder, bitstream);
      }
    }
  }
};

NvencH264Encoder::NvencH264Encoder() : impl_(new Impl) {}
NvencH264Encoder::~NvencH264Encoder() = default;

bool NvencH264Encoder::Probe(void* d3d11_device, std::string* error) {
  Session s;
  if (!s.Open(d3d11_device, error)) return false;
  return s.SupportsH264(error);
}

std::unique_ptr<NvencH264Encoder> NvencH264Encoder::Create(
    void* d3d11_device, const SharedScreenPreset& preset,
    const std::vector<void*>& nv12_textures, std::string* error) {
  const std::string invalid = ValidatePreset(preset);
  if (!invalid.empty()) {
    *error = invalid;
    return nullptr;
  }
  std::unique_ptr<NvencH264Encoder> enc(new NvencH264Encoder());
  Impl& m = *enc->impl_;
  m.preset = preset;
  m.width = preset.output_width;
  m.height = preset.output_height;
  if (!m.session.Open(d3d11_device, error) || !m.session.SupportsH264(error)) {
    return nullptr;
  }
  const auto& fn = m.fn();
  void* e = m.session.encoder;

  NV_ENC_PRESET_CONFIG preset_config = {};
  preset_config.version = NV_ENC_PRESET_CONFIG_VER;
  preset_config.presetCfg.version = NV_ENC_CONFIG_VER;
  NVENCSTATUS s = fn.nvEncGetEncodePresetConfigEx(
      e, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID,
      NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset_config);
  if (s != NV_ENC_SUCCESS) {
    *error = Status("nvEncGetEncodePresetConfigEx", s);
    return nullptr;
  }
  NV_ENC_CONFIG config = preset_config.presetCfg;
  config.version = NV_ENC_CONFIG_VER;
  config.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;

  const uint32_t bitrate = static_cast<uint32_t>(preset.bitrate_kbps) * 1000u;
  const uint32_t gop = static_cast<uint32_t>(KeyframeIntervalFrames(preset));
  config.gopLength = gop;      // fixed interval; peers can't change it
  config.frameIntervalP = 1;   // I/P only: no B-frames
  config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  config.rcParams.averageBitRate = bitrate;
  config.rcParams.maxBitRate = bitrate;
  // Two frames of VBV: tight latency, but a keyframe is not starved.
  const uint32_t vbv = bitrate / static_cast<uint32_t>(preset.frame_rate) * 2;
  config.rcParams.vbvBufferSize = vbv;
  config.rcParams.vbvInitialDelay = vbv;
  config.rcParams.zeroReorderDelay = 1;
  config.rcParams.enableLookahead = 0;

  NV_ENC_CONFIG_H264& h264 = config.encodeCodecConfig.h264Config;
  h264.idrPeriod = gop;
  h264.repeatSPSPPS = 1;  // every IDR is a self-contained join point
  h264.disableSPSPPS = 0;
  h264.chromaFormatIDC = 1;
  h264.level = preset.profile_level_id == "640034" ? NV_ENC_LEVEL_H264_52
                                                   : NV_ENC_LEVEL_H264_51;
  h264.sliceMode = 0;
  h264.maxNumRefFrames = 1;
  // The video processor writes BT.709 limited range; say so in the stream.
  h264.h264VUIParameters.videoSignalTypePresentFlag = 1;
  h264.h264VUIParameters.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
  h264.h264VUIParameters.videoFullRangeFlag = 0;
  h264.h264VUIParameters.colourDescriptionPresentFlag = 1;
  h264.h264VUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
  h264.h264VUIParameters.transferCharacteristics =
      NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
  h264.h264VUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;

  NV_ENC_INITIALIZE_PARAMS init = {};
  init.version = NV_ENC_INITIALIZE_PARAMS_VER;
  init.encodeGUID = NV_ENC_CODEC_H264_GUID;
  init.presetGUID = NV_ENC_PRESET_P4_GUID;
  init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
  init.encodeWidth = init.darWidth = static_cast<uint32_t>(m.width);
  init.encodeHeight = init.darHeight = static_cast<uint32_t>(m.height);
  init.frameRateNum = static_cast<uint32_t>(preset.frame_rate);
  init.frameRateDen = 1;
  init.enablePTD = 1;
  init.encodeConfig = &config;
  s = fn.nvEncInitializeEncoder(e, &init);
  if (s != NV_ENC_SUCCESS) {
    *error = Status("nvEncInitializeEncoder", s);
    return nullptr;
  }

  NV_ENC_CREATE_BITSTREAM_BUFFER bs = {};
  bs.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
  s = fn.nvEncCreateBitstreamBuffer(e, &bs);
  if (s != NV_ENC_SUCCESS) {
    *error = Status("nvEncCreateBitstreamBuffer", s);
    return nullptr;
  }
  m.bitstream = bs.bitstreamBuffer;

  for (void* tex : nv12_textures) {
    NV_ENC_REGISTER_RESOURCE reg = {};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.width = static_cast<uint32_t>(m.width);
    reg.height = static_cast<uint32_t>(m.height);
    reg.pitch = 0;
    reg.resourceToRegister = tex;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    s = fn.nvEncRegisterResource(e, &reg);
    if (s != NV_ENC_SUCCESS) {
      *error = Status("nvEncRegisterResource", s);
      return nullptr;
    }
    m.inputs.push_back({tex, reg.registeredResource});
  }
  return enc;
}

bool NvencH264Encoder::Encode(size_t texture_index, int64_t pts_us,
                              EncodedAccessUnit* out, std::string* error) {
  Impl& m = *impl_;
  if (texture_index >= m.inputs.size()) {
    *error = "texture index out of range";
    return false;
  }
  const auto& fn = m.fn();
  void* e = m.session.encoder;

  NV_ENC_MAP_INPUT_RESOURCE map = {};
  map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
  map.registeredResource = m.inputs[texture_index].handle;
  NVENCSTATUS s = fn.nvEncMapInputResource(e, &map);
  if (s != NV_ENC_SUCCESS) {
    *error = Status("nvEncMapInputResource", s);
    return false;
  }

  NV_ENC_PIC_PARAMS pic = {};
  pic.version = NV_ENC_PIC_PARAMS_VER;
  pic.inputWidth = static_cast<uint32_t>(m.width);
  pic.inputHeight = static_cast<uint32_t>(m.height);
  pic.inputBuffer = map.mappedResource;
  pic.outputBitstream = m.bitstream;
  pic.bufferFmt = map.mappedBufferFmt;
  pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
  pic.inputTimeStamp = static_cast<uint64_t>(pts_us);
  if (m.force_idr_first) {
    pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    m.force_idr_first = false;
  }
  s = fn.nvEncEncodePicture(e, &pic);
  bool ok = s == NV_ENC_SUCCESS;
  if (!ok) *error = Status("nvEncEncodePicture", s);

  if (ok) {
    NV_ENC_LOCK_BITSTREAM lock = {};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = m.bitstream;
    lock.doNotWait = 0;
    s = fn.nvEncLockBitstream(e, &lock);
    if (s != NV_ENC_SUCCESS) {
      *error = Status("nvEncLockBitstream", s);
      ok = false;
    } else {
      const uint8_t* p = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
      out->data.assign(p, p + lock.bitstreamSizeInBytes);
      out->is_keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR;
      out->pts_us = pts_us;
      fn.nvEncUnlockBitstream(e, m.bitstream);
    }
  }
  fn.nvEncUnmapInputResource(e, map.mappedResource);
  return ok;
}

}  // namespace shared_encoder
}  // namespace discourse
