Add-Type -AssemblyName System.Management
$devices = Get-PnpDevice -Class USB -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match '98:A3' }
foreach ($d in $devices) {
    $props = Get-PnpDeviceProperty -InstanceId $d.InstanceId -ErrorAction SilentlyContinue
    Write-Host "=== $($d.InstanceId) ==="
    $props | Where-Object { $_.KeyName -match 'COM|ADDRESS|SERIAL|FRIENDLY|DEVICE_DESC' } | ForEach-Object { Write-Host "  $($_.KeyName) = $($_.Data)" }
    # 看 children
    $children = Get-PnpDevice -Class USB -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId.StartsWith($d.InstanceId) }
    Write-Host "  Children: $($children.Count)"
    $children | ForEach-Object { Write-Host "    $($_.InstanceId) status=$($_.Status)" }
}