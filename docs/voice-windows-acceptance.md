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

## Follow-up with `discourse.m150.7871.07.3`

The 2026-11-09 local Release build used app revision `4e4d2c7cb`. The
Windows plugin symlink check still required the local NTFS junction and
CMake workaround. CMake downloaded the new ZIP, whose SHA-256 matched
`92f4d7f0cc6a2bee9f834179b5c780c90e8a6c145c0290e2badf8ca0bfe01253`.
The bundled DLL exported the shared-screen API and matched the DLL in
that archive, with SHA-256
`928f3750de53ffe68a5eb47f9b3e32b3a76d494e450f0193c8bb0a054ed091c1`.
The archive identifies fork revision `4a888ed583ca83c4631de2645d5340cda45646cd`.

The host was the same RTX 5090 with driver 621.01 and the High performance
power plan. Windows DisplayConfig reported 7680×2160 at 240 Hz, HDR off
and Wide Color Gamut (WCG) on. With WCG on, the DXGI probe observed
`DXGI_FORMAT_R16G16B16A16_FLOAT` for both the descriptor and the acquired
texture. A separate explicit BGRA8 duplication request returned
`DXGI_ERROR_UNSUPPORTED` (`0x887a0004`). With approval, WCG was temporarily
disabled for the SDR hardware runs. The descriptor and texture then both
reported BGRA8. HDR remained off and the original WCG setting was restored.

The original `.3` app connected to three simulated receivers and opened
NVENC. Native video senders negotiated H.264 High 5.1, but Voice still had
the parameters from transceiver creation, so it rejected all three
receivers. Refreshing the sender reached the next check. libwebrtc then
refused the sender limits when the bridge round-tripped an empty
scalability mode as a named mode. App commit `286768011` refreshes the sender and
omits the empty value. All 29 targeted media tests passed and the one
off-Windows test was skipped.

### SDR mesh results

The first test after the app fix used one process for one sender and three
simulated receivers. All 12 peer-connection endpoints connected. The
sender attached the track to all three receivers and each decoded
3840×1080 H.264 High 5.1. WebRTC reported 59–60 fps for the outbound
passthrough encoders during the sampled part of the run, and NVIDIA
reported one session. Receivers decoded 187, 186 and 187 frames total
before stalling while packet-loss counters rose and sender encoding
continued.

The second test used the same app fix and `.3` with one sender and three
simulated receivers in four separate processes. All processes ran on the
same Windows host. No physical remote receiver machines were used.

| Phase 2 check | Result | Evidence |
| --- | --- | --- |
| 1. 3840×1080 at 60 fps, H.264 High 5.1, to three receivers | Pass in the separate-process run | Three receivers decoded at 59.94, 59.97 and 59.97 fps over the measured counter windows. RTP stats showed 3840×1080, `video/H264`, and `profile-level-id=640033` for all three, with zero RTP packets lost in this run. |
| 2. Exactly one shared NVENC session | Pass | `nvidia-smi encodersessions` showed session 107 for sender PID 12652 at 3840×1080 H.264. All 23 active telemetry samples showed one session. |
| 3. GPU capture and scaling with no raw CPU frame copies | GPU path verified from source and runtime. No independent memory trace. | Capture converts into D3D11 NV12 textures with the video processor and passes those textures to NVENC. The unpaused sender averaged 1.85% machine CPU (maximum 2.37%) and the GPU video encoder averaged 9.65% utilization (maximum 12%). |
| 4. One paused peer freezes and recovers on the next keyframe | Pause and recovery pass. Exact recovery keyframe unverified. | Receiver 2 was suspended for 5.386 seconds. The other two averaged 59.90 and 59.93 fps in that run. After resume, receiver 2 initially decoded more frames with its keyframe counter unchanged, then recorded two additional keyframes in the next sample. The counters cannot distinguish frames queued before suspension from the first live recovery frame. |
| 5. Software fallback when NVENC is unavailable | Pass with shared encoding unavailable in the test | Forced unavailable capability with `.3` delivered software video to all three receivers. The stock DLL with no shared encoder also completed six timed software runs in separate processes. A physical non-NVIDIA machine was unavailable. |
| 6. “You're sharing” placeholder while the shared encoder is active | Pass in app state and widget checks | The live media session reported a withheld preview during the shared run. The targeted widget test renders the placeholder for that state. |
| 7. Upload hint for each preset | Pass in widget checks | With two viewers the displayed values were 20, 32 and 50 Mbps for 720p60, 1080p60 and half-source 60 fps. |

Both separate-process runs stopped the share and disposed all four clients
with exit code zero. The single-process teardown failure below remains
reproducible.

### Teardown with the published DLL

| Scenario | Last completed action | Outcome |
| --- | --- | --- |
| Forced software fallback, three simulated receivers in one process | All receivers decoded video and share stop returned | Crash during sender disposal |
| Shared capture active, before the admission fix | NVENC was active and sender disposal began | Crash during sender disposal |
| Shared capture after the admission fix | All receivers decoded video and share stop returned | Crash during sender disposal |
| Connected SDR mesh with no share | Removing one peer began | Crash during peer removal |

ProcDump captured minidumps. The exception stream in each of the above
showed `0xc0000005`, a read at address zero, at
`libwebrtc.dll+0x218f7`. This reproduces the fault without any share
having started. Share stop itself returned in the stop scenarios.
The released archive does not include the matching PDB. The DLL's
CodeView record identifies `libwebrtc.dll.pdb`, GUID
`7F0BEFFE-36BD-F5D4-4C4C-44205044422E`, age 1.

### Teardown crash: diagnosis and fix (`.4`)

The `.3` DLL in the release archive has the same PE timestamp (`0x6a49e4d0`)
and CodeView GUID as the crashing module, so its code at `+0x218f7` was
disassembled directly (no PDB needed):

```
mov rcx, [rcx+0x18]     ; this->rtc_peerconnection_  (null after Close())
mov rax, [rcx]          ; <- faults here: read at address zero
mov rax, [rax+0x80]     ; virtual call on PeerConnectionInterface
call rax                ; returns a vector, which the loop then walks
```

That is `RTCPeerConnectionImpl::senders()` (`transceivers()` and
`receivers()` have the same shape). `RTCPeerConnectionImpl::Close()` sets
`rtc_peerconnection_ = nullptr`, but these accessors, the state getters and
the Add/Remove/description methods dereferenced it unchecked. A caller that
reaches the wrapper after `close()` faults, which is why it also reproduced
with no share active. It is an upstream wrapper defect, not part of the
shared encoder.

`.4` guards every such use and returns empty lists, "closed" states, null
transceivers or a failure callback instead. A regression test
(`PeerConnectionClosed.AccessorsAreSafeAfterClose`) ran in CI.

**Not yet verified on hardware.** That `.4` stops these crashes in the app
is an inference from the disassembly and the passing unit test. Rerun the
three teardown scenarios above on `.4`. A crash that remains at a different
offset is a separate bug, and the Dart side still calls into a closed
connection and may want its own guard.

### Timed software baseline

All six stock software runs completed and are recorded in
[the baseline tables](voice-windows-baseline.md). Raw CPU and WebRTC logs
are kept with the test evidence.
