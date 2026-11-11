$I = 'C:\setup\impair.ps1'
$base = '--receivers 2 --seconds 40 --report-every 10 --max-height 720 --kbps 10000'
$bw = '--bandwidth on --bandwidth-bandwidth {0} --bandwidth-inbound off --bandwidth-outbound on'
$cases = @(
  @('control: no impairment (app setting: minimum estimate = preset rate)', 'none', $base),
  @('link big enough: 3000 KB/s (24 Mbps) for 2 x 10 Mbps, app setting', ($bw -f 3000), $base),
  @('undersized 2000 KB/s (16 Mbps) for 20 Mbps: APP setting (min estimate 100%)', ($bw -f 2000), $base),
  @('undersized 2000 KB/s: minimum estimate 25% of the preset', ($bw -f 2000), "$base --bwe-min-fraction 0.25"),
  @('undersized 2000 KB/s: minimum estimate 5% of the preset', ($bw -f 2000), "$base --bwe-min-fraction 0.05"),
  @('badly undersized 600 KB/s (4.8 Mbps): APP setting (min estimate 100%)', ($bw -f 600), $base),
  @('badly undersized 600 KB/s: minimum estimate 25%', ($bw -f 600), "$base --bwe-min-fraction 0.25"),
  @('badly undersized 600 KB/s: minimum estimate 5%', ($bw -f 600), "$base --bwe-min-fraction 0.05")
)
foreach ($c in $cases) { & powershell -NoProfile -ExecutionPolicy Bypass -File $I -Label $c[0] -Clumsy $c[1] -Peers $c[2] -TimeoutSec 150 }
