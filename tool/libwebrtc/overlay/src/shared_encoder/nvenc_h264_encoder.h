// NVENC H.264 session fed directly with D3D11 NV12 textures (no CPU copy of
// pixels). Written against the MIT-licensed nvEncodeAPI.h; nvEncodeAPI64.dll
// is loaded at run time so the binary still starts on machines without it.
//
// The D3D11 device and textures are passed as opaque pointers so this file has
// no D3D11 header dependency; they are `ID3D11Device*` / `ID3D11Texture2D*`.
#ifndef DISCOURSE_SHARED_ENCODER_NVENC_H264_ENCODER_H_
#define DISCOURSE_SHARED_ENCODER_NVENC_H264_ENCODER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "shared_screen_preset.h"

namespace discourse {
namespace shared_encoder {

struct EncodedAccessUnit {
  std::vector<uint8_t> data;  // Annex B
  bool is_keyframe = false;
  int64_t pts_us = 0;
};

class NvencH264Encoder {
 public:
  ~NvencH264Encoder();

  // Loads the NVENC runtime and opens (then closes) a throwaway session on
  // `d3d11_device` to prove H.264 encoding works. Never reports success
  // without actually opening a session.
  static bool Probe(void* d3d11_device, std::string* error);

  static std::unique_ptr<NvencH264Encoder> Create(
      void* d3d11_device, const SharedScreenPreset& preset,
      const std::vector<void*>& nv12_textures, std::string* error);

  // Encodes `nv12_textures[texture_index]` (must be one of the textures given
  // to Create, filled and idle on the GPU). Synchronous: no B-frames and no
  // lookahead, so the access unit is ready when this returns.
  bool Encode(size_t texture_index, int64_t pts_us, EncodedAccessUnit* out,
              std::string* error);

 private:
  NvencH264Encoder();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace shared_encoder
}  // namespace discourse

#endif  // DISCOURSE_SHARED_ENCODER_NVENC_H264_ENCODER_H_
