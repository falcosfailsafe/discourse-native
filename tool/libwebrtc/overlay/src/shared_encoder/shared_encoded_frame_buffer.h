// A webrtc::VideoFrame buffer that carries an already-encoded H.264 access
// unit instead of pixels. The shared NVENC session produces one per captured
// frame; the track source hands the same buffer to every peer connection.
#ifndef DISCOURSE_SHARED_ENCODER_SHARED_ENCODED_FRAME_BUFFER_H_
#define DISCOURSE_SHARED_ENCODER_SHARED_ENCODED_FRAME_BUFFER_H_

#include <memory>

#include "api/scoped_refptr.h"
#include "api/video/video_frame_buffer.h"
#include "passthrough_core.h"
#include "rtc_base/ref_counted_object.h"

namespace discourse {
namespace shared_encoder {

class SharedEncodedFrameBuffer : public webrtc::VideoFrameBuffer {
 public:
  static constexpr char kStorageRepresentation[] = "discourse-shared-encoded-h264";

  static webrtc::scoped_refptr<SharedEncodedFrameBuffer> Create(
      std::shared_ptr<const SharedEncodedFrame> frame) {
    return webrtc::make_ref_counted<SharedEncodedFrameBuffer>(std::move(frame));
  }

  // Returns the payload when `buffer` is one of ours, otherwise null.
  static const SharedEncodedFrame* From(webrtc::VideoFrameBuffer* buffer) {
    // No RTTI in the libwebrtc build: recognise our own buffers by their
    // marker. Only this class returns it for a kNative buffer.
    if (!buffer || buffer->type() != Type::kNative) return nullptr;
    if (buffer->storage_representation() != kStorageRepresentation) {
      return nullptr;
    }
    return static_cast<SharedEncodedFrameBuffer*>(buffer)->frame_.get();
  }

  Type type() const override { return Type::kNative; }
  int width() const override { return frame_->width; }
  int height() const override { return frame_->height; }

  // There are no pixels. Anything that asks for them (a software encoder
  // chosen by mistake, a local renderer) gets null and must fail closed; the
  // frame is never turned into a different codec's input.
  webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override {
    return nullptr;
  }
  // Identity only. Any real crop or scale needs pixels, so it fails closed.
  webrtc::scoped_refptr<webrtc::VideoFrameBuffer> CropAndScale(
      int offset_x, int offset_y, int crop_width, int crop_height,
      int scaled_width, int scaled_height) override {
    if (offset_x == 0 && offset_y == 0 && crop_width == width() &&
        crop_height == height() && scaled_width == width() &&
        scaled_height == height()) {
      return webrtc::scoped_refptr<webrtc::VideoFrameBuffer>(this);
    }
    return nullptr;
  }
  std::string storage_representation() const override {
    return kStorageRepresentation;
  }

 explicit SharedEncodedFrameBuffer(
      std::shared_ptr<const SharedEncodedFrame> frame)
      : frame_(std::move(frame)) {}

 private:
  const std::shared_ptr<const SharedEncodedFrame> frame_;
};

}  // namespace shared_encoder
}  // namespace discourse

#endif  // DISCOURSE_SHARED_ENCODER_SHARED_ENCODED_FRAME_BUFFER_H_
