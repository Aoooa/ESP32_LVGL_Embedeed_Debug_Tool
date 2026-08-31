$MyVid = '303A'
$MyPid = '4010'
$devices = Get-WmiObject Win32_PnPEntity | Where-Object { $_.DeviceID -match "USB.VID_$MyVid.PID_$MyPid" }
foreach ($d in $devices) {
    Write-Host "Device: $($d.DeviceID)"
    Write-Host "  Status: $($d.Status)"
}
Write-Host ""
Write-Host "Getting device interface GUID..."
# Use setupapi to enumerate USB device interface
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class SetupApi {
    [DllImport("setupapi.dll", CharSet=CharSet.Auto)]
    public static extern IntPtr SetupDiGetClassDevs(ref Guid ClassGuid, string Enumerator, IntPtr hwndParent, uint Flags);
    [DllImport("setupapi.dll", CharSet=CharSet.Auto)]
    public static extern bool SetupDiEnumDeviceInterfaces(IntPtr DeviceInfoSet, IntPtr DeviceInfoData, ref Guid InterfaceClassGuid, uint MemberIndex, IntPtr DeviceInterfaceData);
    [DllImport("setupapi.dll", CharSet=CharSet.Auto)]
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
$usbGuid = [Guid]"A5DCBF10-6530-11D2-901F-00C04FB951ED"
$devInfo = [SetupApi]::SetupDiGetClassDevs([ref]$usbGuid, $null, [IntPtr]::Zero, 18)
if ($devInfo -ne [IntPtr]::Zero) {
    $idx = 0
    while ($true) {
        $did = New-Object SetupApi+SP_DEVICE_INTERFACE_DATA
        $did.cbSize = 32
        $ok = [SetupApi]::SetupDiEnumDeviceInterfaces($devInfo, [IntPtr]::Zero, [ref]$usbGuid, $idx, [IntPtr]::Zero)  # actually need ref to struct
        if (-not $ok) { break }
        $idx++
    }
    Write-Host "Enumerated $idx USB devices"
    [SetupApi]::SetupDiDestroyDeviceInfoList($devInfo)
}
