#include "passthrough_video_encoder.h"

#include "api/video/encoded_image.h"
#include "api/video/video_codec_type.h"
#include "modules/video_coding/codecs/interface/common_constants.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "shared_encoded_frame_buffer.h"

namespace discourse {
namespace shared_encoder {

int PassthroughVideoEncoder::InitEncode(const webrtc::VideoCodec* codec_settings,
                                        const Settings& settings) {
  if (!codec_settings || codec_settings->codecType != webrtc::kVideoCodecH264) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  // A (re)initialised encoder has no picture history on the peer side that we
  // can vouch for: wait for the next keyframe. The software encoder, if it is
  // needed, is initialised lazily with these settings on the first pixel frame.
  core_.Reset();
  if (software_ && software_inited_) software_->Release();
  software_inited_ = false;
  software_needs_key_ = false;
  mode_ = Mode::kUnknown;
  codec_settings_ = *codec_settings;
  settings_ = settings;
  have_init_ = true;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t PassthroughVideoEncoder::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  callback_ = callback;
  if (software_ && software_inited_) {
    software_->RegisterEncodeCompleteCallback(callback);
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t PassthroughVideoEncoder::Release() {
  callback_ = nullptr;
  core_.Reset();
  if (software_ && software_inited_) software_->Release();
  software_inited_ = false;
  have_init_ = false;
  mode_ = Mode::kUnknown;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t PassthroughVideoEncoder::EncodeSoftware(
    const webrtc::VideoFrame& frame,
    const std::vector<webrtc::VideoFrameType>* frame_types) {
  if (!software_ || !have_init_) {
    // Fail closed: never pass pixels on as something they are not.
    RTC_LOG(LS_ERROR) << "PassthroughVideoEncoder got a frame with pixels and "
                         "has no software H.264 encoder; refusing.";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  if (!software_inited_) {
    const int rc = software_->InitEncode(&codec_settings_, settings_);
    if (rc != WEBRTC_VIDEO_CODEC_OK) return rc;
    software_->RegisterEncodeCompleteCallback(callback_);
    if (last_rates_) software_->SetRates(*last_rates_);
    software_inited_ = true;
  }
  if (software_needs_key_) {
    // Coming back from a shared-encode frame: the camera picture the peer
    // last saw is long gone, start it on a keyframe.
    software_needs_key_ = false;
    std::vector<webrtc::VideoFrameType> types(
        frame_types ? frame_types->size() : 1,
        webrtc::VideoFrameType::kVideoFrameKey);
    return software_->Encode(frame, &types);
  }
  return software_->Encode(frame, frame_types);
}

int32_t PassthroughVideoEncoder::Encode(
    const webrtc::VideoFrame& frame,
    const std::vector<webrtc::VideoFrameType>* frame_types) {
  if (!callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;

  const SharedEncodedFrame* shared =
      SharedEncodedFrameBuffer::From(frame.video_frame_buffer().get());
  if (!shared) {
    // Pixels (camera): ordinary software encoding, requests honoured.
    if (mode_ == Mode::kShared) software_needs_key_ = true;
    mode_ = Mode::kSoftware;
    return EncodeSoftware(frame, frame_types);
  }
  if (mode_ != Mode::kShared) {
    // First shared frame, or the sender switched from camera to screen: the
    // gate starts closed and waits for the next keyframe.
    core_.Reset();
    mode_ = Mode::kShared;
  }

  // libwebrtc asking for a keyframe (new peer, PLI/FIR, loss) is counted and
  // ignored: the sender's fixed interval is the only source of keyframes.
  if (frame_types) {
    for (auto type : *frame_types) {
      if (type == webrtc::VideoFrameType::kVideoFrameKey) {
        core_.OnKeyframeRequested();
        break;
      }
    }
  }

  if (!core_.ShouldForward(*shared)) {
    return WEBRTC_VIDEO_CODEC_OK;  // gate closed: this peer freezes
  }

  webrtc::EncodedImage image;
  image.SetEncodedData(webrtc::EncodedImageBuffer::Create(
      shared->data->data(), shared->data->size()));
  image._encodedWidth = shared->width;
  image._encodedHeight = shared->height;
  image.SetRtpTimestamp(frame.rtp_timestamp());
  image.capture_time_ms_ = frame.render_time_ms();
  image.ntp_time_ms_ = frame.ntp_time_ms();
  image.set_frame_type(shared->is_keyframe
                           ? webrtc::VideoFrameType::kVideoFrameKey
                           : webrtc::VideoFrameType::kVideoFrameDelta);
  image.SetSpatialIndex(0);

  webrtc::CodecSpecificInfo info;
  info.codecType = webrtc::kVideoCodecH264;
  info.codecSpecific.H264.packetization_mode =
      webrtc::H264PacketizationMode::NonInterleaved;
  info.codecSpecific.H264.temporal_idx = webrtc::kNoTemporalIdx;
  info.codecSpecific.H264.base_layer_sync = false;
  info.codecSpecific.H264.idr_frame = shared->is_keyframe;

  const auto result = callback_->OnEncodedImage(image, &info);
  if (result.error != webrtc::EncodedImageCallback::Result::OK) {
    core_.OnDeliveryFailed();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  if (result.drop_next_frame) {
    // libwebrtc will not send the frame after this one. A delta frame would
    // then reference a picture the peer never gets: close the gate now and
    // reopen on the next keyframe.
    core_.OnDeliveryFailed();
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

void PassthroughVideoEncoder::SetRates(const RateControlParameters& parameters) {
  core_.OnSetRates();  // the preset's rate is fixed for shared frames
  last_rates_ = parameters;
  if (software_ && software_inited_ && mode_ == Mode::kSoftware) {
    software_->SetRates(parameters);  // the camera keeps normal rate control
  }
}

webrtc::VideoEncoder::EncoderInfo PassthroughVideoEncoder::GetEncoderInfo()
    const {
  EncoderInfo info;
  if (software_ && mode_ == Mode::kSoftware && software_inited_) {
    // Camera frames: the software encoder's own behaviour (scaling, rate
    // control), but frames may still be native handles from a screen share.
    info = software_->GetEncoderInfo();
    info.supports_native_handle = true;
    return info;
  }
  info.implementation_name = "SharedNVENC";
  info.is_hardware_accelerated = true;
  info.supports_native_handle = true;      // frames carry no pixels
  info.supports_simulcast = false;
  info.scaling_settings = ScalingSettings::kOff;  // never resize
  info.has_trusted_rate_controller = true;        // libwebrtc must not drop for rate
  info.requested_resolution_alignment = 1;
  info.fps_allocation[0] = {EncoderInfo::kMaxFramerateFraction};
  return info;
}

}  // namespace shared_encoder
}  // namespace discourse
