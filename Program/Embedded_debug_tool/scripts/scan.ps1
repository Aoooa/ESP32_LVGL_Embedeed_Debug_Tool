$ErrorActionPreference = 'SilentlyContinue'
$id = 'USB\VID_303A&PID_2987\0001'
Write-Host "=== current user ==="
whoami /groups | Select-String 'Admin'
Write-Host ""
Write-Host "=== run pnputil /scan-devices ==="
& pnputil /scan-devices 2>&1
Start-Sleep -Seconds 2
Write-Host "=== after scan ==="
Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VID_303A' -and $_.InstanceId -match '2987' } | Select-Object InstanceId, Status | Format-Table -AutoSize | Out-String -Width 200