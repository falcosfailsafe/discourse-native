# Voice shared NVENC acceptance on Windows

## 2026-11-09

| Item | Observed |
| --- | --- |
| Source revision | `a54f65fa04d7fd3a884c7efd7a48bda3e2f637e9` |
| Windows | Windows 11 Pro x64, build 26300.9711 |
| Sender | Ryzen 7 5800X3D, 32 GB RAM, GeForce RTX 5090 |
| NVIDIA | Driver 621.01, WDDM, `nvEncodeAPI64.dll` present |
| Display | Odyssey G95NC, 7680×2160 at 240 Hz |
| HDR | Off in Windows Settings. DXGI output color space was `DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709` and the output reported 10 bits per color. |
| Power plan | High performance |
| WebRTC package | `discourse.m150.7871.07.2`, downloaded archive matched SHA-256 `b6d427990ee9f5b883204c06c321c854d97c662d1715b995fb52262a5cdba0d6` |

## Release build

Both lockfile checks passed with Flutter 3.47.7. The libavif 0.1.1 cache
received the CI workaround for `C:/o` and `libdav1d.a`. The local build
used Visual Studio 2022 Build Tools 17.14.44, Windows SDK 10.0.26100.0, and
Rust 1.92.0.

The Flutter CLI stopped at the Windows plugin symlink privilege check. The
workaround was to create local NTFS junctions at the generated plugin paths
and invoke Flutter's generated CMake project directly:

```powershell
cmake -S windows -B build/windows/x64 -G "Visual Studio 17 2022" -A x64 -DFLUTTER_TARGET_PLATFORM=windows-x64
cmake --build build/windows/x64 --config Release --target INSTALL
```

The Release bundle installed successfully and rebuilt after the software
capture fix. `findstr` found
`LibWebRTCSharedScreenCreateSource` in the bundled DLL. Its SHA-256 was
`6df1c61625a2815b7485d216d08512090f9a090eeb58d7ce7db1c0c415b1dc94`,
matching the DLL extracted from the verified archive.

## Capture blocker

The verified release DLL exported `LibWebRTCSharedScreenCreateSource`.
Calling `LibWebRTCSharedScreenGetCapabilities` on the physical sender returned
API version 1, `available=0`, and this reason:

```text
the display is not 8-bit BGRA (HDR or wide-color is on); screen share needs an SDR display
```

A separate x64 D3D11 diagnostic called `IDXGIOutput1::DuplicateOutput` on the
same display and queried both the duplication descriptor and a captured texture.

```text
Output \\.\DISPLAY1, bits per color 10, color space 0
DuplicateOutput HRESULT 0x00000000
Duplication mode 7680 x 2160, format 10 (BGRA8 is 87)
AcquireNextFrame HRESULT 0x00000000
Frame texture 7680 x 2160, format 87
```

The descriptor reported `DXGI_FORMAT_R16G16B16A16_FLOAT` (10). The actual
texture was `DXGI_FORMAT_B8G8R8A8_UNORM` (87). The branch's
`CheckOutputSupported` checks the descriptor in `OpenDuplication`, before
`CaptureNext` can validate the texture. This rejects the SDR output before
the capability probe opens NVENC. The current pin and native source remain
as tested. Correcting the descriptor check requires a new libwebrtc release
from the fork. Retain rotation and output color-space validation and validate
the acquired texture before converting it.

## Phase 2 results

| Acceptance item | Status | Evidence |
| --- | --- | --- |
| 1. 3840×1080 at 60 fps, H.264 High 5.1 or 5.2, to three peers | Fail | The pinned DLL rejected the target display during capture initialization. No shared stream or negotiated codec was measured. |
| 2. Exactly one NVENC session for the share | Blocked | Initialization failed before NVENC. An idle `nvidia-smi encodersessions` check showed no active sessions. |
| 3. GPU-only capture and scaling, with sender CPU and GPU encode usage | Blocked | Shared capture did not initialize. No sharing load was measured. |
| 4. One lagging peer freezes and recovers on the next keyframe | Blocked | Requires an active shared stream and three receivers. |
| 5. Software fallback when NVENC is unavailable | Fail during local teardown | After the app fix, all three simulated receivers decoded software-encoded video. The diagnostic then hit an access violation in the pinned DLL. A separate non-NVIDIA sender was unavailable. |
| 6. “You're sharing” placeholder during shared encoding | Widget pass. Hardware blocked. | The Windows widget test verified the placeholder for a withheld preview. The pinned DLL could not start a shared stream. |
| 7. Upload requirement in preset picker | Widget pass | With two viewers the picker displayed 20, 32 and 50 Mbps for 720p60, 1080p60 and half-source 60 fps, respectively. The widget test supplied encoder availability. |

## Software fallback defect

A local Release diagnostic ran the app's `MeshVoiceMediaSession` and bundled
Windows WebRTC plugin with one sender and three simulated receivers in one
process. Signaling stayed on the test machine. The first attempt reached the
software fallback and failed with `Unable to getDisplayMedia: source not found!`.
The plugin expects a populated desktop source cache, which Voice had not
populated on Windows. The app fix enumerates screens and sends an explicit
source ID to the ordinary capture method.

After the fix, all 12 peer connection endpoints in the four-client mesh
connected. The three simulated receivers decoded 2, 3 and 3 frames at the
final check. All three outbound RTP reports identified `libvpx` as the
encoder and showed adaptation from 3840×1080 to 2560×720 during the smoke
test. NVIDIA session count and encoder utilization stayed at zero in the
recorded samples. This was a short functional test with zero remote physical
receiver machines.

A repeat delivered 6, 10 and 15 decoded frames. Windows Application Error
event 1000 recorded `libwebrtc.dll` faulting with exception `0xc0000005` at
offset `0x218f7` on both runs. A third run decoded video on all three
receivers and logged `shareStopped=true` before entering disposal of the
sender. The process faulted before sender disposal returned. Its behavior
with separate app processes remains unverified.

The targeted Voice media group finished with 28 passes and one skip.
The skipped case exercises the real bridge off Windows. Running it on
Windows reached an uninitialized platform channel in the unit-test process.
Four targeted screen-share widget tests also passed on Windows, including
a local assertion for the 720p preset.

## Software baseline

The [baseline](voice-windows-baseline.md) tables contain measured call results
only. No timed runs with the stock libwebrtc build were completed at either
resolution. The local smoke used the pinned DLL, colocated receivers and
no timed motion workload.
