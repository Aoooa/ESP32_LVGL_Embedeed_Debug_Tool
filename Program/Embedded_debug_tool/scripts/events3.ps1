$ErrorActionPreference = 'SilentlyContinue'
Get-WinEvent -LogName Application -MaxEvents 1000 -ErrorAction SilentlyContinue | ForEach-Object {
  $msg = $_.Message
  if ($msg -match 'xfz|2987|IddCx|303A|UMDF|indirect') {
    [PSCustomObject]@{
      Time = $_.TimeCreated.ToString('HH:mm:ss')
      Provider = $_.ProviderName
      Id = $_.Id
      Level = $_.LevelDisplayName
      Msg = $msg.Substring(0, [Math]::Min(300, $msg.Length))
    }
  }
} | Format-Table -AutoSize -Wrap | Out-String -Width 250