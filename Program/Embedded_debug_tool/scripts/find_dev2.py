import ctypes
from ctypes import wintypes, byref, c_uint8, c_uint16, c_uint32, c_void_p, Structure, c_wchar_p, create_string_buffer, POINTER, c_int

setupapi = ctypes.WinDLL('setupapi', use_last_error=True)
kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)

GUID_DEVINTERFACE_USB_DEVICE = b'\x10\xbf\xdc\xa5\x50\x30\x65\x11\x1f\x90\x9c\x00\x4f\xfb\x51\x3a'  # wait this is wrong
# Actually GUID_DEVINTERFACE_USB_DEVICE = {A5DCBF10-6530-11D2-901F-00C04FB951ED}
# In bytes (little-endian for first 3, then big-endian for last 8):
import struct
g_bytes = struct.pack('<IHH', 0x6530DCBF, 0x11D2, 0x901F) + b'\x00\xC0\x4F\xFB\x51\x3A'
print(f'GUID bytes: {g_bytes.hex()}')

guid = ctypes.create_string_buffer(16)
ctypes.memmove(guid, g_bytes, 16)

class SP_DEVICE_INTERFACE_DATA(Structure):
    _fields_ = [("cbSize", c_uint32), ("InterfaceClassGuid", c_uint8 * 16),
                ("Flags", c_uint32), ("Reserved", c_uint32)]

SetupDiGetClassDevsW = setupapi.SetupDiGetClassDevsW
SetupDiGetClassDevsW.restype = c_void_p
SetupDiGetClassDevsW.argtypes = [c_void_p, c_wchar_p, c_void_p, c_uint32]

SetupDiEnumDeviceInterfaces = setupapi.SetupDiEnumDeviceInterfaces
SetupDiEnumDeviceInterfaces.restype = c_int
SetupDiEnumDeviceInterfaces.argtypes = [c_void_p, c_void_p, c_void_p, c_uint32, c_uint32, POINTER(SP_DEVICE_INTERFACE_DATA)]

SetupDiGetDeviceInterfaceDetailW = setupapi.SetupDiGetDeviceInterfaceDetailW
SetupDiGetDeviceInterfaceDetailW.restype = c_int
SetupDiGetDeviceInterfaceDetailW.argtypes = [c_void_p, POINTER(SP_DEVICE_INTERFACE_DATA), c_void_p, c_uint32, POINTER(c_uint32), c_void_p]

SetupDiDestroyDeviceInfoList = setupapi.SetupDiDestroyDeviceInfoList
SetupDiDestroyDeviceInfoList.argtypes = [c_void_p]

dev_info = SetupDiGetClassDevsW(guid, None, None, 0x12)  # DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
if not dev_info:
    print(f'GetClassDevs failed: {ctypes.get_last_error()}')
    raise SystemExit

sp_did = SP_DEVICE_INTERFACE_DATA()
sp_did.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DATA)
print(f'cbSize = {sp_did.cbSize}')

idx = 0
paths = []
while True:
    ok = SetupDiEnumDeviceInterfaces(dev_info, None, guid, idx, byref(sp_did))
    if not ok:
        break
    idx += 1
    needed = c_uint32(0)
    SetupDiGetDeviceInterfaceDetailW(dev_info, byref(sp_did), None, 0, byref(needed), None)
    if needed.value == 0:
        continue
    buf = (c_uint8 * needed.value)()
    ok = SetupDiGetDeviceInterfaceDetailW(dev_info, byref(sp_did), buf, needed.value, byref(needed), None)
    if not ok:
        err = ctypes.get_last_error()
        print(f'GetDetail failed: err={err}')
        continue
    # Path starts at offset 4 in detail data
    path_bytes = bytes(buf[4:needed.value]).split(b'\x00\x00', 1)[0]
    try:
        path = path_bytes.decode('utf-16-le')
    except:
        continue
    if '303A' in path.upper():
        paths.append(path)
        print(f'  {path}')

SetupDiDestroyDeviceInfoList(dev_info)
print(f'\nTotal PID_4010/4010 devices: {len(paths)}')
