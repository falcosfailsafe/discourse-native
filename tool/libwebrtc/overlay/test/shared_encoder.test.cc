// Tests the actual libwebrtc VideoEncoder adapter, not only its portable policy.
#include "api/video/video_frame.h"
#include "api/video/video_bitrate_allocation.h"
#include "api/video_codecs/video_codec.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "src/shared_encoder/passthrough_video_encoder.h"
#include "src/shared_encoder/shared_encoded_frame_buffer.h"
#include "test/gtest.h"

namespace discourse::shared_encoder {
namespace {

webrtc::VideoFrame Frame(uint64_t sequence, bool keyframe) {
  auto shared = std::make_shared<SharedEncodedFrame>();
  shared->sequence = sequence;
  shared->is_keyframe = keyframe;
  shared->width = 3840;
  shared->height = 1080;
  const std::vector<uint8_t> key = {0,0,0,1,0x67,0x11, 0,0,0,1,0x68,0x22,
                                    0,0,0,1,0x65,0x33};
  const std::vector<uint8_t> delta = {0,0,0,1,0x41,0x33};
  shared->data = std::make_shared<const std::vector<uint8_t>>(keyframe ? key : delta);
  return webrtc::VideoFrame::Builder()
      .set_video_frame_buffer(SharedEncodedFrameBuffer::Create(shared))
      .set_timestamp_us(static_cast<int64_t>(sequence) * 16667)
      .set_rtp_timestamp(static_cast<uint32_t>(sequence) * 1500)
      .build();
}

class Callback : public webrtc::EncodedImageCallback {
 public:
  Result OnEncodedImage(const webrtc::EncodedImage& image,
                        const webrtc::CodecSpecificInfo* info) override {
    ++calls;
    last_timestamp = image.RtpTimestamp();
    last_key = image.IsKey();
    EXPECT_EQ(image._encodedWidth, 3840u);
    EXPECT_EQ(image._encodedHeight, 1080u);
    EXPECT_EQ(info->codecType, webrtc::kVideoCodecH264);
    EXPECT_EQ(info->codecSpecific.H264.idr_frame, image.IsKey());
    Result result(fail ? Result::ERROR_SEND_FAILED : Result::OK);
    result.drop_next_frame = drop_next;
    return result;
  }
  void OnFrameDropped(uint32_t, int, bool) override {}
  int calls = 0;
  uint32_t last_timestamp = 0;
  bool last_key = false;
  bool fail = false;
  bool drop_next = false;
};

class PassthroughAdapterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    webrtc::VideoCodec codec;
    codec.codecType = webrtc::kVideoCodecH264;
    codec.width = 3840;
    codec.height = 1080;
    codec.maxFramerate = 60;
    const webrtc::VideoEncoder::Settings settings(
        webrtc::VideoEncoder::Capabilities(false), 1, 1200);
    ASSERT_EQ(encoder.InitEncode(&codec, settings), WEBRTC_VIDEO_CODEC_OK);
    ASSERT_EQ(encoder.RegisterEncodeCompleteCallback(&callback), WEBRTC_VIDEO_CODEC_OK);
  }
  PassthroughVideoEncoder encoder{nullptr};
  Callback callback;
};

TEST_F(PassthroughAdapterTest, PreservesPayloadMetadataAndIgnoresPeerKeyframeRequests) {
  std::vector<webrtc::VideoFrameType> request{webrtc::VideoFrameType::kVideoFrameKey};
  EXPECT_EQ(encoder.Encode(Frame(0, true), &request), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(encoder.Encode(Frame(1, false), &request), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(callback.calls, 2);
  EXPECT_EQ(callback.last_timestamp, 1500u);
  EXPECT_FALSE(callback.last_key);
  EXPECT_EQ(encoder.core().ignored_keyframe_requests(), 2u);
  webrtc::VideoEncoder::RateControlParameters rate(webrtc::VideoBitrateAllocation(), 1);
  encoder.SetRates(rate);
  EXPECT_EQ(encoder.core().ignored_rate_changes(), 1u);
  EXPECT_TRUE(encoder.GetEncoderInfo().has_trusted_rate_controller);
  EXPECT_TRUE(encoder.GetEncoderInfo().supports_native_handle);
  EXPECT_FALSE(encoder.GetEncoderInfo().scaling_settings.thresholds.has_value());
}

TEST_F(PassthroughAdapterTest, GapFreezesOnlyAffectedPeerUntilIdr) {
  ASSERT_EQ(encoder.Encode(Frame(0, true), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(encoder.Encode(Frame(2, false), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(encoder.Encode(Frame(3, false), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(callback.calls, 1);
  EXPECT_EQ(encoder.Encode(Frame(4, true), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(callback.calls, 2);
  EXPECT_TRUE(callback.last_key);
}

TEST_F(PassthroughAdapterTest, PacketizerFailureClosesGate) {
  callback.fail = true;
  EXPECT_NE(encoder.Encode(Frame(0, true), nullptr), WEBRTC_VIDEO_CODEC_OK);
  callback.fail = false;
  EXPECT_EQ(encoder.Encode(Frame(1, false), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(callback.calls, 1);
  EXPECT_EQ(encoder.Encode(Frame(2, true), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(callback.calls, 2);
}

TEST_F(PassthroughAdapterTest, DropSignalClosesGateAndReleaseRequiresNewKeyframe) {
  callback.drop_next = true;
  EXPECT_EQ(encoder.Encode(Frame(0, true), nullptr), WEBRTC_VIDEO_CODEC_OK);
  callback.drop_next = false;
  EXPECT_EQ(encoder.Encode(Frame(1, false), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(callback.calls, 1);
  EXPECT_EQ(encoder.Encode(Frame(2, true), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(callback.calls, 2);
  EXPECT_EQ(encoder.Release(), WEBRTC_VIDEO_CODEC_OK);
  SetUp();  // reinitialisation must still wait for a fresh IDR
  EXPECT_EQ(encoder.Encode(Frame(3, false), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(callback.calls, 2);
  EXPECT_EQ(encoder.Encode(Frame(4, true), nullptr), WEBRTC_VIDEO_CODEC_OK);
  EXPECT_EQ(callback.calls, 3);
}

}  // namespace
}  // namespace discourse::shared_encoder
