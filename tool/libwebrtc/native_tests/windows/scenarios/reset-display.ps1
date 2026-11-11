$B = 'C:\w\build\nvenc_smoke_test'
function Console($cmd, $t = 60) { & powershell -NoProfile -ExecutionPolicy Bypass -File C:\setup\run-in-console.ps1 -Command $cmd -TimeoutSec $t }
(Get-Content C:\VirtualDisplayDriver\vdd_settings.xml -Raw) -replace '<count>2</count>','<count>1</count>' | Set-Content C:\VirtualDisplayDriver\vdd_settings.xml -Encoding utf8
& C:\setup\vdd\x\Dependencies\devcon.exe restart Root\MttVDD 2>&1 | Select -Last 1
Start-Sleep 8
$list = Console "$B\display_tool.exe list"
$vdd = ($list | Select-String 'Virtual Display Driver' | Select -First 1).Line.Split('|')[0].Trim()
"virtual display is $vdd"
Console "$B\display_tool.exe set $vdd 7680 2160 60 0 0 primary" | Out-Null
foreach ($l in ($list | Select-String 'DISPLAY' | ? { $_.Line -notmatch 'Virtual Display Driver' })) {
  $n = $l.Line.Split('|')[0].Trim()
  if ($l.Line -match 'ATTACHED') { Console "$B\display_tool.exe detach $n" | Out-Null }
}
Start-Sleep 3
Console "$B\display_tool.exe list" | Select-String 'ATTACHED'
