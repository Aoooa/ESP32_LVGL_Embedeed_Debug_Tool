$ErrorActionPreference = 'Stop'
$firmPath = "D:\Task\embedded_debugger_tool\Program\Embedded_debug_tool\build\Embedded_debug_tool.bin"
$esptool = "D:\Espressif\tools\python\v5.5.3\venv\Scripts\python.exe"

Write-Host "Waiting for ESP32 in download mode (BOOT+RST dance)..." -ForegroundColor Cyan
Write-Host "Press Ctrl+C to cancel" -ForegroundColor Yellow

while ($true) {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    foreach ($port in $ports) {
        try {
            $output = & $esptool -m esptool --chip esp32s3 -p $port read_mac 2>&1
            if ($LASTEXITCODE -eq 0 -and $output -match "USB-Serial/JTAG") {
                Write-Host ""
                Write-Host "[Found] $port in serial mode, trying flash..." -ForegroundColor Green
                $test = & $esptool -m esptool --chip esp32s3 -p $port --before no_reset --after hard_reset write_flash 0x10000 $firmPath 2>&1
                if ($LASTEXITCODE -eq 0) {
                    Write-Host "[SUCCESS] $port flashed" -ForegroundColor Green
                    $verify = & $esptool -m esptool --chip esp32s3 -p $port verify_flash 0x10000 $firmPath 2>&1
                    if ($LASTEXITCODE -eq 0) {
                        Write-Host "[VERIFIED]" -ForegroundColor Green
                        Start-Sleep -Seconds 8
                        $pid = Get-PnpDevice | Where-Object { $_.InstanceId -eq "USB\VID_303A&PID_4010\0001" }
                        if ($pid) { Write-Host "[PID_4010] status = $($pid.Status)" -ForegroundColor Cyan }
                    }
                    break
                } else {
                    Write-Host "[Info] $port not in download, waiting..." -ForegroundColor Yellow
                }
            }
        } catch {}
    }
    Start-Sleep -Milliseconds 500
    Write-Host "." -NoNewline
}