$I = 'C:\setup\impair.ps1'
$p720 = '--receivers 2 --seconds 40 --report-every 10 --max-height 720 --kbps 10000'
$loss = { param($c) "--drop on --drop-chance $c --drop-inbound off --drop-outbound on" }
$cases = @(
  @('control: no impairment', '', $p720),
  @('1% packet loss', (& $loss 1.0), $p720),
  @('3% packet loss', (& $loss 3.0), $p720),
  @('10% packet loss', (& $loss 10.0), $p720),
  @('50 ms added each way (about 100 ms RTT)', '--lag on --lag-time 50 --lag-inbound off --lag-outbound on', $p720),
  @('150 ms added each way (about 300 ms RTT)', '--lag on --lag-time 150 --lag-inbound off --lag-outbound on', $p720),
  @('3% loss + 50 ms each way', ((& $loss 3.0) + ' --lag on --lag-time 50 --lag-inbound off --lag-outbound on'), $p720),
  @('bandwidth ceiling ~4.8 Mbps total for 2 x 10 Mbps streams', '--bandwidth on --bandwidth-bandwidth 600 --bandwidth-inbound off --bandwidth-outbound on', $p720),
  @('bandwidth ceiling ~16 Mbps total for 2 x 10 Mbps streams', '--bandwidth on --bandwidth-bandwidth 2000 --bandwidth-inbound off --bandwidth-outbound on', $p720),
  @('3% loss at 3840x1080 (25 Mbps x 2)', (& $loss 3.0), '--receivers 2 --seconds 40 --report-every 10')
)
foreach ($c in $cases) { & powershell -NoProfile -ExecutionPolicy Bypass -File $I -Label $c[0] -Clumsy $c[1] -Peers $c[2] -TimeoutSec 150 }
