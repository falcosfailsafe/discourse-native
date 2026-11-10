// Phase 2 acceptance at the native layer: one shared-encode screen share sent
// to several peers through the real libwebrtc.dll, then torn down.
//
//   * creates the shared source exactly as the Flutter plugin does
//     (LibWebRTCSharedScreenCreateSource, then CreateVideoTrack);
//   * N sender peer connections, each paired with a receiver peer connection
//     in the same process (loopback ICE), negotiate H.264 High 5.1+ the same
//     way MeshVoiceMediaSession does (codec preferences, SetPeerBitrate,
//     sender limits, then the track);
//   * every receiver decodes the stream and is measured (size, frame rate);
//   * nvidia-smi must show exactly one NVENC session however many peers watch;
//   * optional: a peer leaving mid-stream must not disturb the others, a late
//     joiner must start decoding at the next key frame, and the whole session
//     can be repeated to catch teardown crashes;
//   * --software sends synthetic frames through the ordinary software encoder
//     instead, to cover the fallback path and its teardown.
//
// Needs a display the session can duplicate (see README.md) for the shared
// path. Exit codes: 0 pass, 1 check failed, 2 usage, 3 shared encoder
// unavailable (reason printed).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "libwebrtc.h"
#include "rtc_logging.h"
#include "rtc_mediaconstraints.h"
#include "rtc_peerconnection.h"
#include "rtc_peerconnection_factory.h"
#include "rtc_rtp_capabilities.h"
#include "rtc_rtp_parameters.h"
#include "rtc_rtp_receiver.h"
#include "rtc_rtp_sender.h"
#include "rtc_rtp_transceiver.h"
#include "rtc_shared_screen.h"
#include "rtc_video_frame.h"
#include "rtc_video_source.h"
#include "rtc_video_track.h"
#include "smoke_util.h"

using namespace libwebrtc;
using clk = std::chrono::steady_clock;
#define STEP(msg) std::printf("    step: %s\n", msg)

namespace {

std::string g_log_filter;
void LogSink(const string& message) {
  const std::string m = message.std_string();
  if (g_log_filter.empty() || m.find(g_log_filter) != std::string::npos) {
    std::printf("    [webrtc] %s", m.c_str());
    if (m.empty() || m.back() != '\n') std::printf("\n");
  }
}

struct Options {
  std::string source_id;
  int receivers = 3;
  int seconds = 20;
  int fps = 60;
  int kbps = 25000;
  int keyframe_seconds = 2;
  double scale = 0.5;      // of the source; 7680x2160 -> 3840x1080
  int iterations = 1;
  int late_join_at = -1;   // seconds into the run
  int drop_peer_at = -1;
  bool software = false;
  std::string log_filter;  // print libwebrtc log lines containing this text
  std::string json_path = "shared_screen_peers.json";
};

// ---------------------------------------------------------------- helpers

std::string Std(const string& s) { return s.std_string(); }

std::string JsonField(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) return "";
  p = json.find(':', p);
  if (p == std::string::npos) return "";
  ++p;
  while (p < json.size() && (json[p] == ' ' || json[p] == '"')) ++p;
  size_t e = p;
  while (e < json.size() && json[e] != ',' && json[e] != '}' && json[e] != '"') ++e;
  return json.substr(p, e - p);
}

double ToD(const std::string& s) { return s.empty() ? 0.0 : std::atof(s.c_str()); }

struct Waiter {
  std::mutex m;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  std::string a, b;
  void Set(bool success, std::string x = "", std::string y = "") {
    std::lock_guard<std::mutex> l(m);
    ok = success;
    a = std::move(x);
    b = std::move(y);
    done = true;
    cv.notify_all();
  }
  bool Wait(int seconds) {
    std::unique_lock<std::mutex> l(m);
    return cv.wait_for(l, std::chrono::seconds(seconds), [this] { return done; }) &&
           ok;
  }
};

// ---------------------------------------------------------------- receiving

class Renderer : public RTCVideoRenderer<scoped_refptr<RTCVideoFrame>> {
 public:
  void OnFrame(scoped_refptr<RTCVideoFrame> frame) override {
    if (!frame.get()) return;
    width_ = frame->width();
    height_ = frame->height();
    if (frames_.fetch_add(1) == 0) first_ms_ = NowMs();
  }
  static int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               clk::now().time_since_epoch())
        .count();
  }
  std::atomic<int> frames_{0};
  std::atomic<int> width_{0};
  std::atomic<int> height_{0};
  std::atomic<int64_t> first_ms_{0};
};

struct Candidate {
  std::string mid;
  int mline;
  std::string sdp;
};

class Observer : public RTCPeerConnectionObserver {
 public:
  explicit Observer(Renderer* renderer, const char* label = "?")
      : renderer_(renderer), label_(label) {}
  void OnSignalingState(RTCSignalingState) override {}
  void OnPeerConnectionState(RTCPeerConnectionState s) override { state_ = s; }
  void OnIceGatheringState(RTCIceGatheringState) override {}
  void OnIceConnectionState(RTCIceConnectionState s) override {
    ice_ = s;
    std::printf("    [%s] ice connection state %d\n", label_, static_cast<int>(s));
  }
  void OnIceCandidate(scoped_refptr<RTCIceCandidate> c) override {
    std::lock_guard<std::mutex> l(m_);
    ++candidates_seen_;
    queue_.push_back({Std(c->sdp_mid()), c->sdp_mline_index(), Std(c->candidate())});
  }
  void OnAddStream(scoped_refptr<RTCMediaStream>) override {}
  void OnRemoveStream(scoped_refptr<RTCMediaStream>) override {}
  void OnDataChannel(scoped_refptr<RTCDataChannel>) override {}
  void OnRenegotiationNeeded() override {}
  void OnTrack(scoped_refptr<RTCRtpTransceiver> t) override {
    if (!renderer_ || t->media_type() != RTCMediaType::VIDEO) return;
    scoped_refptr<RTCMediaTrack> track = t->receiver()->track();
    if (!track.get()) return;
    video_track_ = scoped_refptr<RTCVideoTrack>(
        static_cast<RTCVideoTrack*>(track.get()));
    video_track_->AddRenderer(renderer_);
  }
  void OnAddTrack(vector<scoped_refptr<RTCMediaStream>>,
                  scoped_refptr<RTCRtpReceiver>) override {}
  void OnRemoveTrack(scoped_refptr<RTCRtpReceiver>) override {}

  std::vector<Candidate> Drain() {
    std::lock_guard<std::mutex> l(m_);
    std::vector<Candidate> out(queue_.begin(), queue_.end());
    queue_.clear();
    return out;
  }
  void DetachRenderer() {
    if (video_track_.get() && renderer_) video_track_->RemoveRenderer(renderer_);
    video_track_ = nullptr;
  }
  bool Connected() const {
    return ice_ == RTCIceConnectionStateConnected ||
           ice_ == RTCIceConnectionStateCompleted;
  }
  int candidates_seen() { std::lock_guard<std::mutex> l(m_); return candidates_seen_; }
  std::atomic<RTCPeerConnectionState> state_{RTCPeerConnectionStateNew};
  std::atomic<RTCIceConnectionState> ice_{RTCIceConnectionStateNew};

 private:
  Renderer* renderer_;
  const char* label_;
  int candidates_seen_ = 0;
  std::mutex m_;
  std::deque<Candidate> queue_;
  scoped_refptr<RTCVideoTrack> video_track_;
};

struct Peer {
  int index = 0;
  Renderer renderer;
  Observer send_obs{nullptr, "sender"};
  Observer recv_obs{&renderer, "receiver"};
  scoped_refptr<RTCPeerConnection> send, recv;
  scoped_refptr<RTCRtpTransceiver> transceiver;
  std::string negotiated;  // profile-level-id the sender settled on
  bool negotiated_high = false;
  bool closed = false;
  bool remote_set_ = false;
  int64_t added_ms = 0;
};

// ---------------------------------------------------------------- the test

class Harness {
 public:
  explicit Harness(const Options& o) : opt_(o) {}

  bool Init() {
    if (!LibWebRTC::Initialize()) return false;
    factory_ = LibWebRTC::CreateRTCPeerConnectionFactory();
    return factory_.get() && factory_->Initialize();
  }

  void Shutdown() {
    if (factory_.get()) factory_->Terminate();
    factory_ = nullptr;
    LibWebRTC::Terminate();
  }

  scoped_refptr<RTCPeerConnectionFactory> factory() { return factory_; }

  // Builds the codec preference list the way voice_screen_share.dart does:
  // H.264 High at level 5.1 or above first (highest level first), then the rest.
  static std::vector<scoped_refptr<RTCRtpCodecCapability>> Preferences(
      scoped_refptr<RTCRtpCapabilities> caps) {
    std::vector<std::pair<int, scoped_refptr<RTCRtpCodecCapability>>> high;
    std::vector<scoped_refptr<RTCRtpCodecCapability>> rest;
    const auto codecs = caps->codecs();
    for (size_t i = 0; i < codecs.size(); ++i) {
      auto c = codecs[i];
      std::string mime = Std(c->mime_type());
      std::string fmtp = Std(c->sdp_fmtp_line());
      std::transform(mime.begin(), mime.end(), mime.begin(), ::tolower);
      int level = 0;
      const size_t p = fmtp.find("profile-level-id=");
      if (mime == "video/h264" && p != std::string::npos &&
          fmtp.find("packetization-mode=1") != std::string::npos) {
        std::string id = fmtp.substr(p + 17, 6);
        if (id.size() == 6 && (id.substr(0, 2) == "64")) {
          level = std::strtol(id.substr(4, 2).c_str(), nullptr, 16);
        }
      }
      if (level >= 51) high.push_back({level, c}); else rest.push_back(c);
    }
    std::stable_sort(high.begin(), high.end(),
                     [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<scoped_refptr<RTCRtpCodecCapability>> out;
    for (auto& h : high) out.push_back(h.second);
    for (auto& r : rest) out.push_back(r);
    return out;
  }

  bool Negotiate(Peer* p, std::string* error) {
    auto constraints = RTCMediaConstraints::Create();
    // offer
    auto w1 = std::make_shared<Waiter>();
    Waiter* wp1 = w1.get();
    p->send->CreateOffer(
        [wp1](const string sdp, const string type) { wp1->Set(true, Std(sdp), Std(type)); },
        [wp1](const char* e) { wp1->Set(false, e ? e : "offer failed"); }, constraints);
    if (!w1->Wait(20)) { *error = "CreateOffer: " + w1->a; return false; }
    const std::string offer = w1->a, offer_type = w1->b;

    auto setlocal = [&](scoped_refptr<RTCPeerConnection> pc, const std::string& sdp,
                        const std::string& type, const char* what) {
      auto w = std::make_shared<Waiter>();
      Waiter* wp = w.get();
      pc->SetLocalDescription(sdp.c_str(), type.c_str(), [wp] { wp->Set(true); },
                              [wp](const char* e) { wp->Set(false, e ? e : ""); });
      if (!w->Wait(20)) { *error = std::string(what) + ": " + w->a; return false; }
      return true;
    };
    auto setremote = [&](scoped_refptr<RTCPeerConnection> pc, const std::string& sdp,
                         const std::string& type, const char* what) {
      auto w = std::make_shared<Waiter>();
      Waiter* wp = w.get();
      pc->SetRemoteDescription(sdp.c_str(), type.c_str(), [wp] { wp->Set(true); },
                               [wp](const char* e) { wp->Set(false, e ? e : ""); });
      if (!w->Wait(20)) { *error = std::string(what) + ": " + w->a; return false; }
      return true;
    };

    if (!setlocal(p->send, offer, offer_type, "sender SetLocalDescription")) return false;
    if (!setremote(p->recv, offer, offer_type, "receiver SetRemoteDescription")) return false;
    auto w2 = std::make_shared<Waiter>();
    Waiter* wp2 = w2.get();
    p->recv->CreateAnswer(
        [wp2](const string sdp, const string type) { wp2->Set(true, Std(sdp), Std(type)); },
        [wp2](const char* e) { wp2->Set(false, e ? e : "answer failed"); }, constraints);
    if (!w2->Wait(20)) { *error = "CreateAnswer: " + w2->a; return false; }
    const std::string answer = w2->a, answer_type = w2->b;
    if (!setlocal(p->recv, answer, answer_type, "receiver SetLocalDescription")) return false;
    if (!setremote(p->send, answer, answer_type, "sender SetRemoteDescription")) return false;
    p->remote_set_ = true;
    return true;
  }

  void PumpIce() {
    for (auto& p : peers_) {
      if (!p->remote_set_ || p->closed) continue;
      for (auto& c : p->send_obs.Drain()) {
        std::printf("    cand sender->receiver mid='%s' mline=%d '%s'\n", c.mid.c_str(), c.mline, c.sdp.c_str());
        p->recv->AddCandidate(c.mid.c_str(), c.mline, c.sdp.c_str());
      }
      for (auto& c : p->recv_obs.Drain()) {
        std::printf("    cand receiver->sender mid='%s' mline=%d '%s'\n", c.mid.c_str(), c.mline, c.sdp.c_str());
        p->send->AddCandidate(c.mid.c_str(), c.mline, c.sdp.c_str());
      }
    }
  }

  // Adds a viewer: its own sender and receiver connections, negotiated and
  // given the shared track.
  Peer* AddPeer(scoped_refptr<RTCVideoTrack> track, bool shared,
                const LibWebRTCSharedScreenResolved* resolved, std::string* error) {
    auto peer = std::make_unique<Peer>();
    peer->index = static_cast<int>(peers_.size());
    RTCConfiguration config;
    STEP("create constraints");
    auto constraints = RTCMediaConstraints::Create();
    STEP("create sender PeerConnection");
    peer->send = factory_->Create(config, constraints);
    STEP("create receiver PeerConnection");
    peer->recv = factory_->Create(config, RTCMediaConstraints::Create());
    if (!peer->send.get() || !peer->recv.get()) { *error = "PeerConnection create failed"; return nullptr; }
    STEP("register observers");
    peer->send->RegisterRTCPeerConnectionObserver(&peer->send_obs);
    peer->recv->RegisterRTCPeerConnectionObserver(&peer->recv_obs);

    STEP("create transceiver init");
    auto init = RTCRtpTransceiverInit::Create(
        RTCRtpTransceiverDirection::kSendOnly, vector<string>(),
        vector<scoped_refptr<RTCRtpEncodingParameters>>());
    STEP("AddTransceiver");
    peer->transceiver = peer->send->AddTransceiver(RTCMediaType::VIDEO, init);
    if (!peer->transceiver.get()) { *error = "AddTransceiver failed"; return nullptr; }
    if (shared) {
      STEP("codec preferences");
      auto prefs = Preferences(factory_->GetRtpSenderCapabilities(RTCMediaType::VIDEO));
      peer->transceiver->SetCodecPreferences(vector<scoped_refptr<RTCRtpCodecCapability>>(prefs));
    }
    Peer* raw = peer.get();
    peers_.push_back(std::move(peer));
    STEP("negotiate");
    if (!Negotiate(raw, error)) return nullptr;
    STEP("negotiated");

    // Did the sender settle on High 5.1+?
    auto params = raw->transceiver->sender()->parameters();
    auto codecs = params->codecs();
    for (size_t i = 0; i < codecs.size() && raw->negotiated.empty(); ++i) {
      const std::string name = Std(codecs[i]->name());
      if (name == "rtx" || name == "red" || name == "ulpfec" || name == "flexfec-03") continue;
      std::string plid, mode;
      auto kv = codecs[i]->parameters();
      for (size_t k = 0; k < kv.size(); ++k) {
        if (Std(kv[k].first) == "profile-level-id") plid = Std(kv[k].second);
        if (Std(kv[k].first) == "packetization-mode") mode = Std(kv[k].second);
      }
      raw->negotiated = name + " " + plid + " mode " + mode;
      raw->negotiated_high =
          (name == "H264" || name == "h264") && plid.size() == 6 &&
          plid.substr(0, 2) == "64" &&
          std::strtol(plid.substr(4, 2).c_str(), nullptr, 16) >= 51 && mode == "1";
    }

    if (shared) {
      char err[256] = {};
      const long long bps = resolved->bitrate_bps;
      if (!LibWebRTCSharedScreenSetPeerBitrate(raw->send.get(), static_cast<int>(bps),
                                               static_cast<int>(bps), static_cast<int>(bps),
                                               err, sizeof(err))) {
        *error = std::string("SetPeerBitrate: ") + err;
        return nullptr;
      }
      auto encodings = params->encodings();
      for (size_t i = 0; i < encodings.size(); ++i) {
        encodings[i]->set_max_bitrate_bps(static_cast<int>(bps));
        encodings[i]->set_max_framerate(resolved->frame_rate);
        encodings[i]->set_scale_resolution_down_by(1.0);
      }
      params->set_encodings(encodings);
      params->SetDegradationPreference(RTCDegradationPreference::MAINTAIN_RESOLUTION);
      if (!raw->transceiver->sender()->set_parameters(params)) {
        *error = "the sender limits were refused";
        return nullptr;
      }
    }
    raw->transceiver->sender()->set_track(track);
    raw->added_ms = Renderer::NowMs();
    return raw;
  }

  bool WaitConnected(Peer* p, int seconds) {
    const auto deadline = clk::now() + std::chrono::seconds(seconds);
    while (clk::now() < deadline) {
      PumpIce();
      if (p->send_obs.Connected() && p->recv_obs.Connected()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
  }

  std::string Stats(scoped_refptr<RTCPeerConnection> pc, const char* type) {
    auto w = std::make_shared<Waiter>();
    Waiter* wp = w.get();
    const std::string want = type;
    pc->GetStats(
        [wp, want](const vector<scoped_refptr<MediaRTCStats>> reports) {
          std::string found;
          for (size_t i = 0; i < reports.size(); ++i) {
            const std::string json = Std(reports[i]->ToJson());
            if (json.find("\"type\":\"" + want + "\"") != std::string::npos &&
                json.find("\"kind\":\"video\"") != std::string::npos) {
              found = json;
            }
          }
          wp->Set(true, found);
        },
        [wp](const char* e) { wp->Set(false, e ? e : ""); });
    return w->Wait(10) ? w->a : "";
  }

  void ClosePeer(Peer* p) {
    if (p->closed) return;
    p->closed = true;
    p->recv_obs.DetachRenderer();
    p->send->Close();
    p->recv->Close();
  }

  // The accessors the app calls during teardown on a connection that was
  // already closed. They used to dereference the released PeerConnection and
  // crash (libwebrtc.dll+0x218f7 in .3 and earlier); they must now be safe.
  static void TouchClosed(Peer* p) {
    (void)p->send->senders().size();
    (void)p->send->transceivers().size();
    (void)p->recv->receivers().size();
    (void)p->send->signaling_state();
    (void)p->send->ice_connection_state();
    (void)p->recv->peer_connection_state();
    (void)p->send->AddTransceiver(RTCMediaType::VIDEO);
  }

  void DisposePeers() {
    for (auto& p : peers_) {
      ClosePeer(p.get());
      TouchClosed(p.get());
      p->send->DeRegisterRTCPeerConnectionObserver();
      p->recv->DeRegisterRTCPeerConnectionObserver();
      factory_->Delete(p->send);
      factory_->Delete(p->recv);
      p->transceiver = nullptr;
      p->send = nullptr;
      p->recv = nullptr;
    }
    peers_.clear();
  }

  std::vector<std::unique_ptr<Peer>> peers_;

 private:
  Options opt_;
  scoped_refptr<RTCPeerConnectionFactory> factory_;
};

struct Verdict {
  std::vector<std::string> failures;
  void Check(bool ok, const std::string& what) {
    if (!ok) failures.push_back(what);
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what.c_str());
  }
};

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  // libwebrtc opens its sockets without initialising Winsock; the Flutter
  // runtime does that for the app, so a standalone process must do it itself.
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) smoke::Die("WSAStartup failed");
  Options opt;
  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* n) { return std::strcmp(argv[i], n) == 0 && i + 1 < argc; };
    if (arg("--source-id")) opt.source_id = argv[++i];
    else if (arg("--receivers")) opt.receivers = std::atoi(argv[++i]);
    else if (arg("--seconds")) opt.seconds = std::atoi(argv[++i]);
    else if (arg("--fps")) opt.fps = std::atoi(argv[++i]);
    else if (arg("--kbps")) opt.kbps = std::atoi(argv[++i]);
    else if (arg("--keyframe-seconds")) opt.keyframe_seconds = std::atoi(argv[++i]);
    else if (arg("--scale")) opt.scale = std::atof(argv[++i]);
    else if (arg("--iterations")) opt.iterations = std::atoi(argv[++i]);
    else if (arg("--late-join-at")) opt.late_join_at = std::atoi(argv[++i]);
    else if (arg("--drop-peer-at")) opt.drop_peer_at = std::atoi(argv[++i]);
    else if (arg("--json")) opt.json_path = argv[++i];
    else if (arg("--log-filter")) opt.log_filter = argv[++i];
    else if (std::strcmp(argv[i], "--software") == 0) opt.software = true;
    else { std::fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]); return 2; }
  }

  DWORD session = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  std::printf("session id: %lu, mode: %s, receivers %d, %d s, %d iteration(s)\n", session,
              opt.software ? "software" : "shared NVENC", opt.receivers, opt.seconds,
              opt.iterations);

  if (!opt.log_filter.empty() || std::getenv("PEERS_LOG")) {
    g_log_filter = opt.log_filter;
    LibWebRTCLogging::setLogSink(Info, &LogSink);
  }
  Harness h(opt);
  if (!h.Init()) smoke::Die("libwebrtc factory initialisation failed");

  LibWebRTCSharedScreenCapabilities caps = {};
  LibWebRTCSharedScreenGetCapabilities(&caps);
  std::printf("shared encoder: available=%d adapter=%s reason=%s\n", caps.available,
              caps.adapter_name, caps.reason);
  if (!opt.software && !caps.available) {
    std::printf("shared encoder unavailable on this machine: %s\n", caps.reason);
    h.Shutdown();
    return 3;
  }

  Verdict verdict;
  std::string json_runs;

  for (int iteration = 1; iteration <= opt.iterations; ++iteration) {
    std::printf("\n=== iteration %d/%d ===\n", iteration, opt.iterations);
    std::string error;
    scoped_refptr<RTCVideoSource> source;
    scoped_refptr<RTCVideoTrack> track;
    LibWebRTCSharedScreenResolved resolved = {};
    std::atomic<bool> stop_frames{false};
    std::thread frame_thread;

    if (!opt.software) {
      LibWebRTCSharedScreenRequest req = {};
      req.id = "peers-test";
      req.max_height = 0;
      req.source_scale = opt.scale;
      req.frame_rate = opt.fps;
      req.bitrate_bps = static_cast<long long>(opt.kbps) * 1000;
      req.keyframe_interval_seconds = opt.keyframe_seconds;
      req.codec = "H264";
      libwebrtc::RTCVideoSource* raw = nullptr;
      char err[256] = {};
      const int rc = LibWebRTCSharedScreenCreateSource(
          h.factory().get(), opt.source_id.c_str(), &req, &raw, &resolved, err, sizeof(err));
      if (rc != LIBWEBRTC_SHARED_SCREEN_STARTED || !raw) {
        std::printf("CreateSource failed (%d): %s\n", rc, err);
        verdict.Check(false, std::string("shared source started: ") + err);
        break;
      }
      source = scoped_refptr<RTCVideoSource>(raw);
      raw->Release();  // the plugin adopts the one reference, then releases it
      std::printf("source: %dx%d @ %d fps, %lld bps, %s, key frame every %d s\n", resolved.width,
                  resolved.height, resolved.frame_rate, resolved.bitrate_bps,
                  resolved.profile_level_id, resolved.keyframe_interval_seconds);
    } else {
      auto constraints = RTCMediaConstraints::Create();
      source = h.factory()->CreateCustomVideoSource("software-test", constraints);
      resolved.width = 1280;
      resolved.height = 720;
      resolved.frame_rate = 30;
      frame_thread = std::thread([&] {
        const int w = 1280, hgt = 720;
        std::vector<uint8_t> y(w * hgt), u(w * hgt / 4), v(w * hgt / 4);
        int n = 0;
        auto next = clk::now();
        while (!stop_frames) {
          for (int r = 0; r < hgt; ++r)
            for (int c = 0; c < w; ++c) y[r * w + c] = static_cast<uint8_t>(((c + n * 4) / 8 + r / 8) * 7);
          std::fill(u.begin(), u.end(), static_cast<uint8_t>(96 + (n % 64)));
          std::fill(v.begin(), v.end(), static_cast<uint8_t>(160 - (n % 64)));
          source->OnCapturedFrame(RTCVideoFrame::Create(w, hgt, y.data(), w, u.data(), w / 2,
                                                        v.data(), w / 2));
          ++n;
          next += std::chrono::milliseconds(33);
          std::this_thread::sleep_until(next);
        }
      });
    }
    STEP("CreateVideoTrack");
    std::printf("    source=%p refs-ok=%d\n", static_cast<void*>(source.get()), source.get() != nullptr);
    track = h.factory()->CreateVideoTrack(source, "peers-test-track");
    STEP("CreateVideoTrack returned");
    if (!track.get()) { verdict.Check(false, "video track created"); break; }

    // ---- peers
    bool setup_ok = true;
    for (int i = 0; i < opt.receivers && setup_ok; ++i) {
      Peer* p = h.AddPeer(track, !opt.software, &resolved, &error);
      if (!p) { verdict.Check(false, "peer " + std::to_string(i) + " set up: " + error); setup_ok = false; break; }
      std::printf("peer %d negotiated: %s\n", i, p->negotiated.c_str());
      if (!opt.software) verdict.Check(p->negotiated_high, "peer " + std::to_string(i) + " negotiated H.264 High 5.1+ (" + p->negotiated + ")");
    }
    if (setup_ok) {
      for (auto& p : h.peers_) {
        const bool up = h.WaitConnected(p.get(), 20);
        std::printf("  peer %d: candidates sender=%d receiver=%d, ice sender=%d receiver=%d, pc state sender=%d receiver=%d\n",
                    p->index, p->send_obs.candidates_seen(), p->recv_obs.candidates_seen(),
                    static_cast<int>(p->send_obs.ice_.load()), static_cast<int>(p->recv_obs.ice_.load()),
                    static_cast<int>(p->send_obs.state_.load()), static_cast<int>(p->recv_obs.state_.load()));
        verdict.Check(up, "peer " + std::to_string(p->index) + " ICE connected");
      }

      // ---- run, sampling once a second
      std::vector<std::vector<int>> fps_series(opt.receivers + 1);
      std::vector<int> last(opt.receivers + 1, 0);
      std::string smi;
      bool dropped = false, joined = false;
      int late_first_ms = -1;
      const auto start = clk::now();
      for (int s = 1; s <= opt.seconds; ++s) {
        const auto tick = start + std::chrono::seconds(s);
        while (clk::now() < tick) { h.PumpIce(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        for (size_t i = 0; i < h.peers_.size(); ++i) {
          const int now = h.peers_[i]->renderer.frames_.load();
          if (fps_series[i].size() < 4096) fps_series[i].push_back(now - last[i]);
          last[i] = now;
        }
        if (s == opt.seconds / 2 && !opt.software) {
          smi = smoke::RunCommand(
              "nvidia-smi --query-gpu=encoder.stats.sessionCount,utilization.encoder,"
              "utilization.gpu --format=csv,noheader 2>&1");
          std::printf("nvidia-smi at %d s: %s\n", s, smi.c_str());
        }
        if (opt.drop_peer_at == s && !dropped && !h.peers_.empty()) {
          std::printf("t=%d s: closing peer 0 (viewer leaves)\n", s);
          h.ClosePeer(h.peers_[0].get());
          Harness::TouchClosed(h.peers_[0].get());
          dropped = true;
        }
        if (opt.late_join_at == s && !joined) {
          std::printf("t=%d s: a late viewer joins\n", s);
          Peer* p = h.AddPeer(track, !opt.software, &resolved, &error);
          if (p) {
            fps_series.resize(std::max<size_t>(fps_series.size(), h.peers_.size()));
            last.resize(fps_series.size(), 0);
          } else {
            verdict.Check(false, "late peer set up: " + error);
          }
          joined = true;
        }
        if (joined && late_first_ms < 0 && h.peers_.size() > static_cast<size_t>(opt.receivers)) {
          Peer* lp = h.peers_.back().get();
          if (lp->renderer.frames_.load() > 0) {
            late_first_ms = static_cast<int>(lp->renderer.first_ms_.load() - lp->added_ms);
          }
        }
      }

      // ---- report per receiver
      std::printf("\nper-receiver frames per second (one value per second):\n");
      for (size_t i = 0; i < h.peers_.size(); ++i) {
        auto& series = fps_series[i];
        int total = 0, counted = 0;
        std::string line;
        for (int v : series) { line += std::to_string(v) + " "; }
        // steady state: skip the first 3 s while the key frame / bandwidth ramps
        for (size_t k = 3; k < series.size(); ++k) { total += series[k]; ++counted; }
        const double avg = counted ? static_cast<double>(total) / counted : 0.0;
        std::printf("  peer %zu: %dx%d decoded %d frames, steady avg %.1f fps | %s\n", i,
                    h.peers_[i]->renderer.width_.load(), h.peers_[i]->renderer.height_.load(),
                    h.peers_[i]->renderer.frames_.load(), avg, line.c_str());
        const bool is_dropped = dropped && i == 0;
        const bool is_late = i >= static_cast<size_t>(opt.receivers);
        if (is_dropped) continue;
        if (!opt.software && !is_late) {
          verdict.Check(h.peers_[i]->renderer.width_ == resolved.width &&
                            h.peers_[i]->renderer.height_ == resolved.height,
                        "peer " + std::to_string(i) + " decodes " + std::to_string(resolved.width) + "x" + std::to_string(resolved.height));
          verdict.Check(avg >= opt.fps * 0.5,
                        "peer " + std::to_string(i) + " steady rate " + std::to_string(avg).substr(0, 5) +
                            " fps (software decode at this size may trail the encoder; floor is half the target)");
        } else if (opt.software) {
          verdict.Check(h.peers_[i]->renderer.frames_ > 30, "peer " + std::to_string(i) + " received software-encoded video");
        } else if (is_late) {
          verdict.Check(h.peers_[i]->renderer.frames_ > 0, "late viewer decodes video");
        }
      }
      if (!opt.software) {
        verdict.Check(smi.find(", ") != std::string::npos && smi.compare(0, 2, "1,") == 0,
                      "exactly one NVENC session while " + std::to_string(h.peers_.size()) +
                          " peers watch (nvidia-smi: " + smi + ")");
        if (joined && late_first_ms >= 0) {
          std::printf("late viewer: first frame %d ms after joining (key frame every %d s)\n",
                      late_first_ms, opt.keyframe_seconds);
          verdict.Check(late_first_ms <= (opt.keyframe_seconds + 3) * 1000,
                        "late viewer starts within a key frame interval + handshake");
        }
        if (dropped) {
          // the others must not have noticed: compare each remaining peer's rate before/after
          for (size_t i = 1; i < static_cast<size_t>(opt.receivers); ++i) {
            const auto& series = fps_series[i];
            double before = 0, after = 0; int nb = 0, na = 0;
            for (size_t k = 3; k < series.size(); ++k) {
              if (static_cast<int>(k) + 1 <= opt.drop_peer_at) { before += series[k]; ++nb; }
              else if (static_cast<int>(k) + 1 > opt.drop_peer_at + 1) { after += series[k]; ++na; }
            }
            if (nb && na) {
              const double b = before / nb, a = after / na;
              std::printf("  peer %zu rate before leave %.1f, after %.1f\n", i, b, a);
              verdict.Check(a >= b * 0.8, "peer " + std::to_string(i) + " unaffected when peer 0 leaves");
            }
          }
        }
        // sender-side stats for the first live peer
        for (auto& p : h.peers_) {
          if (p->closed) continue;
          const std::string out = h.Stats(p->send, "outbound-rtp");
          std::printf("  sender %d outbound-rtp: encoder=%s %sx%s frames=%s fps=%s limit=%s bytes=%s\n",
                      p->index, JsonField(out, "encoderImplementation").c_str(),
                      JsonField(out, "frameWidth").c_str(), JsonField(out, "frameHeight").c_str(),
                      JsonField(out, "framesEncoded").c_str(), JsonField(out, "framesPerSecond").c_str(),
                      JsonField(out, "qualityLimitationReason").c_str(), JsonField(out, "bytesSent").c_str());
          verdict.Check(ToD(JsonField(out, "frameWidth")) == resolved.width,
                        "sender " + std::to_string(p->index) + " sends " + std::to_string(resolved.width) + " wide (never rescaled)");
          const std::string enc = JsonField(out, "encoderImplementation");
          if (!opt.software) verdict.Check(enc.find("SharedNVENC") != std::string::npos,
                                           "sender " + std::to_string(p->index) + " uses the shared NVENC passthrough encoder (reported as '" + enc + "')");
          break;
        }
      }
    }

    // ---- teardown, the scenario that used to crash
    std::printf("tearing down...\n");
    const auto t0 = clk::now();
    stop_frames = true;
    if (frame_thread.joinable()) frame_thread.join();
    h.DisposePeers();
    track = nullptr;
    source = nullptr;
    const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    std::printf("teardown completed in %.0f ms without a crash\n", ms);
    if (!opt.software) {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      const std::string after = smoke::RunCommand(
          "nvidia-smi --query-gpu=encoder.stats.sessionCount --format=csv,noheader 2>&1");
      std::printf("nvidia-smi after teardown: %s\n", after.c_str());
      verdict.Check(after.compare(0, 1, "0") == 0, "NVENC session released after teardown (sessionCount " + after + ")");
    }
    json_runs += std::string(json_runs.empty() ? "" : ", ") + "{\"iteration\": " +
                 std::to_string(iteration) + ", \"teardown_ms\": " + std::to_string(static_cast<int>(ms)) + "}";
  }

  h.Shutdown();
  std::printf("factory terminated cleanly\n");

  FILE* json = std::fopen(opt.json_path.c_str(), "wb");
  if (json) {
    std::fprintf(json, "{\"mode\": \"%s\", \"receivers\": %d, \"seconds\": %d, \"iterations\": [%s], \"failures\": [",
                 opt.software ? "software" : "shared", opt.receivers, opt.seconds, json_runs.c_str());
    for (size_t i = 0; i < verdict.failures.size(); ++i)
      std::fprintf(json, "%s\"%s\"", i ? ", " : "", smoke::JsonEscape(verdict.failures[i]).c_str());
    std::fprintf(json, "]}\n");
    std::fclose(json);
  }
  if (!verdict.failures.empty()) {
    std::printf("\nFAILED CHECKS:\n");
    for (auto& f : verdict.failures) std::printf("  - %s\n", f.c_str());
    return 1;
  }
  std::printf("\nALL CHECKS PASSED\n");
  return 0;
}
