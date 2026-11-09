#include "flutter_shared_screen.h"

#include <cstring>
#include <functional>
#include <memory>
#include <string>

#include "task_runner.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include "rtc_peerconnection.h"
#include "rtc_peerconnection_factory.h"
#include "rtc_video_source.h"

// Mirrors rtc_shared_screen.h in the libwebrtc fork. Duplicated, not included,
// so the plugin builds against a stock libwebrtc header set too.
extern "C" {
struct SharedScreenCapabilitiesC {
  int available;
  char reason[256];
  char adapter_name[128];
  int max_width, max_height, max_frame_rate;
};
struct SharedScreenRequestC {
  const char* id;
  int max_height;
  double source_scale;
  int frame_rate;
  long long bitrate_bps;
  int keyframe_interval_seconds;
  const char* codec;
};
struct SharedScreenResolvedC {
  int width, height, frame_rate;
  long long bitrate_bps;
  int keyframe_interval_seconds;
  char profile_level_id[8];
};
}

namespace flutter_webrtc_plugin {
namespace {

using ApiVersionFn = int (*)();
using GetCapsFn = void (*)(SharedScreenCapabilitiesC*);
using CreateSourceFn = int (*)(RTCPeerConnectionFactory*, const char*,
                               const SharedScreenRequestC*, RTCVideoSource**,
                               SharedScreenResolvedC*, char*, int);
using SetBitrateFn = int (*)(RTCPeerConnection*, int, int, int, char*, int);

constexpr int kApiVersion = 1;
constexpr int kStarted = 0;
constexpr int kUnavailable = 1;

struct Api {
  GetCapsFn get_caps = nullptr;
  CreateSourceFn create = nullptr;
  SetBitrateFn set_bitrate = nullptr;
};

// Resolved once. All null when libwebrtc.dll lacks the exports (stock build)
// or has a different API version.
const Api& SharedApi() {
  static const Api api = [] {
    Api a;
#ifdef _WIN32
    HMODULE module = GetModuleHandleW(L"libwebrtc.dll");
    if (!module) return a;
    auto version = reinterpret_cast<ApiVersionFn>(
        GetProcAddress(module, "LibWebRTCSharedScreenApiVersion"));
    if (!version || version() != kApiVersion) return a;
    a.get_caps = reinterpret_cast<GetCapsFn>(
        GetProcAddress(module, "LibWebRTCSharedScreenGetCapabilities"));
    a.create = reinterpret_cast<CreateSourceFn>(
        GetProcAddress(module, "LibWebRTCSharedScreenCreateSource"));
    a.set_bitrate = reinterpret_cast<SetBitrateFn>(
        GetProcAddress(module, "LibWebRTCSharedScreenSetPeerBitrate"));
    if (!a.get_caps || !a.create || !a.set_bitrate) a = Api();
#endif
    return a;
  }();
  return api;
}

EncodableMap UnavailableCapabilities(const std::string& reason) {
  EncodableMap m;
  m[EncodableValue("available")] = EncodableValue(false);
  m[EncodableValue("codecs")] = EncodableValue(EncodableList());
  m[EncodableValue("reason")] = EncodableValue(reason);
  return m;
}

}  // namespace

FlutterSharedScreen::FlutterSharedScreen(FlutterWebRTCBase* base)
    : base_(base) {
  // Stop a stream's loopback audio as soon as its track is disposed.
  base_->local_track_removed_ = [this](const std::string& track_id) {
    auto it = loopbacks_.find(track_id);
    if (it == loopbacks_.end()) return;
    it->second.capturer->Stop();
    loopbacks_.erase(it);
  };
}

FlutterSharedScreen::~FlutterSharedScreen() {
  base_->local_track_removed_ = nullptr;
  for (auto& entry : loopbacks_) entry.second.capturer->Stop();
  loopbacks_.clear();
}

// All three methods run synchronously on the platform thread, like the other
// plugin methods. The first capabilities call opens a real capture and NVENC
// session and getSharedScreenMedia opens another: both block the platform
// thread for a moment, which is the price of having no background work that
// could outlive the plugin.

void FlutterSharedScreen::GetCapabilities(
    std::unique_ptr<MethodResultProxy> result) {
  const Api& api = SharedApi();
  if (!api.get_caps) {
    result->Success(EncodableValue(UnavailableCapabilities(
        "this libwebrtc build has no shared screen encoder")));
    return;
  }
  SharedScreenCapabilitiesC caps;
  std::memset(&caps, 0, sizeof caps);
  api.get_caps(&caps);
  EncodableMap m;
  if (caps.available) {
    m[EncodableValue("available")] = EncodableValue(true);
    m[EncodableValue("codecs")] =
        EncodableValue(EncodableList{EncodableValue(std::string("H264"))});
    m[EncodableValue("maxWidth")] = EncodableValue(caps.max_width);
    m[EncodableValue("maxHeight")] = EncodableValue(caps.max_height);
    m[EncodableValue("maxFrameRate")] = EncodableValue(caps.max_frame_rate);
  } else {
    m = UnavailableCapabilities(caps.reason);
  }
  result->Success(EncodableValue(m));
}

void FlutterSharedScreen::GetSharedScreenMedia(
    const EncodableMap& params, std::unique_ptr<MethodResultProxy> result) {
  const Api& api = SharedApi();
  if (!api.create) {
    result->Error("sharedScreenUnavailable",
                  "this libwebrtc build has no shared screen encoder");
    return;
  }
  const EncodableMap preset = findMap(params, "preset");
  if (preset.empty()) {
    result->Error("Bad Arguments", "preset is required");
    return;
  }
  const std::string id = findString(preset, "id");
  const std::string codec =
      preset.count(EncodableValue("codec")) ? findString(preset, "codec")
                                            : std::string("H264");
  // "" is the primary monitor; any other id is used exactly as given.
  const std::string source_id = findString(params, "sourceId");
  const bool capture_audio = findBoolean(params, "audio");

  SharedScreenRequestC request;
  std::memset(&request, 0, sizeof request);
  request.id = id.c_str();
  request.codec = codec.c_str();
  request.max_height = findInt(preset, "maxHeight");
  request.source_scale = findDouble(preset, "sourceScale");
  request.frame_rate = findInt(preset, "frameRate");
  request.bitrate_bps = static_cast<long long>(findLongInt(preset, "bitrate"));
  request.keyframe_interval_seconds = findInt(preset, "keyframeIntervalSeconds");

  SharedScreenResolvedC resolved;
  std::memset(&resolved, 0, sizeof resolved);
  char error[256] = {0};
  RTCVideoSource* raw = nullptr;
  const int code = api.create(base_->factory_.get(), source_id.c_str(),
                              &request, &raw, &resolved, error, sizeof error);
  if (code != kStarted || !raw) {
    // Unavailable = the caller may fall back to ordinary capture. Anything
    // else (a bad preset) is a real error.
    result->Error(code == kUnavailable ? "sharedScreenUnavailable"
                                       : "Bad Arguments",
                  error);
    return;
  }
  // Adopt the reference the export handed over.
  scoped_refptr<RTCVideoSource> video_source(raw);
  raw->Release();

  const std::string uuid = base_->GenerateUUID();
  scoped_refptr<RTCMediaStream> stream =
      base_->factory_->CreateStream(uuid.c_str());
  EncodableMap reply;
  reply[EncodableValue("streamId")] = EncodableValue(uuid);

  EncodableList audio_tracks;
  if (capture_audio) {
    RTCAudioOptions opts;
    opts.echo_cancellation = false;
    opts.auto_gain_control = false;
    opts.noise_suppression = false;
    scoped_refptr<RTCAudioSource> audio_source =
        base_->factory_->CreateAudioSource(
            ("shared_screen_loopback_" + base_->GenerateUUID()).c_str(),
            RTCAudioSource::SourceType::kCustom, opts);
    scoped_refptr<RTCAudioTrack> audio_track =
        base_->factory_->CreateAudioTrack(audio_source,
                                          base_->GenerateUUID().c_str());
    // Same default as getDisplayMedia: source "0" when none was chosen.
    std::unique_ptr<LoopbackCapturer> capturer =
        CreateLoopbackCapturer(source_id.empty() ? "0" : source_id);
    if (capturer && capturer->Start(audio_source)) {
      const std::string audio_id = audio_track->id().std_string();
      EncodableMap info;
      info[EncodableValue("id")] = EncodableValue(audio_id);
      info[EncodableValue("label")] = EncodableValue(audio_id);
      info[EncodableValue("kind")] =
          EncodableValue(audio_track->kind().std_string());
      info[EncodableValue("enabled")] = EncodableValue(audio_track->enabled());
      audio_tracks.push_back(EncodableValue(info));
      stream->AddTrack(audio_track);
      base_->local_tracks_[audio_id] = audio_track;
      loopbacks_[audio_id] = {std::move(capturer), audio_source};
    }  // else: no loopback available: continue without audio
  }
  reply[EncodableValue("audioTracks")] = EncodableValue(audio_tracks);

  scoped_refptr<RTCVideoTrack> track =
      base_->factory_->CreateVideoTrack(video_source, uuid.c_str());
  EncodableMap info;
  info[EncodableValue("id")] = EncodableValue(track->id().std_string());
  info[EncodableValue("label")] = EncodableValue(track->id().std_string());
  info[EncodableValue("kind")] = EncodableValue(track->kind().std_string());
  info[EncodableValue("enabled")] = EncodableValue(track->enabled());
  reply[EncodableValue("videoTracks")] =
      EncodableValue(EncodableList{EncodableValue(info)});

  stream->AddTrack(track);
  base_->local_tracks_[track->id().std_string()] = track;
  base_->video_sources_[track->id().std_string()] = {video_source};
  base_->local_streams_[uuid] = stream;

  // What was actually started, for the picker and for diagnostics.
  EncodableMap shared_info;
  shared_info[EncodableValue("width")] = EncodableValue(resolved.width);
  shared_info[EncodableValue("height")] = EncodableValue(resolved.height);
  shared_info[EncodableValue("frameRate")] = EncodableValue(resolved.frame_rate);
  shared_info[EncodableValue("bitrate")] =
      EncodableValue(static_cast<int64_t>(resolved.bitrate_bps));
  shared_info[EncodableValue("keyframeIntervalSeconds")] =
      EncodableValue(resolved.keyframe_interval_seconds);
  shared_info[EncodableValue("profileLevelId")] =
      EncodableValue(std::string(resolved.profile_level_id));
  reply[EncodableValue("sharedEncoder")] = EncodableValue(shared_info);
  result->Success(EncodableValue(reply));
}

void FlutterSharedScreen::SetPeerConnectionBitrate(
    const EncodableMap& params, std::unique_ptr<MethodResultProxy> result) {
  const Api& api = SharedApi();
  if (!api.set_bitrate) {
    result->Error("sharedScreenUnavailable",
                  "this libwebrtc build cannot set bandwidth estimation");
    return;
  }
  RTCPeerConnection* pc =
      base_->PeerConnectionForId(findString(params, "peerConnectionId"));
  if (!pc) {
    result->Error("setPeerBitrateFailed",
                  "peer connection not found");
    return;
  }
  // Absent keys stay unchanged (-1). Bits per second.
  auto value = [&](const char* key) {
    return params.count(EncodableValue(key))
               ? static_cast<int>(findLongInt(params, key))
               : -1;
  };
  char error[256] = {0};
  if (!api.set_bitrate(pc, value("minBitrate"), value("startBitrate"),
                       value("maxBitrate"), error, sizeof error)) {
    result->Error("setPeerBitrateFailed", error);
    return;
  }
  EncodableMap reply;
  reply[EncodableValue("result")] = EncodableValue(true);
  result->Success(EncodableValue(reply));
}

}  // namespace flutter_webrtc_plugin
