$ErrorActionPreference = 'SilentlyContinue'
$id = 'USB\VID_303A&PID_4010\0001'
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -eq $id }
if ($dev) {
    Write-Host "Before: Status=$($dev.Status)"
    $dev | Disable-PnpDevice -Confirm:$false
    Start-Sleep -Seconds 2
    Get-PnpDevice | Where-Object { $_.InstanceId -eq $id } | Enable-PnpDevice -Confirm:$false
    Start-Sleep -Seconds 3
    $dev2 = Get-PnpDevice | Where-Object { $_.InstanceId -eq $id }
    Write-Host "After: Status=$($dev2.Status)"
}
# 显示所有设备属性
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -eq $id }
$props = Get-PnpDeviceProperty -InstanceId $id -ErrorAction SilentlyContinue | Where-Object { $_.KeyName -match 'Problem|Status|Driver|Desc|Inf' }
$props | Format-Table KeyName, Data -AutoSize | Out-String -Width 200