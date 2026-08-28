$ErrorActionPreference = 'SilentlyContinue'
# Try Microsoft-Windows-Kernel-PnP
Get-WinEvent -LogName System -MaxEvents 500 -ErrorAction SilentlyContinue | Where-Object {
    $_.ProviderName -match 'Wudf|DriverFrameworks|Kernel-PnP|UMDF|PlugPlay' -or
    $_.Id -in @(225, 226, 227, 228, 229, 230)
} | Select-Object TimeCreated, ProviderName, Id, LevelDisplayName, @{n='Msg';e={ $_.Message.Substring(0, [Math]::Min(300, $_.Message.Length)) }} | Format-Table -AutoSize -Wrap | Out-String -Width 250