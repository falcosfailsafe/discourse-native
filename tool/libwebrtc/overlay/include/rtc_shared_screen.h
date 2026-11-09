// Shared-encode screen share entry points (Discourse fork).
//
// These are plain `extern "C"` functions with POD arguments on purpose: the
// Flutter plugin resolves them at run time with GetProcAddress, so a plugin
// built against this header still loads against a stock libwebrtc.dll that
// does not export them. A missing export means "shared encoder unavailable".
// Every function is safe to call on any platform; off Windows or without
// NVENC they report unavailable instead of failing.
#ifndef LIB_WEBRTC_RTC_SHARED_SCREEN_H_
#define LIB_WEBRTC_RTC_SHARED_SCREEN_H_

#include "rtc_peerconnection.h"
#include "rtc_peerconnection_factory.h"
#include "rtc_types.h"
#include "rtc_video_source.h"

extern "C" {

// Increase when the struct layouts below change.
#define LIBWEBRTC_SHARED_SCREEN_API_VERSION 1

typedef struct LibWebRTCSharedScreenCapabilities {
  int available;  // 1 only when a real capture + NVENC session opened
  char reason[256];
  char adapter_name[128];
  int max_width;
  int max_height;
  int max_frame_rate;
} LibWebRTCSharedScreenCapabilities;

typedef struct LibWebRTCSharedScreenRequest {
  const char* id;
  int max_height;        // exactly one of max_height / source_scale is set
  double source_scale;
  int frame_rate;
  long long bitrate_bps;
  int keyframe_interval_seconds;
  const char* codec;     // "H264"
} LibWebRTCSharedScreenRequest;

typedef struct LibWebRTCSharedScreenResolved {
  int width;
  int height;
  int frame_rate;
  long long bitrate_bps;
  int keyframe_interval_seconds;
  char profile_level_id[8];  // "640033" or "640034"
} LibWebRTCSharedScreenResolved;

enum {
  LIBWEBRTC_SHARED_SCREEN_STARTED = 0,
  // The encoder cannot run here (no NVENC, window source, device lost...).
  // The caller may fall back to ordinary screen capture.
  LIBWEBRTC_SHARED_SCREEN_UNAVAILABLE = 1,
  // The request itself is wrong. Do not fall back silently.
  LIBWEBRTC_SHARED_SCREEN_INVALID_REQUEST = 2,
};

LIB_WEBRTC_API int LibWebRTCSharedScreenApiVersion(void);

LIB_WEBRTC_API void LibWebRTCSharedScreenGetCapabilities(
    LibWebRTCSharedScreenCapabilities* out);

// Starts capture + NVENC and returns a video source. On success `*out_source`
// holds one reference the caller must adopt (wrap in a scoped_refptr, then
// Release once) or Release. `source_id` is a getDesktopSources id: "" is the
// primary monitor, any other id is used exactly as reported and is never
// mapped to a different monitor.
LIB_WEBRTC_API int LibWebRTCSharedScreenCreateSource(
    libwebrtc::RTCPeerConnectionFactory* factory, const char* source_id,
    const LibWebRTCSharedScreenRequest* request,
    libwebrtc::RTCVideoSource** out_source,
    LibWebRTCSharedScreenResolved* resolved, char* error, int error_len);

// PeerConnection::SetBitrate: min/start/max for every stream of this
// connection. A value < 0 leaves that field unchanged. Returns 1 on success.
LIB_WEBRTC_API int LibWebRTCSharedScreenSetPeerBitrate(
    libwebrtc::RTCPeerConnection* peer_connection, int min_bps, int start_bps,
    int max_bps, char* error, int error_len);

}  // extern "C"

#endif  // LIB_WEBRTC_RTC_SHARED_SCREEN_H_
