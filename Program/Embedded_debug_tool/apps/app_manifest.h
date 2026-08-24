#ifndef APP_MANIFEST_H
#define APP_MANIFEST_H

/* app_manifest：统一 APP 描述符（launcher 通过回调表操作 APP，不关心内部实现）。
 *
 * 通用回调（所有 APP 必须实现，launcher 统一分发）：
 *   launch  — 启动 APP（parent=当前 screen，arg 为可选参数：
 *             NULL=无参启动；非 NULL=带参启动，如阅读器传 txt 路径直接打开）
 *   destroy — 销毁 APP（释放资源、删除对象树；回到桌面时 launcher 保证调用，
 *             外设/任务/GPIO 的释放由 APP 自行完成）
 *   back    — 返回事件（贴边右滑手势/系统返回）：true=请求关闭回来源，false=内部已处理
 *   gesture — 应用级手势事件（可选，NULL=不处理；全局右滑/左滑等非返回手势）。
 *             true=已处理；false=未处理（launcher 兜底）。见 launcher.h app_gesture_t
 *   rotate  — 旋转事件（deg=0/90/180/270，APP 自行重排/重建）
 *   refresh — SD 卡就绪事件（可为 NULL）
 *   debug_event — 调试事件（测试模块用，可为 NULL；evt 为自定义事件码）
 *
 * 调用约定：回调全部在 LVGL 线程（或持 esp_lv_adapter 锁）执行。
 */

#include "lvgl.h"
#include "launcher.h"

typedef struct app_manifest {
    launch_app_id_t id;
    const char *name;   /* 显示名（桌面卡片/日志） */
    void *(*launch)(lv_obj_t *parent, void (*back)(void *ctx), void *ctx);  /* 创建 APP */
    void (*destroy)(void *app);
    bool (*back)(void *app);
    bool (*gesture)(void *app, app_gesture_t evt);   /* 应用级手势（可选） */
    void (*rotate)(void *app, int deg);
    void (*refresh)(void *app);
    void (*debug_event)(void *app, int evt);
    void (*entered)(void *app);   /* 可选：进入动画完成时调用（APP 启动重业务时延迟到此处） */
    void (*drag_exit)(void *app); /* 可选：拖动返回滑出动画完成时调用（NULL=默认弹栈销毁）。
                                   * 全屏 APP 需"滑出后不销毁、自行收尾"（如书架模式阅读页
                                   * 关闭覆盖层回书架）时实现；内部调用 launcher_app_close 即默认销毁 */
} app_manifest_t;

/* 全部 APP 描述符表（索引 = launch_app_id_t） */
extern const app_manifest_t app_manifests[LAUNCH_APP_COUNT];

#endif /* APP_MANIFEST_H */
