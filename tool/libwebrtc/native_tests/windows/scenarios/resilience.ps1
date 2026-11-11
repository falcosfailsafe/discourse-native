$B = 'C:\w\build\nvenc_smoke_test'
function Smi { (& nvidia-smi --query-gpu=encoder.stats.sessionCount --format=csv,noheader 2>&1).Trim() }
function StartPeers($seconds) {
  Unregister-ScheduledTask -TaskName peersbg -Confirm:$false -ErrorAction SilentlyContinue
  $a = New-ScheduledTaskAction -Execute cmd.exe -Argument "/c cd /d $B && shared_screen_peers_test.exe --receivers 2 --seconds $seconds --report-every 5 --max-height 720 --kbps 10000 --json C:\setup\res.json > C:\setup\res.txt 2>&1"
  $p = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\gputest" -LogonType Interactive -RunLevel Highest
  Register-ScheduledTask -TaskName peersbg -Action $a -Principal $p | Out-Null
  Remove-Item C:\setup\res.txt -ErrorAction SilentlyContinue
  Start-ScheduledTask -TaskName peersbg
}
Get-Process shared_screen_peers_test,noise_window -ErrorAction SilentlyContinue | Stop-Process -Force
& powershell -NoProfile -ExecutionPolicy Bypass -File C:\setup\start-noise.ps1 -Seconds 600 -Block 8 -Amp 0.2 | Out-Null

"=== A. the sender process is killed mid-share (taskkill /f)"
StartPeers 90; Start-Sleep 22
"NVENC sessions while sharing: $(Smi)"
Get-Process shared_screen_peers_test | Stop-Process -Force
Start-Sleep 4
"NVENC sessions 4 s after the kill: $(Smi)"
Start-Sleep 6
"NVENC sessions 10 s after the kill: $(Smi)"

"=== B. the virtual display is removed under a live share (devcon disable), then restored"
Get-Process noise_window -ErrorAction SilentlyContinue | Stop-Process -Force
& powershell -NoProfile -ExecutionPolicy Bypass -File C:\setup\start-noise.ps1 -Seconds 600 -Block 8 -Amp 0.2 | Out-Null
StartPeers 70; Start-Sleep 22
"NVENC sessions while sharing: $(Smi)"
& C:\setup\vdd\x\Dependencies\devcon.exe disable Root\MttVDD 2>&1 | Select -Last 1
Start-Sleep 12
"NVENC sessions 12 s after the display went away: $(Smi)"
"peers process alive: $([bool](Get-Process shared_screen_peers_test -ErrorAction SilentlyContinue))"
& C:\setup\vdd\x\Dependencies\devcon.exe enable Root\MttVDD 2>&1 | Select -Last 1
Start-Sleep 15
"--- what the peers test saw"
Get-Content C:\setup\res.txt -ErrorAction SilentlyContinue | Select-String '^\[t=|crash|teardown|FAIL' | Select -First 12 | ForEach-Object { $_.Line.Substring(0,[Math]::Min($_.Line.Length,200)) }
Get-Process shared_screen_peers_test -ErrorAction SilentlyContinue | Stop-Process -Force
