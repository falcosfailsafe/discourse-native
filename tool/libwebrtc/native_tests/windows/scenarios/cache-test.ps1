$B = 'C:\w\build\nvenc_smoke_test'
$DLL = 'C:\w\build\windows\x64\runner\Release\libwebrtc.dll'
function Console($cmd, $t = 60) { & powershell -NoProfile -ExecutionPolicy Bypass -File C:\setup\run-in-console.ps1 -Command $cmd -TimeoutSec $t }
Get-Process noise_window,discourse_native -ErrorAction SilentlyContinue | Stop-Process -Force
Console "$B\display_tool.exe set \\.\DISPLAY6 7680 2160 60" | Out-Null
# Start with the display rotated (unavailable), probe repeatedly, and restore it between the 1st and 2nd answer.
Console "$B\display_tool.exe rotate \\.\DISPLAY6 90" | Out-Null; Start-Sleep 3
$task = 'probe-bg'
Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
$a = New-ScheduledTaskAction -Execute cmd.exe -Argument "/c $B\shared_screen_probe.exe $DLL 4 6 > C:\setup\probe_repeat.txt 2>&1"
$p = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\gputest" -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName $task -Action $a -Principal $p | Out-Null
Start-ScheduledTask -TaskName $task
Start-Sleep 3
"display is rotated now; the first answer should be unavailable"
Console "$B\display_tool.exe rotate \\.\DISPLAY6 0" | Out-Null
"display restored; the later answers show whether the capability is re-evaluated"
Start-Sleep 25
Get-Content C:\setup\probe_repeat.txt
Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
