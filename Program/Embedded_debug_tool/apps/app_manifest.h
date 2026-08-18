#ifndef APP_MANIFEST_H
#define APP_MANIFEST_H

/* app_manifest：统一 APP 描述符（launcher 通过回调表操作 APP，不关心内部实现）。
 *
 * 通用回调（所有 APP 必须实现，launcher 统一分发）：
 *   launch  — 启动 APP（parent=当前 screen，arg 为可选参数：
 *             NULL=无参启动；非 NULL=带参启动，如阅读器传 txt 路径直接打开）
 *   destroy — 销毁 APP（释放资源、删除对象树）
 *   back    — 返回事件（右滑手势/系统返回）：true=请求关闭回来源，false=内部已处理
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
    void (*rotate)(void *app, int deg);
    void (*refresh)(void *app);
    void (*debug_event)(void *app, int evt);
} app_manifest_t;

/* 全部 APP 描述符表（索引 = launch_app_id_t） */
extern const app_manifest_t app_manifests[LAUNCH_APP_COUNT];

#endif /* APP_MANIFEST_H */
