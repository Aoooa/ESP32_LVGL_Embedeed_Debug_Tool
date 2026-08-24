/* dap_link.c —— DAP Link（CMSIS-DAP 烧录器/调试器）APP（LVGL 9）
 *
 * 界面：Cyberpunk HUD（Miku 青绿霓虹主题）——主界面只展示连接状态 +
 * 三选一模式下拉（关闭 / USB DAP / 无线 DAP），细节信息在 INFO 对话框。
 */

#include "dap_link.h"
#include "app_dap.h"
#include "app_display.h"
#include "esp_lv_adapter.h"
#include "app_font.h"
#include "tinyusb.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <stdio.h>

/* ── 配色：Miku 青绿霓虹（桌面主题 #39C5BB 的亮化主色） ── */
#define DL_BG        lv_color_hex(0x000000)   /* 深黑蓝底 */
#define DL_PANEL     lv_color_hex(0x000000)   /* 面板 */
#define DL_EDGE      lv_color_hex(0x000000)   /* 面板边框 */
#define DL_TEXT      lv_color_hex(0xE8F0F8)
#define DL_DIM       lv_color_hex(0x5A7085)
#define DL_MIKU      lv_color_hex(0x35E0C5)   /* 霓虹青绿（USB 激活） */
#define DL_MIKU_DIM  lv_color_hex(0x14524A)   /* 暗青绿（USB 未激活描边） */
#define DL_PINK      lv_color_hex(0xFF3E9E)   /* 霓虹粉（无线激活） */
#define DL_PINK_DIM  lv_color_hex(0x5C1F42)   /* 暗粉（无线未激活描边） */
#define DL_ERR       lv_color_hex(0xFF2A5C)

typedef void (*dap_link_back_cb_t)(void *ctx);

struct dap_link {
    lv_obj_t *root;
    lv_obj_t *lbl_mode; /* 模式文字（OFF/USB/USBIP） */
    int cur_mode;       /* 当前显示模式（-1=未初始化） */
    lv_obj_t *dd;       /* 模式下拉（关闭/USB/无线） */
    lv_timer_t *timer;
    bool pc_attached;   /* USB 物理连接（tud_connected，LVGL 线程轮询） */
    bool pc_mounted;    /* USB 枚举完成（tud_mounted） */
};

/* 无线开启任务句柄（WiFi init 需内部 RAM 栈；非 NULL = 启动中） */
static TaskHandle_t s_wifi_task;

static void dl_wifi_enable_task(void *arg);

/* APP 句柄（manifest launch 返回值） */
typedef struct dap_link dap_link_t;

static lv_font_t *dl_font(void)
{
    lv_font_t *f = app_font_get(16);
    return f ? f : &lv_font_montserrat_14;
}

/* ── 状态 → 界面 ── */

static void dl_update(struct dap_link *dl)
{
    dap_state_t st = app_dap_get_state();
    dap_wifi_state_t wst = app_dap_wifi_get_state();

    /* ── 模式指示器（控件组切换 + 霓虹着色） ── */
    int mode;
    if (st == DAP_STATE_READY) {
        mode = 1;      /* USB DAP */
    } else if (wst == DAP_WIFI_ON || s_wifi_task) {
        mode = 2;      /* 无线 USBIP */
    } else {
        mode = 0;      /* 关闭 */
    }
    /* 模式变化时更新文字 + 霓虹着色 */
    if (dl->cur_mode != mode) {
        dl->cur_mode = mode;
        if (dl->lbl_mode) {
            lv_color_t mc = mode == 1 ? lv_color_hex(0x35E0C5) :
                            mode == 2 ? lv_color_hex(0xFF3E9E) :
                            lv_color_hex(0x8A9BB0);
            const char *txt = mode == 1 ? "USB" : mode == 2 ? "USBIP" : "OFF";
            lv_label_set_text(dl->lbl_mode, txt);
            lv_obj_set_style_text_color(dl->lbl_mode, mc, 0);
        }
    }

    /* ── 模式下拉同步（无线启动中视为无线模式） ── */
    int sel;
    if (st == DAP_STATE_READY) {
        sel = 1;
    } else if (wst == DAP_WIFI_ON || s_wifi_task) {
        sel = 2;
    } else {
        sel = 0;
    }
    if (lv_dropdown_get_selected(dl->dd) != sel) {
        lv_dropdown_set_selected(dl->dd, sel);
    }
}

/* ── 模式指示器绘制（lv_canvas 实时绘制，零 flash 占用） ──
 * 赛博朋克霓虹风：外圈霓虹环 + 四角 HUD 角标 + 图标发光层（粗透明描边
 * 模拟辉光，LVGL 无 text-shadow，用双层绘制） */

static void dl_timer_cb(lv_timer_t *t)
{
    struct dap_link *dl = lv_timer_get_user_data(t);
    if (!dl) return;

    /* DAP 开启时轮询 USB 栈状态（LVGL 线程，栈安全；tud_* 只读全局状态） */
    if (app_dap_get_state() == DAP_STATE_READY) {
        bool c = tud_connected();
        bool m = tud_mounted();
        if (c != dl->pc_attached || m != dl->pc_mounted) {
            dl->pc_attached = c;
            dl->pc_mounted = m;
        }
    } else {
        dl->pc_attached = false;
        dl->pc_mounted = false;
    }
    dl_update(dl);
}

/* ── 模式下拉回调（三选一：关闭 / USB / 无线） ── */

static void dl_dd_cb(lv_event_t *e)
{
    struct dap_link *dl = lv_event_get_user_data(e);
    if (!dl) return;

    int sel = lv_dropdown_get_selected(dl->dd);
    switch (sel) {
    case 0:   /* 关闭 */
        if (app_dap_get_state() == DAP_STATE_READY) {
            app_dap_disable();
        }
        if (app_dap_wifi_get_state() == DAP_WIFI_ON) {
            app_dap_wifi_disable();
            esp_lv_adapter_lock(-1);
            app_display_restore_buffers();
            esp_lv_adapter_unlock();
        }
        break;
    case 1:   /* USB DAP（app_dap_enable 内部自动关闭无线） */
        app_dap_enable();
        break;
    case 2:   /* 无线 DAP：WiFi init 需内部 RAM 栈，独立任务执行 */
        if (s_wifi_task) break;   /* 已在启动中 */
        xTaskCreate(dl_wifi_enable_task, "dap_wifi", 4096, NULL, 5, &s_wifi_task);
        break;
    default:
        break;
    }
    dl_update(dl);
}

static void dl_wifi_enable_task(void *arg)
{
    (void)arg;
    /* WiFi 启动需要较多内部 RAM（实测仅剩 ~18KB，WiFi 需 ~40KB）：
     * 无线期间临时收缩显示缓冲（双→单，腾 ~15KB），关闭无线时
     * dl_dd_cb 自动恢复双缓冲——仅无线烧录期间生效，不影响日常显示 */
    esp_lv_adapter_lock(-1);
    app_display_shrink_buffers();
    esp_lv_adapter_unlock();
    app_dap_wifi_enable();   /* 内部自动关闭 USB DAP */
    s_wifi_task = NULL;
    vTaskDelete(NULL);
}

/* ── INFO 对话框（只显示当前设备信息） ── */

static void dl_info_close_cb(lv_event_t *e)
{
    lv_obj_t *mask = lv_event_get_user_data(e);
    lv_obj_delete(mask);
}

static void dl_info_item(lv_obj_t *parent, const char *key, const char *val)
{
    lv_obj_t *k = lv_label_create(parent);
    lv_label_set_text(k, key);
    lv_obj_set_width(k, lv_pct(100));
    lv_obj_set_style_text_color(k, DL_MIKU, 0);
    lv_obj_set_style_text_font(k, dl_font(), 0);

    lv_obj_t *v = lv_label_create(parent);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_label_set_text(v, val);
    lv_obj_set_width(v, lv_pct(100));
    lv_obj_set_style_text_color(v, DL_TEXT, 0);
    lv_obj_set_style_text_font(v, dl_font(), 0);
}

static void dl_info_sep(lv_obj_t *parent)
{
    lv_obj_t *sep = lv_obj_create(parent);
    lv_obj_set_size(sep, lv_pct(100), 1);
    lv_obj_set_style_bg_color(sep, DL_EDGE, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
}

static void dl_info_cb(lv_event_t *e)
{
    (void)e;
    /* 序列号 = 芯片 MAC（不依赖 DAP 开启状态） */
    char sn[13];
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(sn, sizeof(sn), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* 全屏遮罩：半透明黑，点击关闭。
     * 注意挂在 screen（而非 lv_layer_top）：下拉列表弹层由 LVGL 固定在
     * screen 顶层（move_to_index -1），若 mask 在 lv_layer_top 会盖住列表 */
    lv_obj_t *mask = lv_obj_create(lv_screen_active());
    lv_obj_set_size(mask, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_50, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_set_style_radius(mask, 0, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(mask, dl_info_close_cb, LV_EVENT_CLICKED, mask);

    /* 对话框：宽度固定、高度自适应内容（内容少无空白），居中 */
    lv_obj_t *box = lv_obj_create(mask);
    lv_obj_set_size(box, 210, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(box, DL_PANEL, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, DL_MIKU, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_style_pad_gap(box, 6, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    /* 不加 EVENT_BUBBLE：box 内点击（含下拉）不冒泡到 mask，不会误关 */

    /* 标题 */
    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "DAP LINK · INFO");
    lv_obj_set_style_text_color(title, DL_MIKU, 0);
    lv_obj_set_style_text_font(title, dl_font(), 0);

    /* 内容小节（分割线分隔） */
    dl_info_item(box, "SWD 接口", "SWD1: SWDIO=11 SWCLK=12\nnRESET=13 · GND 共地");
    dl_info_sep(box);
    dl_info_item(box, "无线", "IP: 192.168.4.1\nUSBIP 端口: 872");
    dl_info_sep(box);
    dl_info_item(box, "USB 序列号", sn);

    /* 关闭按钮（宽度 100%，文字居中） */
    lv_obj_t *btn = lv_button_create(box);
    lv_obj_set_size(btn, lv_pct(100), 30);
    lv_obj_set_style_bg_color(btn, DL_PANEL, 0);
    lv_obj_set_style_border_color(btn, DL_PINK, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 4, 0);
    lv_obj_set_style_text_color(btn, DL_PINK, 0);
    lv_obj_set_style_text_font(btn, dl_font(), 0);
    lv_obj_add_event_cb(btn, dl_info_close_cb, LV_EVENT_CLICKED, mask);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "关闭");
    lv_obj_center(bl);
}

/* ── 控件构建 ── */

/* 下拉列表弹层是独立对象（LVGL 固定挂 screen），需在 READY 事件（弹层
 * 创建钩子）时设置列表项字体，否则默认字体不支持中文会乱码 */
static void dl_dd_ready_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_current_target(e);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_text_font(list, dl_font(), LV_PART_SELECTED);
    lv_obj_set_style_text_color(list, DL_TEXT, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(list, DL_PANEL, LV_PART_MAIN);
    lv_obj_set_style_border_color(list, DL_EDGE, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 1, LV_PART_MAIN);
    /* 非选中行由 list 的 label 对象渲染（draw_box_label 只覆盖选中/按下行）：
     * label 默认字体不支持中文、颜色深不可见——必须显式设置 */
    lv_obj_t *lbl = lv_obj_get_child(list, 0);
    lv_obj_set_style_text_font(lbl, dl_font(), 0);
    lv_obj_set_style_text_color(lbl, DL_TEXT, 0);
}

/* 模式下拉（三选一） */
static lv_obj_t *dl_dropdown(lv_obj_t *parent, struct dap_link *dl)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_obj_set_size(dd, 110, 34);
    lv_dropdown_set_options(dd, "关闭\nUSB DAP\n无线 DAP");
    lv_dropdown_set_selected(dd, 0);
    lv_dropdown_set_symbol(dd, LV_SYMBOL_UP);   /* 箭头朝上 */   /* 默认箭头（FontAwesome） */
    /* 不设箭头符号：项目字体不含 ▼/FontAwesome 符号，显示乱码；
     * 以霓虹边框+文本本身提示可展开 */
    lv_dropdown_set_dir(dd, LV_DIR_TOP);   /* 展开向上 */   /* 显式向下展开 */

    /* 主按钮区样式 */
    lv_obj_set_style_bg_color(dd, DL_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dd, DL_MIKU, LV_PART_MAIN);
    lv_obj_set_style_border_width(dd, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(dd, 6, LV_PART_MAIN);
    lv_obj_set_style_text_color(dd, DL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(dd, dl_font(), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(dd, 10, LV_PART_MAIN);
    /* 指示箭头：字体必须用含 FontAwesome 符号字形的内置字体（项目字体
     * 不含 U+F0D7 会乱码），颜色用霓虹青绿 */
    lv_obj_set_style_text_color(dd, DL_MIKU, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_14, LV_PART_INDICATOR);
    /* 弹出列表 */
    lv_obj_set_style_bg_color(dd, DL_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_border_color(dd, DL_EDGE, LV_PART_ITEMS);
    lv_obj_set_style_border_width(dd, 1, LV_PART_ITEMS);
    lv_obj_set_style_text_color(dd, DL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(dd, dl_font(), LV_PART_ITEMS);
    /* 选中项 + 列表项字体（draw_box_label 从 LV_PART_SELECTED 取字体，
     * 默认字体不支持中文会乱码） */
    lv_obj_set_style_bg_color(dd, DL_MIKU_DIM, LV_PART_SELECTED);
    lv_obj_set_style_text_color(dd, DL_MIKU, LV_PART_SELECTED);
    lv_obj_set_style_text_font(dd, dl_font(), LV_PART_SELECTED);

    lv_obj_add_event_cb(dd, dl_dd_cb, LV_EVENT_VALUE_CHANGED, dl);
    lv_obj_add_event_cb(dd, dl_dd_ready_cb, LV_EVENT_READY, NULL);
    return dd;
}

dap_link_t *dap_link_create(lv_obj_t *parent, dap_link_back_cb_t back_cb, void *ctx)
{
    (void)back_cb;
    (void)ctx;
    dap_link_t *dl = lv_malloc(sizeof(dap_link_t));
    if (!dl) return NULL;
    lv_memzero(dl, sizeof(*dl));

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, DL_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    dl->root = root;

    /* ── 标题栏 ── */
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "DAP LINK");
    lv_obj_set_pos(title, 10, 8);
    lv_obj_set_style_text_color(title, DL_MIKU, 0);
    lv_obj_set_style_text_font(title, dl_font(), 0);

    lv_obj_t *info_btn = lv_button_create(root);
    lv_obj_set_size(info_btn, 52, 24);
    lv_obj_align(info_btn, LV_ALIGN_TOP_RIGHT, -10, 6);
    lv_obj_set_style_bg_color(info_btn, DL_PANEL, 0);
    lv_obj_set_style_border_color(info_btn, DL_PINK, 0);
    lv_obj_set_style_border_width(info_btn, 1, 0);
    lv_obj_set_style_radius(info_btn, 4, 0);
    lv_obj_set_style_text_color(info_btn, DL_PINK, 0);
    lv_obj_set_style_text_font(info_btn, dl_font(), 0);
    lv_obj_add_event_cb(info_btn, dl_info_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *info_lbl = lv_label_create(info_btn);
    lv_label_set_text(info_lbl, "INFO");
    lv_obj_center(info_lbl);

    /* ── 中部：状态卡片 + 模式下拉（flex column，子项水平居中） ── */
    int sh = lv_display_get_vertical_resolution(lv_display_get_default());
    int top = 40;
    lv_obj_t *mid = lv_obj_create(root);
    lv_obj_set_pos(mid, 10, top);
    lv_obj_set_size(mid, lv_pct(100) - 20, sh - top - 8);
    lv_obj_set_style_bg_opa(mid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mid, 0, 0);
    lv_obj_set_style_pad_all(mid, 0, 0);
    lv_obj_set_style_pad_gap(mid, 10, 0);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(mid, LV_OBJ_FLAG_SCROLLABLE);

    /* 模式指示器（120x120 霓虹位图，居中） */
    /* 模式文字：OFF/USB/USBIP + 光晕副本（偏移半透明模拟霓虹发光） */
    lv_obj_t *glow = lv_label_create(root);
    lv_label_set_text(glow, "OFF");
    lv_obj_align(glow, LV_ALIGN_CENTER, 1, 1);
    lv_obj_set_style_text_font(glow, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(glow, lv_color_hex(0x8A9BB0), 0);
    lv_obj_set_style_text_opa(glow, LV_OPA_50, 0);

    dl->lbl_mode = lv_label_create(root);
    lv_label_set_text(dl->lbl_mode, "OFF");
    lv_obj_align(dl->lbl_mode, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(dl->lbl_mode, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(dl->lbl_mode, lv_color_hex(0x8A9BB0), 0);

    lv_obj_t *mode_row = lv_obj_create(root);
    lv_obj_set_size(mode_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(mode_row, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(mode_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mode_row, 0, 0);
    lv_obj_set_style_radius(mode_row, 0, 0);
    lv_obj_set_style_pad_all(mode_row, 0, 0);
    lv_obj_set_style_pad_column(mode_row, 10, 0);
    lv_obj_set_flex_flow(mode_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mode_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(mode_row, LV_OBJ_FLAG_SCROLLABLE);

    /* "模式" 标签 */
    lv_obj_t *mode_lbl = lv_label_create(mode_row);
    lv_label_set_text(mode_lbl, "模式");
    lv_obj_set_style_text_color(mode_lbl, DL_DIM, 0);
    lv_obj_set_style_text_font(mode_lbl, dl_font(), 0);

    /* 模式下拉（收起时显示选项文本，LV_PART_MAIN 已用项目字体） */
    dl->dd = dl_dropdown(mode_row, dl);

    dl_update(dl);

    /* 状态轮询：服务事件来自 TinyUSB 任务，经 lv_timer 回到 LVGL 线程刷新 */
    dl->cur_mode = -1;
    dl->timer = lv_timer_create(dl_timer_cb, 250, dl);

    /* 首次渲染前切换图标组（隐藏未选中模式，避免叠加） */
    dl_update(dl);

    return dl;
}

void dap_link_destroy(dap_link_t *dl)
{
    if (!dl) return;
    if (app_dap_get_state() == DAP_STATE_READY) {
        ESP_LOGI("dap_link", "exit -> disable DAP");
        app_dap_disable();
    }
    if (app_dap_wifi_get_state() == DAP_WIFI_ON) {
        ESP_LOGI("dap_link", "exit -> disable wireless DAP");
        app_dap_wifi_disable();
        /* 恢复无线开启时收缩的显示缓冲（与关闭路径一致） */
        esp_lv_adapter_lock(-1);
        app_display_restore_buffers();
        esp_lv_adapter_unlock();
    }
    if (dl->timer) lv_timer_delete(dl->timer);
    lv_obj_add_flag(dl->root, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);
    lv_obj_delete(dl->root);
    lv_free(dl);
}

bool dap_link_swipe_back(dap_link_t *dl)
{
    (void)dl;
    return true;
}
