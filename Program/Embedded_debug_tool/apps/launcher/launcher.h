#ifndef LAUNCHER_H
#define LAUNCHER_H

/* launcher：桌面启动器（卡片列表滚筒 + 调速拨轮），基于 LVGL 9.4。
 *
 * 结构：
 *   root（全屏背景，纯色，随主题动画渐变）
 *   ├── drum（滚筒容器：可滚动，卡片为刚性整体，跟手 + 惯性）
 *   │   └── cards[]（圆角边框卡片，图标 + 名称）
 *   └── wheel（右侧调速拨轮：上下轨道 + 矩形钮，拖动控制滚筒速度）
 *
 * APP 管理：中央描述符表（app_manifests）+ 返回栈（深度 4）：
 *   启动 = 压栈（来源 APP 保活，arg 带参启动）；返回 = 栈顶 back 回调，
 *   true 弹栈回来源，false APP 内部处理。系统事件（右滑返回/旋转/SD 就绪/调试）
 *   统一路由到栈顶 APP 回调。
 *
 * 线程：全部交互在 LVGL 线程；app/event 接口须在 LVGL 线程或持锁调用。
 */

#include "lvgl.h"

/* 可启动的 APP 标识 */
typedef enum {
    LAUNCH_APP_FILES = 0,   /* SD 文件浏览器 */
    LAUNCH_APP_READER,      /* TXT 书架/阅读器 */
    LAUNCH_APP_UART,        /* 串口数据显示终端 */
    LAUNCH_APP_NET,         /* 网络服务信息 */
    LAUNCH_APP_CARDREADER,  /* USB 读卡器（SD 卡 MSC） */
    LAUNCH_APP_DAPLINK,     /* DAP Link（CMSIS-DAP 烧录器/调试器） */
    LAUNCH_APP_COUNT,
} launch_app_id_t;

/* 创建启动器（parent 通常为当前 screen；root 铺满 parent） */
lv_obj_t *launcher_create(lv_obj_t *parent);

/* 屏幕旋转/分辨率变化后重排几何（须在 LVGL 线程或持锁调用） */
void launcher_relayout(lv_obj_t *obj);

/* 切换主题：dark=true 黑夜（#0D0D0D 底白字），false 白天（#F5F5F5 底深灰字） */
void launcher_set_theme(lv_obj_t *obj, bool dark);

/* ── APP 栈管理 ── */

/* 启动 APP：压栈（来源 APP 保活，状态保留）。arg=带参启动参数（可 NULL，
 * 阅读器传 txt 路径=直接打开）。 */
void launcher_app_launch(launch_app_id_t id, const char *arg);

/* 带参启动参数查询（launcher 保存，APP 在 launch 内调用获取）：
 * 阅读器：arg=NULL=书架模式；非 NULL=txt 路径直接打开 */
const char *launcher_app_get_arg(void);

/* 关闭栈顶 APP：弹栈回来源。签名兼容 back 回调（ctx 忽略） */
void launcher_app_close(void *ctx);

/* 当前是否有 APP 在运行（栈非空） */
bool launcher_app_running(void);

/* ── 系统事件路由（栈顶 APP 回调） ── */

/* 返回事件（输入层右滑触发） */
void launcher_app_swipe_back(void *ctx);

/* 旋转事件（平台旋转完成；栈顶无 rotate 回调则弹栈关闭回桌面） */
void launcher_event_rotate(int deg);

/* SD 挂载就绪事件（栈顶 refresh；桌面无操作） */
void launcher_event_sd_ready(void);

/* 调试事件（测试模块调用；栈顶 debug_event，无则忽略） */
void launcher_event_debug(int evt);

#endif /* LAUNCHER_H */
