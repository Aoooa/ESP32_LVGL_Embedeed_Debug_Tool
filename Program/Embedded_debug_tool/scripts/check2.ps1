$ErrorActionPreference = 'SilentlyContinue'
$id = 'USB\VID_303A&PID_2987\0001'
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -eq $id }
if ($dev) {
    Write-Host "=== PID_2987 props ==="
    $props = Get-PnpDeviceProperty -InstanceId $id -ErrorAction SilentlyContinue | Where-Object { $_.KeyName -match 'Problem|Indir|Driver|Hardware|Upper' }
    $props | Format-Table KeyName, Data -AutoSize -Wrap | Out-String -Width 200
}
# 看显示器
Write-Host "=== Display adapters ==="
Get-PnpDevice -Class Display -ErrorAction SilentlyContinue | Select InstanceId, Status, FriendlyName | Format-Table -AutoSize -Wrap | Out-String -Width 200
Write-Host "=== IndirectDisplay devices ==="
Get-PnpDevice -Class Display -ErrorAction SilentlyContinue | ForEach-Object {
    $p = Get-PnpDeviceProperty -InstanceId $_.InstanceId -ErrorAction SilentlyContinue | Where-Object { $_.KeyName -eq 'DEVPKEY_IndirectDisplay' }
    if ($p.Data -and ($p.Data | Out-String).Trim()) { Write-Host "$($_.FriendlyName) : $($p.Data)" }
}