# Runs the peers test while clumsy impairs same-host UDP. Usage:
#   impair.ps1 -Label "3% loss" -Clumsy "--drop on --drop-chance 3.0" -Peers "--receivers 2 ..."
param([string]$Label, [string]$Clumsy = 'none', [string]$Peers, [int]$TimeoutSec = 200)
$B = 'C:\w\build\nvenc_smoke_test'
$ip = (Get-NetIPAddress -AddressFamily IPv4 | ? { $_.IPAddress -like '172.31.*' } | Select -First 1).IPAddress
function Stop-Clumsy { Get-Process clumsy -ErrorAction SilentlyContinue | Stop-Process -Force; Unregister-ScheduledTask -TaskName clumsy -Confirm:$false -ErrorAction SilentlyContinue }
Stop-Clumsy
if ($Clumsy -ne 'none') {
  $filter = "udp and outbound and ip.SrcAddr == $ip and ip.DstAddr == $ip"
  $a = New-ScheduledTaskAction -Execute C:\setup\clumsy\x\clumsy.exe -Argument "--filter `"$filter`" $Clumsy"
  $p = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\gputest" -LogonType Interactive -RunLevel Highest
  Register-ScheduledTask -TaskName clumsy -Action $a -Principal $p | Out-Null
  Start-ScheduledTask -TaskName clumsy
  Start-Sleep 4
  if (-not (Get-Process clumsy -ErrorAction SilentlyContinue)) { "!! clumsy did not start" }
}
"=== $Label"
& powershell -NoProfile -ExecutionPolicy Bypass -File C:\setup\run-in-console.ps1 -Command "cd /d $B && shared_screen_peers_test.exe $Peers --json C:\setup\impair.json" -TimeoutSec $TimeoutSec |
  Select-String '^\[t=|load summary|received \(network|steady avg|\[FAIL\]|ALL CHECKS|FAILED|crash|task last'
Stop-Clumsy
