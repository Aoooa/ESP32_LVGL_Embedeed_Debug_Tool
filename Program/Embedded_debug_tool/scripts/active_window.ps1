Add-Type -TypeDefinition @"
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
public class WinHelper {
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    public static extern int GetWindowText(IntPtr hwnd, System.Text.StringBuilder sb, int max);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    public static string GetActiveTitle() {
        var hwnd = GetForegroundWindow();
        if (hwnd == IntPtr.Zero) return "(no window)";
        var sb = new System.Text.StringBuilder(256);
        GetWindowText(hwnd, sb, 256);
        return sb.ToString();
    }
}
"@
Write-Host "Active window: $([WinHelper]::GetActiveTitle())"
Get-Process | Where-Object { $_.Name -match 'CodeSetup|setup|install' } | ForEach-Object {
    Write-Host "$($_.Name) (PID $($_.Id)) - Main: $($_.MainWindowTitle)"
}