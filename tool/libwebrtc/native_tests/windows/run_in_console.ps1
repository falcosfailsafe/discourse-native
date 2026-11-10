# Runs a command line in the interactive console session as gputest (via a
# scheduled task with an Interactive logon), waits for it, prints its output.
param([Parameter(Mandatory)][string]$Command, [int]$TimeoutSec = 120, [string]$Out = 'C:\setup\console-out.txt')
$ErrorActionPreference = 'Stop'
Remove-Item $Out -ErrorAction SilentlyContinue
$name = 'console-run'
Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
$action = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument ('/c ' + $Command + ' > "' + $Out + '" 2>&1')
$principal = New-ScheduledTaskPrincipal -UserId "$env:COMPUTERNAME\gputest" -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds ($TimeoutSec + 30)) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Settings $settings | Out-Null
Start-ScheduledTask -TaskName $name
$deadline = (Get-Date).AddSeconds($TimeoutSec)
do { Start-Sleep -Seconds 1; $state = (Get-ScheduledTask -TaskName $name).State } while ($state -eq 'Running' -and (Get-Date) -lt $deadline)
if ($state -eq 'Running') { Stop-ScheduledTask -TaskName $name; 'TIMEOUT: task stopped' }
$info = Get-ScheduledTaskInfo -TaskName $name
if (Test-Path $Out) { Get-Content $Out }
"[task last result: 0x{0:X}]" -f $info.LastTaskResult
Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
