$ErrorActionPreference = 'SilentlyContinue'
# Trigger USB re-scan
$hub = Get-WmiObject Win32_USBHub | Where-Object { $_.PNPDeviceID -match 'VID_303A' }
# Use devcon or power shell to refresh
$action = 'pnputil /scan-devices'  
Start-Process -FilePath 'pnputil.exe' -ArgumentList '/scan-devices' -NoNewWindow -Wait
Start-Sleep -Seconds 3
[System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object