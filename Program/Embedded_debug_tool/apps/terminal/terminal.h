#ifndef TERMINAL_H
#define TERMINAL_H

/* terminal：串口终端 APP（apps/terminal）。
 *
 * 显示 UART1/UART2 透传日志（flow_view 滚动流）+ 状态栏（活跃 UART/IO/RUN·PAUSED）
 * + 3 按钮（UART 切换 / Pause / Clear）。
 *
 * 数据流：UART 服务把数据投递到 g_display_queue（app_uart），
 * terminal_task（本模块常驻任务）消费队列 → flow_view_append（仅活跃 UART）。
 *
 * 线程：UI 构建/销毁在 LVGL 线程；terminal_task 消费队列后经 flow_view
 * 内部锁 + LVGL 定时器渲染。
 */

#include "lvgl.h"

/* 返回回调（无 UI 按钮，仅系统返回/测试用） */
typedef void (*terminal_back_cb_t)(void *ctx);

/* 初始化：创建 terminal_task（常驻，随平台启动） */
void terminal_init(void);

/* 创建终端 UI（parent 通常为当前 screen；back_cb=NULL 不显示返回按钮） */
lv_obj_t *terminal_create(lv_obj_t *parent, terminal_back_cb_t back_cb, void *ctx);

/* 进入完成（launcher 回调）：装载桥接 UART 驱动（惰性占用 IO2/4、IO16/17） */
void terminal_entered(void *app);

/* 销毁终端 UI：先停 UART（释放 IO），再清全局对象引用后删除对象树 */
void terminal_destroy(lv_obj_t *root);

/* 右滑返回（launcher 分发）：无内部分级，直接请求关闭回桌面 */
bool terminal_swipe_back(lv_obj_t *root);

/* 调试事件（测试模块用）：打印终端状态 */
void terminal_debug_event(lv_obj_t *root, int evt);

/* 状态栏刷新（外部通知，如 WebSocket 消息后；无 UI 时无操作） */
void terminal_notify_status(void);

#endif /* TERMINAL_H */
