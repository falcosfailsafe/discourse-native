// Minimal H.264 Annex B scanner. Portable.
#ifndef DISCOURSE_SHARED_ENCODER_H264_ANNEXB_H_
#define DISCOURSE_SHARED_ENCODER_H264_ANNEXB_H_

#include <cstddef>
#include <cstdint>

namespace discourse {
namespace shared_encoder {

struct H264AccessUnitInfo {
  int nal_count = 0;
  bool has_sps = false;
  bool has_pps = false;
  bool has_idr = false;
  bool has_non_idr_slice = false;

  // A decoder can start here: an IDR picture plus its parameter sets.
  bool IsRandomAccessPoint() const { return has_idr && has_sps && has_pps; }
};

inline H264AccessUnitInfo ScanH264AnnexB(const uint8_t* data, size_t size) {
  H264AccessUnitInfo info;
  size_t i = 0;
  while (i + 3 <= size) {
    // Find 00 00 01 start code.
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      size_t nal = i + 3;
      if (nal < size) {
        const int type = data[nal] & 0x1f;
        ++info.nal_count;
        switch (type) {
          case 1: info.has_non_idr_slice = true; break;
          case 5: info.has_idr = true; break;
          case 7: info.has_sps = true; break;
          case 8: info.has_pps = true; break;
          default: break;
        }
      }
      i = nal;
    } else {
      ++i;
    }
  }
  return info;
}

}  // namespace shared_encoder
}  // namespace discourse

#endif  // DISCOURSE_SHARED_ENCODER_H264_ANNEXB_H_
