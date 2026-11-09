# Run from the custom libwebrtc fork root on Windows x64.
# This intentionally does not invoke upstream's batch script, which can carry
# on after a failed checkout, sync, patch, or GN generation.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
function Run([string] $Program, [string[]] $Arguments) {
  & $Program @Arguments
  if ($LASTEXITCODE -ne 0) { throw "$Program failed ($LASTEXITCODE): $Arguments" }
}
$root = (Get-Location).Path
$source = Get-Content "$root/discourse-build/source.json" | ConvertFrom-Json
New-Item -ItemType Directory -Force "$root/build" | Out-Null
Set-Location "$root/build"
$env:DEPOT_TOOLS_UPDATE = '0'
if (-not (Test-Path 'depot_tools/.git')) {
  Run git @('clone', 'https://chromium.googlesource.com/chromium/tools/depot_tools.git', 'depot_tools')
}
Run git @('-C', 'depot_tools', 'checkout', '--detach', $source.depot_tools_revision)
$env:PATH = "$root/build/depot_tools;$env:PATH"
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'
$env:DEPOT_TOOLS_UPDATE = '0'
$env:GYP_MSVS_VERSION = '2022'
$env:PYTHONUTF8 = '1'
# Pinning disables depot_tools self-update, not its Python/CIPD bootstrap.
# gclient may otherwise find a runner-provided vpython while GN's Python shim
# remains uninitialised (python3_bin_reldir.txt missing).
Run "$root/build/depot_tools/bootstrap/win_tools.bat" @()
# .gclient is rewritten rather than appending target_os on each invocation.
@"
solutions = [{
  'name': 'src',
  'url': '$($source.webrtc_repository)@$($source.webrtc_revision)',
  'custom_deps': {}, 'deps_file': 'DEPS', 'managed': False,
}]
target_os = ['win']
"@ | Set-Content -Encoding ascii .gclient
# A restored checkout may contain our previous core patches. Reset tracked
# files to the pinned commit before syncing/reapplying; retain fetched deps.
if (Test-Path 'src/.git') {
  Run git @('-C', 'src', 'reset', '--hard', $source.webrtc_revision)
}
Run gclient.bat @('sync', '--no-history', '--with_branch_heads', '--jobs=8')
# The build fork's tip is not a Gerrit Change-Id commit. With shallow sync the
# default lastchange filter finds nothing and writes epoch zero; Chromium then
# derives a negative PE timestamp, which lld correctly rejects. Record the
# actual pinned build dependency commit instead, without the Gerrit filter.
Run python3 @('src/build/util/lastchange.py', '--source-dir', 'src/build', '--filter=', '-o', 'src/build/util/LASTCHANGE')
$commitTime = [long](Get-Content 'src/build/util/LASTCHANGE.committime')
if ($commitTime -le 0 -or $commitTime -gt [uint32]::MaxValue) {
  throw 'Invalid pinned build dependency timestamp'
}
Run git @('-C', 'src', 'rev-parse', 'HEAD')
$actual = & git -C src rev-parse HEAD
if ($actual.Trim() -ne $source.webrtc_revision) { throw 'Unexpected WebRTC revision' }
$wrapper = "$root/build/src/libwebrtc"
if (Test-Path $wrapper) { Remove-Item -Recurse -Force $wrapper }
New-Item -ItemType Directory -Force $wrapper | Out-Null
foreach ($entry in @('include', 'src', 'patches', 'test', 'BUILD.gn', 'LICENSE')) {
  Copy-Item -Recurse -Force "$root/$entry" $wrapper
}
Set-Location "$root/build/src"
Copy-Item -Force '.vpython3' '..'
foreach ($patch in @('custom_audio_source_m150.patch', 'add_libwebrtc_build_target.patch', 'allow_176k4_192k_shared_mode_formats.patch')) {
  Run git @('apply', '--check', "libwebrtc/patches/$patch")
  Run git @('apply', "libwebrtc/patches/$patch")
}
# Any core patches (as distinct from wrapper overlay files) are reviewed and
# shipped in the same fork revision as this script.
if (Test-Path "$root/discourse-build/core-patches") {
  foreach ($patch in Get-ChildItem "$root/discourse-build/core-patches/*.patch" | Sort-Object Name) {
    Run git @('apply', '--check', $patch.FullName)
    Run git @('apply', $patch.FullName)
  }
}
Set-Location "$root/build"
$gnArgs = 'is_debug=false is_clang=true target_cpu="x64" use_custom_libcxx=false rtc_libvpx_build_vp9=true enable_libaom=true rtc_include_tests=true rtc_build_examples=false rtc_build_tools=false is_component_build=false rtc_enable_protobuf=false rtc_use_h264=true ffmpeg_branding="Chrome" rtc_use_h265=true symbol_level=0 enable_iterator_debugging=false'
Run "$root/build/src/buildtools/win/gn.exe" @('gen', 'src/out-x64-release', '--root=src', "--args=$gnArgs")
$targets = @('-C', 'src/out-x64-release', 'libwebrtc', 'libwebrtc_cpp_api_unittests')
if (Test-Path "$root/test/shared_encoder.test.cc") {
  $targets += 'discourse_shared_encoder_unittests'
  # Compile our wrapper/adapter objects first. Otherwise Ninja may spend an
  # hour building codecs before reporting a typo in our handful of sources.
  $allTargets = & ninja.exe -C 'src/out-x64-release' -t targets all
  if ($LASTEXITCODE -ne 0) { throw 'Could not enumerate Ninja targets' }
  $objects = @($allTargets | Where-Object {
    $_ -match '^obj/libwebrtc/(libwebrtc|discourse_shared_encoder_unittests)/.+\.obj:'
  } | ForEach-Object { ($_ -split ': ', 2)[0] })
  if ($objects.Count -eq 0) { throw 'Custom wrapper compile targets missing' }
  Run ninja.exe (@('-C', 'src/out-x64-release') + $objects)
}
Run ninja.exe $targets
Run "$root/build/src/out-x64-release/libwebrtc_cpp_api_unittests.exe" @()
if (Test-Path "$root/test/shared_encoder.test.cc") {
  Run "$root/build/src/out-x64-release/discourse_shared_encoder_unittests.exe" @()
}
$artifacts = "$root/build/libwebrtc-x64-release"
if (Test-Path $artifacts) { Remove-Item -Recurse -Force $artifacts }
New-Item -ItemType Directory -Force "$artifacts/lib", "$artifacts/include" | Out-Null
Copy-Item 'src/out-x64-release/libwebrtc.dll', 'src/out-x64-release/libwebrtc.dll.lib' "$artifacts/lib"
Copy-Item -Recurse -Force "$wrapper/include/*" "$artifacts/include"
Copy-Item "$root/LICENSE" $artifacts
Copy-Item "$root/discourse-build/source.json" "$artifacts/SOURCE.json"
$forkRevision = & git -C $root rev-parse HEAD
$forkRevision | Set-Content "$artifacts/WRAPPER_REVISION.txt"
Set-Location $root
Compress-Archive -Path $artifacts -DestinationPath $source.asset
$hash = (Get-FileHash -Algorithm SHA256 $source.asset).Hash.ToLower()
"$hash  $($source.asset)" | Set-Content -Encoding ascii "$($source.asset).sha256"
