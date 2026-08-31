Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class FindPath {
    [DllImport("setupapi.dll", CharSet=CharSet.Auto, SetLastError=true)]
    public static extern IntPtr SetupDiGetClassDevs(ref Guid ClassGuid, string Enumerator, IntPtr hwndParent, uint Flags);
    [DllImport("setupapi.dll", CharSet=CharSet.Auto, SetLastError=true)]
    public static extern bool SetupDiEnumDeviceInterfaces(IntPtr DeviceInfoSet, IntPtr DeviceInfoData, ref Guid InterfaceClassGuid, uint MemberIndex, IntPtr DeviceInterfaceData);
    [DllImport("setupapi.dll", CharSet=CharSet.Auto, SetLastError=true)]
    public static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr DeviceInfoSet, IntPtr DeviceInterfaceData, IntPtr DeviceInterfaceDetailData, uint DeviceInterfaceDetailDataSize, out uint RequiredSize, IntPtr DeviceInfoData);
    [DllImport("setupapi.dll")]
    public static extern void SetupDiDestroyDeviceInfoList(IntPtr DeviceInfoSet);
    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVICE_INTERFACE_DATA {
        public uint cbSize;
        public Guid InterfaceClassGuid;
        public uint Flags;
        public uint Reserved;
    }
}
"@
$guid = [Guid]"A5DCBF10-6530-11D2-901F-00C04FB951ED"
$devInfo = [FindPath]::SetupDiGetClassDevs([ref]$guid, $null, [IntPtr]::Zero, 18)
if ($devInfo -eq [IntPtr]::Zero) {
    Write-Host "GetClassDevs failed"
    exit
}
$did = New-Object FindPath+SP_DEVICE_INTERFACE_DATA
$did.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($did)
$idx = 0
while ($true) {
    $ok = [FindPath]::SetupDiEnumDeviceInterfaces($devInfo, [IntPtr]::Zero, [ref]$guid, $idx, [ref]$did)
    if (-not $ok) { break }
    $idx++
    $needed = 0
    [FindPath]::SetupDiGetDeviceInterfaceDetail($devInfo, [ref]$did, [IntPtr]::Zero, 0, [ref]$needed, [IntPtr]::Zero) | Out-Null
    if ($needed -eq 0) { continue }
    $buf = New-Object byte[] $needed
    $ok = [FindPath]::SetupDiGetDeviceInterfaceDetail($devInfo, [ref]$did, $buf, $needed, [ref]$needed, [IntPtr]::Zero)
    if (-not $ok) { continue }
    $path = [System.Text.Encoding]::Unicode.GetString($buf, 4, $needed - 5)
    if ($path -match '303A') {
        Write-Host "FOUND: $path"
    }
}
[FindPath]::SetupDiDestroyDeviceInfoList($devInfo)
