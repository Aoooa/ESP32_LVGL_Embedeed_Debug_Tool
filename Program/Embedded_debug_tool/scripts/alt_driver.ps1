$ErrorActionPreference = 'SilentlyContinue'
$id = 'USB\VID_303A&PID_2987\0001'
# SetupAPI: 列出设备所有可能的驱动
$setup = New-Object System.Text.StringBuilder
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class SetupApi {
    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Auto)]
    public static extern IntPtr SetupDiGetClassDevs(IntPtr classGuid, string enumerator, IntPtr hwndParent, uint flags);
    [DllImport("setupapi.dll", SetLastError=true)]
    public static extern bool SetupDiDestroyDeviceInfoList(IntPtr devInfo);
    [DllImport("setupapi.dll", SetLastError=true, CharSet=CharSet.Auto)]
    public static extern bool SetupDiGetDeviceRegistryProperty(IntPtr devInfo, ref SP_DEVINFO_DATA devData, uint property, out uint propertyRegDataType, IntPtr propertyBuffer, uint propertyBufferSize, out uint requiredSize);
    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVINFO_DATA { public uint cbSize; public Guid ClassGuid; public uint DevInst; public IntPtr Reserved; }
}
"@
# 显示设备驱动列表 (使用 WMI 替代方案)
$dev = Get-Win32PNPEntity | Where-Object { $_.PNPDeviceID -eq $id }
Write-Host "Device: $($dev.Name) status=$($dev.Status) Problem=$($dev.ConfigManagerErrorCode)"
$class = $dev.PNPClass
Write-Host "Class: $class"
# 看是否有其他驱动候选
$alt = Get-WmiObject Win32_PnPSignedDriver -ErrorAction SilentlyContinue | Where-Object { $_.DeviceID -eq $id -or $_.DeviceName -like '*xfz*' } | Select-Object DeviceName, DriverVersion, Manufacturer, InfName
Write-Host '=== Signed drivers for xfz ==='
$alt | Format-Table -AutoSize -Wrap | Out-String -Width 200