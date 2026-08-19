#ifndef APP_CARDREADER_H
#define APP_CARDREADER_H

/* app_cardreader：USB 读卡器服务（SD 卡经 TinyUSB MSC 暴露给 PC）。
 *
 * 原理（全部使用 ESP 官方组件 esp_tinyusb）：
 *   开启 = 卸载 /sdcard 的 VFS 挂载 → 原始卡初始化（sdspi + sdmmc_card_init）→
 *          tinyusb_msc_new_storage_sdmmc(MOUNT_USB) → tinyusb_driver_install。
 *          PC 通过 SCSI 直接读写 SD 扇区（MOUNT_USB 下不挂 FatFs，绝不格式化）。
 *   关闭 = tinyusb_msc_delete_storage（等待挂起写落盘）→ tinyusb_driver_uninstall
 *          （PC 看到设备安全移除）→ 原始卡释放 → drv_sdcard_init 恢复 /sdcard。
 *   自动关闭：USB 拔出（TINYUSB_EVENT_DETACHED）或 PC 安全弹出后延迟执行。
 *
 * 独占性：同一时刻 /sdcard 只属于一个持有者（esp_vfs_fat 或 MSC 组件），
 * 防止 FatFS 双访问损坏；do_not_format 保证绝不自动格式化。
 *
 * 线程约定：
 *   enable/disable 须在 LVGL 线程或持 esp_lv_adapter 锁调用（内部做 SD 卡
 *   重挂载/重识别，属 SPI 慢操作）。storage/TinyUSB 事件回调运行在 TinyUSB
 *   任务，只更新状态并调度自动关闭，不直接操作驱动。
 */

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    CARDREADER_IDLE = 0,   /* 未启用：/sdcard 归 drv_sdcard（esp_vfs_fat） */
    CARDREADER_EXPOSED,    /* 已暴露：PC 可访问 SD 卡（TinyUSB MSC） */
    CARDREADER_APP_OWNED,  /* 存储归 APP：PC 已弹出/拔出，/sdcard 由组件挂载 */
    CARDREADER_ERROR,      /* 上次操作失败（如 SD 缺失） */
} cardreader_state_t;

/* 状态名（UI/日志用） */
const char *app_cardreader_state_str(cardreader_state_t st);

cardreader_state_t app_cardreader_get_state(void);

/* SD 卡是否物理就绪（可被本服务或文件浏览器访问） */
bool app_cardreader_sd_ready(void);

/* 开启读卡器。SD 未挂载返回 ESP_ERR_INVALID_STATE（并置 ERROR）。 */
esp_err_t app_cardreader_enable(void);

/* 关闭读卡器（任意状态幂等），恢复 /sdcard 给文件浏览器/阅读器 */
esp_err_t app_cardreader_disable(void);

#endif /* APP_CARDREADER_H */
