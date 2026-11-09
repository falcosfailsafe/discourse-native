// Capture (GPU) -> scale/convert to NV12 (GPU, in D3D11Capture) -> NVENC H.264
// -> SharedScreenTrackSource, on one thread paced at the preset's frame rate.
#ifndef DISCOURSE_SHARED_ENCODER_SHARED_SCREEN_PIPELINE_H_
#define DISCOURSE_SHARED_ENCODER_SHARED_SCREEN_PIPELINE_H_

#include <memory>
#include <string>

#include "api/scoped_refptr.h"
#include "shared_encoder_factory.h"
#include "shared_screen_preset.h"
#include "shared_screen_track_source.h"

namespace discourse {
namespace shared_encoder {

// Opens a real capture + NVENC session to prove it works. Windows only; on
// every other platform (or without NVENC) returns available=false + reason.
EncoderCapabilities ProbeEncoderCapabilities();

enum class StartResult {
  kStarted,
  kUnavailable,    // encoder can't run here: caller may fall back
  kInvalidRequest  // bad preset/source: do NOT fall back silently
};

// Creates the track source and starts the pipeline. `source_id` is the
// desktop source id from getDesktopSources ("" = primary monitor, any
// other id is taken exactly as getDesktopSources reported it).
StartResult StartSharedScreen(
    const std::string& source_id, const SharedScreenRequest& request,
    webrtc::scoped_refptr<SharedScreenTrackSource>* source,
    SharedScreenPreset* resolved, std::string* error);

}  // namespace shared_encoder
}  // namespace discourse

#endif  // DISCOURSE_SHARED_ENCODER_SHARED_SCREEN_PIPELINE_H_
