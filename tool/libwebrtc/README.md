# Discourse Windows libwebrtc build

The custom Windows wrapper fork is <https://github.com/falcosfailsafe/libwebrtc>.
`source.json` pins the upstream wrapper, the underlying WebRTC source and
`depot_tools` separately. Upstream's **wrapper** repository does not have an
`m150_release` branch: that branch belongs to `webrtc-sdk/webrtc`. The baseline
wrapper is the `libwebrtc.m150.7871.07` tag, matching the vendored plugin.

## Reproduce / update

1. Clone the wrapper and check out `wrapper_revision` from `source.json`.
2. Run `python3 tool/libwebrtc/stage.py /path/to/libwebrtc` from this app checkout.
   This copies the reviewed wrapper overlay, build recipe and core patches into
   the fork. It does not publish anything. Inspect the complete diff, including
   the native overlay, before committing and pushing.
3. On a Windows x64 machine with VS 2022 C++ tools, Windows SDK, Python, Git,
   PowerShell 7 and Ninja available, run `./discourse-build/build-windows.ps1`
   from the staged wrapper root. Allow substantial disk space and build time.
   Alternatively dispatch `discourse-windows.yml` in the fork. The workflow
   builds the DLL and import library, runs the C++ API tests, and publishes an
   immutable release only if every step succeeds.
4. Download the release ZIP, check its `.sha256`, and record the release tag,
   download base URL and ZIP SHA-256 as `windows_binary_version`,
   `windows_download_url` and `windows_sha256` in the plugin's
   `third_party/libwebrtc_version.ini`. **Do not point the app at an unbuilt or
   untested release.** Linux, Apple and other architectures retain their existing
   upstream source. The Windows override applies only to x64.
5. Build the Windows app against that exact release and run the acceptance
   procedure in `docs/voice-windows-baseline.md`. A successful DLL build is not
   proof of NVENC hardware, GPU-only capture, or multi-peer behavior.

Never replace an existing release asset in place. Increment `release_tag` for
any build-recipe, overlay, source or dependency change. The ZIP contains
`SOURCE.json` and `WRAPPER_REVISION.txt` so the shipped binary can be traced to
both underlying source and the wrapper/overlay commit.

## Local tests

```sh
python3 -m unittest discover -s tool/libwebrtc -p 'test_*.py' -v
bash tool/libwebrtc/run_native_tests.sh
```

The Python tests exercise Windows-only binary selection, unchanged Linux
selection, required SHA-256 pins, corrupted-cache recovery, and re-extraction
when switching releases. The portable C++ tests exercise encoder policy;
Windows GPU behavior needs the real Windows build and hardware run.

## Dependency and licensing boundary

The NVENC API is dynamically loaded from the NVIDIA driver. No NVIDIA sample
implementation or bundled FFmpeg DLL is used. See the overlay's
`third_party/nv-codec-headers/README.md` for the API-header origin and licence.
H.264 royalties remain a legal review item; the build recipe is not a legal
clearance.
