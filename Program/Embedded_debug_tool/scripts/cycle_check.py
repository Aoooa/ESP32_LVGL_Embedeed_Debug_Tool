import subprocess, time
for i in range(8):
    result = subprocess.run(['powershell', '-NoProfile', '-Command',
        'Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match "303A" } | ForEach-Object { Write-Host $_.InstanceId $_.Status }'],
        capture_output=True, text=True)
    print(f"\n--- cycle {i+1} ---")
    print(result.stdout.strip())
    time.sleep(3)
