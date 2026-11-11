param([string]$Name = 'shot', [int]$W = 3840, [int]$H = 1080)
# Captures the primary monitor from the console session without leaving a console window on top.
$a = New-ScheduledTaskAction -Execute powershell.exe -Argument "-WindowStyle Hidden -NoProfile -Command `"& C:\w\build\nvenc_smoke_test\capture_encode_test.exe --out ${W}x${H} --frames 8 --fps 10 --h264 C:\setup\$Name.h264 --json C:\setup\$Name.json`""
$p = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\gputest" -LogonType Interactive -RunLevel Highest
Remove-Item C:\setup\$Name.h264 -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName cap -Confirm:$false -ErrorAction SilentlyContinue
Register-ScheduledTask -TaskName cap -Action $a -Principal $p | Out-Null
Start-ScheduledTask -TaskName cap
for ($i = 0; $i -lt 40 -and -not (Test-Path C:\setup\$Name.json); $i++) { Start-Sleep 1 }
Start-Sleep 1
Unregister-ScheduledTask -TaskName cap -Confirm:$false -ErrorAction SilentlyContinue
