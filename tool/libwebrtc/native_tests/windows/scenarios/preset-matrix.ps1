$R = 'powershell -NoProfile -ExecutionPolicy Bypass -File C:\setup\run-in-console.ps1'
$B = 'C:\w\build\nvenc_smoke_test'
function Case($label, $w, $h, $peerArgs) {
  "=== $label (display ${w}x${h})"
  & powershell -NoProfile -ExecutionPolicy Bypass -File $R.Split(' ')[-1] -Command "$B\display_tool.exe set \\.\DISPLAY6 $w $h 60" -TimeoutSec 60 | Out-Null
  Start-Sleep 3
  & powershell -NoProfile -ExecutionPolicy Bypass -File C:\setup\start-noise.ps1 -Seconds 120 -W $w -H $h -Block 8 -Amp 0.2 | Out-Null
  & powershell -NoProfile -ExecutionPolicy Bypass -File C:\setup\run-in-console.ps1 -Command "cd /d $B && shared_screen_peers_test.exe $peerArgs --json C:\setup\preset.json" -TimeoutSec 120 |
    Select-String '^source:|^peer 0 negotiated:|steady avg|^\[t=|\[FAIL\]|ALL CHECKS|FAILED|crash|task last'
}
Case '4K source, source_scale 1.0 (needs level 5.2)' 3840 2160 '--receivers 1 --seconds 25 --scale 1.0 --kbps 35000 --report-every 12'
Case '720p60 preset on 32:9' 7680 2160 '--receivers 2 --seconds 25 --max-height 720 --kbps 10000 --report-every 12'
Case '1080p60 preset on 32:9' 7680 2160 '--receivers 2 --seconds 25 --max-height 1080 --kbps 16000 --report-every 12'
Case '1080p source, 1080p60 preset' 1920 1080 '--receivers 2 --seconds 25 --max-height 1080 --kbps 16000 --report-every 12'
Case '30 fps on the 32:9 source (half scale)' 7680 2160 '--receivers 2 --seconds 25 --scale 0.5 --fps 30 --kbps 15000 --report-every 12'
