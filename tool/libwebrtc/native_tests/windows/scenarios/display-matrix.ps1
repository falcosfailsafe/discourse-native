$B = 'C:\w\build\nvenc_smoke_test'
$RUN = 'C:\setup\run-in-console.ps1'
function Console($cmd, $t = 60) { & powershell -NoProfile -ExecutionPolicy Bypass -File $RUN -Command $cmd -TimeoutSec $t }
function Noise($w, $h) { & powershell -NoProfile -ExecutionPolicy Bypass -File C:\setup\start-noise.ps1 -Seconds 150 -W $w -H $h -Block 8 -Amp 0.2 | Out-Null }

"=== A. resolution changes under a live share (7680x2160 -> 3840x2160 at t=12 s), then a fresh share"
Console "$B\display_tool.exe set \\.\DISPLAY6 7680 2160 60" | Out-Null; Start-Sleep 3; Noise 7680 2160
Console "cd /d $B && shared_screen_peers_test.exe --receivers 2 --seconds 26 --iterations 2 --change-mode-at 12 --change-mode 3840x2160x60 --report-every 6 --json C:\setup\chg.json" 220 |
  Select-String '=== iteration|^source:|changing the display|display_tool exit|^\[t=|display changed|teardown completed|ALL CHECKS|FAILED|\[FAIL\]|\[ok\] peer . decodes|\[ok\] NVENC|task last'

"=== C. HDR on (Windows advanced color), then back off"
Console "$B\display_tool.exe set \\.\DISPLAY6 7680 2160 60" | Out-Null; Start-Sleep 2
Console "$B\display_tool.exe hdr \\.\DISPLAY6 on"; Start-Sleep 4
Console "$B\display_tool.exe hdr \\.\DISPLAY6 query"
Console "cd /d $B && shared_screen_probe.exe C:\w\build\windows\x64\runner\Release\libwebrtc.dll" | Select-String 'available|reason'
Console "cd /d $B && capture_encode_test.exe --frames 30" | Select-String 'no usable|display:|Create on|FAIL|ALL CHECKS'
Console "$B\display_tool.exe hdr \\.\DISPLAY6 off"; Start-Sleep 4
Console "$B\display_tool.exe hdr \\.\DISPLAY6 query"
Console "cd /d $B && shared_screen_probe.exe C:\w\build\windows\x64\runner\Release\libwebrtc.dll" | Select-String 'available|reason'
