param([int]$Seconds = 120, [int]$X = 0, [int]$Y = 0, [int]$W = 7680, [int]$H = 2160, [int]$Block = 4, [string]$Amp = "1.0")
Unregister-ScheduledTask -TaskName noise -Confirm:$false -ErrorAction SilentlyContinue
Get-Process noise_window -ErrorAction SilentlyContinue | Stop-Process -Force
$a = New-ScheduledTaskAction -Execute C:\w\build\nvenc_smoke_test\noise_window.exe -Argument "$X $Y $W $H $Seconds $Block $Amp"
$p = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\gputest" -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName noise -Action $a -Principal $p | Out-Null
Start-ScheduledTask -TaskName noise
Start-Sleep 4
"noise: $((Get-ScheduledTask noise).State)"
