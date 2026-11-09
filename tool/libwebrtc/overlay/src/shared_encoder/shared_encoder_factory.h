// VideoEncoderFactory wrapper: everything goes to the wrapped factory except
// H.264 High level 5.1/5.2. Those formats are listed (last) whenever the
// wrapped factory has an H.264 encoder, so every peer can negotiate them
// whether or not it has NVENC. With a working NVENC session (probed lazily,
// when such an encoder is created) the encoder is the shared passthrough that
// still sends camera frames to the software encoder; without NVENC it is the
// stock software encoder itself.
#ifndef DISCOURSE_SHARED_ENCODER_SHARED_ENCODER_FACTORY_H_
#define DISCOURSE_SHARED_ENCODER_SHARED_ENCODER_FACTORY_H_

#include <memory>
#include <string>

#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"

namespace discourse {
namespace shared_encoder {

struct EncoderCapabilities {
  bool available = false;
  std::string reason;        // why not, when !available
  std::string adapter_name;
  int max_width = 0;
  int max_height = 0;
  int max_frame_rate = 0;
};

// Probed once per process on first use and cached (Windows + NVENC only).
const EncoderCapabilities& GetEncoderCapabilities();

std::unique_ptr<webrtc::VideoEncoderFactory> CreateSharedEncoderFactory(
    std::unique_ptr<webrtc::VideoEncoderFactory> inner);

// Receivers must be able to answer an offer for H.264 High level 5.1/5.2 even
// without NVENC: the stock decoder list stops at level 3.1, so the offer would
// otherwise match no decoder. Advertises those two formats (only when the
// wrapped factory decodes H.264) and decodes them with its H.264 decoder.
std::unique_ptr<webrtc::VideoDecoderFactory> CreateSharedDecoderFactory(
    std::unique_ptr<webrtc::VideoDecoderFactory> inner);

}  // namespace shared_encoder
}  // namespace discourse

#endif  // DISCOURSE_SHARED_ENCODER_SHARED_ENCODER_FACTORY_H_
