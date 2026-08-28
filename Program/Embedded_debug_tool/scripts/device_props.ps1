$ErrorActionPreference = 'SilentlyContinue'
$id = 'USB\VID_303A&PID_2987\0001'
$props = Get-PnpDeviceProperty -InstanceId $id -ErrorAction SilentlyContinue
Write-Host '=== ALL DEVPKEY for PID_2987 ==='
$props | Where-Object { $_.KeyName -match 'Power|State|Capab|Required|Description|Status|Driver|Safe|Location|Capabilities|Upper|Class|Compatible|Hardware|Container|Instance|Problem|Install|Address|Manufacturer' } | Format-Table KeyName, Data -AutoSize -Wrap | Out-String -Width 250