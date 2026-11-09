// Sender-chosen screen share preset: validation and derived encoder values.
// Portable. Peers never change any of this.
#ifndef DISCOURSE_SHARED_ENCODER_SHARED_SCREEN_PRESET_H_
#define DISCOURSE_SHARED_ENCODER_SHARED_SCREEN_PRESET_H_

#include <cstdint>
#include <string>

namespace discourse {
namespace shared_encoder {

struct SharedScreenPreset {
  std::string id;
  int output_width = 0;
  int output_height = 0;
  int frame_rate = 0;
  int bitrate_kbps = 0;
  int keyframe_interval_ms = 0;
  std::string codec = "H264";
  // H.264 profile-level-id, e.g. "640033" (High, level 5.1).
  std::string profile_level_id;
};

// H.264 level_idc (e.g. 51, 52) the picture size and rate need, or 0 when no
// level up to 5.2 can carry it.
inline int RequiredH264LevelIdc(int width, int height, int frame_rate) {
  if (width < 1 || height < 1 || frame_rate < 1) return 0;
  const int64_t mbs_per_frame =
      ((static_cast<int64_t>(width) + 15) / 16) *
      ((static_cast<int64_t>(height) + 15) / 16);
  // Levels 5.1 / 5.2 (Table A-1): MaxFS 36864, MaxMBPS 983040 / 2073600.
  if (mbs_per_frame > 36864) return 0;
  const int64_t mbs_per_second = mbs_per_frame * frame_rate;
  if (mbs_per_second <= 983040) return 51;
  if (mbs_per_second <= 2073600) return 52;
  return 0;
}

// Returns an empty string when valid, otherwise a human readable reason.
inline std::string ValidatePreset(const SharedScreenPreset& p) {
  if (p.codec != "H264") return "only H264 is supported";
  if (p.output_width < 64 || p.output_height < 64 || p.output_width > 4096 ||
      p.output_height > 4096) {
    return "output size out of range";
  }
  if ((p.output_width % 2) != 0 || (p.output_height % 2) != 0) {
    return "output size must be even (NV12)";
  }
  if (p.frame_rate < 1 || p.frame_rate > 120) return "frame rate out of range";
  if (p.bitrate_kbps < 500 || p.bitrate_kbps > 400000) {
    return "bitrate out of range";
  }
  if (p.keyframe_interval_ms < 500 || p.keyframe_interval_ms > 10000) {
    return "keyframe interval out of range";
  }
  const int required =
      RequiredH264LevelIdc(p.output_width, p.output_height, p.frame_rate);
  if (required == 0) return "size and frame rate exceed H.264 level 5.2";
  if (p.profile_level_id != "640033" && p.profile_level_id != "640034") {
    return "profileLevelId must be 640033 or 640034";
  }
  const int level = p.profile_level_id == "640033" ? 51 : 52;
  if (level < required) return "profileLevelId level too low for this preset";
  return std::string();
}

// What Dart sends (`getSharedScreenMedia` preset). Exactly one of max_height
// and source_scale is set. `bitrate_bps` is bits per second.
struct SharedScreenRequest {
  std::string id;
  int max_height = 0;
  double source_scale = 0;
  int frame_rate = 0;
  int64_t bitrate_bps = 0;
  int keyframe_interval_seconds = 0;
  std::string codec = "H264";
};

// Turns the request plus the real source size into a validated preset. The
// output keeps the source's aspect ratio, is even-sized and is never larger
// than the source. The H.264 level is the lowest that carries the result.
// Returns an empty string on success, otherwise the reason; `out` is only
// meaningful on success.
inline std::string ResolvePreset(const SharedScreenRequest& r, int src_w,
                                 int src_h, SharedScreenPreset* out) {
  if (src_w < 2 || src_h < 2) return "source size unknown";
  // Validate before narrowing or multiplying caller-controlled values.
  if (r.frame_rate < 1 || r.frame_rate > 120) return "frame rate out of range";
  if (r.bitrate_bps < 500000 || r.bitrate_bps > 400000000) {
    return "bitrate out of range";
  }
  if (r.keyframe_interval_seconds < 1 || r.keyframe_interval_seconds > 10) {
    return "keyframe interval out of range";
  }
  if ((r.max_height > 0) == (r.source_scale > 0)) {
    return "exactly one of maxHeight and sourceScale is required";
  }
  int h = 0;
  if (r.max_height > 0) {
    h = r.max_height < src_h ? r.max_height : src_h;
  } else {
    if (r.source_scale > 1.0) return "sourceScale must be at most 1";
    h = static_cast<int>(src_h * r.source_scale + 0.5);
  }
  h &= ~1;
  const int w = static_cast<int>((static_cast<int64_t>(src_w) * h + src_h / 2) /
                                 src_h) & ~1;
  SharedScreenPreset p;
  p.id = r.id;
  p.output_width = w;
  p.output_height = h;
  p.frame_rate = r.frame_rate;
  p.bitrate_kbps = static_cast<int>(r.bitrate_bps / 1000);
  p.keyframe_interval_ms = r.keyframe_interval_seconds * 1000;
  p.codec = r.codec;
  const int level = RequiredH264LevelIdc(w, h, r.frame_rate);
  p.profile_level_id = level == 52 ? "640034" : "640033";
  std::string reason = ValidatePreset(p);
  if (!reason.empty()) return reason;
  *out = p;
  return std::string();
}

inline int KeyframeIntervalFrames(const SharedScreenPreset& p) {
  const int64_t frames =
      static_cast<int64_t>(p.keyframe_interval_ms) * p.frame_rate / 1000;
  return frames < 1 ? 1 : static_cast<int>(frames);
}

// Frame interval of the capture clock, in microseconds.
inline int64_t FrameIntervalUs(int frame_rate) {
  return 1000000 / (frame_rate < 1 ? 1 : frame_rate);
}

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool operator==(const Rect& o) const {
    return x == o.x && y == o.y && w == o.w && h == o.h;
  }
};

// Largest centred rectangle with the source's aspect ratio inside the output
// (even-aligned). The video processor scales into this and the rest stays
// black, so a non-matching source is letterboxed instead of stretched.
inline Rect AspectFitDestination(int src_w, int src_h, int out_w, int out_h) {
  Rect r;
  if (src_w <= 0 || src_h <= 0 || out_w <= 0 || out_h <= 0) return r;
  const int64_t lhs = static_cast<int64_t>(src_w) * out_h;
  const int64_t rhs = static_cast<int64_t>(src_h) * out_w;
  if (lhs == rhs) return Rect{0, 0, out_w, out_h};
  if (lhs > rhs) {  // source is wider: fit width
    r.w = out_w;
    r.h = static_cast<int>(static_cast<int64_t>(out_w) * src_h / src_w) & ~1;
  } else {
    r.h = out_h;
    r.w = static_cast<int>(static_cast<int64_t>(out_h) * src_w / src_h) & ~1;
  }
  r.x = ((out_w - r.w) / 2) & ~1;
  r.y = ((out_h - r.h) / 2) & ~1;
  return r;
}

}  // namespace shared_encoder
}  // namespace discourse

#endif  // DISCOURSE_SHARED_ENCODER_SHARED_SCREEN_PRESET_H_
