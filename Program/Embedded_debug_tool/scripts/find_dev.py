import usb.core, usb.util
print('pyusb OK')
devs = usb.core.find(find_all=True, idVendor=0x303A)
print('Found devices:')
for d in devs:
    print(' ', hex(d.idVendor), hex(d.idProduct), d.product)
