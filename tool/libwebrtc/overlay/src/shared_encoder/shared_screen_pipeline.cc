#include "shared_screen_pipeline.h"

#include <atomic>
#include <future>
#include <thread>

#include "rtc_base/logging.h"

#if defined(WEBRTC_WIN)
#include "d3d11_capture.h"
#include "nvenc_h264_encoder.h"
#endif

namespace discourse {
namespace shared_encoder {

#if !defined(WEBRTC_WIN)

EncoderCapabilities ProbeEncoderCapabilities() {
  EncoderCapabilities caps;
  caps.reason = "shared screen encoder is only available on Windows";
  return caps;
}

StartResult StartSharedScreen(const std::string&, const SharedScreenRequest&,
                              webrtc::scoped_refptr<SharedScreenTrackSource>*,
                              SharedScreenPreset*, std::string* error) {
  *error = "shared screen encoder is only available on Windows";
  return StartResult::kUnavailable;
}

#else  // WEBRTC_WIN

namespace {

// Owns the capture, the NVENC session and their thread. D3D11Capture must be
// created, driven and destroyed on one thread, so all of that happens on this
// pipeline's own thread; the constructor only waits for it to be ready.
class Pipeline : public EncodedFrameProducer {
 public:
  // Returns null and sets *error when the capture or encoder cannot open.
  static std::unique_ptr<Pipeline> Start(SharedScreenTrackSource* source,
                                         const std::string& source_id,
                                         const SharedScreenPreset& preset,
                                         std::string* error) {
    std::unique_ptr<Pipeline> p(new Pipeline(source, preset));
    std::future<std::string> ready = p->ready_.get_future();
    p->thread_ = std::thread([raw = p.get(), source_id] { raw->Run(source_id); });
    std::string start_error = ready.get();
    if (!start_error.empty()) {
      *error = start_error;
      return nullptr;  // destructor joins the finished thread
    }
    return p;
  }

  ~Pipeline() override {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

 private:
  Pipeline(SharedScreenTrackSource* source, const SharedScreenPreset& preset)
      : source_(source), preset_(preset) {}

  void Run(const std::string& source_id) {
    std::string error;
    auto capture = D3D11Capture::Create(source_id, preset_.output_width,
                                        preset_.output_height,
                                        preset_.frame_rate, &error);
    if (!capture) {
      ready_.set_value(error.empty() ? "capture failed to open" : error);
      return;
    }
    auto encoder = NvencH264Encoder::Create(capture->device(), preset_,
                                            capture->textures(), &error);
    if (!encoder) {
      ready_.set_value(error.empty() ? "NVENC failed to open" : error);
      return;
    }
    ready_.set_value(std::string());

    uint64_t sequence = 0;
    while (!stop_ && !capture->failed()) {
      std::string capture_error;
      size_t index = 0;
      // Paces itself to the preset's frame rate and repeats the last picture
      // on a static screen. Null with no error: nothing presented yet.
      void* texture = capture->CaptureNext(&capture_error, &index);
      if (!texture) {
        if (!capture_error.empty()) {
          RTC_LOG(LS_ERROR) << "shared capture: " << capture_error;
        }
        continue;
      }
      EncodedAccessUnit unit;
      const int64_t pts_us = webrtc::TimeMicros();
      std::string encode_error;
      if (encoder->Encode(index, pts_us, &unit, &encode_error)) {
        auto frame = std::make_shared<SharedEncodedFrame>();
        frame->sequence = sequence++;
        frame->is_keyframe = unit.is_keyframe;
        frame->width = preset_.output_width;
        frame->height = preset_.output_height;
        frame->capture_time_us = pts_us;
        frame->data =
            std::make_shared<const std::vector<uint8_t>>(std::move(unit.data));
        source_->Publish(std::move(frame));
      } else {
        // A lost frame is a sequence gap: every peer's gate closes until the
        // next keyframe, so a failed encode only freezes video.
        ++sequence;
        RTC_LOG(LS_ERROR) << "shared encode: " << encode_error;
      }
    }
    if (capture->failed()) {
      RTC_LOG(LS_ERROR) << "shared capture failed permanently; peers freeze";
    }
    // `encoder` then `capture` are destroyed here, on the capture's thread.
  }

  SharedScreenTrackSource* source_;  // owns this pipeline
  const SharedScreenPreset preset_;
  std::atomic<bool> stop_{false};
  std::promise<std::string> ready_;
  std::thread thread_;
};

}  // namespace

EncoderCapabilities ProbeEncoderCapabilities() {
  EncoderCapabilities caps;
  // A real capture of the primary output at a small size, and a real NVENC
  // session on the same device, all on a throwaway thread (the capture is
  // thread-confined). Nothing is reported without both opening.
  std::thread([&caps] {
    std::string error;
    // "" is the primary monitor.
    auto capture = D3D11Capture::Create("", 1280, 720, 30, &error);
    if (!capture) {
      caps.reason = error;
      return;
    }
    SharedScreenPreset probe;
    probe.id = "probe";
    probe.output_width = 1280;
    probe.output_height = 720;
    probe.frame_rate = 30;
    probe.bitrate_kbps = 4000;
    probe.keyframe_interval_ms = 2000;
    probe.profile_level_id = "640033";
    auto encoder = NvencH264Encoder::Create(capture->device(), probe,
                                            capture->textures(), &error);
    if (!encoder) {
      caps.reason = error;
      return;
    }
    caps.available = true;
    caps.adapter_name = capture->adapter_name();
    caps.max_width = 4096;
    caps.max_height = 4096;
    caps.max_frame_rate = 120;
  }).join();
  if (!caps.available && caps.reason.empty()) caps.reason = "probe failed";
  return caps;
}

StartResult StartSharedScreen(
    const std::string& source_id, const SharedScreenRequest& request,
    webrtc::scoped_refptr<SharedScreenTrackSource>* out_source,
    SharedScreenPreset* resolved, std::string* error) {
  const EncoderCapabilities& caps = GetEncoderCapabilities();
  if (!caps.available) {
    *error = caps.reason;
    return StartResult::kUnavailable;
  }
  // Source size and monitor lookup are plain queries, safe on any thread.
  int src_w = 0, src_h = 0;
  if (!D3D11Capture::QueryOutput(source_id, &src_w, &src_h, error)) {
    return StartResult::kUnavailable;  // e.g. a window, not a monitor
  }
  SharedScreenPreset preset;
  const std::string invalid = ResolvePreset(request, src_w, src_h, &preset);
  if (!invalid.empty()) {
    *error = invalid;
    return StartResult::kInvalidRequest;
  }
  auto source = webrtc::make_ref_counted<SharedScreenTrackSource>();
  auto pipeline = Pipeline::Start(source.get(), source_id, preset, error);
  if (!pipeline) return StartResult::kUnavailable;
  source->SetProducer(std::move(pipeline));
  *out_source = source;
  *resolved = preset;
  return StartResult::kStarted;
}

#endif  // WEBRTC_WIN

}  // namespace shared_encoder
}  // namespace discourse
