Get-PnpDevice | Where-Object { $_.InstanceId -eq 'USB\VID_303A&PID_1001&MI_00\6&8934092&0&0000' } | ForEach-Object {
    $svc = Get-PnpDeviceProperty -InstanceId $_.InstanceId -ErrorAction SilentlyContinue | Where-Object { $_.KeyName -match 'DEVPKEY_Device_Service|DEVPKEY_Device_DriverName' }
    Write-Host "Service: $($svc.Data)"
}