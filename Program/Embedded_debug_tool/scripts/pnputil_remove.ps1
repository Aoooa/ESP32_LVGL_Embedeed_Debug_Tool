$ErrorActionPreference = 'SilentlyContinue'
$id = 'USB\VID_303A&PID_2987\0001'
Write-Host "=== remove-device ==="
& pnputil /remove-device $id 2>&1
Start-Sleep -Seconds 2
Write-Host "=== enumerate after remove ==="
Get-PnpDevice -Class Display -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match '303A' } | Format-Table InstanceId, Status -AutoSize | Out-String -Width 200