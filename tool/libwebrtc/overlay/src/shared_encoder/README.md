# Shared NVENC screen share (Phase 2 native)

Overlay copied over the libwebrtc wrapper (`tool/libwebrtc/stage.py`). Replaces
two wrapper files whole (derived from tag 4515f9c): `BUILD.gn` (sources, deps,
d3d11/dxgi libs, include dirs) and `src/rtc_peerconnection_factory_impl.cc`
(wraps the encoder and decoder factories). Re-derive both on a wrapper bump.

Flow: `D3D11Capture` (DXGI duplication, video processor to NV12, GPU only) ->
`NvencH264Encoder` (one session) -> `SharedScreenTrackSource` (VideoBroadcaster)
-> each peer's `PassthroughVideoEncoder` -> `KeyframeGate`
(`passthrough_core.h`) -> packetizer.

* Frames carry `SharedEncodedFrameBuffer` (kNative, no pixels, `ToI420()` is
  null; recognised by `storage_representation()`, the build has no RTTI). A
  frame that reaches any other encoder fails closed.
* One sender may carry camera and screen: `PassthroughVideoEncoder` sends
  pixel frames to a software H.264 encoder (from the wrapped factory) and
  shared frames through the gate; switching either way restarts on a keyframe.
  While in camera mode the encoder reports the software encoder's info.
* The factories advertise H.264 High 5.1/5.2 (`640033`/`640034`, listed last)
  whenever the wrapped factory has H.264, for decoding and encoding, with or
  without NVENC (one transceiver carries camera and screen; a receiver must be
  able to agree on High). NVENC is only asked when an encoder for those formats
  is created (`ProbeEncoderCapabilities()`, real capture + NVENC session):
  with it the encoder is `PassthroughVideoEncoder`; without it, the stock
  software encoder itself, so a sender without NVENC keeps today's pixel path.
  Limitation: only this build advertises High 5.x. Stock-libwebrtc peers (other
  platforms, browsers without it) do not list it until our build ships there;
  none of that is verified here.
* Drops: libwebrtc may drop frames before `Encode()`; the gap in `sequence`
  closes the peer's gate until the next IDR with SPS/PPS.
* Exports for the plugin: `include/rtc_shared_screen.h` (`extern "C"`).

Verification status: `tool/libwebrtc/run_native_tests.sh` builds the portable
core tests (gate, passthrough, preset math, Annex B) and compile-checks the
NVENC wrapper against the vendored header on Linux. `d3d11_capture.cc` and
`nvenc_h264_encoder.cc` also cross-compile against Windows API headers with
MinGW (`-std=c++17 -Wall -Wextra -Werror`). This is a compile check, not a
Windows execution or a GPU test. The fork's Windows MSVC/Clang build additionally
builds/runs `discourse_shared_encoder_unittests`, which tests the actual
libwebrtc passthrough adapter. That integration build and hardware acceptance
remain required; nothing has run on a GPU yet.
