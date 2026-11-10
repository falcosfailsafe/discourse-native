# Animated full-display content on the virtual monitor, for capture tests.
param([int]$Seconds = 60, [int]$X = 2080, [int]$Y = 0, [int]$W = 7680, [int]$H = 2160)
$script:W = $W; $script:H = $H
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$form = New-Object System.Windows.Forms.Form
$form.FormBorderStyle = 'None'; $form.StartPosition = 'Manual'
$form.Location = New-Object System.Drawing.Point($X, $Y); $form.Size = New-Object System.Drawing.Size($W, $H)
$form.TopMost = $true; $form.ShowInTaskbar = $false
$form.GetType().GetProperty('DoubleBuffered', [Reflection.BindingFlags]'Instance,NonPublic').SetValue($form, $true, $null)
$script:t = 0
$font = New-Object System.Drawing.Font('Consolas', 120, [System.Drawing.FontStyle]::Bold)
$form.Add_Paint({
  param($s, $e)
  $g = $e.Graphics
  $g.Clear([System.Drawing.Color]::FromArgb(20, 24, 40))
  for ($i = 0; $i -lt 24; $i++) {
    $x = (($script:t * 23 + $i * 400) % ($script:W + 600)) - 300
    $c = [System.Drawing.Color]::FromArgb(255, (80 + $i * 7) % 256, (200 - $i * 6) % 256, (40 + $i * 11 + $script:t) % 256)
    $br = New-Object System.Drawing.SolidBrush($c)
    $g.FillRectangle($br, $x, ($i * 90) % $script:H, 520, 300)
    $br.Dispose()
  }
  $g.DrawString("frame $($script:t)", $font, [System.Drawing.Brushes]::White, 200, 200)
})
$timer = New-Object System.Windows.Forms.Timer; $timer.Interval = 10
$timer.Add_Tick({ $script:t++; $form.Invalidate() })
$stop = New-Object System.Windows.Forms.Timer; $stop.Interval = $Seconds * 1000
$stop.Add_Tick({ $form.Close() })
$form.Add_Shown({ $timer.Start(); $stop.Start() })
[System.Windows.Forms.Application]::Run($form)
