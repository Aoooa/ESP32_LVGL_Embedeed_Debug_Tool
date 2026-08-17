#ifndef LAUNCHER_H
#define LAUNCHER_H

/* launcher：桌面启动器（卡片列表滚筒 + 调速拨轮），基于 LVGL 9.4。
 *
 * 结构：
 *   root（全屏背景，纯色，随主题动画渐变）
 *   ├── drum（滚筒容器：可滚动，卡片为刚性整体，跟手 + 惯性）
 *   │   └── cards[]（圆角边框卡片，纯文字内容，最多 3 行）
 *   └── wheel（右侧调速拨轮：上下轨道 + 矩形钮，拖动控制滚筒速度）
 *
 * APP 管理：点击可启动卡片 → launcher_app_launch 创建对应全屏界面
 * （覆盖在启动器之上）；APP 返回 → launcher_app_close 销毁界面回桌面。
 * 当前支持：文件浏览器（LAUNCH_APP_FILES）、TXT 书架（LAUNCH_APP_READER）；
 * 其余卡片为占位（无操作）。
 *
 * 主题：launcher_set_theme 切换黑/白，背景 300ms 渐变，文字色即时反转。
 *
 * 线程：全部交互在 LVGL 线程；set_theme/app 接口须在 LVGL 线程或持锁调用。
 */

#include "lvgl.h"

/* 可启动的 APP 标识 */
typedef enum {
    LAUNCH_APP_FILES = 0,   /* SD 文件浏览器 */
    LAUNCH_APP_READER,      /* TXT 书架 */
    LAUNCH_APP_UART,        /* 串口数据显示终端 */
    LAUNCH_APP_NET,         /* 网络服务信息 */
    LAUNCH_APP_COUNT,
} launch_app_id_t;

/* 创建启动器（parent 通常为当前 screen；root 铺满 parent） */
lv_obj_t *launcher_create(lv_obj_t *parent);

/* 屏幕旋转/分辨率变化后重排几何（须在 LVGL 线程或持锁调用） */
void launcher_relayout(lv_obj_t *obj);

/* 切换主题：dark=true 黑夜（#0D0D0D 底白字），false 白天（#F5F5F5 底深灰字），
 * 背景 300ms 渐变，绿色元素不变 */
void launcher_set_theme(lv_obj_t *obj, bool dark);

/* 启动 APP（创建全屏界面覆盖启动器）。当前无 APP 运行时有效 */
void launcher_app_launch(launch_app_id_t id);

/* 关闭当前 APP（销毁界面，回到桌面）。无 APP 时无操作。
 * 签名兼容 bookshelf_back_cb_t（ctx 忽略） */
void launcher_app_close(void *ctx);

/* 当前是否有 APP 在运行 */
bool launcher_app_running(void);

/* SD 卡挂载就绪通知：转发给当前 APP（书架/浏览器重新扫描刷新；桌面无操作）。
 * 须在 LVGL 线程或持锁调用 */
void launcher_notify_sd_ready(void);

#endif /* LAUNCHER_H */
