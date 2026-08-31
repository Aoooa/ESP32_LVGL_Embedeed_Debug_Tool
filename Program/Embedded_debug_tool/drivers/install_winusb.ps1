# Install WinUSB driver for ESP32 USB Display (PID 0x4010)
# Run this as Administrator (right-click PowerShell, Run as administrator)

$ErrorActionPreference = 'Stop'
$InfPath = "D:\Task\embedded_debugger_tool\Program\Embedded_debug_tool\drivers\esp32_winusb.inf"

Write-Host "[1/3] Adding ESP32 WinUSB INF to driver store..." -ForegroundColor Cyan
$result = pnputil /add-driver "$InfPath" /install
Write-Host $result

Start-Sleep -Seconds 2

Write-Host "" "[2/3] Scanning for new devices..." -ForegroundColor Cyan
$result = pnputil /scan-devices
Write-Host $result

Start-Sleep -Seconds 2

Write-Host "" "[3/3] Re-enumerating ESP32 USB devices..." -ForegroundColor Cyan
$espressifDevices = Get-PnpDevice -Class USB -ErrorAction SilentlyContinue | Where-Object {
    $_.InstanceId -match 'VID_303A&PID_4010'
}
foreach ($dev in $espressifDevices) {
    Write-Host "Found: $($dev.InstanceId) status=$($dev.Status)"
    if ($dev.Status -ne 'OK') {
        Write-Host "  -> Disable and re-enable to trigger driver match..."
        $dev | Disable-PnpDevice -Confirm:$false
        Start-Sleep -Seconds 2
        Get-PnpDevice | Where-Object { $_.InstanceId -eq $dev.InstanceId } | Enable-PnpDevice -Confirm:$false
    }
}

Start-Sleep -Seconds 3

Write-Host "" "[Verify] ESP32 USB Display final state:" -ForegroundColor Cyan
$final = Get-PnpDevice -Class USB -ErrorAction SilentlyContinue | Where-Object {
    $_.InstanceId -match 'VID_303A&PID_4010'
}
foreach ($dev in $final) {
    $props = Get-PnpDeviceProperty -InstanceId $dev.InstanceId -ErrorAction SilentlyContinue
    Write-Host "  $($dev.InstanceId)"
    Write-Host "    Status: $($dev.Status)"
    $driver = $props | Where-Object { $_.KeyName -eq 'DEVPKEY_Device_DriverDesc' }
    if ($driver) { Write-Host "    Driver: $($driver.Data)" }
    $compat = $props | Where-Object { $_.KeyName -eq 'DEVPKEY_Device_CompatibleIds' }
    if ($compat) { Write-Host "    Compat: $($compat.Data)" }
}

Write-Host "" "Done. The ESP32 USB Display should now use WinUSB." -ForegroundColor Green
Write-Host "Next: run 'python D:\Task\embedded_debugger_tool\Program\Embedded_debug_tool\scripts\pc_push.py'"
