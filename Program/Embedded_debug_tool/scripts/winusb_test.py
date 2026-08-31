#!/usr/bin/env python3
"""Direct WinUSB API access for ESP32 USB Display (PID 0x4010)
Uses ctypes to call SetupAPI + WinUSB. Bypasses pyusb (no backend needed)."""
import sys, time, struct, ctypes
from ctypes import wintypes, byref, POINTER, c_uint8, c_uint16, c_uint32, c_void_p, c_int, Structure, c_wchar_p, c_byte, create_string_buffer, addressof

# Windows DLL handles
setupapi = ctypes.WinDLL('setupapi', use_last_error=True)
winusb = ctypes.WinDLL('winusb', use_last_error=True)
kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)

# SetupAPI prototypes
SetupDiGetClassDevsW = setupapi.SetupDiGetClassDevsW
SetupDiGetClassDevsW.restype = c_void_p
SetupDiGetClassDevsW.argtypes = [c_void_p, c_wchar_p, c_void_p, c_uint32]

SetupDiEnumDeviceInterfaces = setupapi.SetupDiEnumDeviceInterfaces
SetupDiEnumDeviceInterfaces.restype = c_int
SetupDiEnumDeviceInterfaces.argtypes = [c_void_p, c_void_p, c_void_p, c_uint32, c_uint32, POINTER(c_byte*28)]

SetupDiGetDeviceInterfaceDetailW = setupapi.SetupDiGetDeviceInterfaceDetailW
SetupDiGetDeviceInterfaceDetailW.restype = c_int
SetupDiGetDeviceInterfaceDetailW.argtypes = [c_void_p, POINTER(c_byte*28), c_void_p, c_uint32, POINTER(c_uint32), c_void_p]

SetupDiDestroyDeviceInfoList = setupapi.SetupDiDestroyDeviceInfoList
SetupDiDestroyDeviceInfoList.argtypes = [c_void_p]

# WinUSB prototypes
WinUsb_Initialize = winusb.WinUsb_Initialize
WinUsb_Initialize.restype = c_int
WinUsb_Initialize.argtypes = [c_void_p, POINTER(c_void_p)]

WinUsb_Free = winusb.WinUsb_Free
WinUsb_Free.argtypes = [c_void_p]

WinUsb_GetAssociatedInterface = winusb.WinUsb_GetAssociatedInterface
WinUsb_GetAssociatedInterface.restype = c_int
WinUsb_GetAssociatedInterface.argtypes = [c_void_p, c_uint8, POINTER(c_void_p)]

WinUsb_GetDescriptor = winusb.WinUsb_GetDescriptor
WinUsb_GetDescriptor.restype = c_int
WinUsb_GetDescriptor.argtypes = [c_void_p, c_uint8, c_uint8, c_uint16, c_uint16, POINTER(c_uint8), c_uint16, POINTER(c_uint32)]

WinUsb_ControlTransfer = winusb.WinUsb_ControlTransfer
WinUsb_ControlTransfer.restype = c_int
WinUsb_ControlTransfer.argtypes = [c_void_p, c_uint8, c_uint8, c_uint16, c_uint16, POINTER(c_uint8), c_uint16, POINTER(c_uint32)]

# kernel32
CreateFileW = kernel32.CreateFileW
CreateFileW.restype = c_void_p
CreateFileW.argtypes = [c_wchar_p, c_uint32, c_uint32, c_void_p, c_uint32, c_uint32, c_void_p]

GENERIC_READ_WRITE = 0xC0000000
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = -1

GUID_DEVINTERFACE_WINUSB = bytes.fromhex('DA812CD0-5CBE-4742-95F2-DB5F2DDF8B30'.replace('-', ''))[4:]  # wrong; use actual
# Actual GUID_DEVINTERFACE_USB_DEVICE: {A5DCBF10-6530-11D2-901F-00C04FB951ED}
def hex_to_guid_bytes(s):
    s = s.replace('-', '')
    return bytes.fromhex(s)

GUID_USB_DEVICE = hex_to_guid_bytes('A5DCBF10-6530-11D2-901F-00C04FB951ED')

class GUID(Structure):
    _fields_ = [("Data1", c_uint32), ("Data2", c_uint16), ("Data3", c_uint16),
                ("Data4", c_uint8 * 8)]
    @classmethod
    def from_bytes(cls, b):
        import struct
        g = cls()
        g.Data1, g.Data2, g.Data3 = struct.unpack_from("<IHH", b, 0)
        g.Data4[:] = b[8:16]
        return g

class SP_DEVICE_INTERFACE_DATA(Structure):
    _fields_ = [("cbSize", c_uint32), ("InterfaceClassGuid", GUID),
                ("Flags", c_uint32), ("Instance", c_uint32)]

def find_device_path(target_vid, target_pid):
    """Find device path for VID_xxxx&PID_yyyy"""
    guid = GUID.from_bytes(GUID_USB_DEVICE)
    dev_info = SetupDiGetClassDevsW(byref(guid), None, None, 0x12)  # DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    if not dev_info:
        print(f"SetupDiGetClassDevs failed: {ctypes.get_last_error()}")
        return None

    did = (c_byte * 28)()
    did_ptr = ctypes.cast(did, c_void_p).value
    sp_did = SP_DEVICE_INTERFACE_DATA()
    sp_did.cbSize = 28 + 8  # SP_DEVICE_INTERFACE_DATA size

    paths = []
    idx = 0
    while SetupDiEnumDeviceInterfaces(dev_info, None, byref(guid), idx, byref(sp_did)):
        idx += 1
        needed = c_uint32(0)
        # First call to get required size
        SetupDiGetDeviceInterfaceDetailW(dev_info, byref(sp_did), None, 0, byref(needed), None)
        if needed.value == 0:
            continue
        # Allocate buffer for detail
        buf = (c_byte * needed.value)()
        # Set cbSize in first DWORD
        ctypes.cast(buf, POINTER(c_uint32))[0] = 8  # SP_DEVICE_INTERFACE_DETAIL_DATA size on Win10+
        ok = SetupDiGetDeviceInterfaceDetailW(dev_info, byref(sp_did), buf, needed.value, None, None)
        if not ok:
            continue
        # Extract device path (starts at offset 4 in detail data)
        # On Win 10+, SP_DEVICE_INTERFACE_DETAIL_DATA has DevicePath at offset 4
        # On older: header is larger. Try offset 4 first.
        path_bytes = bytes(buf[4:needed.value]).split(b'\x00', 1)[0]
        try:
            path = path_bytes.decode('utf-16-le')
        except:
            continue
        if f'vid_{target_vid:04x}&pid_{target_pid:04x}' in path.lower():
            paths.append(path)
        elif f'VID_{target_vid:04x}' in path and f'PID_{target_pid:04x}' in path:
            paths.append(path)
    SetupDiDestroyDeviceInfoList(dev_info)
    return paths

def open_winusb(path):
    """Open device, init WinUSB"""
    INVALID_HANDLE_VALUE = -1
    h = CreateFileW(path, GENERIC_READ_WRITE, 0x3, None, OPEN_EXISTING, 0x60, None)  # FILE_FLAG_OVERLAPPED = 0x60
    if h.value == INVALID_HANDLE_VALUE or h.value == 0:
        err = ctypes.get_last_error()
        print(f"CreateFileW failed: err={err}")
        print(f"path: {path}")
        return None, None
    iface = c_void_p()
    ok = WinUsb_Initialize(h, byref(iface))
    if not ok:
        err = ctypes.get_last_error()
        kernel32.CloseHandle(h)
        print(f"WinUsb_Initialize failed: err={err}")
        return None, None
    return h, iface

def main():
    paths = find_device_path(0x303A, 0x4010)
    print(f"Found {len(paths)} device path(s)")
    for p in paths:
        print(f"  {p}")
    if not paths:
        print("No device found")
        return
    path = paths[0]
    print(f"\nOpening: {path}")
    h, iface = open_winusb(path)
    if not h:
        print("Failed to open")
        return
    print(f"Opened! handle={h.value:#x}, iface={iface.value:#x}")
    print("\nDevice accessible! We could now:")
    print("  1. Get associated interface (WinUsb_GetAssociatedInterface)")
    print("  2. Find bulk OUT endpoint")
    print("  3. Write frames via WinUsb_WriteBulk")
    print("\nFor now, just verify we can open it. PC-side script can be extended.")

    WinUsb_Free(iface)
    kernel32.CloseHandle(h)

if __name__ == "__main__":
    main()