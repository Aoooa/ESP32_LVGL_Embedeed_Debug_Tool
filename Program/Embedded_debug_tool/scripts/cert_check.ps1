$ErrorActionPreference = 'SilentlyContinue'
Write-Host "=== bcdedit test signing ==="
bcdedit | Select-String -Pattern 'testsigning' | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "=== Certificate details ==="
$s = Get-AuthenticodeSignature 'C:\Windows\System32\drivers\UMDF\xfz1986_usb_graphic.dll'
Write-Host "Status: $($s.Status)"
Write-Host "NotValidReason: $($s.StatusMessage)"
if ($s.SignerCertificate) {
    Write-Host "Subject: $($s.SignerCertificate.Subject)"
    Write-Host "Issuer: $($s.SignerCertificate.Issuer)"
    Write-Host "ValidFrom: $($s.SignerCertificate.NotBefore)"
    Write-Host "ValidTo: $($s.SignerCertificate.NotAfter)"
    Write-Host "Thumbprint: $($s.SignerCertificate.Thumbprint)"
    Write-Host "SelfSigned: $($s.SignerCertificate.Subject -eq $s.SignerCertificate.Issuer)"
}