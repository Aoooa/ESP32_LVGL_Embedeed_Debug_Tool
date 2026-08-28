#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pc_push.py - PC 端程序: 截屏 -> JPEG 压缩 -> 通过 USB bulk 发给 ESP32-LCD

协议: 16 字节帧头 (xfz1986-like 协议) + JPEG payload
  帧头 packed:
    uint16_t crc16   = 0 (ESP32 忽略)
    uint8_t  type    = 3 (JPEG)
    uint8_t  cmd     = 0
    uint16_t x_lo    = 0
    uint16_t y_hi    = 0
    uint16_t w_lo    = 240 (10)
    uint16_t h_hi    = 320 (200)
    uint32_t id10_len22 = (frame_id << 22) | payload_size

依赖:
  pip install pyusb Pillow numpy
  Windows 还需要 libusb-win32 (https://github.com/libusb/libusb/wiki/Windows) 或
  使用 Zadig (https://zadig.akeo.ie/) 将设备切换为 WinUSB driver (本程序自带)

用法:
  python pc_push.py                  # 截整个主屏, 缩小到 240x320
  python pc_push.py --region 100,100,800,600  # 截指定区域
  python pc_push.py --fps 15
"""
import sys, time, struct, argparse
import threading
try:
    import usb.core, usb.util
except ImportError:
    print("ERROR: pyusb not installed. Run: pip install pyusb")
    sys.exit(1)

VID = 0x303A
PID = 0x4010
EP_OUT = 0x01  # host -> device bulk OUT
FRAME_HDR_FMT = "<HBBHHHHI"  # little-endian, packed

def make_frame_header(frame_id, jpg_bytes, w=240, h=320):
    payload = len(jpg_bytes)
    return struct.pack(
        FRAME_HDR_FMT,
        0,        # crc16
        3,        # type: JPG
        0,        # cmd
        0, 0,     # x
        w, h,     # width, height
        (frame_id & 0x3FF) << 22 | (payload & 0x3FFFFF),
    ) + jpg_bytes

def find_and_open():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print(f"device {VID:04X}:{PID:04X} not found. \n"
              "请确保: 1) ESP32 已插入 USB 2) UsbDisp APP 已点开启按钮 3) Windows 已加载 winusb.sys")
        sys.exit(1)
    print(f"found device: {VID:04X}:{PID:04X}")
    # set first configuration
    dev.set_configuration()
    # claim interface 0
    usb.util.claim_interface(dev, 0)
    print(f"claimed interface 0")
    return dev

# 截图 (用 mss 最快) 或 PIL.ImageGrab (fallback)
try:
    from mss import mss
    HAS_MSS = True
except ImportError:
    HAS_MSS = False
    try:
        from PIL import ImageGrab
        HAS_PIL = True
    except ImportError:
        HAS_PIL = False

def capture_screen(region=None):
    """Capture screen (or region) as RGB numpy array"""
    if HAS_MSS:
        with mss() as sct:
            if region:
                x, y, w, h = region
                monitor = {"left": x, "top": y, "width": w, "height": h}
            else:
                monitor = sct.monitors[1]  # primary
            img = sct.grab(monitor)
            import numpy as np
            return np.array(img)[:, :, :3]  # drop alpha
    elif HAS_PIL:
        if region:
            x, y, w, h = region
            img = ImageGrab.grab(bbox=(x, y, x+w, y+h))
        else:
            img = ImageGrab.grab()
        import numpy as np
        return np.array(img)[:, :, :3]
    else:
        print("ERROR: need mss or Pillow. Run: pip install mss Pillow")
        sys.exit(1)

def encode_jpeg(rgb_array, w=240, h=320, quality=60):
    """Resize to w*h and encode as JPEG"""
    from PIL import Image
    img = Image.fromarray(rgb_array)
    img = img.resize((w, h), Image.LANCZOS)
    import io
    buf = io.BytesIO()
    img.save(buf, format='JPEG', quality=quality)
    return buf.getvalue()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--region', type=str, default=None, help='x,y,w,h of capture region')
    parser.add_argument('--fps', type=float, default=15.0, help='target fps')
    parser.add_argument('--quality', type=int, default=60, help='JPEG quality 1-100')
    parser.add_argument('--w', type=int, default=240)
    parser.add_argument('--h', type=int, default=320)
    parser.add_argument('--port', choices=['bulk', 'cdc'], default='bulk',
                        help='transport: bulk (WinUSB/libusb) or cdc (COMx serial)')
    parser.add_argument('--com', type=str, default=None, help='COM port for cdc mode')
    args = parser.parse_args()

    region = None
    if args.region:
        region = tuple(int(x) for x in args.region.split(','))

    if args.port == 'bulk':
        dev = find_and_open()
        send = lambda data: dev.write(EP_OUT, data, timeout=2000)
    else:
        # CDC mode fallback
        import serial
        if not args.com:
            print("cdc mode requires --com COMx")
            sys.exit(1)
        ser = serial.Serial(args.com, 921600*4, timeout=2)
        send = lambda data: ser.write(data)

    print(f"streaming: {args.w}x{args.h} @ {args.fps} fps, q={args.quality}")
    print("press Ctrl+C to stop")

    frame_id = 0
    interval = 1.0 / args.fps
    stats_start = time.time()
    stats_frames = 0

    try:
        while True:
            t0 = time.time()
            try:
                rgb = capture_screen(region)
                jpg = encode_jpeg(rgb, args.w, args.h, args.quality)
                frame = make_frame_header(frame_id, jpg, args.w, args.h)
                send(frame)
                frame_id = (frame_id + 1) & 0x3FF
                stats_frames += 1
                if frame_id % 30 == 0:
                    elapsed = time.time() - stats_start
                    if elapsed > 0:
                        print(f"  frames: {stats_frames} | {stats_frames/elapsed:.1f} fps | last jpg: {len(jpg)} bytes")
            except usb.core.USBError as e:
                if e.errno == 110:  # ETIMEDOUT
                    pass  # ignore timeout
                else:
                    print(f"USB error: {e}")
                    break
            except Exception as e:
                print(f"frame error: {e}")
            dt = time.time() - t0
            if dt < interval:
                time.sleep(interval - dt)
    except KeyboardInterrupt:
        print(f"\nstopped. total frames: {stats_frames}")

if __name__ == '__main__':
    main()