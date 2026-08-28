$ErrorActionPreference = 'SilentlyContinue'
$id = 'USB\VID_303A&PID_2987\0001'
# Registry 直接查
$key = "HKLM:\SYSTEM\CurrentControlSet\Enum\USB\VID_303A&PID_2987"
if (Test-Path $key) {
    Write-Host "=== Enum key exists ==="
    Get-ChildItem $key -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host "  $($_.PSChildName)"
        Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue | Select-Object FriendlyName, HardwareID, Service, DeviceDesc, ClassGUID, Problem, ConfigFlags | Format-List | Out-String -Width 200
    }
} else {
    Write-Host "No Enum key for $id"
}
Write-Host ""
Write-Host "=== xfz1986 service install ==="
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\WUDFRd' -ErrorAction SilentlyContinue | Select-Object DisplayName, ImagePath | Format-List | Out-String -Width 200