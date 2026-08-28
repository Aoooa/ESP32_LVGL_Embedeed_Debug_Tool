p = open('app_usbdisp.c', 'r', encoding='utf-8').read()
old = '    jpeg_dec_close(dec);\n    free(io); free(hi);'
new = '    jpeg_dec_close(dec);\n    uint16_t w = hi->width, h = hi->height;\n    free(io); free(hi);'
p = p.replace(old, new, 1)
old2 = '    s_rgb_back_w = hi->width;\n    s_rgb_back_h = hi->height;'
new2 = '    s_rgb_back_w = w;\n    s_rgb_back_h = h;'
p = p.replace(old2, new2, 1)
open('app_usbdisp.c', 'w', encoding='utf-8').write(p)
print('fixed uaf')