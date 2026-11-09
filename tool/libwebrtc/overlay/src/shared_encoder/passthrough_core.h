// Per-peer passthrough logic. Portable: the libwebrtc VideoEncoder adapter
// (passthrough_video_encoder.cc) is a thin shell around this.
#ifndef DISCOURSE_SHARED_ENCODER_PASSTHROUGH_CORE_H_
#define DISCOURSE_SHARED_ENCODER_PASSTHROUGH_CORE_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "h264_annexb.h"
#include "keyframe_gate.h"

namespace discourse {
namespace shared_encoder {

// One encoded access unit from the shared NVENC session. Immutable; shared by
// every peer so no per-peer copy of the bitstream is made.
struct SharedEncodedFrame {
  uint64_t sequence = 0;  // strictly +1 per frame from the encoder
  bool is_keyframe = false;  // as reported by the encoder (IDR picture)
  int width = 0;
  int height = 0;
  int64_t capture_time_us = 0;
  std::shared_ptr<const std::vector<uint8_t>> data;  // Annex B

  // A frame a decoder may join on. Trusts the bitstream, not only the flag:
  // an "IDR" without parameter sets is not a safe resume point.
  bool IsRandomAccessPoint() const {
    if (!is_keyframe || !data || data->empty()) return false;
    return ScanH264AnnexB(data->data(), data->size()).IsRandomAccessPoint();
  }
};

class PassthroughCore {
 public:
  // True when `frame` must be sent to this peer.
  bool ShouldForward(const SharedEncodedFrame& frame) {
    return gate_.Admit(frame.sequence, frame.IsRandomAccessPoint()) ==
           KeyframeGate::Verdict::kForward;
  }

  // libwebrtc rate control and keyframe requests are accepted and ignored: the
  // sender's preset is the only authority.
  void OnSetRates() { ++ignored_rate_changes_; }
  void OnKeyframeRequested() { ++ignored_keyframe_requests_; }

  void OnDeliveryFailed() { gate_.NotifyDeliveryFailed(); }
  void Reset() { gate_.Reset(); }

  const KeyframeGate& gate() const { return gate_; }
  uint64_t ignored_rate_changes() const { return ignored_rate_changes_; }
  uint64_t ignored_keyframe_requests() const {
    return ignored_keyframe_requests_;
  }

 private:
  KeyframeGate gate_;
  uint64_t ignored_rate_changes_ = 0;
  uint64_t ignored_keyframe_requests_ = 0;
};

}  // namespace shared_encoder
}  // namespace discourse

#endif  // DISCOURSE_SHARED_ENCODER_PASSTHROUGH_CORE_H_
