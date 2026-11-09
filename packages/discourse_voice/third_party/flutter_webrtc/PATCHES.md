# Local patches

Voice vendors the published `flutter_webrtc` 1.6.3 package under
`packages/discourse_voice/third_party/flutter_webrtc`, from
<https://pub.dev/packages/flutter_webrtc/versions/1.6.3>. The owner-local
`packages/discourse_voice/tool/vendor_contract.json` connects that external
source tree to this reviewed inventory without teaching the root runner about
Voice or WebRTC.

- Archive SHA-256: `b98ef03376401e54ff87fc78907b38e562b15ca8424aba589c263eddc3c1ecf9`
- Upstream source: <https://github.com/flutter-webrtc/flutter-webrtc/tree/v1.6.3>

Version 1.6.3 updates the native WebRTC libraries and preserves every local
patch below. LiveKit 2.13.2 uses this same baseline.

The archive is the review baseline. The sections below enumerate every known
local delta; this file is not a claim that the directory is otherwise
byte-for-byte identical to a Git checkout, because package-manager metadata is
not part of the published archive.

## iOS privacy manifest

The upstream iOS screen-broadcast reader calls `mach_absolute_time()` to create
elapsed video-frame timestamps, but version 1.6.3 does not provide a privacy
manifest for that required-reason API.

This copy adds `PrivacyInfo.xcprivacy` to the `flutter_webrtc` Swift package and
CocoaPods resource bundle. It declares the System Boot Time category with
approved reason `35F9.1`, for measuring elapsed time between events inside the
app.

Files:

- `ios/flutter_webrtc/Sources/flutter_webrtc/PrivacyInfo.xcprivacy`
- `ios/flutter_webrtc/Package.swift`
- `ios/flutter_webrtc.podspec`

Introduced with the vendored package in commit `b4d47550958766790d2c1ea2fa1ef29328dc9fcc`.

## Darwin remote-track identity

The upstream `FlutterRTCMediaStream` implementation constructs a new Dart-side
track identifier while reading remote stream tracks. Voice needs the native
WebRTC track identifier to stay stable across the stream and renderer APIs, so
the iOS/macOS/common Darwin copies now preserve `trackId`.

Files:

- `common/darwin/Classes/FlutterRTCMediaStream.m`
- `ios/flutter_webrtc/Sources/flutter_webrtc/FlutterRTCMediaStream.m`
- `macos/flutter_webrtc/Sources/flutter_webrtc/FlutterRTCMediaStream.m`

Commit: `46169d8a2a22c5667a62791ccb17974404316bd0`.

## Awaited native log severity

The published logger setter returns before its native `setLogSeverity` method
finishes. Voice diagnostics must know whether native logging was installed
before reporting capture as active, so the local API returns and awaits that
operation.

Files:

- `lib/src/helper.dart`
- `lib/src/native_logs_listener.dart`

Commit: `7400253b0de111c79957416ebf6b1df8c5da2831`.

## Renderer track lifecycle

The published renderer binds videos by stream and can keep stale native track
state after a track is replaced or detached. Voice switches camera, screen,
and remote participant tracks on a long-lived renderer. The local patch adds a
track-aware renderer binding, clears bindings deterministically, and mirrors
that behavior across Android, Darwin, C++, and Dart implementations.

Files:

- `android/src/main/java/com/cloudwebrtc/webrtc/MethodCallHandlerImpl.java`
- `common/cpp/include/flutter_video_renderer.h`
- `common/cpp/include/flutter_webrtc_base.h`
- `common/cpp/src/flutter_peerconnection.cc`
- `common/cpp/src/flutter_video_renderer.cc`
- `common/cpp/src/flutter_webrtc.cc`
- `common/cpp/src/flutter_webrtc_base.cc`
- `common/darwin/Classes/FlutterWebRTCPlugin.m`
- `ios/flutter_webrtc/Sources/flutter_webrtc/FlutterWebRTCPlugin.m`
- `lib/src/native/rtc_video_platform_view_controller.dart`
- `lib/src/native/rtc_video_renderer_impl.dart`
- `macos/flutter_webrtc/Sources/flutter_webrtc/FlutterWebRTCPlugin.m`

Commit: `b401810ff43d3e20fe4bc41ece6fee33722dd60f`.

## Shared screen encoder bridge

Voice shares a Windows screen through one NVENC session whose encoded frames
every peer connection sends (docs/plans/voice-hardware-encoding.md). The Dart
bridge adds `SharedScreenEncoder` with three native method-channel calls, none
of which exists upstream. Each answers "unavailable" off Windows or when the
native side lacks it, so callers keep ordinary capture.

- `getSharedScreenEncoderCapabilities` returns `{available, codecs, maxWidth,
  maxHeight, maxFrameRate, reason}`.
- `getSharedScreenMedia` takes `{preset: {id, maxHeight | sourceScale,
  frameRate, bitrate, keyframeIntervalSeconds, codec}, audio, sourceId?}` and returns the
  `getDisplayMedia` shape `{streamId, audioTracks, videoTracks}`. It captures
  the monitor named by the optional top-level `sourceId`, or the primary
  monitor, without a picker, when it is absent or empty. Error code
  `sharedScreenUnavailable` means the encoder cannot run and the caller may
  fall back; any other error is final.
- `setPeerBitrate` takes `{peerConnectionId, minBitrate, startBitrate,
  maxBitrate}` in bits per second and returns `{result: true}`. It maps to
  `PeerConnectionInterface::SetBitrate`; a negative start or maximum leaves
  that value unchanged.

Files:

- `lib/flutter_webrtc.dart`
- `lib/src/native/rtc_peerconnection_impl.dart`
- `lib/src/native/shared_screen_encoder.dart`
- `lib/src/web/shared_screen_encoder.dart`
- `test/unit/shared_screen_encoder_test.dart`

## Provenance metadata

The patch manifest itself is local review metadata and is not part of the
published archive.

Files:

- `PATCHES.md`

## Windows plugin files

The application has a Windows runner (`windows/` at the repository root) that
builds this package's `windows/` plugin, the shared `common/cpp` sources and the
`third_party/` libwebrtc download. Phase 0 built them as published; the shared
screen encoder bridge below now patches some of them. Every Windows-built file
is compared like any other vendored file, so each local change, including a
`libwebrtc_version.ini` bump to our own build, must be listed under a Files
heading before the check passes.
`test/vendor_provenance_contract_test.dart` pins that those files are part of
the compared tree and that the Windows build manifest is inventoried.

## Shared screen encoder bridge (native)

Windows shared NVENC screen share needs the plugin to start a screen share whose
frames are encoded once, and to set per-connection bandwidth estimation limits.
`common/cpp/src/flutter_shared_screen.cc` handles
`getSharedScreenEncoderCapabilities`, `getSharedScreenMedia` and
`setPeerBitrate`. It looks up `extern "C"` exports of our own
libwebrtc build (`rtc_shared_screen.h`) with `GetProcAddress`, so the plugin
still loads against a stock `libwebrtc.dll` and then reports the encoder as
unavailable. `FlutterWebRTCBase` befriends the new class so it can register
tracks like `FlutterScreenCapture` does. The calls run synchronously on the
platform thread (no background work that could outlive the plugin).
`FlutterWebRTCBase` also gains a `local_track_removed_` callback, fired when a
local track or stream is disposed, so the shared stream's loopback audio stops
with its track.

Files:

- `common/cpp/include/flutter_shared_screen.h`
- `common/cpp/src/flutter_shared_screen.cc`
- `common/cpp/src/flutter_media_stream.cc`
- `common/cpp/include/flutter_webrtc.h`
- `windows/CMakeLists.txt`
- `linux/CMakeLists.txt`
- `elinux/CMakeLists.txt`

## Pinned Windows release selection

The download CMake supports separate Windows x64 release URL, version and
SHA-256 keys without moving Linux or other architectures off their stock
release. Custom Windows selection requires all three values. Downloads are
verified before promotion, cached by release version and extracted with an
origin/version stamp, so changing a release cannot silently reuse stale DLLs.
The custom Windows wrapper fork is
<https://github.com/falcosfailsafe/libwebrtc>. The source build recipe and
overlay are reviewed in `tool/libwebrtc` in the application repository; see
its README for immutable source pins, release and hardware gates.

Files:

- `third_party/CMakeLists.txt`

## Verifying the review diff

From the application repository root, run:

```sh
dart run tool/vendor_provenance_contract.dart
```

The check downloads only the official pub.dev metadata and archive, verifies
the archive SHA-256 above, and compares every regular file in the vendor tree.
It passes only when the differing files are exactly the inventory above; empty
package-manager metadata directories are deliberately outside the file contract.
Any additional vendored source difference must be documented here before
release.

Remove this vendored copy and restore the hosted dependency only after an
upstream release provides equivalents for every patch above. Then run
`flutter pub get`, repeat the iOS archive privacy inspection, and exercise
Voice track replacement on each supported native platform.
