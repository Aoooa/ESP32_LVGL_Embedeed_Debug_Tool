$ErrorActionPreference = 'SilentlyContinue'
$id = 'USB\VID_303A&PID_2987\0001'
$dev = Get-CimInstance Win32_PnPEntity -Filter "DeviceID='$id'"
Write-Host "Status: $($dev.Status)"
Write-Host "Problem: $($dev.ConfigManagerErrorCode)"
Write-Host "Class: $($dev.PNPClass)"
Write-Host "Service: $($dev.Service)"
Write-Host "CompatID: $($dev.CompatibleID)"
Write-Host "HardwareID: $($dev.HardwareID)"
# 看设备上关联的所有驱动（可能多个）
Write-Host ""
Write-Host "=== DevNode Driver Key ==="
$key = "HKLM:\SYSTEM\CurrentControlSet\Enum\$id"
if (Test-Path $key) {
    Get-ChildItem $key | ForEach-Object {
        $sub = $_.Name
        Write-Host "$sub"
        $props = Get-ItemProperty "$sub\Device Parameters" -ErrorAction SilentlyContinue
        $props | Out-String -Width 200
    }
}