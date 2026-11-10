# Windows GPU test programs

Standalone programs that exercise the shared NVENC screen share on a Windows
machine with an NVIDIA GPU, without Flutter or libwebrtc. Build them all with:

```powershell
powershell -File tool\libwebrtc\native_tests\windows\build_nvenc_smoke_test.ps1
```

Output goes to `build\nvenc_smoke_test\`. Needs Visual Studio (or Build Tools)
with the C++ workload. Exit code 0 means every check passed.

| Program | What it proves | Needs a display |
| --- | --- | --- |
| `nvenc_smoke_test.exe` | Hardware H.264 encoding works: a synthetic 7680x2160 picture is scaled to 3840x1080 on the GPU and encoded through the real `NvencH264Encoder`. Checks the SPS profile/level, key frame interval, frame rate and the NVENC session count, and writes the stream for ffprobe/ffmpeg. | No |
| `capture_encode_test.exe` | The real DXGI desktop capture feeding the real NVENC wrapper (the shared screen pipeline without libwebrtc). With `--expect-no-display` it asserts that a missing display is reported as an error, not a crash. | Yes (not for `--expect-no-display`) |
| `shared_screen_peers_test.exe` | The Phase 2 acceptance at the native layer, through the real `libwebrtc.dll`: creates the shared source as the plugin does, negotiates H.264 High 5.1 with N loopback peers, measures what every receiver decodes, checks for exactly one NVENC session, and covers `--drop-peer-at` (a viewer leaves), `--late-join-at`, `--throttle-peer-at/--unthrottle-at/--throttle-kbps` (a peer that cannot keep up must freeze and recover without disturbing the others), `--iterations` (repeated teardown), the closed-connection accessors, and `--software` (ordinary encoder). Build it with `-WebRtcRelease <unzipped libwebrtc release dir>`. | Yes (not for `--software`) |
| `shared_screen_probe.exe <libwebrtc.dll>` | Loads the DLL the way the Flutter plugin does and calls the shared-screen API. Must never crash; reports `available` and a reason. | No |
| `display_tool.exe` | Lists GDI displays, attaches a monitor to the desktop and sets its mode. | Yes |

`animate.ps1` draws moving content on a chosen display so captures see real
motion, and `run_in_console.ps1` runs a command inside the interactive console
session from a non-interactive one (SSH, services).

## Why a display matters

DXGI desktop duplication needs a display attached to an interactive desktop.
SSH, services and scheduled tasks without an interactive logon run in
session 0, where `IDXGIAdapter::EnumOutputs` fails with
`DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` for every index. RDP sessions use the
Remote Display Adapter, which does not belong to the GPU. A headless GPU
server (for example an AWS g4dn instance) therefore needs, besides the GPU
driver:

1. A virtual display (an Indirect Display Driver) that the GPU owns. This was
   tested with [Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)
   25.7.23 (MIT, signed by the SignPath Foundation). Windows rejects it with
   `0xE0000242` until that signing certificate is added to the machine's
   Trusted Publishers store. Settings live in
   `C:\VirtualDisplayDriver\vdd_settings.xml`; add the modes you want.
2. A user logged in at the console, so the virtual monitor is on an
   interactive desktop. Use a dedicated local user with auto-logon
   (`Winlogon` `AutoAdminLogon`/`DefaultPassword`, which stores the password
   in the registry: only do this on a disposable test machine).
3. Run the tests in that session. From SSH, `run_in_console.ps1` does it
   through a scheduled task with an Interactive logon. With a single virtual
   monitor the capture source id is its zero-based GDI display index
   (`display_tool list`), because `""` selects the primary monitor, which may
   belong to another adapter.

The virtual monitor must be 8-bit SDR; the capture refuses HDR and rotated
displays.

Notes for the peers test: it initialises Winsock itself (libwebrtc does not;
the Flutter runtime does that for the app) and must be built with
`RTC_DESKTOP_DEVICE` and `LIB_WEBRTC_API_DLL` like the plugin, which the build
script does. Windows Firewall may block a new program's inbound UDP; allow it
if peers fail to connect (`New-NetFirewallRule ... -Program <exe>`).

## The Flutter session driver

`integration_test/voice_windows_hardware_driver.dart` runs the real
`MeshVoiceMediaSession` with the real plugin and DLL (see its header). It is
not a `flutter test`; build it as an app and run it in the console session:

```powershell
flutter build windows --release --target integration_test/voice_windows_hardware_driver.dart
# then, from the console session (run_in_console.ps1 from SSH):
build\windows\x64\runner\Release\discourse_native.exe
```

Results go to `VOICE_DRIVER_LOG` (default `C:\setup\voice_driver.log`) and the
exit code is 0 only if every check passed. Start `animate.ps1` first so the
display has motion, and make the virtual display the primary monitor.

## Load, presets, display changes and the software baseline

`noise_window.exe X Y W H [seconds] [block] [amplitude]` fills a monitor with GPU
noise, a realistic worst case for a screen capture. Amplitude 1.0 is full-range
white noise, which no constant-bitrate encoder can reach a 25 Mbps target with
(it is already at the maximum quantiser, about 195 Mbps at 3840×1080). Use 0.1
to 0.3 with 8-pixel blocks for heavy but controllable content.
`start_noise.ps1` starts it in the console session.

`shared_screen_peers_test.exe` also takes `--max-height`, `--scale`, `--fps`,
`--kbps`, `--report-every` (periodic bitrate, drop, freeze, memory, handle, CPU
and GPU lines; automatic for runs over 40 s) and `--change-mode-at S
--change-mode WxHxHZ` (change a display under a live share).

`scenarios/` has the scripts behind the results in
`docs/voice-windows-acceptance.md`; they assume the layout in this README
(`C:\w`, `C:\setup`, a `gputest` console user):

- `preset-matrix.ps1`: 4K level 5.2, 720p60, 1080p60 and 30 fps presets.
- `display-matrix.ps1`: a resolution change under a live share, a rotated
  display and HDR (`display_tool hdr`).
- `cache-test.ps1`: the capability is re-evaluated after a display is fixed.
- `launch-baseline.ps1`: runs `integration_test/voice_windows_baseline_driver.dart`
  as two processes (viewers and sender) and prints the RESULT lines.
