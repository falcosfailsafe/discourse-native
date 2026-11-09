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
| Mesh call with screen share verified on Windows | See the local diagnostic and shared-capture blocker in the [acceptance report](voice-windows-acceptance.md). |
| Baseline runs | No timed runs completed for cases A or B. |

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
| A, 1080p source | — | — | — | — | — | — | — |
| B, 7680×2160 source | — | — | — | — | — | — | — |

Measured on: —. Commit: —.

The 2026-11-09 RTX 5090 session did not include the three timed stock-libwebrtc runs at either resolution. The local Release diagnostic used simulated receivers in the sender process and is recorded separately in the [acceptance report](voice-windows-acceptance.md).

## Limits of this method

- CPU is process CPU as a percentage of all logical processors. It excludes
  GPU use and other processes' work (for example the window compositor).
- The sampler needs an installed Windows PowerShell; it has not been
  exercised in CI.
- The frame-rate figures depend on the local stats log described above.
