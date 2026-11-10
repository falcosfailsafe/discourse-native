# Voice on Windows: software-encoding baseline

Phase 0 of [voice-hardware-encoding](plans/voice-hardware-encoding.md) ends
with CPU and frame-rate numbers for today's software screen share, at 1080p
and at the full 7680×2160 source. Later phases are judged against them.

**No number appears in this file until someone has measured it on Windows
hardware and filled in the run record below.** The tables start empty on
purpose. Do not copy figures from the plan's estimates, from another
platform, or from a different build. Prefer a blank over a guess.

## Status

| Item | State |
| --- | --- |
| Windows runner and CI build | CI builds the debug bundle. A local x64 Release bundle built successfully against the pinned custom DLL on 2026-11-09. See the [acceptance report](voice-windows-acceptance.md). |
| Mesh call with screen share verified on Windows | Three simulated receivers decoded video with the published .3 DLL and the app admission fix. See the [acceptance report](voice-windows-acceptance.md). |
| Baseline runs | 6 of 6 timed runs produced complete CPU and WebRTC measurements. |

## Prerequisites

- A Windows 10/11 x64 machine. For the reference runs use the RTX 5090 sender
  with the 7680×2160 240 Hz display from the plan; record the actual hardware
  if it differs.
- A build from this repository with the stock libwebrtc:
  `flutter build windows --release`. Debug builds are not representative.
  Record the commit and `third_party/libwebrtc_version.ini` value.
- At least two other participants in the same call (the plan's reference
  case is three peers). Say in the record how many were real machines.
- Windows HDR, game mode and power plan noted and kept the same across runs.
  Use the High performance power plan.
- Nothing else heavy running. Note anything that is.

### Building on Windows

`flutter build windows` currently needs two workarounds that the CI job
(`windows-bundle` in `.github/workflows/ci.yml`) applies; without them the
build fails for reasons unrelated to Voice:

- **libavif 0.1.1 (AVIF decoder).** Its dav1d build passes `cl.exe` source
  paths longer than MAX_PATH from the deep `.dart_tool` tree, and its
  `build.rs` looks for `dav1d.lib` while Meson writes `libdav1d.a`. CI clones
  to `C:\w`, sets `PUB_CACHE=C:\p` and patches `native/build.rs` in the CI
  copy of the package (see the "Patch libavif" step). Locally, build from a
  short path and apply the same patch to your pub cache, or expect to fail.
- **webview_all_windows.** Uses `/await`; `windows/CMakeLists.txt` defines
  `_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS` for the newer MSVC STL.

## Procedure

Do each case below separately, in a fresh call, and repeat it three times.

| Case | Shared source | Notes |
| --- | --- | --- |
| A | Display set to 1920×1080 | 1080p source |
| B | Display set to 7680×2160 at 240 Hz | Full source |

For each run:

1. Start the app, join the call, and wait until all peers are connected.
2. Start screen share of the whole display. Play fast-moving content (a game
   or a scrolling test pattern); say which. Static content flatters software
   encoders.
3. Let it settle for 30 seconds, then start the CPU sampler for 60 seconds:

   ```powershell
   powershell -File tool/voice_windows_baseline.ps1 -Label caseB-3peers-run1 -DurationSeconds 60
   ```

   It writes a per-second CSV and a JSON summary to `baseline-results/`
   (mean, 95th percentile and maximum CPU as a share of the whole machine,
   plus hardware names). It samples only the app process.
4. During the same 60 seconds, record the sender's frame rate and encoder
   state from WebRTC stats for the screen-share `outbound-rtp` report:
   `framesPerSecond`, `framesEncoded` (take the difference over the window and
   divide by the seconds), `frameWidth`/`frameHeight`,
   `qualityLimitationReason`, `encoderImplementation`. Also record the frame
   rate a receiving peer actually sees (`inbound-rtp` `framesDecoded` over the
   same window).

   The app has no stats screen today, so this needs a temporary local log of
   `RTCPeerConnection.getStats()` output (the Voice session already calls it,
   in `_pollSpeakingLevels`). Do not commit that log. If you cannot capture
   the stats, write "not measured" rather than infer a figure.
5. Stop the share. If the app crashed, froze or the call dropped, discard
   that run and note it.

Also record, once per case: whether the share started at all, and what the
receiver saw (frozen, stuttering, downscaled). A case that fails to run is a
valid baseline result; write what happened.

## Reporting

Report the median of the three runs and the full range. Keep the raw CSV and
JSON files with the record (attach them to the pull request or issue; do not
commit them).

Run record, one per case:

```text
Case:
Date:
Commit:
libwebrtc_version.ini binary_version:
Machine (CPU, GPU, RAM, OS build):
Display mode:
Peers (real / simulated):
Content shared:
Runs:
  run 1: cpu mean/p95/max, sent fps, received fps, resolution, limit reason
  run 2:
  run 3:
Observed problems:
```

## Results

Fill in from the run records. Leave `—` for anything not measured.

| Case | Peers | CPU mean (% of machine) | CPU p95 | Sent fps | Received fps | Resolution sent | Limit reason |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A, 1080p source | 3 simulated | 3.74 (3.49–3.85) | 5.01 (4.91–5.31) | 28.29 (28.29–28.30) | 28.27 (28.27–28.27) | 960×540 | bandwidth |
| B, 7680×2160 source | 3 simulated | 7.38 (6.89–7.65) | 8.67 (8.49–8.76) | 17.81 (17.78–18.68) | 17.82 (17.77–18.67) | 1920×540, 2560×720 | bandwidth |

Measured on 2026-11-09 with a Ryzen 7 5800X3D, 32 GB RAM, GeForce RTX
5090, NVIDIA driver 621.01, Windows 11 Pro x64 build 26300.9711 and the
High performance power plan. Both source resolutions ran at 240 Hz with
HDR off and Wide Color Gamut on. Game Mode was not independently verified.

The Release diagnostic used the app at `4e4d2c7cb` with a local
shared-sender admission change and a temporary stats entry point. It
loaded the stock Windows `libwebrtc.m150.7871.07` DLL from its upstream
release in an isolated bundle. One sender and three simulated receivers
ran in four separate processes on the same host. There were no remote
physical receiver machines. The sender process was sampled by the repo
script after 30 seconds of warmup. A fullscreen FFplay `testsrc2` pattern
configured for 60 fps played at 960×540 for case A and 1920×540 for case
B, scaled to fill the source display.

Each result above is the median with the observed run range in parentheses.
For each run, sent and received fps are the medians of three RTP counter
rates. Counters were paired with the nearest stats samples to the CPU
measurement window. Resolution lists every outbound size in the valid
windows. Limit reason is the most frequent sampled reason.

| Run | CPU mean (%) | CPU p95 (%) | CPU max (%) | Sent fps | Received fps | Outbound size | Limit reason |
| --- | --- | --- | --- | --- | --- | --- | --- |
| caseA-run1 | 3.49 | 4.91 | 5.46 | 28.29 | 28.27 | 960×540 | bandwidth |
| caseA-run2 | 3.74 | 5.01 | 5.58 | 28.29 | 28.27 | 960×540 | bandwidth |
| caseA-run3 | 3.85 | 5.31 | 5.86 | 28.30 | 28.27 | 960×540 | bandwidth |
| caseB-run1 | 7.65 | 8.67 | 9.64 | 17.78 | 17.77 | 2560×720 | bandwidth |
| caseB-run2 | 7.38 | 8.76 | 9.42 | 17.81 | 17.82 | 2560×720 | bandwidth |
| caseB-run3 | 6.89 | 8.49 | 8.87 | 18.68 | 18.67 | 1920×540, 2560×720 | bandwidth |

## Second machine: Tesla T4 server, software versus shared NVENC

A different machine and different content, so these numbers are **not**
comparable with the RTX 5090 rows above; they compare the two paths with each
other. AWS `g4dn.2xlarge` (8 logical CPUs, Tesla T4), Windows Server 2025, a
60 Hz virtual display, GPU noise as the moving content, one sender process and
one separate process for the three viewers (so the sender's CPU does not include
the viewers' decoders). Procedure and the full table are in
[voice-windows-acceptance.md](voice-windows-acceptance.md#software-versus-shared-nvenc-at-the-same-sizes).

| Case | Path | Sender CPU (% of machine) | Frames encoded | Resolution sent | Limit reason |
| --- | --- | --- | --- | --- | --- |
| A, 1080p source | software, 6 runs | 5.8 to 7.6 | 20.3 to 20.8 fps | 480×270 to 640×360 | bandwidth |
| A, 1080p source | shared NVENC, 3 runs | 4.0 to 5.0 | 60.0 fps | 1920×1080 | none |
| B, 7680×2160 source | software, 6 runs | 18.1 to 40.9 | 12.1 to 13.9 fps | 1280×360 to 2560×720 | bandwidth |
| B, 7680×2160 source | shared NVENC, 3 runs | 5.8 to 8.6 | 60.0 fps | 3840×1080 | none |

Raw logs: `baseline.log` (software) and `baseline_nvenc.log`.

## Limits of this method

- CPU is process CPU as a percentage of all logical processors. It excludes
  GPU use and other processes' work (for example the window compositor).
- The sampler needs an installed Windows PowerShell; it has not been
  exercised in CI.
- The frame-rate figures depend on the local stats log described above.
