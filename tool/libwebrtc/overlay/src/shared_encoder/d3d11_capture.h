// GPU-only desktop capture for the shared NVENC screen share.
//
// DXGI Desktop Duplication delivers each desktop frame as a D3D11 texture;
// one ID3D11VideoProcessor pass scales it to the target size and converts it
// to NV12 into a small ring of textures. No pixel is ever mapped or read back
// to the CPU, so the NV12 textures can be handed straight to NVENC.
//
// Like nvenc_h264_encoder.h this header has no D3D11 dependency: the device
// and textures are opaque pointers, `ID3D11Device*` and `ID3D11Texture2D*`.
// They stay owned by the capture object and are not AddRef'd for the caller.
//
// Threading: Create(), CaptureNext() and destruction must happen on one
// thread. The device is switched to multithread-protected mode so NVENC may
// use it from its own thread.
//
// Rotated outputs and HDR / wide-gamut color spaces are refused at Create().
// The acquired desktop texture must be 8-bit BGRA before conversion.
// The mouse cursor is not part of the duplicated image and is not drawn.
// Protected content is captured black by the OS.
//
// Colors: sRGB full-range RGB is converted to BT.709 limited-range ("16-235")
// NV12. The encoder must tag its stream the same way.
//
// On non-Windows builds Create() always fails with an error, so the shared
// encoder sources keep compiling in the portable tests.
#ifndef DISCOURSE_SHARED_ENCODER_D3D11_CAPTURE_H_
#define DISCOURSE_SHARED_ENCODER_D3D11_CAPTURE_H_

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace discourse {
namespace shared_encoder {

class D3D11Capture {
 public:
  // Number of NV12 textures in the ring. A texture returned by CaptureNext()
  // stays untouched by the capture for kPoolSize - 1 further calls.
  static constexpr size_t kPoolSize = 4;

  // Opens the `output_index`-th monitor attached to the desktop, counting
  // across all adapters in DXGI enumeration order, on that monitor's own GPU.
  // Frames are scaled (stretched, no letterboxing: choose a target with the
  // source's aspect ratio) to `target_width` x `target_height`, which must be
  // even and in [2, 8192]. `frame_rate` in [1, 240] is the rate CaptureNext()
  // paces itself to, independent of the display's refresh rate.
  // Returns null and sets *error (when non-null) on failure.
  static std::unique_ptr<D3D11Capture> Create(int output_index,
                                              int target_width,
                                              int target_height,
                                              int frame_rate,
                                              std::string* error);

  // As above, selecting the monitor by a `getDesktopSources` screen id:
  //  * "": the primary monitor;
  //  * "0" is the EnumDisplayDevices index 0, not a primary-monitor alias;
  //  * a decimal HMONITOR value of an attached output;
  //  * otherwise a decimal GDI display-device index N (< 1024), meaning the
  //    output that EnumDisplayDevices lists at position N (its DeviceName,
  //    e.g. \\.\DISPLAY2, equals the output's name).
  // Anything else, including an id that matches no attached output, fails
  // with an error; an id is never silently mapped to another monitor.
  static std::unique_ptr<D3D11Capture> Create(const std::string& source_id,
                                              int target_width,
                                              int target_height,
                                              int frame_rate,
                                              std::string* error);

  // Reports the pixel size of the monitor `source_id` selects (same id rules
  // as Create), without opening a duplication, so the caller can pick a preset
  // with the right aspect ratio. Fails for ids Create would reject and for
  // rotated or HDR / wide-color monitors.
  static bool QueryOutput(const std::string& source_id, int* width,
                          int* height, std::string* error);

  ~D3D11Capture();
  D3D11Capture(const D3D11Capture&) = delete;
  D3D11Capture& operator=(const D3D11Capture&) = delete;

  // `ID3D11Device*` the textures belong to; the device NVENC must be opened on.
  void* device() const;

  // The NV12 ring, as `ID3D11Texture2D*`, fixed for the object's lifetime so
  // it can be registered with the encoder once. kPoolSize entries.
  const std::vector<void*>& textures() const;

  // Blocks until the next frame tick (1 / frame_rate after the previous one),
  // then returns the NV12 texture holding the desktop as of that tick, and
  // its index in textures() through `texture_index` when non-null.
  //
  // If the desktop did not change since the previous tick, the previous
  // texture is returned again (the stream keeps its frame rate and keyframe
  // schedule on a static screen). The texture is complete on the GPU when
  // this returns: it may be encoded immediately and synchronously.
  //
  // Returns null when there is no frame to give:
  //  * `*error` empty: nothing has been presented on the desktop yet, try again.
  //  * `*error` set: capture failed; failed() says whether it is permanent.
  // A lost duplication (display mode change, secure desktop, GPU reset) is
  // retried on later calls and only becomes permanent after repeated failure
  // or if the output's size changed.
  void* CaptureNext(std::string* error, size_t* texture_index = nullptr);

  // True after an unrecoverable error. CaptureNext() then returns null.
  bool failed() const;

  // Name of the GPU that drives the captured monitor (UTF-8).
  const std::string& adapter_name() const;

  // Size of the captured desktop image in pixels (the source, before scaling),
  // for choosing a preset.
  int width() const;
  int height() const;
  int target_width() const;
  int target_height() const;
  int frame_rate() const;

 private:
  class Impl;
  explicit D3D11Capture(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace shared_encoder
}  // namespace discourse

#endif  // DISCOURSE_SHARED_ENCODER_D3D11_CAPTURE_H_
