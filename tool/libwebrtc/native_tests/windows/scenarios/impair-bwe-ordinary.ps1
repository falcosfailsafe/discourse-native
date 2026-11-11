# Does a lower minimum bandwidth estimate cost anything under ordinary loss and latency?
$I = 'C:\setup\impair.ps1'
$base = '--receivers 2 --seconds 40 --report-every 10 --max-height 720 --kbps 10000'
$loss = '--drop on --drop-chance {0} --drop-inbound off --drop-outbound on'
$lag = '--lag on --lag-time {0} --lag-inbound off --lag-outbound on'
$cases = @()
foreach ($f in 1.0, 0.25) {
  $cases += ,@("healthy link, minimum estimate $([int]($f*100))%", 'none', "$base --bwe-min-fraction $f")
  $cases += ,@("3% loss, minimum estimate $([int]($f*100))%", ($loss -f 3.0), "$base --bwe-min-fraction $f")
  $cases += ,@("10% loss, minimum estimate $([int]($f*100))%", ($loss -f 10.0), "$base --bwe-min-fraction $f")
  $cases += ,@("150 ms each way, minimum estimate $([int]($f*100))%", ($lag -f 150), "$base --bwe-min-fraction $f")
  $cases += ,@("5% loss + 80 ms each way, minimum estimate $([int]($f*100))%", ((($loss -f 5.0) + ' ' + ($lag -f 80))), "$base --bwe-min-fraction $f")
}
foreach ($c in $cases) { & powershell -NoProfile -ExecutionPolicy Bypass -File $I -Label $c[0] -Clumsy $c[1] -Peers $c[2] -TimeoutSec 150 }
