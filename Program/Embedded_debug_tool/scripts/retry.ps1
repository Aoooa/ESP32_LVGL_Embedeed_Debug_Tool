$ErrorActionPreference = 'SilentlyContinue'
$id = 'USB\VID_303A&PID_2987\0001'

# 尝试最多 5 次 disable/enable 看会不会有时成功
for ($i = 1; $i -le 5; $i++) {
    Write-Host "=== Attempt $i ==="
    $dev = Get-PnpDevice | Where-Object { $_.InstanceId -eq $id }
    if (-not $dev) {
        Write-Host "Device not present"
        break
    }
    Write-Host "Status before: $($dev.Status)"
    $dev | Disable-PnpDevice -Confirm:$false
    Start-Sleep -Seconds 2
    Get-PnpDevice | Where-Object { $_.InstanceId -eq $id } | Enable-PnpDevice -Confirm:$false
    Start-Sleep -Seconds 5
    $dev2 = Get-PnpDevice | Where-Object { $_.InstanceId -eq $id }
    Write-Host "Status after: $($dev2.Status)"
    $props = Get-PnpDeviceProperty -InstanceId $id -ErrorAction SilentlyContinue | Where-Object { $_.KeyName -eq 'DEVPKEY_IndirectDisplay' -or $_.KeyName -eq 'DEVPKEY_Device_ProblemCode' }
    foreach ($p in $props) {
        $data = if ($p.Data -is [byte[]]) { ($p.Data | ForEach-Object { $_.ToString('X2') }) -join ' ' } else { $p.Data }
        Write-Host "  $($p.KeyName) = $data"
    }
    if ($dev2.Status -eq 'OK') {
        Write-Host "  *** SUCCESS ***"
        break
    }
    Start-Sleep -Seconds 3
}