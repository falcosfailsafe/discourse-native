// Per-peer keyframe gate. Portable: no libwebrtc or Windows dependencies.
//
// Every frame the shared encoder produces carries a strictly increasing
// sequence number. A peer's passthrough encoder sees only the frames libwebrtc
// chose to hand it. If a frame was dropped for that peer (a gap in sequence
// numbers), or a delivery failed, the next delta frame would reference a
// picture the peer never received and decode as corruption. The gate closes
// on any discontinuity and reopens only on a random access point (a keyframe
// that carries its own SPS/PPS), so the peer freezes instead of corrupting.
#ifndef DISCOURSE_SHARED_ENCODER_KEYFRAME_GATE_H_
#define DISCOURSE_SHARED_ENCODER_KEYFRAME_GATE_H_

#include <cstdint>

namespace discourse {
namespace shared_encoder {

class KeyframeGate {
 public:
  enum class Verdict { kForward, kDrop };

  // Decides whether the frame with `sequence` may be sent to this peer.
  // `is_random_access_point` is true for a keyframe a decoder can join on.
  Verdict Admit(uint64_t sequence, bool is_random_access_point) {
    if (have_last_ && sequence != last_sequence_ + 1) {
      // Frames went missing (or time went backwards): close until a keyframe.
      Close();
    }
    have_last_ = true;
    last_sequence_ = sequence;

    if (!open_) {
      if (!is_random_access_point) {
        ++dropped_frames_;
        return Verdict::kDrop;
      }
      open_ = true;
      ++resumes_;
    }
    ++forwarded_frames_;
    return Verdict::kForward;
  }

  // Call when a forwarded frame was not delivered (encode callback failure,
  // encoder released/re-initialised). The next delta frame is unsafe.
  void NotifyDeliveryFailed() { Close(); }

  // Forget everything, e.g. when the peer's encoder is re-initialised.
  void Reset() {
    open_ = false;
    have_last_ = false;
    last_sequence_ = 0;
  }

  bool is_open() const { return open_; }
  uint64_t forwarded_frames() const { return forwarded_frames_; }
  uint64_t dropped_frames() const { return dropped_frames_; }
  // Number of times the gate reopened on a keyframe (including the first).
  uint64_t resumes() const { return resumes_; }
  uint64_t closes() const { return closes_; }

 private:
  void Close() {
    if (open_) ++closes_;
    open_ = false;
  }

  bool open_ = false;
  bool have_last_ = false;
  uint64_t last_sequence_ = 0;
  uint64_t forwarded_frames_ = 0;
  uint64_t dropped_frames_ = 0;
  uint64_t resumes_ = 0;
  uint64_t closes_ = 0;
};

}  // namespace shared_encoder
}  // namespace discourse

#endif  // DISCOURSE_SHARED_ENCODER_KEYFRAME_GATE_H_
