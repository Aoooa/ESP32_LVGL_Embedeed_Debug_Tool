#ifndef IO_PICKER_DEMO_H
#define IO_PICKER_DEMO_H

/* io_picker_demo —— IO 选择模块的演示入口（正式使用前查看效果用） */

#include "lvgl.h"

typedef struct io_picker_demo io_picker_demo_t;

io_picker_demo_t *io_picker_demo_create(lv_obj_t *parent, void (*back_cb)(void *ctx), void *ctx);
void io_picker_demo_destroy(io_picker_demo_t *demo);
bool io_picker_demo_swipe_back(io_picker_demo_t *demo);

#endif /* IO_PICKER_DEMO_H */