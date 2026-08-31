import subprocess
result = subprocess.run(['powershell', '-NoProfile', '-Command', """
Add-Type -TypeDefinition @\"
using System;
using System.Runtime.InteropServices;
public class Cfg {
    [DllImport(\"setupapi.dll\", CharSet=CharSet.Auto, SetLastError=true)]
    public static extern int CM_Get_Device_Interface_List_Size(out uint pulLen, IntPtr InterfaceClassGuid, string DeviceID, uint Flags);
    [DllImport(\"setupapi.dll\", CharSet=CharSet.Auto, SetLastError=true)]
    public static extern int CM_Get_Device_Interface_List(IntPtr InterfaceClassGuid, string DeviceID, uint Flags, char[] Buffer, uint BufferLen, out uint ActualLen);
}
\"@
$guid = [Guid]\"A5DCBF10-6530-11D2-901F-00C04FB951ED\"
$deviceId = \"USB\\\\VID_303A&PID_4010\\\\0001\"
$len = 0
$hresult = [Cfg]::CM_Get_Device_Interface_List_Size([ref]$len, [ref]$guid, $deviceId, 0)
Write-Host \"CM_Get_Device_Interface_List_Size hresult=$hresult len=$len\"
$buf = New-Object char[] $len
$actual = 0
$hresult = [Cfg]::CM_Get_Device_Interface_List([ref]$guid, $deviceId, 0, $buf, $len, [ref]$actual)
Write-Host \"CM_Get_Device_Interface_List hresult=$hresult actual=$actual\"
$paths = (-join $buf) -split \";?\\0\"
foreach ($p in $paths) {
    if ($p.Length -gt 0) { Write-Host \"  $p\" }
}
"""], capture_output=True, text=True)
print("STDOUT:", result.stdout)
print("STDERR:", result.stderr)