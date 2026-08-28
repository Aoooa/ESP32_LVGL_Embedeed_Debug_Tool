$ErrorActionPreference = 'SilentlyContinue'
$devs = Get-WmiObject Win32_PnPEntity | Where-Object { $_.DeviceID -match 'VID_303A' }
$devs | Select-Object Caption, DeviceID, Status | Format-Table -AutoSize | Out-String -Width 250