# Builds the Windows GPU test programs with MSVC:
#   nvenc_smoke_test.exe   - NVENC hardware encode through the real wrapper
#   capture_encode_test.exe - real DXGI capture -> NVENC (needs a display)
#   display_tool.exe       - list/attach/set modes of GDI displays (virtual display setup)
#   shared_screen_probe.exe - loads libwebrtc.dll and reports its shared-screen API
#
#   powershell -File build_nvenc_smoke_test.ps1 [-Run] [test arguments]
#
# Needs Visual Studio or Build Tools with the C++ workload. Output goes to
# <repo>\build\nvenc_smoke_test\. With -Run the smoke test runs afterwards and
# its exit code is returned.
param(
  [switch]$Run,
  [Parameter(ValueFromRemainingArguments = $true)]
  [string[]]$TestArgs
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$overlay = (Resolve-Path (Join-Path $here '..\..\overlay\src\shared_encoder')).Path
$repo = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$out = Join-Path $repo 'build\nvenc_smoke_test'
New-Item -ItemType Directory -Force $out | Out-Null

$vsroot = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
  $vsroot = & $vswhere -latest -products * -property installationPath
}
if (-not $vsroot) {
  $candidates = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\BuildTools'),
    (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\2022\Community'),
    (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\18\Enterprise')
  )
  foreach ($c in $candidates) { if (Test-Path $c) { $vsroot = $c; break } }
}
if (-not $vsroot) { throw 'Visual Studio with the C++ workload was not found' }
$vcvars = Join-Path $vsroot 'VC\Auxiliary\Build\vcvars64.bat'

function Q([string]$s) { return '"' + $s + '"' }

$includes = @($overlay, (Join-Path $overlay 'third_party\nv-codec-headers'))
$incArgs = ($includes | ForEach-Object { '/I' + (Q $_) }) -join ' '
$common = 'cl /nologo /std:c++17 /EHsc /O2 /W3 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ' + $incArgs

$targets = @(
  @{ Name = 'nvenc_smoke_test'
     Sources = @((Join-Path $here 'nvenc_smoke_test.cc'), (Join-Path $overlay 'nvenc_h264_encoder.cc'))
     Libs = 'd3d11.lib dxgi.lib' },
  @{ Name = 'capture_encode_test'
     Sources = @((Join-Path $here 'capture_encode_test.cc'), (Join-Path $overlay 'd3d11_capture.cc'), (Join-Path $overlay 'nvenc_h264_encoder.cc'))
     Libs = 'd3d11.lib dxgi.lib user32.lib' },
  @{ Name = 'display_tool'
     Sources = @((Join-Path $here 'display_tool.cc'))
     Libs = 'user32.lib' },
  @{ Name = 'shared_screen_probe'
     Sources = @((Join-Path $here 'shared_screen_probe.cc'))
     Libs = '' }
)
foreach ($t in $targets) {
  $srcArgs = ($t.Sources | ForEach-Object { Q $_ }) -join ' '
  $objDir = Join-Path $out ($t.Name + '_obj')
  New-Item -ItemType Directory -Force $objDir | Out-Null
  $exe = Join-Path $out ($t.Name + '.exe')
  $line = (Q $vcvars) + ' >nul && ' + $common + ' ' + $srcArgs +
          ' /Fo' + (Q ($objDir + '\\')) + ' /Fe' + (Q $exe) + ' ' + $t.Libs
  cmd /c $line
  if ($LASTEXITCODE -ne 0) { throw ('compile of ' + $t.Name + ' failed (' + $LASTEXITCODE + ')') }
  Write-Host ('built ' + $exe)
}

if ($Run) {
  Push-Location $out
  try {
    & (Join-Path $out 'nvenc_smoke_test.exe') @TestArgs
    exit $LASTEXITCODE
  } finally { Pop-Location }
}
