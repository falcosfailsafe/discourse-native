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

That is one of the vector accessors on `RTCPeerConnectionImpl`. The virtual
slot called on the released object identifies which: `+0x78` is `GetSenders`
(`senders()`, crash at `+0x211b7`), `+0x80` is `GetReceivers` (`receivers()`,
the `+0x218f7` crash the Windows run recorded), and `transceivers()` is the
next slot. They all have the same shape. `RTCPeerConnectionImpl::Close()` sets
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

## 2026-11-10: AWS GPU server (Tesla T4)

A second machine, so these numbers are for a **T4, not the RTX 5090** reference
sender, and a **60 Hz** virtual display, not the 240 Hz monitor. They show the
design works; they say nothing about 5090 headroom.

| Item | Observed |
| --- | --- |
| Machine | AWS `g4dn.2xlarge` (8 vCPU), Windows Server 2025 Datacenter |
| GPU | Tesla T4, driver 596.86, WDDM, `nvEncodeAPI64.dll` present |
| Display | [Virtual Display Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) 25.7.23 (MIT, signed by the SignPath Foundation) as the primary monitor, **7680×2160 at 60 Hz**, SDR |
| Session | A dedicated local user auto-logged-on at the console, tests started through a scheduled task with an Interactive logon (see `tool/libwebrtc/native_tests/windows/README.md`) |
| libwebrtc | `discourse.m150.7871.07.4` for the first runs (the bundled DLL's SHA-256 matched the verified release). The multi-peer scenarios were rerun on `discourse.m150.7871.07.5` (ZIP SHA-256 `d1f5c83c…9083`, built from fork commit `f3c983f`) with the same results, and `.5` is what the app now pins. |
| Programs | `tool/libwebrtc/native_tests/windows/` |

### Results

| Check | Result |
| --- | --- |
| NVENC works, no display needed (`nvenc_smoke_test`): synthetic 7680×2160 → 3840×1080 at 60 fps, 25 Mbps | 600 frames in 10.19 s = 58.9 fps (59.2 fps in a second run), 22 Mbps, key frame every 2000 ms, SPS `640033`. GPU scale + convert 1.0 ms, encode 12.9 ms mean (p95 13.3). Unpaced: 119 fps. `nvidia-smi`: 1 NVENC session, encoder 72%. |
| Independent decode | `ffprobe`: H.264 High, level 5.1, 3840×1080, 600 frames. `ffmpeg` decoded the whole stream without errors. |
| Real capture into NVENC (`capture_encode_test`): DXGI duplication of the 7680×2160 display → 3840×1080 at 60 fps | 600 frames in 10.08 s = 59.5 fps, 0 capture errors, 5 key frames, SPS `640033`, 1 NVENC session (encoder 70%). Decoded frames show the moving test content. |
| **Three peers through the real DLL (`shared_screen_peers_test`)**, 20 s | All three negotiated `H264 640033 packetization-mode 1`. All three receivers decoded **3840×1080 at 59.8 to 59.9 fps** (steady state). `nvidia-smi`: **exactly one NVENC session**. Sender stats: encoder `SharedNVENC`, 3840×1080, 60 fps, no quality limitation, never rescaled. |
| **A peer that cannot keep up** (peer 0 capped at 300 kbps against a stream of about 2 Mbps; 3 peers) | Peer 0 fell from 60 fps to 11 fps on average, with two full seconds at zero frames while it waited for a key frame. When the cap was lifted it was back at 59 fps within three seconds. **The other two peers stayed at 59.9 fps the whole time** and the encoder kept one session. (A 2 Mbps cap did not bite because the animation only needs about 2 Mbps; this is a real drop-and-recover, not a simulation.) |
| A viewer leaves mid-stream | The other two receivers stayed at 59.9 fps before and after. |
| A viewer joins late | The fourth viewer's first frame arrived 1842 ms after joining (key frame interval 2 s). Still one NVENC session with four peers. |
| Teardown, five full create/destroy cycles | 71 to 83 ms each, no crash, and the NVENC session count was back to 0 after every cycle. |
| Closed-connection regression, on the real DLLs | The harness closes a viewer and then calls `senders()`, `transceivers()`, `receivers()` and the state getters on it. `.3`: crash `0xC0000005` at `+0x211b7`. `.4`: passes. |
| `GetCapabilities` in a session with no display (SSH), on the real DLLs | `.4`: crash `0xC0000005`. `.5`: `available: 0, reason: no attached display matches screen id ''`, exit 0. |
| Software sender through the same harness | Two receivers decoded 30 fps and teardown was clean. This used a custom source with default negotiation (VP8, libwebrtc downscaled to 480×270), so it covers the fallback plumbing and teardown, **not** software H.264 High. |

### The Flutter session layer (`integration_test/voice_windows_hardware_driver.dart`)

The real `MeshVoiceMediaSession` (the Dart class the app uses), the real
`flutter_webrtc` plugin and the pinned `.5` DLL, run in one process: a sender
and three viewers joined by an in-memory signaling bus, with the sender on the
7680×2160 virtual display. It is built as a Release app and run in the console
session; it writes its results to a log file. **All 33 checks passed, none failed.**

| Check | Result |
| --- | --- |
| Capability and preview | The sender reports the shared encoder available, the self-preview is replaced by the "You're sharing" placeholder (`screenSharePreviewHidden`), and no viewer was refused the shared stream (`screenShareUnsupportedPeerIds` empty). |
| Three viewers, `halfSource60` preset | Each viewer decoded **3840×1080 at 60.2, 60.5 and 59.9 fps**. Sender statistics: encoder `SharedNVENC`, 3840 wide, no quality limitation. `nvidia-smi`: exactly **one** NVENC session. |
| A viewer leaves (roster change plus session dispose, the app's peer-removal path) | The other two stayed at 60.2 fps (60.3 before). Still one session. No crash. |
| Stop and restart the share, three rounds | Each stop released the NVENC session (0 after 3 s); each restart came back at 60.0 to 60.6 fps with one session. |
| Whole room torn down | Zero NVENC sessions afterwards. |
| Sender **without** the hardware encoder (capability faked as unavailable) | The session kept today's path: ordinary screen capture, self-preview kept, viewers decoded 27 fps, encoder `OpenH264`, no NVENC session, clean teardown. The capture took the 800×600 Basic Display, so this shows the fallback works, not a quality number. |

Harness notes: the session ignores signals from participants it has not synced
yet, so the bus holds signals until every member has the roster; ICE candidates
are batched as `{'events': [...]}` and the bus delivers them one by one as the
real signaling layer does; only the sender publishes (receive-only transceivers
cannot send); the machine has no microphone, so the sender's microphone request
fails at once and the roster sync creates the peers.

### Load, presets and display changes (same machine)

Content: a GPU noise window on the 7680×2160 virtual display (8-pixel blocks,
amplitude 0.2: heavy, constantly changing, but within what the encoder can
control). Three peers unless stated. All through the real `libwebrtc.dll`
(`.5`).

| Check | Result |
| --- | --- |
| Does CBR hold the target on heavy content? (`capture_encode_test`, 25 Mbps target) | Yes: **24.2, 24.4, 24.0 and 22.4 Mbps** at noise amplitudes 0.05 to 0.4. P-frames averaged 407 to 414 kbit against 417 kbit per frame, key frames 450 to 770 kbit. |
| Does it hold when content cannot be compressed? | No, and nothing could: with full-range white noise the slice QP is already 51 (the maximum) and a frame is still about 3.3 Mbit, so **195 Mbps at any target** (5, 25 and 100 Mbps gave the same). This is the limit of the codec, not a rate-control fault. |
| What the network sees under that overload (3 peers) | libwebrtc holds each peer to the configured rate: **23.9 Mbps per peer**, so only 8 to 9 frames per second were delivered, with counted freezes and no crash and no upload beyond the cap. The share degrades to a slideshow, not to an upload spike. |
| Sustained load, 3 peers, ~24 Mbps each, **90 s** | Sent 23.9 Mbps per peer, 71.8 Mbps in total. No quality limitation, no packet loss, no freezes. Process memory 224 to 250 MB, handles 565 to 572. |
| **10-minute soak**, same load | RTP delivery **60.0 fps** to every viewer from the third minute on (56.8 in the first minute while ramping up, 59.3 in the second), 24.4 Mbps per peer, no quality limitation, no loss. GPU memory flat at 755 MiB, encoder 69 to 70%. Process memory plateaued around 255 MB (239 to 261 MB, no steady climb), handles 563 to 573. Teardown 114 ms, NVENC sessions 0. |
| What one viewer's software decoder can do (3840×1080, 24 Mbps, worst-case content) | **1 viewer: 55 fps decoded** (RTP delivery 58.7, process CPU 17% of the machine, about 1.3 logical CPUs). 2 viewers: 54 fps. 3 viewers: 49.5 fps, at which point the process is CPU-bound (49%). The sender and the network were never the limit. This is the Phase 4 measurement. |
| Presets and H.264 levels | 1920×1080 at 60 fps (1080p preset on a 1080p display): **60.0 fps**. 3840×1080 at 60 fps (1080p preset on 32:9): 58.2 fps. 2560×720 at 60 fps (720p preset): 57.7 fps. 3840×1080 at **30 fps**: 29.4 fps. 3840×2160 at 60 fps: encoded at level 5.2 (`640034`), 60.1 fps delivered, 35 fps decoded by one software decoder. All with a clean teardown. The 3 to 4% shortfall appeared only on the 7680×2160 display; a likely cause is the noise window rendering at that size on the same GPU, but that was not isolated. |
| Twenty rapid create, stream and teardown cycles (3 peers, heavy load) | All twenty passed: teardown 84 to 119 ms, NVENC sessions 0 after each, no crash. |
| **Six viewers**, 720p60 preset (2560×720, ~10 Mbps each) | 9.6 Mbps per peer (57.4 Mbps in total), **57.8 fps delivered and decoded**, no loss, no freezes, no quality limitation, one NVENC session, memory 202 to 225 MB, teardown 141 ms. |
| Six viewers at 3840×1080 (~25 Mbps each) | **Not a valid sender result:** the six software decoders in one process starve the CPU, loopback sockets overflow and the stream suffers heavy packet loss and retransmission (total 189 to 207 Mbps, delivery collapsed to about 2 fps). The sender kept encoding 60 fps with one NVENC session and no quality limitation, so the limit was this harness and machine; a real test needs the viewers on other machines. |
| A resolution change under a live share (7680×2160 to 3840×2160 at 12 s) | The share **dies**: delivery fell to zero, the NVENC session closed, and every viewer froze on the last frame. No crash, and teardown took 12 ms. A fresh share worked immediately at the new size (1920×1080, 60 fps). The app is not told: nothing signals the failure, so it still believes it is sharing. |
| Rotated display (90 degrees) | Refused: `rotated displays are not supported`, exit code 3 (so the app falls back). Available again once the rotation was undone. |
| HDR on (Windows advanced color) | Refused: `the display uses HDR or a wide color space; screen share needs an SDR display`. Available again with HDR off. |

#### Software versus shared NVENC at the same sizes

One sender process and three viewers in a **separate** process, so the sender's
CPU does not include the viewers' decoders. `integration_test/voice_windows_baseline_driver.dart`,
three runs per cell (the software 1080p and 7680×2160 cells have six, see the
note). CPU is the sender process as a percentage of all 8 logical CPUs, with
cores in brackets.

| Path | Display | Frames encoded | Picture sent | Sender CPU | Sent (3 peers) | Viewers decode |
| --- | --- | --- | --- | --- | --- | --- |
| Software (today) | 1920×1080 | 20.3 to 20.8 fps | 480×270 to 640×360 | 5.8 to 7.6% (0.47 to 0.61) | 1.7 Mbps | 21 fps |
| **Shared NVENC** | 1920×1080 | **60.0 fps** | **1920×1080** | **4.0 to 5.0% (0.32 to 0.40)** | 15.0 Mbps | 61 fps |
| Software (today) | 7680×2160 | 12.1 to 13.9 fps | 1280×360 to 2560×720 | **18.1 to 40.9% (1.44 to 3.27)** | 2.0 to 2.5 Mbps | 12.2 to 14.2 fps |
| **Shared NVENC** | 7680×2160 | **60.0 fps** | **3840×1080** | **5.8 to 8.6% (0.47 to 0.69)** | 23.9 Mbps | 44 to 49 fps |

The software path is held to about 2 Mbps by libwebrtc's default bandwidth
estimate (every run reports `limit=bandwidth`), which is why it sends a tiny
picture at 12 to 20 fps whatever the display. At 7680×2160 it also spends up to
3.3 cores on capture and conversion to deliver 12 fps; the shared path delivers
five times the frame rate at about a fifth of the CPU. Viewers at 7680×2160
are the cost that remains: their software decoders use 3.6 to 3.9 cores for
three viewers.

Note on the runs: the first software matrix contained six runs per size
because a second batch labelled "NVENC" had silently run the software path (see
the capability-cache finding below), so the software cells have six samples.
The NVENC rows come from a separate matrix in a fresh process with
`hardware=true` confirmed in every row, encoder `SharedNVENC` in every row.

### Findings from these runs

1. **The encoder capability was cached forever (fixed in `.6`, verified).**
   `GetEncoderCapabilities` probed once per process. A probe made while the
   display was rotated, in HDR, detached or mid-change stayed "unavailable" for
   the life of the app, even after the display was fixed: reproduced with the
   real DLL (rotated display, then restored: every later answer still said
   `rotated displays are not supported`). It also made a whole benchmark
   quietly run on the software path. A positive answer is now kept and a
   negative one is re-probed after 3 seconds. Verified on the real DLLs with
   `scenarios/cache-test.ps1`: with `.5` all five answers stayed
   `rotated displays are not supported` after the display was restored; with
   `.6` the second answer was `available` (NVIDIA Tesla T4).
2. **A display change kills a live share silently.** The pipeline stops, the
   encoder is released, viewers freeze, and the app is not told. Recommended:
   end the track (so the app sees it end) or restart the pipeline at the new
   size. Not changed here.
3. **The negotiated level can be lower than the stream's.** At 3840×2160 and
   60 fps the stream is level 5.2 but the connection negotiated `640033`
   (level 5.1). The app's admission check only requires 5.1 or above. No shipped
   preset reaches level 5.2, and a software decoder decodes it regardless, but
   a strict hardware decoder may not. The check should compare the negotiated
   level with the one the preset needs.
4. **The bitrate cap is a hard ceiling for upload.** Under overload libwebrtc
   holds each peer to the preset rate and drops frames instead of exceeding it,
   which is the behaviour the plan wants for a 300 Mbps link.
5. **Receiver decode is the ceiling at 3840×1080.** One software decoder tops
   out near 55 fps on worst-case content, three near 50.

### Bug found and fixed on this machine

`LibWebRTCSharedScreenGetCapabilities` crashed with `0xC0000005` whenever the
session had no usable display (SSH, a service, a locked or disconnected
desktop). `FindOutput` stopped enumerating only on `DXGI_ERROR_NOT_FOUND`;
those sessions fail with `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` and leave the
out pointer null, so `GetDesc` ran on null instead of reporting "unavailable"
so the app falls back. Reproduced with a standalone harness and fixed (any
enumeration failure ends the list), released as `discourse.m150.7871.07.5`, verified on the T4 against the real
DLLs (see the table above). `capture_encode_test --expect-no-display` and
`shared_screen_probe` are the regression tests, and CI runs the probe against
the bundled DLL.

### Findings worth knowing

- The capability probe always uses the **primary** monitor (`""`). If the
  primary monitor belongs to another adapter (here the 800×600 Basic Display
  adapter), the shared encoder reports unavailable even though the NVIDIA GPU
  has a working display. The virtual display had to be made primary.
- An RDP session cannot be captured on the GPU (the Remote Display Adapter is
  not the NVIDIA adapter), and a service or SSH session has no outputs at all.
  A headless GPU machine needs a virtual display and a console session.
- libwebrtc opens sockets without calling `WSAStartup`; the Flutter runtime
  does it for the app. A standalone process that does not gets only TCP
  candidates and never connects.
- A test that builds against the wrapper headers must define
  `RTC_DESKTOP_DEVICE` and `LIB_WEBRTC_API_DLL` like the plugin does. Without
  them the factory vtable is read two slots off and calls crash.

### Not covered

- The RTX 5090, 240 Hz, and any other GPU.
- The Flutter **UI**: the session class was run (above), but not the preset
  picker widget, the placeholder tile, the 32:9 tile layout, or the app against
  a real Discourse site and signaling server.
- Packet loss on the network (the starved peer above is bandwidth-driven, not
  random loss) and the exact first recovery key frame.
- A monitor picker (none exists yet).
