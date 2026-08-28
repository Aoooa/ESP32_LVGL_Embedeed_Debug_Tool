try {
    $p = New-Object System.IO.Ports.SerialPort('COM14', 115200)
    $p.Open()
    Write-Host 'COM14 OPENED OK'
    $p.Close()
} catch {
    Write-Host "COM14 err: $($_.Exception.Message)"
}