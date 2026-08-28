$ErrorActionPreference = 'SilentlyContinue'
$id = 'USB\VID_303A&PID_2987\0001'
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -eq $id }
if ($dev) {
    Write-Host "=== Device props (PID_2987) ==="
    $props = Get-PnpDeviceProperty -InstanceId $id -ErrorAction SilentlyContinue | Where-Object { $_.KeyName -match 'Problem|Status|Indir|Driver|Hardware' }
    $props | Format-Table KeyName, Data -AutoSize | Out-String -Width 250
}
Write-Host ""
Write-Host "=== All Monitors ==="
Get-PnpDevice -Class Monitor -ErrorAction SilentlyContinue | Select-Object InstanceId, Status, FriendlyName | Format-Table -AutoSize | Out-String -Width 200
Write-Host ""
Write-Host "=== Recent system errors ==="
Get-WinEvent -LogName System -MaxEvents 200 | Where-Object {
    $_.LevelDisplayName -match '错误|警告' -and ($_.Message -match 'xfz|303A|2987|IddCx|UMDF|Wudf|Display')
} | Select-Object TimeCreated, ProviderName, Id, @{n='Msg';e={ $_.Message.Substring(0, [Math]::Min(250, $_.Message.Length)) }} | Select-Object -First 5 | Format-Table -AutoSize -Wrap | Out-String -Width 200