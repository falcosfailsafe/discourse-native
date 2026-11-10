// Windows GPU smoke test for the shared NVENC encoder. No display is needed.
//
// It runs the same shape of pipeline as the shared screen share, with a
// synthetic animated picture standing in for the desktop:
//
//   BGRA source texture (e.g. 7680x2160)
//     -> ID3D11VideoProcessor scale + BT.709 convert -> NV12 pool textures
//     -> NvencH264Encoder (the real wrapper, NVENC fed D3D11 textures)
//
// It checks that hardware encoding really works (the session opens on the
// NVIDIA adapter, frames come out as H.264 High at the requested level, key
// frames repeat at the requested interval), measures the timing, samples
// nvidia-smi for the NVENC session count while encoding, and writes the stream
// so it can be decoded independently (ffprobe/ffmpeg).
//
// Build (from a VS developer prompt), see build_nvenc_smoke_test.ps1.
// This proves capture-independent encoding only. It does not exercise DXGI
// desktop duplication, libwebrtc, or any peer connection.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "h264_annexb.h"
#include "smoke_util.h"
#include "nvenc_h264_encoder.h"
#include "shared_screen_preset.h"

using Microsoft::WRL::ComPtr;
using discourse::shared_encoder::EncodedAccessUnit;
using discourse::shared_encoder::NvencH264Encoder;
using discourse::shared_encoder::ScanH264AnnexB;
using discourse::shared_encoder::SharedScreenPreset;

using namespace smoke;

namespace {

struct Options {
  int src_w = 7680;
  int src_h = 2160;
  int out_w = 3840;
  int out_h = 1080;
  int fps = 60;
  int kbps = 25000;
  int keyframe_ms = 2000;
  int frames = 600;
  int unpaced_frames = 300;  // 0 skips the throughput phase
  std::string h264_path = "nvenc_smoke.h264";
  std::string json_path = "nvenc_smoke.json";
};

// A static, detailed picture a little larger than the source. Each frame
// copies a window of it at a moving offset, which gives full-frame motion that
// a real encoder can predict, without any CPU work per frame.
constexpr int kMargin = 512;

std::vector<uint32_t> MakePattern(int w, int h) {
  std::vector<uint32_t> px(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      // Smooth colour field.
      const double fx = static_cast<double>(x) / w;
      const double fy = static_cast<double>(y) / h;
      int r = static_cast<int>(127 + 120 * std::sin(fx * 9.0 + fy * 3.0));
      int g = static_cast<int>(127 + 120 * std::sin(fy * 7.0 - fx * 2.0));
      int b = static_cast<int>(127 + 120 * std::cos((fx + fy) * 5.0));
      // Hard-edged blocks and grid lines, like windows and text.
      const int bx = x / 96, by = y / 96;
      const uint32_t hash = static_cast<uint32_t>(bx * 73856093) ^
                            static_cast<uint32_t>(by * 19349663);
      if ((hash & 7) == 0) {
        r = (hash >> 8) & 255;
        g = (hash >> 16) & 255;
        b = (hash >> 4) & 255;
      }
      if ((x % 96) == 0 || (y % 96) == 0) r = g = b = 235;
      // Fine texture so the encoder has real detail to spend bits on.
      const int noise = static_cast<int>(((x * 2654435761u) ^ (y * 40503u)) >> 28) - 8;
      r = std::clamp(r + noise, 0, 255);
      g = std::clamp(g + noise, 0, 255);
      b = std::clamp(b + noise, 0, 255);
      px[static_cast<size_t>(y) * w + x] =
          0xFF000000u | (static_cast<uint32_t>(r) << 16) |
          (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    }
  }
  return px;
}

struct RunStats {
  int frames = 0;
  double seconds = 0;
  uint64_t bytes = 0;
  std::vector<double> convert_ms, encode_ms, total_ms;
  std::vector<int> keyframes;  // frame indices
  int late_frames = 0;         // paced runs only: tick was missed
};

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  Options opt;
  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* name) {
      return std::strcmp(argv[i], name) == 0 && i + 1 < argc;
    };
    if (arg("--src")) {
      std::sscanf(argv[++i], "%dx%d", &opt.src_w, &opt.src_h);
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
    } else if (arg("--unpaced-frames")) {
      opt.unpaced_frames = std::atoi(argv[++i]);
    } else if (arg("--h264")) {
      opt.h264_path = argv[++i];
    } else if (arg("--json")) {
      opt.json_path = argv[++i];
    } else {
      std::fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]);
      return 2;
    }
  }

  // --- Adapter: the NVIDIA GPU, whatever else is installed.
  ComPtr<IDXGIFactory1> factory;
  CHECK_HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter1> adapter;
  std::string adapter_list;
  std::string adapter_name;
  UINT adapter_vendor = 0;
  int adapter_outputs = -1;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> a;
    if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 d = {};
    a->GetDesc1(&d);
    int outputs = 0;
    for (UINT o = 0;; ++o) {
      ComPtr<IDXGIOutput> out;
      // NOT_FOUND ends the list; in a non-interactive session (SSH, services)
      // the call fails with NOT_CURRENTLY_AVAILABLE for every index, so stop
      // on any failure and count only real outputs.
      if (FAILED(a->EnumOutputs(o, &out))) break;
      ++outputs;
    }
    char line[256];
    std::snprintf(line, sizeof(line), "[%u] %s vendor=0x%04X outputs=%d%s", i,
                  Narrow(d.Description).c_str(), d.VendorId, outputs,
                  (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
    std::printf("adapter %s\n", line);
    if (!adapter_list.empty()) adapter_list += "; ";
    adapter_list += line;
    if (!adapter && d.VendorId == 0x10DE) {
      adapter = a;
      adapter_name = Narrow(d.Description);
      adapter_vendor = d.VendorId;
      adapter_outputs = outputs;
    }
  }
  if (!adapter) Die("no NVIDIA adapter found");

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL got = {};
  CHECK_HR(D3D11CreateDevice(
      adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
      levels, 2, D3D11_SDK_VERSION, &device, &got, &context));
  std::printf("D3D11 device on %s, feature level 0x%x\n", adapter_name.c_str(),
              static_cast<unsigned>(got));

  // --- Probe: opens and closes a real NVENC session.
  std::string error;
  if (!NvencH264Encoder::Probe(device.Get(), &error)) {
    Die("NVENC probe failed: " + error);
  }
  std::printf("NVENC probe: ok (H.264 session opened on the NVIDIA adapter)\n");

  // --- Source and pattern textures.
  const int pw = opt.src_w + kMargin, ph = opt.src_h + kMargin;
  std::printf("generating %dx%d test pattern...\n", pw, ph);
  std::vector<uint32_t> pattern = MakePattern(pw, ph);
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = pw;
  td.Height = ph;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init = {pattern.data(),
                                 static_cast<UINT>(pw) * 4, 0};
  ComPtr<ID3D11Texture2D> pattern_tex;
  CHECK_HR(device->CreateTexture2D(&td, &init, &pattern_tex));
  pattern.clear();
  pattern.shrink_to_fit();

  td.Width = opt.src_w;
  td.Height = opt.src_h;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> source;
  CHECK_HR(device->CreateTexture2D(&td, nullptr, &source));

  // --- Video processor: identical settings to d3d11_capture.cc.
  ComPtr<ID3D11VideoDevice> video_device;
  ComPtr<ID3D11VideoContext> video_context;
  CHECK_HR(device.As(&video_device));
  CHECK_HR(context.As(&video_context));
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
  content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  content.InputFrameRate = {static_cast<UINT>(opt.fps), 1};
  content.InputWidth = opt.src_w;
  content.InputHeight = opt.src_h;
  content.OutputFrameRate = content.InputFrameRate;
  content.OutputWidth = opt.out_w;
  content.OutputHeight = opt.out_h;
  content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
  CHECK_HR(video_device->CreateVideoProcessorEnumerator(&content, &enumerator));
  UINT in_support = 0, out_support = 0;
  enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                        &in_support);
  enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &out_support);
  if (!(in_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
      !(out_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
    Die("video processor cannot convert BGRA to NV12");
  }
  ComPtr<ID3D11VideoProcessor> processor;
  CHECK_HR(video_device->CreateVideoProcessor(enumerator.Get(), 0, &processor));
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_space = {};
  in_space.RGB_Range = 0;
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_space = {};
  out_space.YCbCr_Matrix = 1;
  out_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
  video_context->VideoProcessorSetStreamColorSpace(processor.Get(), 0,
                                                   &in_space);
  video_context->VideoProcessorSetOutputColorSpace(processor.Get(), &out_space);
  video_context->VideoProcessorSetStreamFrameFormat(
      processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  video_context->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0,
                                                           FALSE);
  const RECT src_rect = {0, 0, opt.src_w, opt.src_h};
  const RECT dst_rect = {0, 0, opt.out_w, opt.out_h};
  video_context->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE,
                                                   &src_rect);
  video_context->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE,
                                                 &dst_rect);
  video_context->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE,
                                                   &dst_rect);
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv = {};
  iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  ComPtr<ID3D11VideoProcessorInputView> input_view;
  CHECK_HR(video_device->CreateVideoProcessorInputView(
      source.Get(), enumerator.Get(), &iv, &input_view));

  // --- NV12 pool, same description as the capture pool.
  constexpr int kPool = 4;
  D3D11_TEXTURE2D_DESC nd = {};
  nd.Width = opt.out_w;
  nd.Height = opt.out_h;
  nd.MipLevels = 1;
  nd.ArraySize = 1;
  nd.Format = DXGI_FORMAT_NV12;
  nd.SampleDesc.Count = 1;
  nd.Usage = D3D11_USAGE_DEFAULT;
  nd.BindFlags = D3D11_BIND_RENDER_TARGET;
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov = {};
  ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  D3D11_QUERY_DESC qd = {};
  qd.Query = D3D11_QUERY_EVENT;
  ComPtr<ID3D11Texture2D> nv12[kPool];
  ComPtr<ID3D11VideoProcessorOutputView> views[kPool];
  ComPtr<ID3D11Query> done[kPool];
  std::vector<void*> raw_textures;
  for (int i = 0; i < kPool; ++i) {
    CHECK_HR(device->CreateTexture2D(&nd, nullptr, &nv12[i]));
    CHECK_HR(video_device->CreateVideoProcessorOutputView(
        nv12[i].Get(), enumerator.Get(), &ov, &views[i]));
    CHECK_HR(device->CreateQuery(&qd, &done[i]));
    raw_textures.push_back(nv12[i].Get());
  }

  // --- Encoder: the real wrapper.
  SharedScreenPreset preset;
  preset.id = "smoke";
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
  std::unique_ptr<NvencH264Encoder> encoder =
      NvencH264Encoder::Create(device.Get(), preset, raw_textures, &error);
  if (!encoder) Die("NvencH264Encoder::Create failed: " + error);
  std::printf("encoder created: %dx%d @ %d fps, %d kbps, keyframe %d ms, %s\n",
              opt.out_w, opt.out_h, opt.fps, opt.kbps, opt.keyframe_ms,
              preset.profile_level_id.c_str());

  FILE* h264 = std::fopen(opt.h264_path.c_str(), "wb");
  if (!h264) Die("cannot open " + opt.h264_path);

  using clock = std::chrono::steady_clock;
  auto ms_between = [](clock::time_point a, clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };

  std::string smi_sample;
  Sps first_sps;
  bool sps_seen = false;
  int idr_without_params = 0;
  int frame_counter = 0;  // continues across phases so the picture keeps moving

  auto run_phase = [&](int frames, bool paced, bool sample_smi,
                       bool write_file) {
    RunStats st;
    const auto interval = std::chrono::microseconds(1000000 / opt.fps);
    auto next_tick = clock::now();
    const auto phase_start = clock::now();
    for (int n = 0; n < frames; ++n) {
      if (paced) {
        const auto now = clock::now();
        if (now > next_tick + interval) {
          ++st.late_frames;
          next_tick = now;
        }
        const auto coarse = next_tick - std::chrono::milliseconds(2);
        if (clock::now() < coarse) std::this_thread::sleep_until(coarse);
        while (clock::now() < next_tick) std::this_thread::yield();
        next_tick += interval;
      }
      const auto t0 = clock::now();
      // Move the window over the pattern: a fresh picture every frame.
      const int ox = (frame_counter * 7) % kMargin;
      const int oy = (frame_counter * 3) % kMargin;
      D3D11_BOX box = {static_cast<UINT>(ox), static_cast<UINT>(oy), 0,
                       static_cast<UINT>(ox + opt.src_w),
                       static_cast<UINT>(oy + opt.src_h), 1};
      context->CopySubresourceRegion(source.Get(), 0, 0, 0, 0,
                                     pattern_tex.Get(), 0, &box);
      const int slot = frame_counter % kPool;
      D3D11_VIDEO_PROCESSOR_STREAM stream = {};
      stream.Enable = TRUE;
      stream.pInputSurface = input_view.Get();
      CHECK_HR(video_context->VideoProcessorBlt(processor.Get(),
                                                views[slot].Get(), 0, 1,
                                                &stream));
      context->End(done[slot].Get());
      context->Flush();
      BOOL finished = FALSE;
      const auto deadline = clock::now() + std::chrono::seconds(2);
      for (;;) {
        HRESULT hr = context->GetData(done[slot].Get(), &finished,
                                      sizeof(finished), 0);
        if (hr == S_OK && finished) break;
        if (FAILED(hr)) Die(HrString("GPU completion query", hr));
        if (clock::now() > deadline) Die("GPU did not finish within 2 s");
        std::this_thread::yield();
      }
      const auto t1 = clock::now();

      EncodedAccessUnit au;
      const int64_t pts_us =
          static_cast<int64_t>(frame_counter) * 1000000 / opt.fps;
      if (!encoder->Encode(static_cast<size_t>(slot), pts_us, &au, &error)) {
        Die("Encode failed at frame " + std::to_string(frame_counter) + ": " +
            error);
      }
      const auto t2 = clock::now();

      if (au.data.empty()) Die("encoder returned an empty access unit");
      if (!sps_seen) {
        first_sps = FindSps(au.data);
        sps_seen = first_sps.found;
      }
      const auto info = ScanH264AnnexB(au.data.data(), au.data.size());
      if (au.is_keyframe) {
        st.keyframes.push_back(n);
        if (!info.IsRandomAccessPoint()) ++idr_without_params;
      }
      if (write_file) std::fwrite(au.data.data(), 1, au.data.size(), h264);
      st.bytes += au.data.size();
      st.convert_ms.push_back(ms_between(t0, t1));
      st.encode_ms.push_back(ms_between(t1, t2));
      st.total_ms.push_back(ms_between(t0, t2));
      ++st.frames;
      ++frame_counter;

      if (sample_smi && n == frames / 2) {
        smi_sample = RunCommand(
            "nvidia-smi --query-gpu=name,encoder.stats.sessionCount,"
            "encoder.stats.averageFps,encoder.stats.averageLatency,"
            "utilization.gpu,utilization.encoder,memory.used "
            "--format=csv,noheader 2>&1");
        std::printf("nvidia-smi during encode: %s\n", smi_sample.c_str());
      }
    }
    st.seconds =
        std::chrono::duration<double>(clock::now() - phase_start).count();
    return st;
  };

  std::printf("paced phase: %d frames at %d fps...\n", opt.frames, opt.fps);
  RunStats paced = run_phase(opt.frames, true, true, true);
  RunStats burst;
  if (opt.unpaced_frames > 0) {
    std::printf("unpaced phase: %d frames as fast as possible...\n",
                opt.unpaced_frames);
    burst = run_phase(opt.unpaced_frames, false, false, false);
  }
  std::fclose(h264);

  // --- Report.
  auto keyframe_interval_ms = [&](const RunStats& s) {
    if (s.keyframes.size() < 2) return 0.0;
    std::vector<double> gaps;
    for (size_t i = 1; i < s.keyframes.size(); ++i) {
      gaps.push_back((s.keyframes[i] - s.keyframes[i - 1]) * 1000.0 /
                     opt.fps);
    }
    return Mean(gaps);
  };
  const double paced_fps = paced.frames / paced.seconds;
  const double paced_kbps = paced.bytes * 8.0 / paced.seconds / 1000.0;
  const double burst_fps = burst.frames ? burst.frames / burst.seconds : 0;

  std::printf("\n--- result ---\n");
  std::printf("paced:   %d frames in %.2fs = %.2f fps, %.0f kbps, late %d\n",
              paced.frames, paced.seconds, paced_fps, paced_kbps,
              paced.late_frames);
  std::printf("         convert mean %.2f ms p95 %.2f | encode mean %.2f ms "
              "p95 %.2f max %.2f\n",
              Mean(paced.convert_ms), Percentile(paced.convert_ms, 0.95),
              Mean(paced.encode_ms), Percentile(paced.encode_ms, 0.95),
              Percentile(paced.encode_ms, 1.0));
  std::printf("         keyframes %zu, mean interval %.0f ms (want %d)\n",
              paced.keyframes.size(), keyframe_interval_ms(paced),
              opt.keyframe_ms);
  if (burst.frames) {
    std::printf("unpaced: %d frames in %.2fs = %.1f fps (convert %.2f + "
                "encode %.2f ms)\n",
                burst.frames, burst.seconds, burst_fps, Mean(burst.convert_ms),
                Mean(burst.encode_ms));
  }
  std::printf("SPS: %s -> profile-level-id %s (want %s)\n",
              first_sps.found ? "found" : "MISSING",
              first_sps.found ? first_sps.ProfileLevelId().c_str() : "-",
              preset.profile_level_id.c_str());

  // --- Pass / fail checks.
  std::vector<std::string> failures;
  if (!first_sps.found) failures.push_back("no SPS in the stream");
  else if (first_sps.ProfileLevelId().substr(0, 2) != "64") {
    failures.push_back("profile is not High (0x64)");
  } else if (first_sps.level_idc != (level == 51 ? 51 : 52)) {
    failures.push_back("level_idc " + std::to_string(first_sps.level_idc) +
                       " differs from the requested level");
  }
  if (idr_without_params > 0) {
    failures.push_back("key frames without SPS/PPS: " +
                       std::to_string(idr_without_params));
  }
  if (paced.keyframes.empty() || paced.keyframes.front() != 0) {
    failures.push_back("the first frame is not a key frame");
  }
  const double want_interval = opt.keyframe_ms;
  if (paced.keyframes.size() >= 2 &&
      std::fabs(keyframe_interval_ms(paced) - want_interval) >
          want_interval * 0.15) {
    failures.push_back("key frame interval is off the requested value");
  }
  if (paced_fps < opt.fps * 0.97) {
    failures.push_back("sustained frame rate below 97% of the target");
  }
  if (smi_sample.find("[N/A]") == std::string::npos &&
      smi_sample.find(", 1,") == std::string::npos) {
    // sessionCount should read 1 while exactly one session is encoding.
    failures.push_back("nvidia-smi did not report one NVENC session");
  }

  FILE* json = std::fopen(opt.json_path.c_str(), "wb");
  if (json) {
    std::fprintf(
        json,
        "{\n"
        "  \"adapter\": \"%s\",\n"
        "  \"adapters\": \"%s\",\n"
        "  \"adapter_outputs\": %d,\n"
        "  \"source\": \"%dx%d\",\n"
        "  \"output\": \"%dx%d\",\n"
        "  \"fps_target\": %d,\n"
        "  \"kbps_target\": %d,\n"
        "  \"keyframe_ms_target\": %d,\n"
        "  \"requested_profile_level_id\": \"%s\",\n"
        "  \"sps_profile_level_id\": \"%s\",\n"
        "  \"paced\": {\"frames\": %d, \"seconds\": %.3f, \"fps\": %.2f, "
        "\"kbps\": %.0f, \"late_frames\": %d, \"keyframes\": %zu, "
        "\"keyframe_interval_ms\": %.0f,\n"
        "            \"convert_ms\": {\"mean\": %.3f, \"p95\": %.3f}, "
        "\"encode_ms\": {\"mean\": %.3f, \"p95\": %.3f, \"max\": %.3f}},\n"
        "  \"unpaced\": {\"frames\": %d, \"seconds\": %.3f, \"fps\": %.1f},\n"
        "  \"nvidia_smi_during_encode\": \"%s\",\n"
        "  \"failures\": [",
        JsonEscape(adapter_name).c_str(), JsonEscape(adapter_list).c_str(),
        adapter_outputs, opt.src_w, opt.src_h, opt.out_w, opt.out_h, opt.fps,
        opt.kbps, opt.keyframe_ms, preset.profile_level_id.c_str(),
        first_sps.found ? first_sps.ProfileLevelId().c_str() : "", paced.frames,
        paced.seconds, paced_fps, paced_kbps, paced.late_frames,
        paced.keyframes.size(), keyframe_interval_ms(paced),
        Mean(paced.convert_ms), Percentile(paced.convert_ms, 0.95),
        Mean(paced.encode_ms), Percentile(paced.encode_ms, 0.95),
        Percentile(paced.encode_ms, 1.0), burst.frames, burst.seconds,
        burst_fps, JsonEscape(smi_sample).c_str());
    for (size_t i = 0; i < failures.size(); ++i) {
      std::fprintf(json, "%s\"%s\"", i ? ", " : "",
                   JsonEscape(failures[i]).c_str());
    }
    std::fprintf(json, "]\n}\n");
    std::fclose(json);
  }

  if (!failures.empty()) {
    for (const auto& f : failures) std::fprintf(stderr, "CHECK FAILED: %s\n", f.c_str());
    return 1;
  }
  std::printf("ALL CHECKS PASSED\n");
  return 0;
}
