# Get WinUSB device interface path for PID 4010
$targetId = "USB\\VID_303A&PID_4010\\0001"

# Try to use WMI ASSOCIATORS to find child device paths
$rel = Get-WmiObject -Query "ASSOCIATORS OF {Win32_PnPEntity.DeviceID='$targetId'} WHERE AssocClass = Win32_PnPEntityToContainer"
Write-Host "Containers:"
$rel | ForEach-Object { Write-Host "  $($_.DeviceID)" }

# Get all USB devices
Write-Host ""
Write-Host "All USB devices with VID 303A:"
Get-WmiObject Win32_PnPEntity | Where-Object { $_.DeviceID -match '303A' -and $_.PNPClass -eq 'USB' } | ForEach-Object {
    $pnpId = $_.DeviceID
    Write-Host "  $pnpId"
    # Get container
    $cont = Get-WmiObject -Query "ASSOCIATORS OF {Win32_PnPEntity.DeviceID='$pnpId'} WHERE AssocClass = Win32_PnPEntityToContainer"
    foreach ($c in $cont) {
        Write-Host "    container: $($c.DeviceID)"
    }
}
