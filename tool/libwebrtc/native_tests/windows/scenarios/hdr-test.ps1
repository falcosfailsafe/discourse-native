$B = 'C:\w\build\nvenc_smoke_test'
function Console($cmd, $t = 60) { & powershell -NoProfile -ExecutionPolicy Bypass -File C:\setup\run-in-console.ps1 -Command $cmd -TimeoutSec $t }
Get-Process noise_window,hdr_ramp -ErrorAction SilentlyContinue | Stop-Process -Force
$list = Console "$B\display_tool.exe list"
$vdd = ($list | Select-String 'Virtual Display Driver' | Select -First 1).Line.Split('|')[0].Trim()
Console "$B\display_tool.exe hdr $vdd on" | Out-Null; Start-Sleep 4
Console "$B\display_tool.exe hdr $vdd query" | Select-String 'supported'
Unregister-ScheduledTask -TaskName ramp -Confirm:$false -ErrorAction SilentlyContinue
$a = New-ScheduledTaskAction -Execute "$B\hdr_ramp.exe" -Argument "0 0 7680 2160 200 6.0"
$p = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\gputest" -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName ramp -Action $a -Principal $p | Out-Null
Start-ScheduledTask -TaskName ramp; Start-Sleep 6
"=== DuplicateOutput1 (FP16 scRGB) + tone-mapping pass + video processor"
Console "cd /d $B && hdr_capture_probe.exe 0 C:\setup\hdr_probe.nv12" 90
"=== legacy DuplicateOutput"
Console "cd /d $B && hdr_capture_probe.exe 0 C:\setup\hdr_probe_legacy.nv12 --legacy" 90
Get-Process hdr_ramp -ErrorAction SilentlyContinue | Stop-Process -Force
Console "$B\display_tool.exe hdr $vdd off" | Out-Null
