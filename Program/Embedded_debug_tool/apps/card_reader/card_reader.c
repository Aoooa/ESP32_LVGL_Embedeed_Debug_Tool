/* card_reader.c —— USB SD 卡存储 APP（LVGL 9） */

#include "card_reader.h"
#include "app_cardreader.h"
#include "app_font.h"
#include "esp_log.h"

/* 配色（深色，与 net_console 一致） */
#define CR_BG          lv_color_hex(0x111827)
#define CR_CARD        lv_color_hex(0x1F2937)
#define CR_TEXT        lv_color_hex(0xFFFFFF)
#define CR_DIM         lv_color_hex(0x9CA3AF)
#define CR_ACCENT      lv_color_hex(0x39C5BB)
#define CR_ERR         lv_color_hex(0xEF4444)

struct card_reader {
    lv_obj_t *root;
    card_reader_back_cb_t back_cb;
    void *back_ctx;
    lv_obj_t *val_sd;
    lv_obj_t *val_status;
    lv_obj_t *val_hint;
    lv_obj_t *btn_toggle;
    lv_obj_t *lbl_toggle;
    lv_timer_t *timer;
};

static lv_font_t *cr_font(void)
{
    lv_font_t *f = app_font_get(16);
    return f ? f : &lv_font_montserrat_14;
}

/* ── 状态 → 界面 ── */

static void cr_update(card_reader_t *cr)
{
    bool sd = app_cardreader_sd_ready();
    cardreader_state_t st = app_cardreader_get_state();

    lv_label_set_text(cr->val_sd, sd ? "已挂载" : "未就绪");
    lv_obj_set_style_text_color(cr->val_sd, sd ? CR_ACCENT : CR_ERR, 0);

    static const char *st_txt[] = {
        [CARDREADER_IDLE] = "未启用",
        [CARDREADER_EXPOSED] = "运行中 · USB 已连接",
        [CARDREADER_APP_OWNED] = "已弹出 · 自动关闭中",
        [CARDREADER_ERROR] = "异常",
    };
    lv_label_set_text(cr->val_status, st_txt[st]);

    const char *hint;
    switch (st) {
    case CARDREADER_EXPOSED:
        hint = "电脑已显示 SD 卡磁盘，可读写。\n请先在电脑安全弹出，再停止本功能。";
        break;
    case CARDREADER_APP_OWNED:
        hint = "电脑已安全弹出，SD 卡即将\n交还本机使用，请稍候。";
        break;
    case CARDREADER_ERROR:
        hint = "启动失败，请检查 SD 卡\n与 USB 连接后重试。";
        break;
    case CARDREADER_IDLE:
    default:
        hint = sd ? "启动后电脑将显示 SD 卡磁盘，\n可自由读写。停止后自动恢复。"
                  : "未检测到 SD 卡，请插入后重试。";
        break;
    }
    lv_label_set_text(cr->val_hint, hint);

    /* 按钮：仅 EXPOSED 可"关闭"；APP_OWNED 为瞬态（即将自动关闭），禁用 */
    bool active = (st == CARDREADER_EXPOSED);
    bool clickable = active || ((st == CARDREADER_IDLE || st == CARDREADER_ERROR) && sd);
    lv_label_set_text(cr->lbl_toggle, active ? "关闭 USB 存储" : "开启 USB 存储");
    if (clickable) {
        lv_obj_remove_state(cr->btn_toggle, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(cr->btn_toggle, LV_STATE_DISABLED);
    }
}

static void cr_timer_cb(lv_timer_t *t)
{
    cr_update(lv_timer_get_user_data(t));
}

static void cr_btn_cb(lv_event_t *e)
{
    card_reader_t *cr = lv_event_get_user_data(e);
    if (!cr) return;

    if (app_cardreader_get_state() == CARDREADER_EXPOSED) {
        app_cardreader_disable();
    } else {
        app_cardreader_enable();
    }
    cr_update(cr);   /* 立即刷新（enable/disable 期间服务状态已变化） */
}

/* 信息行：key（上）/value（下）两行显示，value 限宽换行完整显示 */
static void cr_row(lv_obj_t *parent, const char *key, lv_obj_t **val_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, CR_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_set_style_pad_gap(row, 2, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    lv_obj_set_style_text_color(k, CR_DIM, 0);
    lv_obj_set_style_text_font(k, cr_font(), 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v, lv_pct(100));   /* 限宽换行，避免超长溢出被裁剪 */
    lv_obj_set_style_text_color(v, CR_ACCENT, 0);
    lv_obj_set_style_text_font(v, cr_font(), 0);
    *val_out = v;
}

card_reader_t *card_reader_create(lv_obj_t *parent, card_reader_back_cb_t back_cb, void *ctx)
{
    card_reader_t *cr = lv_malloc(sizeof(card_reader_t));
    if (!cr) return NULL;
    lv_memzero(cr, sizeof(*cr));
    cr->back_cb = back_cb;
    cr->back_ctx = ctx;

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, CR_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    cr->root = root;

    /* 信息区：三行（SD 卡 / 设备状态 / 提示），超屏可滚动 */
    int sh = lv_display_get_vertical_resolution(lv_display_get_default());
    lv_obj_t *info = lv_obj_create(root);
    lv_obj_set_pos(info, 8, 10);
    lv_obj_set_size(info, lv_pct(100) - 16, sh - 10 - 8 - 56);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_radius(info, 0, 0);
    lv_obj_set_style_pad_all(info, 0, 0);
    lv_obj_set_style_pad_gap(info, 6, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(info, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(info, LV_SCROLLBAR_MODE_OFF);

    cr_row(info, "SD 卡", &cr->val_sd);
    cr_row(info, "设备状态", &cr->val_status);
    cr_row(info, "提示", &cr->val_hint);

    /* 底部开启/关闭按钮 */
    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, 160, 34);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(btn, CR_CARD, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, CR_ACCENT, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_text_color(btn, CR_TEXT, 0);
    lv_obj_set_style_text_font(btn, cr_font(), 0);
    lv_obj_add_event_cb(btn, cr_btn_cb, LV_EVENT_CLICKED, cr);
    cr->btn_toggle = btn;
    cr->lbl_toggle = lv_label_create(btn);
    lv_obj_center(cr->lbl_toggle);

    cr_update(cr);

    /* 状态轮询：服务事件来自 TinyUSB 任务，经 lv_timer 回到 LVGL 线程刷新 */
    cr->timer = lv_timer_create(cr_timer_cb, 250, cr);

    return cr;
}

void card_reader_destroy(card_reader_t *cr)
{
    if (!cr) return;
    /* 退出 APP 自动关闭读卡器（恢复 /sdcard） */
    if (app_cardreader_get_state() == CARDREADER_EXPOSED ||
        app_cardreader_get_state() == CARDREADER_APP_OWNED) {
        ESP_LOGI("card_reader", "exit -> disable card reader");
        app_cardreader_disable();
    }
    if (cr->timer) lv_timer_delete(cr->timer);
    if (cr->root) {
        lv_obj_add_flag(cr->root, LV_OBJ_FLAG_HIDDEN);
        lv_refr_now(NULL);
        lv_obj_delete(cr->root);
    }
    lv_free(cr);
}

bool card_reader_swipe_back(card_reader_t *cr)
{
    (void)cr;
    return true;
}

void card_reader_refresh(card_reader_t *cr)
{
    if (!cr) return;
    cr_update(cr);
}

void card_reader_debug_event(card_reader_t *cr, int evt)
{
    (void)evt;
    if (!cr) return;
    ESP_LOGI("card_reader", "[DBG] state=%s sd=%d",
             app_cardreader_state_str(app_cardreader_get_state()),
             app_cardreader_sd_ready());
}
