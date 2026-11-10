# Is the shared-encoder capability re-evaluated after the display is fixed?
#   cache-test.ps1 -Dll <libwebrtc.dll> [-Display \\.\DISPLAY7]
# Starts a probe process while the display is rotated, restores the display while
# it runs, and prints every answer. .5 and earlier stay "unavailable" forever;
# .6 answers "available" again within a few seconds.
param([Parameter(Mandatory)][string]$Dll, [string]$Display = '\\.\DISPLAY6')
$B = 'C:\w\build\nvenc_smoke_test'
function Console($cmd, $t = 60) { & powershell -NoProfile -ExecutionPolicy Bypass -File C:\setup\run-in-console.ps1 -Command $cmd -TimeoutSec $t }
Get-Process noise_window,discourse_native -ErrorAction SilentlyContinue | Stop-Process -Force
Console "$B\display_tool.exe rotate $Display 90" | Out-Null; Start-Sleep 3
$task = 'probe-bg'
Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
$a = New-ScheduledTaskAction -Execute cmd.exe -Argument "/c $B\shared_screen_probe.exe $Dll 5 6 > C:\setup\probe_repeat.txt 2>&1"
$p = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\gputest" -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName $task -Action $a -Principal $p | Out-Null
Start-ScheduledTask -TaskName $task
Start-Sleep 3
"display rotated: the first answer should be unavailable"
Console "$B\display_tool.exe rotate $Display 0" | Out-Null
"display restored: later answers show whether the capability is re-evaluated"
Start-Sleep 34
Get-Content C:\setup\probe_repeat.txt | Select-String '^\['
Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
