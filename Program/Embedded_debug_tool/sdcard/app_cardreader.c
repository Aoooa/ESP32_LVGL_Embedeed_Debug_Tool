/* app_cardreader.c —— USB 读卡器服务（SD 卡经 TinyUSB MSC 暴露给 PC）
 *
 * 状态机：
 *   IDLE       /sdcard 由 drv_sdcard（esp_vfs_fat_sdspi_mount）挂载
 *   EXPOSED    VFS 已卸载，SD 原始扇区经 TinyUSB MSC 暴露给 PC（官方组件 MOUNT_USB）
 *   APP_OWNED  PC 已安全弹出或 USB 拔出，SD 由组件挂载回 /sdcard（MOUNT_APP）
 *   ERROR      SD 缺失等失败态
 *
 * 线程：
 *   - enable/disable 运行在调用线程（LVGL 线程或自动关闭任务），SD 卡操作
 *     持 esp_lv_adapter 锁（与 LCD 渲染共享 SPI2 总线）
 *   - tinyusb/storage 事件回调运行在 TinyUSB 任务：只更新状态 + 调度自动关闭
 *
 * FatFS 安全：同一时刻仅一个持有者；do_not_format 禁止格式化；删除存储前
 * 组件等待挂起写（deferred write）落盘；卸载驱动让 PC 看到安全移除。
 */

#include "app_cardreader.h"
#include "drv_sdcard.h"
#include "app_dap.h"
#include "app_usb_uart.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "hal/usb_serial_jtag_ll.h"
#endif
#include <stdlib.h>

static const char *TAG = "app_cardreader";

/* PC 弹出/USB 拔出后自动关闭的延迟（ms）。给 Windows 完成内部收尾留余量 */
#define CARDREADER_AUTO_DISABLE_MS   3000
/* 删除存储时等待挂起写入落盘的最大重试次数 */
#define CARDREADER_DELETE_RETRY      20
#define CARDREADER_DELETE_RETRY_MS   50

typedef struct {
    cardreader_state_t state;
    sdmmc_card_t *card;
    tinyusb_msc_storage_handle_t storage;
    TaskHandle_t auto_task;    /* 自动关闭任务（懒创建） */
} cardreader_t;

static cardreader_t s_cr;

/* ── SD 与 LCD 共享 SPI2 总线：并发互斥 ──
 * 官方 esp_lcd_panel_io_spi（异步队列）+ sdspi（acquire+轮询）并发会触发
 * spi_hal_setup_trans 断言崩溃。因此包装 card->host.do_transaction，使 MSC 的
 * 每个 SD 事务（tusb 任务）都持 esp_lv_adapter_lock，与 LVGL 渲染（持锁渲染）
 * 互斥——与本项目"SD 操作须与渲染互斥"的既有约定一致。 */
static esp_err_t (*s_orig_do_transaction)(int, sdmmc_command_t *);

static esp_err_t cardreader_do_transaction_locked(int slot, sdmmc_command_t *cmdinfo)
{
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        ret = s_orig_do_transaction(slot, cmdinfo);
        esp_lv_adapter_unlock();
    }
    return ret;
}

static void cardreader_arm_sd_lock(sdmmc_card_t *card)
{
    if (!card || !card->host.do_transaction) return;
    s_orig_do_transaction = card->host.do_transaction;
    card->host.do_transaction = cardreader_do_transaction_locked;
}

/* 状态为 32 位对齐枚举，单字读写原子，无需加锁 */
static void cardreader_set_state(cardreader_state_t st)
{
    s_cr.state = st;
}

cardreader_state_t app_cardreader_get_state(void)
{
    return s_cr.state;
}

const char *app_cardreader_state_str(cardreader_state_t st)
{
    switch (st) {
    case CARDREADER_IDLE:      return "idle";
    case CARDREADER_EXPOSED:   return "exposed";
    case CARDREADER_APP_OWNED: return "app-owned";
    case CARDREADER_ERROR:     return "error";
    default:                   return "?";
    }
}

bool app_cardreader_sd_ready(void)
{
    return drv_sdcard_is_mounted() || s_cr.card != NULL;
}

/* 读卡器暴露期间控制台（USB-Serial/JTAG）被 TinyUSB 接管，输出被丢弃。
 * 收敛相关组件日志避免任何串行写路径延迟（50ms 超时丢弃机制） */
static void cardreader_quiet_component_logs(bool quiet)
{
    esp_log_level_t lv = quiet ? ESP_LOG_NONE : ESP_LOG_INFO;
    esp_log_level_set("TinyUSB", lv);
    esp_log_level_set("tinyusb_msc_storage", lv);
}

/* ── 自动关闭（USB 拔出 / PC 弹出） ── */

static void cardreader_auto_disable_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(CARDREADER_AUTO_DISABLE_MS));

    cardreader_state_t st = app_cardreader_get_state();
    if (st == CARDREADER_EXPOSED || st == CARDREADER_APP_OWNED) {
        /* PC 弹出后若又自动重连（设备仍枚举、存储交还 USB），取消自动关闭 */
        tinyusb_msc_mount_point_t mp;
        if (s_cr.storage &&
            tinyusb_msc_get_storage_mount_point(s_cr.storage, &mp) == ESP_OK &&
            mp == TINYUSB_MSC_STORAGE_MOUNT_USB) {
            ESP_LOGI(TAG, "auto disable cancelled (PC reconnected)");
            cardreader_set_state(CARDREADER_EXPOSED);
            s_cr.auto_task = NULL;
            vTaskDelete(NULL);
            return;
        }

        ESP_LOGI(TAG, "auto disable (USB disconnected/ejected)");
        if (esp_lv_adapter_lock(-1) == ESP_OK) {
            app_cardreader_disable();
            esp_lv_adapter_unlock();
        }
    }
    ESP_LOGI(TAG, "auto disable done, stack watermark %u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_cr.auto_task = NULL;
    vTaskDelete(NULL);
}

static void cardreader_schedule_auto_disable(void)
{
    if (s_cr.auto_task) return;   /* 已有在途 */
    /* 关闭路径会做完整 SD 卡挂载（FatFs），栈按主任务量级给足并放 PSRAM */
    xTaskCreateWithCaps(cardreader_auto_disable_task, "cr_auto", 8192,
                        NULL, 4, &s_cr.auto_task, MALLOC_CAP_SPIRAM);
}

/* TinyUSB 设备事件（TinyUSB 任务上下文）：USB 拔出 → 交还 APP 并自动关闭 */
static void tusb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id != TINYUSB_EVENT_DETACHED) return;
    ESP_LOGI(TAG, "USB detached");
    if (app_cardreader_get_state() == CARDREADER_EXPOSED) {
        cardreader_set_state(CARDREADER_APP_OWNED);
        cardreader_schedule_auto_disable();
    }
}

/* 存储归属变更事件（TinyUSB 任务上下文）：PC 安全弹出 → 自动关闭 */
static void storage_mount_changed_cb(tinyusb_msc_storage_handle_t handle,
                                     tinyusb_msc_event_t *event, void *arg)
{
    (void)handle;
    (void)arg;
    switch (event->id) {
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
        if (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP &&
            app_cardreader_get_state() == CARDREADER_EXPOSED) {
            ESP_LOGI(TAG, "storage handed to app (PC ejected)");
            cardreader_set_state(CARDREADER_APP_OWNED);
            cardreader_schedule_auto_disable();
        }
        break;
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
        ESP_LOGE(TAG, "storage event: %d", (int)event->id);
        break;
    default:
        break;
    }
}

/* ── 开启 / 关闭 ── */

esp_err_t app_cardreader_enable(void)
{
    cardreader_state_t st = app_cardreader_get_state();
    if (st == CARDREADER_EXPOSED) return ESP_OK;
    /* APP_OWNED 为瞬态（PC 弹出后 3 秒自动关闭），无调用方会在此状态进来 */

    /* USB PHY 互斥：DAP / USB-UART 占用时拒绝（三者共用内部 USB PHY） */
    if (app_dap_get_state() == DAP_STATE_READY) {
        ESP_LOGE(TAG, "DAP is using USB, disable it first");
        cardreader_set_state(CARDREADER_ERROR);
        return ESP_ERR_INVALID_STATE;
    }
    if (app_usb_uart_get_state() == USB_UART_ON) {
        ESP_LOGE(TAG, "USB-UART is using USB, disable it first");
        cardreader_set_state(CARDREADER_ERROR);
        return ESP_ERR_INVALID_STATE;
    }

    if (!drv_sdcard_is_mounted()) {
        ESP_LOGE(TAG, "SD not mounted, can't enable card reader");
        cardreader_set_state(CARDREADER_ERROR);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    /* 1+2. 归还 /sdcard → 原始卡初始化。SPI 慢操作，整段持 LVGL 锁
     * 防 LCD 渲染争抢总线（递归锁，LVGL 线程内调用同样安全） */
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        ESP_LOGE(TAG, "lv adapter lock failed");
        cardreader_set_state(CARDREADER_ERROR);
        return ESP_ERR_INVALID_STATE;
    }
    drv_sdcard_deinit();
    ret = drv_sdcard_card_init(&s_cr.card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "raw card init failed: %s", esp_err_to_name(ret));
        drv_sdcard_init();
        esp_lv_adapter_unlock();
        cardreader_set_state(CARDREADER_ERROR);
        return ret;
    }
    esp_lv_adapter_unlock();
    cardreader_arm_sd_lock(s_cr.card);   /* MSC 的 SD 事务持渲染锁，防 SPI2 并发 */

    /* 3. 创建 MSC 存储：MOUNT_USB 直接暴露原始扇区，不挂 FatFs、不格式化 */
    tinyusb_msc_storage_config_t cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs = {
            .base_path = (char *)DRV_SDCARD_MOUNT_POINT,
            .config = {
                .format_if_mount_failed = false,
                .max_files = 8,
            },
            .do_not_format = true,   /* 绝不自动格式化 */
        },
        .medium.card = s_cr.card,
    };
    ret = tinyusb_msc_new_storage_sdmmc(&cfg, &s_cr.storage);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "msc storage create failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    /* 4. 存储归属事件回调 */
    ret = tinyusb_msc_set_storage_callback(storage_mount_changed_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "storage callback set failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    /* 5. 安装 TinyUSB 驱动：接管 USB PHY → USJ 控制台静默，PC 枚举为 MSC */
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(tusb_event_cb, NULL);
    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb driver install failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    cardreader_set_state(CARDREADER_EXPOSED);
    cardreader_quiet_component_logs(true);
    ESP_LOGI(TAG, "card reader enabled: PC should see an SD drive");
    return ESP_OK;

fail:
    if (s_cr.storage) {
        tinyusb_msc_delete_storage(s_cr.storage);
        s_cr.storage = NULL;
    }
    if (s_cr.card) {
        drv_sdcard_card_deinit(s_cr.card);
        s_cr.card = NULL;
    }
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (drv_sdcard_init() != ESP_OK) {
            ESP_LOGE(TAG, "failed to restore /sdcard mount");
        }
        esp_lv_adapter_unlock();
    }
    cardreader_set_state(CARDREADER_ERROR);
    return ret;
}

esp_err_t app_cardreader_disable(void)
{
    cardreader_state_t st = app_cardreader_get_state();
    /* 不变量：IDLE/ERROR 下 storage/card 已清、/sdcard 已由 drv_sdcard 挂载 */
    if (st == CARDREADER_IDLE || st == CARDREADER_ERROR) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "disabling card reader (%s)", app_cardreader_state_str(st));
    esp_err_t ret = ESP_OK;

    /* 1. 存储删除：组件会先卸载 FatFs（f_sync）并等待挂起写落盘 */
    if (s_cr.storage) {
        for (int i = 0; i < CARDREADER_DELETE_RETRY; i++) {
            ret = tinyusb_msc_delete_storage(s_cr.storage);
            if (ret != ESP_ERR_INVALID_STATE) break;   /* 仅"写未落盘"重试 */
            vTaskDelay(pdMS_TO_TICKS(CARDREADER_DELETE_RETRY_MS));
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "delete storage failed: %s", esp_err_to_name(ret));
        }
        s_cr.storage = NULL;
    }

    /* 2. 卸载 TinyUSB 驱动：USB 设备消失，PC 视为安全移除 */
    esp_err_t ud = tinyusb_driver_uninstall();
    if (ud != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb uninstall failed: %s", esp_err_to_name(ud));
        if (ret == ESP_OK) ret = ud;
    }

    /* 3. 把内部 USB PHY 路由交还 USB-Serial/JTAG（恢复 COM 控制台枚举）。
     * usb_del_phy 只停 OTG 时钟，不留回 USJ；此处复用 USJ 驱动自身的 LL 初始化
     * （usb_serial_jtag_ll_phy_enable_external(false)）将内部 PHY 映射回 USJ。 */
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    usb_serial_jtag_ll_phy_enable_external(false);
#endif

    /* 3. 释放原始卡，恢复 /sdcard 给文件浏览器/阅读器 */
    if (s_cr.card) {
        drv_sdcard_card_deinit(s_cr.card);
        s_cr.card = NULL;
    }
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (drv_sdcard_init() != ESP_OK) {
            ESP_LOGE(TAG, "restore /sdcard mount failed");
        }
        esp_lv_adapter_unlock();
    }

    cardreader_quiet_component_logs(false);
    cardreader_set_state(CARDREADER_IDLE);
    ESP_LOGI(TAG, "card reader disabled, /sdcard restored");
    return ret;
}
