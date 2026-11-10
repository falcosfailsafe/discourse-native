param([string]$Modes = 'nvenc', [string]$Log = 'C:\setup\baseline_nvenc.log')
Remove-Item $Log, C:\setup\baseline_viewers.log -ErrorAction SilentlyContinue
Get-Process noise_window,discourse_native -ErrorAction SilentlyContinue | Stop-Process -Force
"set VOICE_ROLE=viewers`r`nstart /wait `"`" C:\w\build\windows\x64\runner\Release\discourse_native.exe" | Set-Content C:\setup\run-viewers.cmd -Encoding ascii
"set VOICE_ROLE=sender`r`nset VOICE_MODES=$Modes`r`nset VOICE_DRIVER_LOG=$Log`r`nstart /wait `"`" C:\w\build\windows\x64\runner\Release\discourse_native.exe" | Set-Content C:\setup\run-sender.cmd -Encoding ascii
Unregister-ScheduledTask -TaskName viewers -Confirm:$false -ErrorAction SilentlyContinue
$a = New-ScheduledTaskAction -Execute cmd.exe -Argument '/c C:\setup\run-viewers.cmd'
$p = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\gputest" -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName viewers -Action $a -Principal $p | Out-Null
Start-ScheduledTask -TaskName viewers
Start-Sleep 8
& powershell -NoProfile -ExecutionPolicy Bypass -File C:\setup\run-in-console.ps1 -Command C:\setup\run-sender.cmd -TimeoutSec 1500 | Select -Last 1
Get-Content $Log | Select-String 'RESULT|DONE|EXCEPTION'
Unregister-ScheduledTask -TaskName viewers -Confirm:$false -ErrorAction SilentlyContinue
