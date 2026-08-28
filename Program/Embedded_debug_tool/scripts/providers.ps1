$ErrorActionPreference = 'SilentlyContinue'
# 列出所有相关 provider
Get-WinEvent -ListProvider * | Where-Object { $_.Name -match 'Wudf|UMDF|Device|Driver' } | Select-Object Name | Format-Table -AutoSize | Out-String -Width 200