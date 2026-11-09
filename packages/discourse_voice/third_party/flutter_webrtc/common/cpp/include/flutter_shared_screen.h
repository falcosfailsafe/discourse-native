#ifndef FLUTTER_SHARED_SCREEN_HXX
#define FLUTTER_SHARED_SCREEN_HXX

#include <map>
#include <memory>
#include <string>

#include "flutter_common.h"
#include "flutter_webrtc_base.h"
#include "loopback_capturer.h"
#include "rtc_audio_source.h"

namespace flutter_webrtc_plugin {

// getSharedScreenEncoderCapabilities, getSharedScreenMedia and
// setPeerBitrate: the Windows shared NVENC screen share.
//
// The libwebrtc build behind it exports `extern "C"` entry points
// (rtc_shared_screen.h). They are looked up at run time with GetProcAddress,
// so this plugin still loads against a stock libwebrtc.dll; without the
// exports the encoder simply reports unavailable. Off Windows it always does.
class FlutterSharedScreen {
 public:
  explicit FlutterSharedScreen(FlutterWebRTCBase* base);
  ~FlutterSharedScreen();
  FlutterSharedScreen(const FlutterSharedScreen&) = delete;
  FlutterSharedScreen& operator=(const FlutterSharedScreen&) = delete;

  void GetCapabilities(std::unique_ptr<MethodResultProxy> result);
  void GetSharedScreenMedia(const EncodableMap& params,
                            std::unique_ptr<MethodResultProxy> result);
  void SetPeerConnectionBitrate(const EncodableMap& params,
                                std::unique_ptr<MethodResultProxy> result);

 private:
  FlutterWebRTCBase* base_;
  // Loopback audio of each shared screen stream, by audio track id. Stopped
  // when that track is disposed (see FlutterWebRTCBase::local_track_removed_)
  // or when the plugin goes away.
  struct Loopback {
    std::unique_ptr<LoopbackCapturer> capturer;
    scoped_refptr<RTCAudioSource> source;
  };
  std::map<std::string, Loopback> loopbacks_;
};

}  // namespace flutter_webrtc_plugin

#endif  // FLUTTER_SHARED_SCREEN_HXX
