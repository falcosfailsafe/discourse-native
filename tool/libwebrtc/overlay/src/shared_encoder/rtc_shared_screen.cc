#include "rtc_shared_screen.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "api/peer_connection_interface.h"
#include "api/transport/bitrate_settings.h"
#include "rtc_peerconnection_factory_impl.h"
#include "rtc_peerconnection_impl.h"
#include "rtc_video_source_impl.h"
#include "src/shared_encoder/shared_screen_pipeline.h"

namespace {

void CopyString(const std::string& from, char* to, int to_len) {
  if (!to || to_len <= 0) return;
  std::snprintf(to, static_cast<size_t>(to_len), "%s", from.c_str());
}

// Reaches the protected webrtc::PeerConnectionInterface of the wrapper's
// connection without changing its header.
struct PeerConnectionAccess : libwebrtc::RTCPeerConnectionImpl {
  static webrtc::PeerConnectionInterface* Get(
      libwebrtc::RTCPeerConnectionImpl* pc) {
    return (pc->*(&PeerConnectionAccess::rtc_peerconnection_)).get();
  }
};

}  // namespace

extern "C" {

int LibWebRTCSharedScreenApiVersion(void) {
  return LIBWEBRTC_SHARED_SCREEN_API_VERSION;
}

void LibWebRTCSharedScreenGetCapabilities(
    LibWebRTCSharedScreenCapabilities* out) {
  if (!out) return;
  std::memset(out, 0, sizeof *out);
  const auto& caps = discourse::shared_encoder::GetEncoderCapabilities();
  out->available = caps.available ? 1 : 0;
  CopyString(caps.reason, out->reason, sizeof out->reason);
  CopyString(caps.adapter_name, out->adapter_name, sizeof out->adapter_name);
  out->max_width = caps.max_width;
  out->max_height = caps.max_height;
  out->max_frame_rate = caps.max_frame_rate;
}

int LibWebRTCSharedScreenCreateSource(
    libwebrtc::RTCPeerConnectionFactory* factory, const char* source_id,
    const LibWebRTCSharedScreenRequest* request,
    libwebrtc::RTCVideoSource** out_source,
    LibWebRTCSharedScreenResolved* resolved, char* error, int error_len) {
  using namespace discourse::shared_encoder;
  if (out_source) *out_source = nullptr;
  if (!factory || !request || !out_source) {
    CopyString("bad arguments", error, error_len);
    return LIBWEBRTC_SHARED_SCREEN_INVALID_REQUEST;
  }
  SharedScreenRequest req;
  req.id = request->id ? request->id : "";
  req.max_height = request->max_height;
  req.source_scale = request->source_scale;
  req.frame_rate = request->frame_rate;
  req.bitrate_bps = request->bitrate_bps;
  req.keyframe_interval_seconds = request->keyframe_interval_seconds;
  req.codec = request->codec ? request->codec : "H264";

  auto* factory_impl =
      static_cast<libwebrtc::RTCPeerConnectionFactoryImpl*>(factory);
  std::string message;
  webrtc::scoped_refptr<SharedScreenTrackSource> source;
  SharedScreenPreset preset;
  // VideoTrackSource must be created and started on the signaling thread.
  const StartResult result = factory_impl->signaling_thread()->BlockingCall(
      [&] {
        return StartSharedScreen(source_id ? source_id : "", req, &source,
                                 &preset, &message);
      });
  if (result != StartResult::kStarted) {
    CopyString(message, error, error_len);
    return result == StartResult::kUnavailable
               ? LIBWEBRTC_SHARED_SCREEN_UNAVAILABLE
               : LIBWEBRTC_SHARED_SCREEN_INVALID_REQUEST;
  }

  libwebrtc::scoped_refptr<libwebrtc::RTCVideoSourceImpl> video_source(
      new libwebrtc::RefCountedObject<libwebrtc::RTCVideoSourceImpl>(
          webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>(source),
          static_cast<webrtc::internal::VideoCapturer*>(nullptr)));
  *out_source = video_source.release();

  if (resolved) {
    std::memset(resolved, 0, sizeof *resolved);
    resolved->width = preset.output_width;
    resolved->height = preset.output_height;
    resolved->frame_rate = preset.frame_rate;
    resolved->bitrate_bps = static_cast<long long>(preset.bitrate_kbps) * 1000;
    resolved->keyframe_interval_seconds = preset.keyframe_interval_ms / 1000;
    CopyString(preset.profile_level_id, resolved->profile_level_id,
               sizeof resolved->profile_level_id);
  }
  return LIBWEBRTC_SHARED_SCREEN_STARTED;
}

int LibWebRTCSharedScreenSetPeerBitrate(
    libwebrtc::RTCPeerConnection* peer_connection, int min_bps, int start_bps,
    int max_bps, char* error, int error_len) {
  if (!peer_connection) {
    CopyString("no peer connection", error, error_len);
    return 0;
  }
  webrtc::PeerConnectionInterface* pc = PeerConnectionAccess::Get(
      static_cast<libwebrtc::RTCPeerConnectionImpl*>(peer_connection));
  if (!pc) {
    CopyString("peer connection is closed", error, error_len);
    return 0;
  }
  webrtc::BitrateSettings settings;
  if (min_bps >= 0) settings.min_bitrate_bps = min_bps;
  if (start_bps >= 0) settings.start_bitrate_bps = start_bps;
  if (max_bps >= 0) settings.max_bitrate_bps = max_bps;
  const webrtc::RTCError rtc_error = pc->SetBitrate(settings);
  if (!rtc_error.ok()) {
    CopyString(rtc_error.message(), error, error_len);
    return 0;
  }
  return 1;
}

}  // extern "C"
