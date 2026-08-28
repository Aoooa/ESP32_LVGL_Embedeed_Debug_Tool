p = open('app_usbdisp.c', 'r', encoding='utf-8').read()

# Remove old MS OS descriptor data block (was added before in wrong place)
msos_data_start = '/* Microsoft OS 1.0 Descriptor (Extended Compat ID)'
i = p.find(msos_data_start)
if i >= 0:
    line_start = p.rfind('\n', 0, i)
    j = p.find('};\n', i)
    if j >= 0:
        end = j + 3
        p = p[:line_start] + p[end:]
        print('removed old MS OS data block')

# Add correct implementation: s_ms_os_desc data + tud_descriptor_string_cb override
marker = '    .full_speed_config = s_cfg_desc,\n};'
new_code = '''    .high_speed_config = NULL,
};

/* Microsoft OS 1.0 Descriptor (Extended Compat ID)
 * Windows 收到 GET_DESCRIPTOR_STRING (index=0xEE) 时返回此 buffer
 * 声明 interface 0 是 WinUSB 兼容, Windows 自动加载内置 winusb.sys
 * 不需装任何驱动, 不受 Secure Boot / test signing 限制
 */
static const uint8_t s_ms_os_desc[] = {
    0x12, 0x00,
    0x00, 0x01,
    0xEE, 0x00,
    0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x14, 0x00,
    0x04,
    0x00,
    0x01,
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

uint16_t const * tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    static uint16_t buf[32];
    if (index == 0xEE) {
        uint16_t msos_len = sizeof(s_ms_os_desc);
        if (msos_len > sizeof(buf) / 2) msos_len = sizeof(buf) / 2;
        for (uint16_t i = 0; i < msos_len; i++) {
            ((uint8_t *)buf)[i * 2] = s_ms_os_desc[i];
            ((uint8_t *)buf)[i * 2 + 1] = 0;
        }
        buf[0] = (uint16_t)(msos_len * 2);
        return buf;
    }
    static const uint16_t langid_arr[] = {0x0409};
    if (index == 0) { memcpy(buf, langid_arr, sizeof(langid_arr)); return buf; }
    static const char *strs[5] = {
        "",
        USDISP_MANUFACTURER,
        USDISP_PRODUCT,
        "0001",
        USDISP_PRODUCT,
    };
    if (index >= 5) return NULL;
    const char *s = strs[index];
    uint8_t len = strlen(s); if (len > 31) len = 31;
    buf[0] = (uint16_t)((len + 1) * 2);
    for (uint8_t i = 0; i < len; i++) buf[i + 1] = s[i];
    return buf;
}'''

if marker in p:
    p = p.replace(marker, new_code, 1)
    print('OK: MS OS + string_cb added')
else:
    print('FAIL: marker not found')

open('app_usbdisp.c', 'w', encoding='utf-8').write(p)
print('written')
