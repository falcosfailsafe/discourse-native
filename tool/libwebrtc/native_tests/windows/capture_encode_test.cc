// Drives the real D3D11 desktop capture into the real NVENC wrapper, which is
// the shared screen share pipeline without libwebrtc and without peers:
//
//   DXGI desktop duplication -> ID3D11VideoProcessor (scale + NV12)
//     -> NvencH264Encoder -> Annex B file
//
// It needs a display attached to the interactive desktop of the session it
// runs in. With --expect-no-display it instead checks that the absence of a
// display is reported as an error and never crashes (the regression test for
// EnumOutputs failing with NOT_CURRENTLY_AVAILABLE in service/SSH sessions).
//
//   capture_encode_test.exe [--source-id ID] [--out WxH] [--fps N] [--kbps N]
//                           [--keyframe-ms N] [--frames N] [--h264 FILE]
//                           [--json FILE] [--expect-no-display]
// Exit codes: 0 ok, 1 check failed, 2 usage, 3 no display (unless expected).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "d3d11_capture.h"
#include "h264_annexb.h"
#include "nvenc_h264_encoder.h"
#include "shared_screen_preset.h"
#include "smoke_util.h"

using discourse::shared_encoder::D3D11Capture;
using discourse::shared_encoder::EncodedAccessUnit;
using discourse::shared_encoder::NvencH264Encoder;
using discourse::shared_encoder::ScanH264AnnexB;
using discourse::shared_encoder::SharedScreenPreset;
using namespace smoke;

namespace {

struct Options {
  std::string source_id;
  int out_w = 0;  // 0: half of the source, even
  int out_h = 0;
  int fps = 60;
  int kbps = 15000;
  int keyframe_ms = 2000;
  int frames = 300;
  bool expect_no_display = false;
  std::string h264_path = "capture_encode.h264";
  std::string json_path = "capture_encode.json";
};

int Even(int v) { return v < 2 ? 2 : (v & ~1); }

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  // The app's manifest is PerMonitorV2. Without this, DXGI reports a
  // DPI-virtualized desktop size (e.g. 2560x720 for a 7680x2160 display at
  // 300% scale) and the test would not behave like the app.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  Options opt;
  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* name) {
      return std::strcmp(argv[i], name) == 0 && i + 1 < argc;
    };
    if (arg("--source-id")) {
      opt.source_id = argv[++i];
    } else if (arg("--out")) {
      std::sscanf(argv[++i], "%dx%d", &opt.out_w, &opt.out_h);
    } else if (arg("--fps")) {
      opt.fps = std::atoi(argv[++i]);
    } else if (arg("--kbps")) {
      opt.kbps = std::atoi(argv[++i]);
    } else if (arg("--keyframe-ms")) {
      opt.keyframe_ms = std::atoi(argv[++i]);
    } else if (arg("--frames")) {
      opt.frames = std::atoi(argv[++i]);
    } else if (arg("--h264")) {
      opt.h264_path = argv[++i];
    } else if (arg("--json")) {
      opt.json_path = argv[++i];
    } else if (std::strcmp(argv[i], "--expect-no-display") == 0) {
      opt.expect_no_display = true;
    } else {
      std::fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]);
      return 2;
    }
  }

  DWORD session = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  std::printf("session id: %lu\n", session);

  // --- Is there a display this session can duplicate?
  int src_w = 0, src_h = 0;
  std::string error;
  const bool have_output =
      D3D11Capture::QueryOutput(opt.source_id, &src_w, &src_h, &error);
  if (!have_output) {
    std::printf("no usable display: %s\n", error.c_str());
    // Creating a capture must fail the same graceful way, not crash.
    std::string create_error;
    auto capture = D3D11Capture::Create(opt.source_id, 1280, 720, 30,
                                        &create_error);
    std::printf("Create on the same source: %s (%s)\n",
                capture ? "UNEXPECTEDLY SUCCEEDED" : "failed gracefully",
                create_error.c_str());
    if (opt.expect_no_display) {
      if (capture) {
        std::fprintf(stderr, "CHECK FAILED: capture opened with no display\n");
        return 1;
      }
      std::printf("OK: absence of a display is reported, not a crash\n");
      return 0;
    }
    return 3;
  }
  std::printf("display: %dx%d\n", src_w, src_h);
  if (opt.expect_no_display) {
    std::fprintf(stderr, "CHECK FAILED: expected no display but found %dx%d\n",
                 src_w, src_h);
    return 1;
  }

  if (opt.out_w == 0 || opt.out_h == 0) {
    opt.out_w = Even(src_w / 2);
    opt.out_h = Even(src_h / 2);
  }

  // --- Capture + encoder, both on this thread (the capture is thread-bound).
  auto capture = D3D11Capture::Create(opt.source_id, opt.out_w, opt.out_h,
                                      opt.fps, &error);
  if (!capture) Die("D3D11Capture::Create failed: " + error);
  std::printf("capture on %s: %dx%d -> %dx%d @ %d fps\n",
              capture->adapter_name().c_str(), capture->width(),
              capture->height(), capture->target_width(),
              capture->target_height(), capture->frame_rate());

  SharedScreenPreset preset;
  preset.id = "capture-test";
  preset.output_width = opt.out_w;
  preset.output_height = opt.out_h;
  preset.frame_rate = opt.fps;
  preset.bitrate_kbps = opt.kbps;
  preset.keyframe_interval_ms = opt.keyframe_ms;
  const int level = discourse::shared_encoder::RequiredH264LevelIdc(
      opt.out_w, opt.out_h, opt.fps);
  preset.profile_level_id = level == 51 ? "640033" : "640034";
  const std::string invalid = discourse::shared_encoder::ValidatePreset(preset);
  if (!invalid.empty()) Die("preset rejected: " + invalid);

  auto encoder = NvencH264Encoder::Create(capture->device(), preset,
                                          capture->textures(), &error);
  if (!encoder) Die("NvencH264Encoder::Create failed: " + error);
  std::printf("encoder: %dx%d @ %d fps %d kbps %s\n", opt.out_w, opt.out_h,
              opt.fps, opt.kbps, preset.profile_level_id.c_str());

  FILE* h264 = std::fopen(opt.h264_path.c_str(), "wb");
  if (!h264) Die("cannot open " + opt.h264_path);

  using clock = std::chrono::steady_clock;
  std::vector<double> encode_ms, tick_ms;
  std::vector<int> keyframes;
  uint64_t bytes = 0;
  int encoded = 0, capture_errors = 0, idr_without_params = 0;
  bool sps_seen = false;
  Sps sps;
  std::string smi;
  const auto start = clock::now();
  auto last = start;
  int64_t sequence = 0;
  for (int n = 0; n < opt.frames; ++n) {
    size_t index = 0;
    std::string capture_error;
    void* texture = capture->CaptureNext(&capture_error, &index);
    const auto got = clock::now();
    tick_ms.push_back(std::chrono::duration<double, std::milli>(got - last).count());
    last = got;
    if (!texture) {
      ++capture_errors;
      if (capture->failed()) Die("capture failed permanently: " + capture_error);
      continue;
    }
    EncodedAccessUnit au;
    const int64_t pts_us = sequence++ * 1000000 / opt.fps;
    const auto t1 = clock::now();
    if (!encoder->Encode(index, pts_us, &au, &error)) {
      Die("Encode failed: " + error);
    }
    encode_ms.push_back(
        std::chrono::duration<double, std::milli>(clock::now() - t1).count());
    if (au.data.empty()) Die("empty access unit");
    if (!sps_seen) {
      sps = FindSps(au.data);
      sps_seen = sps.found;
    }
    const auto info = ScanH264AnnexB(au.data.data(), au.data.size());
    if (au.is_keyframe) {
      keyframes.push_back(encoded);
      if (!info.IsRandomAccessPoint()) ++idr_without_params;
    }
    std::fwrite(au.data.data(), 1, au.data.size(), h264);
    bytes += au.data.size();
    ++encoded;
    if (n == opt.frames / 2) {
      smi = RunCommand(
          "nvidia-smi --query-gpu=name,encoder.stats.sessionCount,"
          "utilization.encoder,memory.used --format=csv,noheader 2>&1");
      std::printf("nvidia-smi during encode: %s\n", smi.c_str());
    }
  }
  std::fclose(h264);
  const double seconds =
      std::chrono::duration<double>(clock::now() - start).count();

  const double fps = encoded / seconds;
  const double kbps = bytes * 8.0 / seconds / 1000.0;
  std::printf("\n--- result ---\n");
  std::printf("%d frames encoded in %.2fs = %.2f fps, %.0f kbps, capture "
              "errors %d\n",
              encoded, seconds, fps, kbps, capture_errors);
  std::printf("encode mean %.2f ms p95 %.2f | tick mean %.2f ms p95 %.2f\n",
              Mean(encode_ms), Percentile(encode_ms, 0.95), Mean(tick_ms),
              Percentile(tick_ms, 0.95));
  std::printf("keyframes %zu; SPS %s\n", keyframes.size(),
              sps.found ? sps.ProfileLevelId().c_str() : "MISSING");

  std::vector<std::string> failures;
  if (!sps.found) failures.push_back("no SPS in the stream");
  else if (sps.profile_idc != 0x64) failures.push_back("profile is not High");
  if (idr_without_params > 0) failures.push_back("key frame without SPS/PPS");
  if (keyframes.empty() || keyframes.front() != 0) {
    failures.push_back("first frame is not a key frame");
  }
  if (encoded < opt.frames * 0.95) {
    failures.push_back("more than 5% of ticks produced no frame");
  }
  if (fps < opt.fps * 0.95) failures.push_back("frame rate below 95% of target");

  FILE* json = std::fopen(opt.json_path.c_str(), "wb");
  if (json) {
    std::fprintf(json,
                 "{\n  \"adapter\": \"%s\",\n  \"session\": %lu,\n"
                 "  \"source\": \"%dx%d\",\n  \"output\": \"%dx%d\",\n"
                 "  \"fps_target\": %d,\n  \"kbps_target\": %d,\n"
                 "  \"frames\": %d,\n  \"seconds\": %.3f,\n  \"fps\": %.2f,\n"
                 "  \"kbps\": %.0f,\n  \"capture_errors\": %d,\n"
                 "  \"encode_ms\": {\"mean\": %.3f, \"p95\": %.3f},\n"
                 "  \"keyframes\": %zu,\n  \"sps_profile_level_id\": \"%s\",\n"
                 "  \"nvidia_smi_during_encode\": \"%s\",\n  \"failures\": [",
                 JsonEscape(capture->adapter_name()).c_str(), session,
                 capture->width(), capture->height(), opt.out_w, opt.out_h,
                 opt.fps, opt.kbps, encoded, seconds, fps, kbps, capture_errors,
                 Mean(encode_ms), Percentile(encode_ms, 0.95), keyframes.size(),
                 sps.found ? sps.ProfileLevelId().c_str() : "",
                 JsonEscape(smi).c_str());
    for (size_t i = 0; i < failures.size(); ++i) {
      std::fprintf(json, "%s\"%s\"", i ? ", " : "",
                   JsonEscape(failures[i]).c_str());
    }
    std::fprintf(json, "]\n}\n");
    std::fclose(json);
  }
  if (!failures.empty()) {
    for (const auto& f : failures) {
      std::fprintf(stderr, "CHECK FAILED: %s\n", f.c_str());
    }
    return 1;
  }
  std::printf("ALL CHECKS PASSED\n");
  return 0;
}
