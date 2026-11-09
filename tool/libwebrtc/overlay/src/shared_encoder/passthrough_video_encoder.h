// libwebrtc VideoEncoder for one peer connection's video sender.
//
// Shared-encode frames (SharedEncodedFrameBuffer) are forwarded as they are
// behind a keyframe gate: this encoder never touches their bytes beyond the
// hand-off to the packetizer, and ignores rate and keyframe requests.
//
// Frames with pixels (the camera, which shares this sender) go to an ordinary
// software H.264 encoder given at construction, so choosing H.264 High for the
// sender before sharing starts does not break the camera. Without such an
// encoder those frames fail closed. A shared-encode frame is never copied to
// the CPU and a pixel frame is never turned into shared output.
#ifndef DISCOURSE_SHARED_ENCODER_PASSTHROUGH_VIDEO_ENCODER_H_
#define DISCOURSE_SHARED_ENCODER_PASSTHROUGH_VIDEO_ENCODER_H_

#include <memory>
#include <optional>
#include <vector>

#include "api/video_codecs/video_encoder.h"
#include "passthrough_core.h"

namespace discourse {
namespace shared_encoder {

class PassthroughVideoEncoder : public webrtc::VideoEncoder {
 public:
  // `software` may be null: pixel frames are then rejected.
  explicit PassthroughVideoEncoder(std::unique_ptr<webrtc::VideoEncoder> software)
      : software_(std::move(software)) {}

  int InitEncode(const webrtc::VideoCodec* codec_settings,
                 const Settings& settings) override;
  int32_t RegisterEncodeCompleteCallback(
      webrtc::EncodedImageCallback* callback) override;
  int32_t Release() override;
  int32_t Encode(const webrtc::VideoFrame& frame,
                 const std::vector<webrtc::VideoFrameType>* frame_types)
      override;
  void SetRates(const RateControlParameters& parameters) override;
  EncoderInfo GetEncoderInfo() const override;

  const PassthroughCore& core() const { return core_; }

 private:
  enum class Mode { kUnknown, kShared, kSoftware };

  int32_t EncodeSoftware(const webrtc::VideoFrame& frame,
                         const std::vector<webrtc::VideoFrameType>* frame_types);

  PassthroughCore core_;
  webrtc::EncodedImageCallback* callback_ = nullptr;

  std::unique_ptr<webrtc::VideoEncoder> software_;
  bool have_init_ = false;
  webrtc::VideoCodec codec_settings_;
  Settings settings_{webrtc::VideoEncoder::Capabilities(false), 1, 0};
  bool software_inited_ = false;
  bool software_needs_key_ = false;
  std::optional<RateControlParameters> last_rates_;
  Mode mode_ = Mode::kUnknown;
};

}  // namespace shared_encoder
}  // namespace discourse

#endif  // DISCOURSE_SHARED_ENCODER_PASSTHROUGH_VIDEO_ENCODER_H_
