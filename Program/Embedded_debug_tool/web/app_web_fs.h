#ifndef APP_WEB_FS_H
#define APP_WEB_FS_H

/* app_web_fs —— SD 文件管理 Web API（挂到现有 esp_http_server）。
 *
 * 路由（查询参数统一 p=（URL 编码），完整路径须在 /sdcard/ 下）：
 *   GET  /fs                   单页前端
 *   GET  /fs/list?p=<dir>      JSON 目录列表 [{n,d,s,m},...]
 *   GET  /fs/download?p=<file> 流式下载（chunked）
 *   POST /fs/upload?p=<file>   body 流式写入
 *   POST /fs/mkdir?p=<dir>     新建目录
 *   POST /fs/delete?p=<path>   删除文件或空目录
 *   POST /fs/rename?p=<old>&q=<new>  重命名/移动
 * 线程：httpd 任务内执行；所有 SD 访问持 esp_lv_adapter_lock（与 LCD 共享 SPI2）。
 */

#include "esp_http_server.h"
#include <stdint.h>

/* 传输状态（UI 轮询显示用；临界区保护，可跨任务读取） */
typedef struct {
    int busy;              /* 是否有传输进行中 */
    int upload;            /* 1=上传 0=下载 */
    char name[64];         /* 文件名（去路径） */
    uint32_t done;
    uint32_t total;        /* 0=未知 */
} app_web_fs_status_t;

/* httpd 句柄注入（app_web_http 启动后设置；开关路由用） */
extern httpd_handle_t g_web_fs_httpd;

/* 注册/注销全部 /fs* 路由（幂等；默认关闭，由 WebFS App 开关启用） */
esp_err_t app_web_fs_start(void);
void app_web_fs_stop(void);
bool app_web_fs_enabled(void);

/* 快照当前传输状态（任意任务可调） */
esp_err_t app_web_fs_get_status(app_web_fs_status_t *out);

#endif /* APP_WEB_FS_H */