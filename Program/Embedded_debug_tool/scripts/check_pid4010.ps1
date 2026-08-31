$ErrorActionPreference = 'SilentlyContinue'
$id = 'USB\VID_303A&PID_4010\0001'
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -eq $id }
if ($dev) {
    $props = Get-PnpDeviceProperty -InstanceId $id -ErrorAction SilentlyContinue | Where-Object { $_.KeyName -match 'Problem|Hardware|Upper|Driver' }
    $props | Format-Table KeyName, Data -AutoSize -Wrap | Out-String -Width 200
}