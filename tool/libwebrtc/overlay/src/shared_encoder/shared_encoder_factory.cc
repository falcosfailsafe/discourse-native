#include "shared_encoder_factory.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <mutex>
#include <optional>
#include <vector>

#include "api/video/resolution.h"
#include "api/video_codecs/sdp_video_format.h"
#include "passthrough_video_encoder.h"
#include "rtc_base/logging.h"
#include "shared_screen_pipeline.h"

namespace discourse {
namespace shared_encoder {
namespace {

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

// Any H.264 High (profile_idc 0x64) with packetization-mode 1, at any level or
// constraint flags. We advertise only levels 5.1 and 5.2, but SDP negotiation
// may pick a lower level of the same profile (level asymmetry is allowed), so
// those must reach the same software/passthrough path rather than the wrapped
// factory, which lists no High at all and would return null. Whether the
// shared screen path is allowed is the Dart side's level check.
bool IsSharedFormat(const webrtc::SdpVideoFormat& format) {
  if (Lower(format.name) != "h264") return false;
  auto mode = format.parameters.find("packetization-mode");
  if (mode == format.parameters.end() || mode->second != "1") return false;
  auto it = format.parameters.find("profile-level-id");
  if (it == format.parameters.end()) return false;
  const std::string id = Lower(it->second);
  return id.size() == 6 && id.compare(0, 2, "64") == 0;
}

// The encoder side takes no layering: no scalability mode, or plain L1T1.
bool NoLayering(const std::optional<std::string>& scalability_mode) {
  return !scalability_mode || *scalability_mode == "L1T1";
}

// A format the wrapped factory really lists, to build the H.264 codec behind a
// shared format: the first H.264 with packetization-mode 1 (stock lists have
// Baseline, Constrained Baseline and Main at level 3.1), else any H.264. Never
// a hard-coded format: the wrapped factory returns null for formats it does
// not list. Both stock encoder and decoder handle what High level 5.x senders
// and receivers need (a decoder reads any profile; the software camera
// encoder emits a subset every High decoder accepts).
std::optional<webrtc::SdpVideoFormat> FindStockH264Format(
    const std::vector<webrtc::SdpVideoFormat>& formats) {
  std::optional<webrtc::SdpVideoFormat> any;
  for (const auto& f : formats) {
    if (Lower(f.name) != "h264" || IsSharedFormat(f)) continue;
    auto mode = f.parameters.find("packetization-mode");
    if (mode != f.parameters.end() && mode->second == "1") return f;
    if (!any) any = f;
  }
  return any;
}

webrtc::SdpVideoFormat SharedFormat(const char* profile_level_id) {
  return webrtc::SdpVideoFormat(
      "H264", {{"level-asymmetry-allowed", "1"},
               {"packetization-mode", "1"},
               {"profile-level-id", profile_level_id}});
}

class SharedEncoderFactory : public webrtc::VideoEncoderFactory {
 public:
  explicit SharedEncoderFactory(
      std::unique_ptr<webrtc::VideoEncoderFactory> inner)
      : inner_(std::move(inner)),
        has_h264_(HasH264Impl(inner_->GetSupportedFormats())) {}

  // High 5.1/5.2 is a codec format any peer with a software H.264 encoder can
  // negotiate, with or without NVENC: one transceiver carries camera and
  // screen, and a receiver must be able to agree on High. Whether the shared
  // NVENC source can run is a separate question, asked only when a shared
  // source is created (StartSharedScreen), never when formats are listed. The
  // formats go last, so the stock codec order is unchanged.
  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    auto formats = inner_->GetSupportedFormats();
    if (has_h264_) {
      formats.push_back(SharedFormat("640033"));
      formats.push_back(SharedFormat("640034"));
    }
    return formats;
  }

  CodecSupport QueryCodecSupport(
      const webrtc::SdpVideoFormat& format,
      std::optional<std::string> scalability_mode,
      std::optional<webrtc::Resolution> resolution)
      const override {
    if (IsSharedFormat(format)) {
      CodecSupport support;
      support.is_supported = has_h264_ && NoLayering(scalability_mode);
      support.is_power_efficient = false;  // no probe just to answer a query
      return support;
    }
    return inner_->QueryCodecSupport(format, scalability_mode, resolution);
  }

  std::unique_ptr<webrtc::VideoEncoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override {
    if (!IsSharedFormat(format)) return inner_->Create(env, format);
    if (!has_h264_) return nullptr;
    // The software H.264 encoder behind this format: a format the wrapped
    // factory really lists, never a hard-coded one.
    std::unique_ptr<webrtc::VideoEncoder> software;
    if (auto stock = FindStockH264Format(inner_->GetSupportedFormats())) {
      software = inner_->Create(env, *stock);
    }
    // Without NVENC no shared-encode frame can exist: hand out the stock
    // encoder itself, with no wrapper and nothing added to the pixel path.
    if (!GetEncoderCapabilities().available) return software;
    // With NVENC the sender may switch between camera (software) and a shared
    // screen (passthrough); the wrapper routes each frame. Null software means
    // pixel frames fail closed.
    return std::make_unique<PassthroughVideoEncoder>(std::move(software));
  }

 private:
  static bool HasH264Impl(const std::vector<webrtc::SdpVideoFormat>& formats) {
    for (const auto& f : formats) {
      if (Lower(f.name) == "h264") return true;
    }
    return false;
  }

  std::unique_ptr<webrtc::VideoEncoderFactory> inner_;
  const bool has_h264_;
};

bool HasH264(const std::vector<webrtc::SdpVideoFormat>& formats) {
  for (const auto& f : formats) {
    if (Lower(f.name) == "h264") return true;
  }
  return false;
}

class SharedDecoderFactory : public webrtc::VideoDecoderFactory {
 public:
  explicit SharedDecoderFactory(
      std::unique_ptr<webrtc::VideoDecoderFactory> inner)
      : inner_(std::move(inner)), has_h264_(HasH264(inner_->GetSupportedFormats())) {}

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    auto formats = inner_->GetSupportedFormats();
    if (has_h264_) {
      formats.push_back(SharedFormat("640033"));
      formats.push_back(SharedFormat("640034"));
    }
    return formats;
  }

  CodecSupport QueryCodecSupport(
      const webrtc::SdpVideoFormat& format, bool reference_scaling,
      std::optional<webrtc::Resolution> resolution)
      const override {
    if (IsSharedFormat(format)) {
      CodecSupport support;
      support.is_supported = has_h264_ && !reference_scaling;
      return support;
    }
    return inner_->QueryCodecSupport(format, reference_scaling, resolution);
  }

  std::unique_ptr<webrtc::VideoDecoder> Create(
      const webrtc::Environment& env,
      const webrtc::SdpVideoFormat& format) override {
    if (IsSharedFormat(format)) {
      if (!has_h264_) return nullptr;
      // The stock decoder reads any H.264 profile and level: build it from a
      // format the wrapped factory really lists.
      auto stock = FindStockH264Format(inner_->GetSupportedFormats());
      return stock ? inner_->Create(env, *stock) : nullptr;
    }
    return inner_->Create(env, format);
  }

 private:
  std::unique_ptr<webrtc::VideoDecoderFactory> inner_;
  const bool has_h264_;
};

}  // namespace

EncoderCapabilities GetEncoderCapabilities() {
  // A working encoder does not go away, so a positive answer is kept for the
  // life of the process. A negative answer can be stale: the display may have
  // been rotated, in HDR, detached or in the middle of a change when it was
  // asked, and that must not switch hardware encoding off until the app is
  // restarted. Ask again once the last negative answer is a few seconds old.
  static std::mutex mutex;
  static EncoderCapabilities cached;
  static bool have = false;
  static std::chrono::steady_clock::time_point probed;
  constexpr auto kNegativeLifetime = std::chrono::seconds(3);
  std::lock_guard<std::mutex> lock(mutex);
  if (!have || (!cached.available &&
                std::chrono::steady_clock::now() - probed >= kNegativeLifetime)) {
    cached = ProbeEncoderCapabilities();
    probed = std::chrono::steady_clock::now();
    have = true;
  }
  return cached;
}

std::unique_ptr<webrtc::VideoEncoderFactory> CreateSharedEncoderFactory(
    std::unique_ptr<webrtc::VideoEncoderFactory> inner) {
  return std::make_unique<SharedEncoderFactory>(std::move(inner));
}

std::unique_ptr<webrtc::VideoDecoderFactory> CreateSharedDecoderFactory(
    std::unique_ptr<webrtc::VideoDecoderFactory> inner) {
  return std::make_unique<SharedDecoderFactory>(std::move(inner));
}

}  // namespace shared_encoder
}  // namespace discourse
