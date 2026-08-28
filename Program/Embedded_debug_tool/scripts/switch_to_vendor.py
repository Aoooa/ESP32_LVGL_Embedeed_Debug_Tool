p = open('app_usbdisp.c', 'r', encoding='utf-8').read()

# Remove the conflicting tud_descriptor_string_cb (and its associated MS OS data block)
start_marker = '/* Microsoft OS 1.0 Descriptor (Extended Compat ID)'
i = p.find(start_marker)
if i >= 0:
    # Find end of the descriptor_string_cb block: search for the closing brace of that function
    end_marker_candidates = []
    # The data block ends with '};\n' and the function ends with 'return buf;\n}\n'
    # Let's just remove from start_marker to a specific anchor in the function
    fn_end_marker = '    return buf;\n}\n'
    j = p.find(fn_end_marker, i)
    if j >= 0:
        end = j + len(fn_end_marker)
        p = p[:i] + p[end:]
        print('removed old MSOS block + descriptor_string_cb')
    else:
        print('end marker not found')
else:
    print('no old block found')

# Now add vendor_control_xfer_cb (weak override, no conflict)
insertion_marker = 'esp_err_t app_usbdisp_enable(void) {'
msos_block = '''/* Microsoft OS 1.0 Descriptor (Extended Compat ID)
 * Windows 主机发起 vendor control request (bRequest=0x01 GET_DESCRIPTOR, wValue=0x03EE)
 * 我们用 tud_control_xfer 返回此 buffer.
 * 声明 interface 0 是 WinUSB 兼容, Windows 自动加载内置 winusb.sys
 * 不需装任何驱动, 不受 Secure Boot / test signing 限制
 */
static const uint8_t s_ms_os_desc[] = {
    /* Header (18 bytes) */
    0x12, 0x00,
    0x00, 0x01,
    0xEE, 0x03,
    0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* Extended Compat ID Descriptor (20 bytes) */
    0x14, 0x00,
    0x04,
    0x00,
    0x01,
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* Override TinyUSB weak default tud_vendor_control_xfer_cb
 * 处理 Microsoft OS 1.0 GET_DESCRIPTOR 请求 (vendor request)
 * 这个 callback 是 WEAK 符号, 不会被 esp_tinyusb 强定义冲突
 */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                 const tusb_control_request_t *request) {
    if (stage == CONTROL_STAGE_SETUP &&
        request->bRequest == 0x01 &&          /* GET_DESCRIPTOR */
        request->wValue == 0x03EE) {          /* MS OS 1.0 vendor descriptor */
        return tud_control_xfer(rhport, request,
                                (void *)s_ms_os_desc, sizeof(s_ms_os_desc));
    }
    return false;
}

'''
if 'tud_vendor_control_xfer_cb' not in p:
    p = p.replace(insertion_marker, msos_block + insertion_marker, 1)
    print('OK: added vendor_control_xfer_cb')
else:
    print('already added')

open('app_usbdisp.c', 'w', encoding='utf-8').write(p)
print('written')