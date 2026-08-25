#ifndef WEB_FS_APP_H
#define WEB_FS_APP_H

/* web_fs_app —— WebFS：SD 文件管理（浏览器访问 http://<esp_ip>/fs）。
 *
 * 互斥（与 MSD 读卡器二选一）：读卡器暴露时 /sdcard VFS 被卸载，SD 归原始
 * 块路径所有——本 APP 显示提示，其余情况显示状态与访问地址。
 */

#include "lvgl.h"

typedef struct web_fs_app web_fs_app_t;

web_fs_app_t *web_fs_app_create(lv_obj_t *parent, void (*back_cb)(void *ctx), void *ctx);
void web_fs_app_destroy(web_fs_app_t *app);
bool web_fs_app_swipe_back(web_fs_app_t *app);

#endif /* WEB_FS_APP_H */