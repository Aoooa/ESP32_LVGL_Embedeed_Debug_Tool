$ErrorActionPreference = 'SilentlyContinue'
$id = 'USB\VID_303A&PID_2987\0001'
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -eq $id }
if (-not $dev) {
    Write-Host "Device not found"
    exit
}
Write-Host "Current status: $($dev.Status)"
$dev | Disable-PnpDevice -Confirm:$false
Start-Sleep -Seconds 2
Get-PnpDevice | Where-Object { $_.InstanceId -eq $id } | Enable-PnpDevice -Confirm:$false
Start-Sleep -Seconds 3
$dev2 = Get-PnpDevice | Where-Object { $_.InstanceId -eq $id }
Write-Host "After re-enable status: $($dev2.Status)"
$props = Get-PnpDeviceProperty -InstanceId $id -ErrorAction SilentlyContinue | Where-Object { $_.KeyName -match 'Problem|IndirectDisplay' }
$props | Format-Table KeyName, Data -AutoSize | Out-String -Width 200