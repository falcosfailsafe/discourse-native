<#
.SYNOPSIS
  Samples CPU use of the running Discourse Windows app and writes a CSV plus a
  JSON summary for docs/voice-windows-baseline.md.

.DESCRIPTION
  Measures only what the operating system can report about the app process:
  CPU time, as a percentage of the whole machine (all logical processors).
  It does not measure frame rate. Frame rate comes from WebRTC stats; see the
  procedure in docs/voice-windows-baseline.md.

  Nothing is estimated or filled in: if the process is absent the script exits
  with an error, and the summary contains only what was sampled.

.EXAMPLE
  powershell -File tool/voice_windows_baseline.ps1 -Label 1080p-2peers -DurationSeconds 60
#>
[CmdletBinding()]
param(
  # Short run name, used in the output file names, e.g. "1080p-2peers".
  [Parameter(Mandatory = $true)][string]$Label,
  [int]$DurationSeconds = 60,
  [int]$IntervalSeconds = 1,
  [string]$ProcessName = 'discourse_native',
  [string]$OutputDirectory = 'baseline-results'
)

$ErrorActionPreference = 'Stop'

if ($DurationSeconds -lt 10 -or $IntervalSeconds -lt 1) {
  throw 'Use at least a 10 second duration and a 1 second interval.'
}

$process = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
  Sort-Object StartTime | Select-Object -First 1
if (-not $process) {
  throw "No running process named '$ProcessName'. Start the app and the call first."
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
$csvPath = Join-Path $OutputDirectory "$Label-$stamp.csv"
$summaryPath = Join-Path $OutputDirectory "$Label-$stamp.json"

$logical = [Environment]::ProcessorCount
$samples = New-Object System.Collections.Generic.List[double]
$rows = New-Object System.Collections.Generic.List[object]

$process.Refresh()
$previousCpu = $process.TotalProcessorTime
$previousAt = [DateTime]::UtcNow
$deadline = $previousAt.AddSeconds($DurationSeconds)

while ([DateTime]::UtcNow -lt $deadline) {
  Start-Sleep -Seconds $IntervalSeconds
  $process.Refresh()
  if ($process.HasExited) {
    throw 'The app exited during sampling; this run is invalid.'
  }
  $cpu = $process.TotalProcessorTime
  $at = [DateTime]::UtcNow
  $elapsed = ($at - $previousAt).TotalSeconds
  $busy = ($cpu - $previousCpu).TotalSeconds
  # 100 means every logical processor was busy for the whole interval.
  $percent = [Math]::Round(100.0 * $busy / ($elapsed * $logical), 2)
  $samples.Add($percent)
  $rows.Add([pscustomobject]@{
    utc = $at.ToString('o')
    cpuPercentOfMachine = $percent
    cpuPercentOfOneCore = [Math]::Round(100.0 * $busy / $elapsed, 1)
    workingSetMiB = [Math]::Round($process.WorkingSet64 / 1MB, 1)
  })
  $previousCpu = $cpu
  $previousAt = $at
}

$rows | Export-Csv -NoTypeInformation -Path $csvPath

$sorted = $samples | Sort-Object
$p95 = $sorted[[Math]::Min($sorted.Count - 1, [int][Math]::Ceiling($sorted.Count * 0.95) - 1)]
$cpuName = (Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name)
$gpuNames = @(Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name })
$summary = [ordered]@{
  label = $Label
  startedUtc = $stamp
  durationSeconds = $DurationSeconds
  intervalSeconds = $IntervalSeconds
  sampleCount = $samples.Count
  processName = $ProcessName
  processId = $process.Id
  logicalProcessors = $logical
  cpuModel = $cpuName
  gpus = $gpuNames
  osVersion = [Environment]::OSVersion.VersionString
  cpuPercentOfMachine = [ordered]@{
    mean = [Math]::Round(($samples | Measure-Object -Average).Average, 2)
    p95 = $p95
    max = ($samples | Measure-Object -Maximum).Maximum
  }
  frameRate = 'not measured by this script; record from WebRTC stats'
}
$summary | ConvertTo-Json -Depth 4 | Set-Content -Path $summaryPath -Encoding utf8

Write-Host "Wrote $csvPath"
Write-Host "Wrote $summaryPath"
