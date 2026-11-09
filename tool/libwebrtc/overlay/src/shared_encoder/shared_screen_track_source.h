// The video track source for a shared-encode screen share. It owns the
// capture+encode pipeline and fans the encoded frames out to every sink (one
// per peer connection's video sender) through a VideoBroadcaster. It never
// resizes, adapts or drops: each peer's passthrough encoder decides, and each
// peer's keyframe gate absorbs whatever libwebrtc drops on the way.
#ifndef DISCOURSE_SHARED_ENCODER_SHARED_SCREEN_TRACK_SOURCE_H_
#define DISCOURSE_SHARED_ENCODER_SHARED_SCREEN_TRACK_SOURCE_H_

#include <atomic>
#include <memory>

#include "api/video/video_frame.h"
#include "media/base/video_broadcaster.h"
#include "pc/video_track_source.h"
#include "rtc_base/time_utils.h"
#include "shared_encoded_frame_buffer.h"

namespace discourse {
namespace shared_encoder {

// Whatever produces encoded frames; stops on destruction.
class EncodedFrameProducer {
 public:
  virtual ~EncodedFrameProducer() = default;
};

class SharedScreenTrackSource : public webrtc::VideoTrackSource {
 public:
  SharedScreenTrackSource() : webrtc::VideoTrackSource(/*remote=*/false) {}

  // Takes ownership of the producer; it runs until this source is destroyed.
  void SetProducer(std::unique_ptr<EncodedFrameProducer> producer) {
    producer_ = std::move(producer);
    SetState(kLive);
  }

  // Called from the producer's thread for every encoded access unit.
  void Publish(std::shared_ptr<const SharedEncodedFrame> frame) {
    webrtc::VideoFrame video_frame =
        webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(SharedEncodedFrameBuffer::Create(frame))
            .set_timestamp_us(frame->capture_time_us)
            .build();
    broadcaster_.OnFrame(video_frame);
  }

  // Screen content: tells libwebrtc to keep resolution over frame rate.
  bool is_screencast() const override { return true; }

 protected:
  ~SharedScreenTrackSource() override {
    // Stop the producer thread before the broadcaster goes away.
    producer_.reset();
  }

 private:
  webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return &broadcaster_;
  }

  webrtc::VideoBroadcaster broadcaster_;
  std::unique_ptr<EncodedFrameProducer> producer_;
};

}  // namespace shared_encoder
}  // namespace discourse

#endif  // DISCOURSE_SHARED_ENCODER_SHARED_SCREEN_TRACK_SOURCE_H_
