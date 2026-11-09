// Portable tests for the shared screen encoder's gate, passthrough logic and
// preset math. No libwebrtc, no Windows. Run: ../run_native_tests.sh
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "shared_encoder/passthrough_core.h"
#include "shared_encoder/shared_screen_preset.h"

using namespace discourse::shared_encoder;

static int g_failures = 0;
#define CHECK(c)                                                        \
  do {                                                                  \
    if (!(c)) {                                                         \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

static SharedEncodedFrame MakeFrame(uint64_t seq, bool key, bool params = true) {
  std::vector<uint8_t> b;
  auto nal = [&](uint8_t header) {
    b.insert(b.end(), {0, 0, 0, 1, header, 0x11, 0x22});
  };
  if (key) {
    if (params) { nal(0x67); nal(0x68); }
    nal(0x65);
  } else {
    nal(0x41);
  }
  SharedEncodedFrame f;
  f.sequence = seq;
  f.is_keyframe = key;
  f.width = 3840;
  f.height = 1080;
  f.data = std::make_shared<const std::vector<uint8_t>>(std::move(b));
  return f;
}

static void GateWaitsForFirstKeyframe() {
  PassthroughCore core;
  CHECK(!core.ShouldForward(MakeFrame(10, false)));
  CHECK(!core.ShouldForward(MakeFrame(11, false)));
  CHECK(core.ShouldForward(MakeFrame(12, true)));
  CHECK(core.ShouldForward(MakeFrame(13, false)));
  CHECK(core.gate().dropped_frames() == 2);
  CHECK(core.gate().resumes() == 1);
}

static void GateClosesOnDropAndResumesOnKeyframe() {
  PassthroughCore core;
  CHECK(core.ShouldForward(MakeFrame(0, true)));
  CHECK(core.ShouldForward(MakeFrame(1, false)));
  // Frame 2 never reached this peer.
  CHECK(!core.ShouldForward(MakeFrame(3, false)));
  CHECK(!core.ShouldForward(MakeFrame(4, false)));
  CHECK(!core.gate().is_open());
  CHECK(core.ShouldForward(MakeFrame(5, true)));
  CHECK(core.ShouldForward(MakeFrame(6, false)));
  CHECK(core.gate().closes() == 1);
  CHECK(core.gate().resumes() == 2);
}

static void GapKeyframeReopensImmediately() {
  PassthroughCore core;
  CHECK(core.ShouldForward(MakeFrame(0, true)));
  CHECK(core.ShouldForward(MakeFrame(7, true)));  // gap, but a keyframe
  CHECK(core.ShouldForward(MakeFrame(8, false)));
}

static void GateRejectsBackwardsAndDuplicateSequences() {
  PassthroughCore core;
  CHECK(core.ShouldForward(MakeFrame(5, true)));
  CHECK(!core.ShouldForward(MakeFrame(5, false)));
  CHECK(!core.ShouldForward(MakeFrame(4, false)));
}

static void KeyframeWithoutParameterSetsIsNotAJoinPoint() {
  PassthroughCore core;
  CHECK(!core.ShouldForward(MakeFrame(0, true, /*params=*/false)));
  CHECK(!core.ShouldForward(MakeFrame(1, false)));
  CHECK(core.ShouldForward(MakeFrame(2, true)));
}

static void DeliveryFailureClosesGate() {
  PassthroughCore core;
  CHECK(core.ShouldForward(MakeFrame(0, true)));
  core.OnDeliveryFailed();
  CHECK(!core.ShouldForward(MakeFrame(1, false)));
  CHECK(core.ShouldForward(MakeFrame(2, true)));
}

static void PeersAreIndependent() {
  PassthroughCore a, b, c;
  for (uint64_t s = 0; s < 10; ++s) {
    const bool key = s % 5 == 0;
    auto f = MakeFrame(s, key);
    a.ShouldForward(f);
    if (s != 3) b.ShouldForward(f);  // b misses frame 3
    c.ShouldForward(f);
  }
  CHECK(a.gate().dropped_frames() == 0);
  CHECK(c.gate().dropped_frames() == 0);
  CHECK(b.gate().dropped_frames() == 1);  // frame 4; key at 5 resumes
  CHECK(b.gate().is_open());
}

static void PassthroughIgnoresRateAndKeyframeRequests() {
  PassthroughCore core;
  CHECK(core.ShouldForward(MakeFrame(0, true)));
  core.OnSetRates();
  core.OnSetRates();
  core.OnKeyframeRequested();
  // Nothing changed for the stream.
  CHECK(core.gate().is_open());
  CHECK(core.ShouldForward(MakeFrame(1, false)));
  CHECK(core.ignored_rate_changes() == 2);
  CHECK(core.ignored_keyframe_requests() == 1);
}

static void AnnexBScan() {
  auto f = MakeFrame(0, true);
  auto info = ScanH264AnnexB(f.data->data(), f.data->size());
  CHECK(info.has_sps && info.has_pps && info.has_idr && info.nal_count == 3);
  const uint8_t three[] = {0, 0, 1, 0x65, 1};  // 3-byte start code
  CHECK(ScanH264AnnexB(three, sizeof three).has_idr);
  CHECK(ScanH264AnnexB(three, 2).nal_count == 0);
  CHECK(ScanH264AnnexB(nullptr, 0).nal_count == 0);
}

static SharedScreenPreset Preset1080p60() {
  SharedScreenPreset p;
  p.id = "1080p60";
  p.output_width = 3840;
  p.output_height = 1080;
  p.frame_rate = 60;
  p.bitrate_kbps = 25000;
  p.keyframe_interval_ms = 2000;
  p.profile_level_id = "640033";
  return p;
}

static void PresetLevelMath() {
  CHECK(RequiredH264LevelIdc(3840, 1080, 60) == 51);  // 979200 MB/s
  CHECK(RequiredH264LevelIdc(3840, 2160, 60) == 52);
  CHECK(RequiredH264LevelIdc(3840, 1080, 120) == 52);
  CHECK(RequiredH264LevelIdc(7680, 4320, 30) == 0);
  CHECK(RequiredH264LevelIdc(0, 1080, 60) == 0);
  CHECK(RequiredH264LevelIdc(3840, 1080, -1) == 0);
  CHECK(RequiredH264LevelIdc(std::numeric_limits<int>::max(),
                           std::numeric_limits<int>::max(),
                           std::numeric_limits<int>::max()) == 0);
  CHECK(ValidatePreset(Preset1080p60()).empty());
  auto p = Preset1080p60();
  p.output_height = 2160;
  CHECK(!ValidatePreset(p).empty());  // 5.1 too low
  p.profile_level_id = "640034";
  CHECK(ValidatePreset(p).empty());
  p = Preset1080p60();
  p.codec = "AV1";
  CHECK(!ValidatePreset(p).empty());
  p = Preset1080p60();
  p.output_width = 3841;
  CHECK(!ValidatePreset(p).empty());
  p = Preset1080p60();
  p.keyframe_interval_ms = 100;
  CHECK(!ValidatePreset(p).empty());
  p = Preset1080p60();
  p.profile_level_id = "42e01f";
  CHECK(!ValidatePreset(p).empty());
  CHECK(KeyframeIntervalFrames(Preset1080p60()) == 120);
  CHECK(FrameIntervalUs(60) == 16666);
}

static void AspectFit() {
  CHECK((AspectFitDestination(7680, 2160, 3840, 1080) == Rect{0, 0, 3840, 1080}));
  Rect r = AspectFitDestination(1920, 1080, 3840, 1080);  // 16:9 into 32:9
  CHECK(r.h == 1080 && r.w == 1920 && r.x == 960 && r.y == 0);
  r = AspectFitDestination(1000, 2000, 3840, 1080);
  CHECK(r.h == 1080 && r.w == 540 && r.y == 0);
  CHECK((AspectFitDestination(0, 0, 10, 10) == Rect{}));
}

static void ResolveRequest() {
  SharedScreenRequest r;
  r.id = "half60";
  r.source_scale = 0.5;
  r.frame_rate = 60;
  r.bitrate_bps = 25000000;
  r.keyframe_interval_seconds = 2;
  SharedScreenPreset p;
  CHECK(ResolvePreset(r, 7680, 2160, &p).empty());
  CHECK(p.output_width == 3840 && p.output_height == 1080);
  CHECK(p.profile_level_id == "640033" && p.bitrate_kbps == 25000);
  CHECK(p.keyframe_interval_ms == 2000);

  r.source_scale = 0;
  r.max_height = 1080;
  CHECK(ResolvePreset(r, 7680, 2160, &p).empty());
  CHECK(p.output_width == 3840 && p.output_height == 1080);
  r.max_height = 1440;  // never upscale
  CHECK(ResolvePreset(r, 1920, 1080, &p).empty());
  CHECK(p.output_width == 1920 && p.output_height == 1080);
  r.max_height = 2160;
  CHECK(ResolvePreset(r, 3840, 2160, &p).empty());
  CHECK(p.profile_level_id == "640034");

  r.source_scale = 0.5;  // both set
  CHECK(!ResolvePreset(r, 1920, 1080, &p).empty());
  r.max_height = 0;
  r.source_scale = 0;  // neither
  CHECK(!ResolvePreset(r, 1920, 1080, &p).empty());
  r.source_scale = 2.0;
  CHECK(!ResolvePreset(r, 1920, 1080, &p).empty());
  r.source_scale = 0.5;
  CHECK(!ResolvePreset(r, 0, 0, &p).empty());
  r.codec = "AV1";
  CHECK(!ResolvePreset(r, 1920, 1080, &p).empty());
  r.codec = "H264";
  r.keyframe_interval_seconds = std::numeric_limits<int>::max();
  CHECK(!ResolvePreset(r, 1920, 1080, &p).empty());
  r.keyframe_interval_seconds = 2;
  r.bitrate_bps = std::numeric_limits<int64_t>::max();
  CHECK(!ResolvePreset(r, 1920, 1080, &p).empty());
  r.bitrate_bps = 25000000;
  r.frame_rate = std::numeric_limits<int>::max();
  CHECK(!ResolvePreset(r, 1920, 1080, &p).empty());
}

int main() {
  GateWaitsForFirstKeyframe();
  GateClosesOnDropAndResumesOnKeyframe();
  GapKeyframeReopensImmediately();
  GateRejectsBackwardsAndDuplicateSequences();
  KeyframeWithoutParameterSetsIsNotAJoinPoint();
  DeliveryFailureClosesGate();
  PeersAreIndependent();
  PassthroughIgnoresRateAndKeyframeRequests();
  AnnexBScan();
  PresetLevelMath();
  AspectFit();
  ResolveRequest();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("shared_encoder tests passed");
  return 0;
}
