$ErrorActionPreference = 'SilentlyContinue'
$dll = 'C:\Windows\System32\drivers\UMDF\xfz1986_usb_graphic.dll'
Write-Host "=== DLL exists: $(Test-Path $dll) ==="
if (Test-Path $dll) {
    Get-AuthenticodeSignature $dll | Format-List Status, IsOSBinary, @{n='Signer';e={$_.SignerCertificate.Subject}} | Out-String -Width 200
}
Write-Host ""
Write-Host "=== UMDF host service ==="
Get-Service WUDFRd -ErrorAction SilentlyContinue | Select-Object Name, Status, StartType | Format-Table -AutoSize | Out-String -Width 200
Write-Host ""
Write-Host "=== IndirectKmd (kernel) ==="
Get-Service IndirectKmd -ErrorAction SilentlyContinue | Select-Object Name, Status | Format-Table -AutoSize | Out-String -Width 200
Write-Host ""
Write-Host "=== IddCx (kernel) ==="
Get-Service IddCx -ErrorAction SilentlyContinue | Select-Object Name, Status | Format-Table -AutoSize | Out-String -Width 200