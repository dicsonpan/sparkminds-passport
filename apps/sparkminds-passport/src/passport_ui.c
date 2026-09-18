// main/passport_ui.c —— 六个屏幕的 LVGL 实现（MENU/IDENTITY/QR/POINTS/CHECKIN/SETTINGS）。
//
// 只运行在 UI 上下文（LVGL 任务）。数据一律读 NVS 缓存与 passport_net 的
// 快照接口；联网动作只发 passport_net_request，不在此等待。
//
// QR 与像素头像用 LV_COLOR_FORMAT_I1 画布直接写位（模块锐利、内存极小）：
//   QR 37x37 模块 × 6 = 222px 画布，约 6.2 KB；
//   头像 8x8 × 8 = 64px 画布，512 B。
#include "passport_ui.h"

#include "passport_core.h"
#include "passport_net.h"
#include "passport_store.h"
#include "passport_ota.h"
#include "ui_pixel.h"
#include "qrcodegen.h"

#include "power_manager.h"
#include "battery.h"
#include "bsp_button.h"
#include "esp_log.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "smpass_ui";

// 中文字体由 tools/gen_font.sh 生成（font_sm_cjk_16.c），仅含 UI 用到的字。
extern const lv_font_t font_sm_cjk_16;
extern lv_font_t lv_font_chinese_16;
static lv_font_t s_font_cjk;

#define FONT_CJK   (&s_font_cjk)
#define FONT_LATIN (&lv_font_montserrat_14)
#define FONT_BIG   (&lv_font_montserrat_20)

#define QR_PX           164 // 164 适合 240x240 屏幕 (支持 Version 6, size 41 * scale 4)
#define QR_STRIDE       ((QR_PX + 7) / 8)   // I1 行字节数
#define AVATAR_PX       64
#define AVATAR_STRIDE ((AVATAR_PX + 7) / 8)

#define DEMO_HOLD_EXTRA_MS 3500          // LONG 事件后再按住这么久即满 5 秒

static QueueHandle_t s_ui_queue;

static lv_obj_t *s_scr;
static passport_screen_t s_current = PASSPORT_SCREEN_MENU;
static int s_sel;                        // MENU/SETTINGS 选中行

// 各屏需要持续引用的对象
static lv_obj_t *s_rows[6];              // 菜单/设置行
static lv_obj_t *s_status;               // 状态行（pid + 在馆标记 + 电量）
static lv_obj_t *s_body;                 // 正文主标签
static lv_obj_t *s_body2;                // 副标签
static lv_obj_t *s_canvas;               // QR / 头像画布
static lv_obj_t *s_bar;                  // QR 倒计时条
static lv_obj_t *s_bat;                  // 顶部电池标签
static uint8_t *s_canvas_buf;
static uint32_t s_qr_window;             // 当前已绘制的二维码窗口
static bool s_sync_ever_ok;              // 本轮开机后是否同步成功过（离线角标用）
static lv_timer_t *s_hold_timer;         // 设置页 5 秒长按检测
static lv_timer_t *s_ota_timer;          // OTA 状态刷新定时器

static uint8_t s_disp_pct = 0xFF;        // 当前平滑显示的电量 (0-100)
static uint8_t s_last_raw_pct = 0xFF;    // 上次采样的原始电量
static uint8_t s_stable_cnt = 0;         // 1% 抖动防抖计数

static uint8_t *s_avatar_blob;
static lv_img_dsc_t s_avatar_dsc;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static void get_pid(char *out, size_t len)
{
    if (passport_store_get_string(PASSPORT_KEY_PID, out, len) != ESP_OK) {
        strlcpy(out, "--", len);
    }
}

static uint32_t now_ts(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)tv.tv_sec;
}

static void update_battery_display(void)
{
    if (!s_bat) return;

    uint16_t mv = battery_get_voltage_mv();
    uint8_t raw_pct = battery_get_pct_raw();
    if (raw_pct > 100) raw_pct = 100;
    bool usb = battery_usb_plugged_stable_get();
    battery_status_t st = battery_get_status();

    if (st == BATTERY_STATUS_NO_BATTERY || mv < 2000) {
        lv_obj_set_style_text_color(s_bat, lv_color_hex(UI_CYAN), 0);
        lv_label_set_text(s_bat, "USB");
        lv_obj_align(s_bat, LV_ALIGN_TOP_RIGHT, -10, 10);
        return;
    }

    // 初次采样直接赋值；后续跳变 >= 2% 或连续 2 次采样一致才更新，消除 1% 边缘抖动
    if (s_disp_pct == 0xFF) {
        s_disp_pct = raw_pct;
        s_last_raw_pct = raw_pct;
        s_stable_cnt = 0;
    } else {
        if (abs((int)raw_pct - (int)s_disp_pct) >= 2) {
            s_disp_pct = raw_pct;
            s_last_raw_pct = raw_pct;
            s_stable_cnt = 0;
        } else if (raw_pct != s_disp_pct) {
            if (raw_pct == s_last_raw_pct) {
                if (++s_stable_cnt >= 2) {
                    s_disp_pct = raw_pct;
                    s_stable_cnt = 0;
                }
            } else {
                s_last_raw_pct = raw_pct;
                s_stable_cnt = 1;
            }
        } else {
            s_stable_cnt = 0;
        }
    }

    bool charging = usb || (st == BATTERY_STATUS_CHARGING) || (st == BATTERY_STATUS_CHARGE_DONE);
    if (charging) {
        lv_obj_set_style_text_color(s_bat, lv_color_hex(UI_GREEN), 0);
        if (st == BATTERY_STATUS_CHARGE_DONE || s_disp_pct >= 99) {
            lv_label_set_text(s_bat, LV_SYMBOL_CHARGE " 100%");
        } else {
            lv_label_set_text_fmt(s_bat, LV_SYMBOL_CHARGE " %d%%", s_disp_pct);
        }
    } else {
        if (s_disp_pct <= 15) {
            lv_obj_set_style_text_color(s_bat, lv_color_hex(0xFF3B30), 0); // 低电量警示红
        } else if (s_disp_pct <= 30) {
            lv_obj_set_style_text_color(s_bat, lv_color_hex(0xFF9500), 0); // 较低电量橙黄
        } else {
            lv_obj_set_style_text_color(s_bat, lv_color_hex(UI_TEXT_DIM), 0);
        }
        lv_label_set_text_fmt(s_bat, "%d%%", s_disp_pct);
    }
    lv_obj_align(s_bat, LV_ALIGN_TOP_RIGHT, -10, 10);
}

static void add_cyber_header(lv_obj_t *scr, const char *title)
{
    lv_obj_t *lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl, FONT_LATIN, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_CYAN), 0);
    lv_label_set_text(lbl, title);
    lv_obj_set_pos(lbl, 10, 10);
    
    s_bat = lv_label_create(scr);
    lv_obj_set_style_text_font(s_bat, FONT_LATIN, 0);
    update_battery_display();

    // Line separator
    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_size(line, 220, 1);
    lv_obj_set_pos(line, 10, 30);
    lv_obj_set_style_bg_color(line, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_border_width(line, 0, 0);
}

static lv_obj_t *body_label(lv_obj_t *parent, int x, int y, const lv_font_t *font,
                            uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

// I1 画布直写位（避开 set_px 对索引格式的歧义）
static void i1_set(uint8_t *buf, int stride, int x, int y, bool on)
{
    uint8_t *byte = &buf[8 + y * stride + x / 8]; // 跳过 LVGL 的 8 字节调色板头部
    uint8_t mask = (uint8_t)(0x80 >> (x % 8));   // LVGL I1：MSB 在前
    if (on) *byte |= mask; else *byte &= (uint8_t)~mask;
}

static lv_obj_t *canvas_i1_create(lv_obj_t *parent, int x, int y, int w, int h,
                                  uint32_t c0, uint32_t c1)
{
    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, s_canvas_buf, w, h, LV_IMG_CF_INDEXED_1BIT);
    lv_canvas_set_palette(canvas, 0, lv_color_hex(c0));
    lv_canvas_set_palette(canvas, 1, lv_color_hex(c1));
    lv_obj_set_pos(canvas, x, y);
    return canvas;
}

// ---------------------------------------------------------------------------
// 二维码绘制（窗口变化才重绘，实现文档 7.1 节）
// ---------------------------------------------------------------------------
static void draw_qr(uint32_t ts)
{
    char pid[32];
    uint8_t secret[PASSPORT_SECRET_LEN];
    if (passport_store_get_string(PASSPORT_KEY_PID, pid, sizeof(pid)) != ESP_OK ||
        passport_store_get_secret(secret) != ESP_OK) {
        memset(secret, 0, sizeof(secret));
        return;
    }

    char code[PASSPORT_CODE_LEN + 1];
    char url[128];
    passport_qr_code(secret, pid, ts, code);
    memset(secret, 0, sizeof(secret));
    if (passport_qr_url(pid, ts, code, url, sizeof(url)) < 0) return;

    // 契约只需到版本 6（实现文档 6.2 节）；静态缓冲避免占用 LVGL 任务栈。
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    if (!qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, 6,
                              qrcodegen_Mask_AUTO, true)) {
        printf(TAG, "二维码编码失败");
        return;
    }
    int size = qrcodegen_getSize(qr);   
    int scale = QR_PX / size;
    int offset = (QR_PX - size * scale) / 2;

    memset(s_canvas_buf + 8, 0, QR_STRIDE * QR_PX);   // 白底
    for (int my = 0; my < size; my++) {
        for (int mx = 0; mx < size; mx++) {
            if (!qrcodegen_getModule(qr, mx, my)) continue;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    i1_set(s_canvas_buf, QR_STRIDE,
                           offset + mx * scale + dx, offset + my * scale + dy, true);
                }
            }
        }
    }
    lv_obj_invalidate(s_canvas);
    s_qr_window = passport_window(ts);
}

static lv_obj_t *create_avatar(lv_obj_t *parent, int x, int y)
{
    size_t len = 0;
    lv_obj_t *obj = NULL;

    if (s_avatar_blob) {
        passport_store_free_avatar(s_avatar_blob);
        s_avatar_blob = NULL;
    }

    if (passport_store_get_avatar(&s_avatar_blob, &len) == ESP_OK && s_avatar_blob && len > 0) {
        s_avatar_dsc.header.always_zero = 0;
        s_avatar_dsc.header.cf = LV_IMG_CF_RAW;
        s_avatar_dsc.header.w = 0;
        s_avatar_dsc.header.h = 0;
        s_avatar_dsc.data_size = len;
        s_avatar_dsc.data = s_avatar_blob;

        obj = lv_img_create(parent);
        lv_img_set_src(obj, &s_avatar_dsc);
        lv_obj_set_pos(obj, x, y);
    } else {
        obj = lv_obj_create(parent);
        lv_obj_set_size(obj, AVATAR_PX, AVATAR_PX);
        lv_obj_set_pos(obj, x, y);
        lv_obj_set_style_bg_color(obj, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(obj, 0, 0);
        lv_obj_set_style_radius(obj, 0, 0);
    }
    return obj;
}

// ---------------------------------------------------------------------------
// MENU
// ---------------------------------------------------------------------------
static const char *const MENU_NAMES[] = {
    "我的身份", "登录二维码", "我的积分", "设置",
};
#define MENU_COUNT 4

static void menu_refresh(void)
{
    for (int i = 0; i < MENU_COUNT; i++) {
        lv_obj_t *card = lv_obj_get_parent(s_rows[i]);
        ui_pixel_set_selected(card, i == s_sel, true);
    }
}

static void build_menu(void)
{
    s_scr = ui_pixel_screen_create("");
    add_cyber_header(s_scr, "SPARKMINDS PASSPORT");

    create_avatar(s_scr, 15, 40);

    char name[32] = "--";
    passport_store_get_string(PASSPORT_KEY_NAME, name, sizeof(name));
    lv_obj_t *n_lbl = body_label(s_scr, 90, 45, FONT_CJK, UI_TEXT);
    lv_label_set_text(n_lbl, name);

    char pid[32];
    get_pid(pid, sizeof(pid));
    lv_obj_t *id_lbl = body_label(s_scr, 90, 70, FONT_LATIN, UI_CYAN);
    lv_label_set_text(id_lbl, pid);

    uint8_t in_lab = 0;
    passport_store_get_u8(PASSPORT_KEY_IN_LAB, &in_lab);
    lv_obj_t *stat = body_label(s_scr, 90, 90, FONT_CJK, in_lab ? UI_GREEN : UI_TEXT_DIM);
    lv_label_set_text(stat, in_lab ? "在线" : "离线");

    for (int i = 0; i < MENU_COUNT; i++) {
        int r = i / 2;
        int c = i % 2;
        lv_obj_t *card = ui_pixel_panel_create(s_scr, 15 + c * 110, 125 + r * 55, 100, 45, UI_PANEL);
        s_rows[i] = lv_label_create(card);
        lv_obj_set_style_text_font(s_rows[i], FONT_CJK, 0);
        lv_obj_set_style_text_color(s_rows[i], lv_color_hex(UI_TEXT), 0);
        lv_label_set_text(s_rows[i], MENU_NAMES[i]);
        lv_obj_center(s_rows[i]);
    }
    s_sel = 0;
    menu_refresh();
}

// ---------------------------------------------------------------------------
// IDENTITY
// ---------------------------------------------------------------------------
static void build_identity(void)
{
    s_scr = ui_pixel_screen_create("");
    add_cyber_header(s_scr, "02 IDENTITY");

    create_avatar(s_scr, (240 - AVATAR_PX) / 2, 45);

    char name[32] = "--";
    passport_store_get_string(PASSPORT_KEY_NAME, name, sizeof(name));
    lv_obj_t *name_label = body_label(s_scr, 0, 120, FONT_CJK, UI_TEXT);
    lv_obj_set_width(name_label, 240);
    lv_obj_set_style_text_align(name_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(name_label, name);

    char pid[32];
    get_pid(pid, sizeof(pid));
    lv_obj_t *id_label = body_label(s_scr, 0, 140, FONT_LATIN, UI_CYAN);
    lv_obj_set_width(id_label, 240);
    lv_obj_set_style_text_align(id_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(id_label, pid);

    uint8_t in_lab = 0;
    passport_store_get_u8(PASSPORT_KEY_IN_LAB, &in_lab);
    
    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 20, 170, 200, 60, UI_PANEL);
    lv_obj_t *status_title = body_label(panel, 0, 5, FONT_CJK, UI_TEXT_DIM);
    lv_obj_set_width(status_title, 180);
    lv_obj_set_style_text_align(status_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_title, "当前状态");

    s_body = body_label(panel, 0, 25, FONT_CJK, in_lab ? UI_GREEN : UI_TEXT_DIM);
    lv_obj_set_width(s_body, 180);
    lv_obj_set_style_text_align(s_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_body, in_lab ? "在线" : "离线");
}

// ---------------------------------------------------------------------------
// QR
// ---------------------------------------------------------------------------
static void build_qr(void)
{
    s_scr = ui_pixel_screen_create("");
    add_cyber_header(s_scr, "03 QR CODE");
    s_qr_window = UINT32_MAX;

    if (!passport_time_is_valid(now_ts())) {
        lv_obj_t *panel = ui_pixel_panel_create(s_scr, 12, 90, 216, 110, UI_PANEL);
        s_body = lv_label_create(panel);
        lv_obj_set_style_text_font(s_body, FONT_CJK, 0);
        lv_obj_set_style_text_color(s_body, lv_color_hex(UI_TEXT), 0);
        lv_obj_set_width(s_body, 190);
        lv_obj_set_style_text_align(s_body, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(s_body, "请联网校时后再使用\n（进积分页或签到页联网）");
        lv_obj_center(s_body);
        return;
    }

    lv_obj_t *bg = lv_obj_create(s_scr);
    lv_obj_set_size(bg, 176, 176);
    lv_obj_set_pos(bg, (240 - 176) / 2, 35); 
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_set_style_radius(bg, 8, 0);

    s_canvas = canvas_i1_create(s_scr, (240 - QR_PX) / 2, 35 + 6, QR_PX, QR_PX,
                                0xFFFFFF, 0x000000);
    draw_qr(now_ts());

    s_body = body_label(s_scr, 0, 215, FONT_CJK, UI_TEXT_DIM);
    lv_obj_set_width(s_body, 240);
    lv_obj_set_style_text_align(s_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_body, "HMAC-SHA256 加密 / 支持离线扫码");

    s_body2 = body_label(s_scr, 0, 230, FONT_LATIN, UI_CYAN);
    lv_obj_set_width(s_body2, 240);
    lv_obj_set_style_text_align(s_body2, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_body2, "30s");

    lv_scr_load(s_scr);
}

// ---------------------------------------------------------------------------
// POINTS
// ---------------------------------------------------------------------------
static void points_refresh(void)
{
    int32_t balance = 0;
    passport_store_get_i32(PASSPORT_KEY_BALANCE, &balance);
    lv_label_set_text_fmt(s_body, "%ld", (long)balance);

    char stamp[24] = { 0 };
    bool has_stamp = passport_store_get_string(PASSPORT_KEY_BALANCE_TS,
                                               stamp, sizeof(stamp)) == ESP_OK;
    if (passport_net_is_busy()) {
        lv_label_set_text(s_body2, "同步中...");
    } else if (s_sync_ever_ok && has_stamp) {
        lv_label_set_text_fmt(s_body2, "更新于 %s", stamp);
    } else {
        lv_label_set_text(s_body2, "离线缓存");
    }

    passport_point_item_t items[PASSPORT_HISTORY_MAX];
    int n = passport_net_history(items);
    for (int i = 0; i < PASSPORT_HISTORY_MAX; i++) {
        if (!s_rows[i]) break;
        if (i < n) {
            lv_label_set_text_fmt(s_rows[i], "%s    %s%ld", items[i].text, items[i].delta >= 0 ? "+" : "", (long)items[i].delta);
            lv_obj_set_style_text_color(s_rows[i], lv_color_hex(items[i].delta >= 0 ? UI_GREEN : UI_RED), 0);
        } else {
            lv_label_set_text(s_rows[i], "");
        }
    }
}

static void build_points(void)
{
    s_scr = ui_pixel_screen_create("");
    add_cyber_header(s_scr, "04 POINTS");

    s_body = body_label(s_scr, 0, 36, FONT_BIG, UI_GOLD);
    lv_obj_set_width(s_body, 240);
    lv_obj_set_style_text_align(s_body, LV_TEXT_ALIGN_CENTER, 0);

    s_body2 = body_label(s_scr, 0, 62, FONT_CJK, UI_CYAN);
    lv_obj_set_width(s_body2, 240);
    lv_obj_set_style_text_align(s_body2, LV_TEXT_ALIGN_CENTER, 0);

    for (int i = 0; i < PASSPORT_HISTORY_MAX; i++) {
        lv_obj_t *card = ui_pixel_panel_create(s_scr, 15, 84 + i * 29, 210, 26, UI_PANEL);
        s_rows[i] = lv_label_create(card);
        lv_obj_set_style_text_font(s_rows[i], FONT_CJK, 0);
        lv_obj_set_style_text_color(s_rows[i], lv_color_hex(UI_TEXT), 0);
        lv_obj_center(s_rows[i]);
    }
    points_refresh();
}


// SETTINGS
// ---------------------------------------------------------------------------
static void settings_refresh(void)
{
    char line[48];
    char stamp[24] = { 0 };
    bool has_stamp = passport_store_get_string(PASSPORT_KEY_BALANCE_TS,
                                               stamp, sizeof(stamp)) == ESP_OK;
    snprintf(line, sizeof(line), "同步: %s", has_stamp ? stamp : "无");
    lv_label_set_text(s_rows[0], line);

    lv_label_set_text(s_rows[1], passport_time_is_valid(now_ts())
                                 ? "校时: 已同步" : "校时: 未同步");

    char pid[32];
    get_pid(pid, sizeof(pid));
    snprintf(line, sizeof(line), "设备: %s", pid);
    lv_label_set_text(s_rows[2], line);

    const passport_ota_info_t *info = passport_ota_get_info();
    if (passport_ota_is_busy()) {
        lv_label_set_text(s_rows[3], passport_ota_get_status_text());
    } else if (info->checking) {
        lv_label_set_text(s_rows[3], "OTA: 正在检测...");
    } else if (info->checked) {
        if (info->has_update) {
            snprintf(line, sizeof(line), "发现新版: %s (按OK)", info->version);
            lv_label_set_text(s_rows[3], line);
        } else {
            snprintf(line, sizeof(line), "固件: %s (已最新)", PASSPORT_FW_VERSION);
            lv_label_set_text(s_rows[3], line);
        }
    } else {
        snprintf(line, sizeof(line), "固件: %s (按OK检测)", PASSPORT_FW_VERSION);
        lv_label_set_text(s_rows[3], line);
    }

    for (int i = 0; i < 4; i++) {
        lv_obj_t *card = lv_obj_get_parent(s_rows[i]);
        ui_pixel_set_selected(card, i == s_sel, true);
    }
}

static void ota_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_current == PASSPORT_SCREEN_SETTINGS && s_rows[3]) {
        const passport_ota_info_t *info = passport_ota_get_info();
        if (passport_ota_is_busy()) {
            lv_label_set_text(s_rows[3], passport_ota_get_status_text());
            lv_obj_center(s_rows[3]);
        } else if (info->checking) {
            lv_label_set_text(s_rows[3], "OTA: 正在检测...");
            lv_obj_center(s_rows[3]);
        } else {
            char line[48];
            if (info->checked) {
                if (info->has_update) {
                    snprintf(line, sizeof(line), "发现新版: %s (按OK)", info->version);
                } else {
                    snprintf(line, sizeof(line), "固件: %s (已最新)", PASSPORT_FW_VERSION);
                }
            } else {
                snprintf(line, sizeof(line), "固件: %s (按OK检测)", PASSPORT_FW_VERSION);
            }
            lv_label_set_text(s_rows[3], line);
            lv_obj_center(s_rows[3]);
        }
    }
}

static void build_settings(void)
{
    s_scr = ui_pixel_screen_create("");
    add_cyber_header(s_scr, "05 SETTINGS");

    for (int i = 0; i < 4; i++) {
        lv_obj_t *card = ui_pixel_panel_create(s_scr, 15, 48 + i * 42, 210, 34, UI_PANEL);
        s_rows[i] = lv_label_create(card);
        lv_obj_set_style_text_font(s_rows[i], FONT_CJK, 0);
        lv_obj_set_style_text_color(s_rows[i], lv_color_hex(UI_CYAN), 0);
        lv_obj_center(s_rows[i]);
    }
    s_sel = 0;
    settings_refresh();
    s_ota_timer = lv_timer_create(ota_timer_cb, 300, NULL);
}

// 5 秒长按检测：LONG 事件（约 1.5s）时再计时 3.5s，仍按住 OK 即满足
static void hold_check_cb(lv_timer_t *timer)
{
    (void)timer;
    s_hold_timer = NULL;
    uint32_t mv = 3300;
    if (mv >= 447 && mv <= 1900) {   // OK 键电压窗口（bsp_pins.h BSP_BTN_MV_TABLE）
        if (s_ui_queue) {
            // 经 UI 队列通知 app 进 demo 菜单，避免跨上下文调用
            passport_ui_evt_t evt = { .type = 0, .arg1 = PASSPORT_ACTION_ENTER_DEMO };
            xQueueSend(s_ui_queue, &evt, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// 屏幕装配/拆解
// ---------------------------------------------------------------------------
typedef void (*build_fn_t)(void);
static const build_fn_t BUILDERS[PASSPORT_SCREEN_COUNT] = {
    build_menu, build_identity, build_qr,
    build_points, build_settings,
};

static void teardown(void)
{
    if (s_hold_timer) {
        lv_timer_del(s_hold_timer);
        s_hold_timer = NULL;
    }
    if (s_ota_timer) {
        lv_timer_del(s_ota_timer);
        s_ota_timer = NULL;
    }
    if (s_scr) {
        lv_obj_del(s_scr);
        s_scr = NULL;
    }
    s_canvas = NULL;
    s_status = s_body = s_body2 = s_bar = s_bat = NULL;
    memset(s_rows, 0, sizeof(s_rows));
    
    if (s_avatar_blob) {
        passport_store_free_avatar(s_avatar_blob);
        s_avatar_blob = NULL;
    }
}

// ---------------------------------------------------------------------------
// 公开 API
// ---------------------------------------------------------------------------
void passport_ui_init(QueueHandle_t ui_queue)
{
    s_font_cjk = font_sm_cjk_16;
    s_font_cjk.fallback = NULL;
    s_ui_queue = ui_queue;

    // 分配最大所需的画布缓冲（PSRAM 中），避免静态数组撑爆 SRAM
    if (!s_canvas_buf) {
        s_canvas_buf = lv_mem_alloc(8 + QR_STRIDE * QR_PX);
    }
}

void passport_ui_show(passport_screen_t screen)
{
    lv_obj_t *old_scr = s_scr;
    s_scr = NULL;
    teardown();
    s_current = screen;
    BUILDERS[screen]();
    if (s_scr) lv_scr_load(s_scr);
    if (old_scr) lv_obj_del(old_scr);
}

passport_screen_t passport_ui_current(void)
{
    return s_current;
}

passport_action_t passport_ui_handle_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    switch (s_current) {
    case PASSPORT_SCREEN_MENU:
        if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP) {
                s_sel = (s_sel + MENU_COUNT - 1) % MENU_COUNT;
                menu_refresh();
            } else if (btn == BSP_BTN_DOWN) {
                s_sel = (s_sel + 1) % MENU_COUNT;
                menu_refresh();
            } else if (btn == BSP_BTN_OK) {
                passport_ui_show((passport_screen_t)(PASSPORT_SCREEN_IDENTITY + s_sel));
            }
        }
        break;

    case PASSPORT_SCREEN_QR:
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK && s_canvas) {
            draw_qr(now_ts());   // OK 立即重绘
        }
        break;

    case PASSPORT_SCREEN_POINTS:
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK && !passport_net_is_busy()) {
            lv_label_set_text(s_body2, "同步中");
            passport_net_request(PASSPORT_NET_REQ_SYNC);
        }
        break;


    case PASSPORT_SCREEN_SETTINGS:
        if (ev == BSP_BTN_CLICK) {
            if (btn == BSP_BTN_UP) {
                s_sel = (s_sel + 3) % 4;
                settings_refresh();
            } else if (btn == BSP_BTN_DOWN) {
                s_sel = (s_sel + 1) % 4;
                settings_refresh();
            } else if (btn == BSP_BTN_OK) {
                if (s_sel == 3 && !passport_ota_is_busy()) {
                    const passport_ota_info_t *info = passport_ota_get_info();
                    if (info->has_update) {
                        lv_label_set_text(s_rows[3], "准备更新中...");
                        lv_obj_center(s_rows[3]);
                        passport_ota_start_latest();
                    } else {
                        lv_label_set_text(s_rows[3], "正在检查更新...");
                        lv_obj_center(s_rows[3]);
                        passport_ota_check_version_async();
                    }
                }
            }
        } else if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG && !s_hold_timer) {
            // 隐藏入口候选：LONG 后再计时 3.5s（合计约 5 秒）
            s_hold_timer = lv_timer_create(hold_check_cb, DEMO_HOLD_EXTRA_MS, NULL);
            lv_timer_set_repeat_count(s_hold_timer, 1);
        }
        break;

    default:
        break;
    }
    return PASSPORT_ACTION_NONE;
}

void passport_ui_on_net_event(const passport_ui_evt_t *evt)
{
    switch (evt->type) {
    case PASSPORT_UI_EVT_SYNC_OK:
        s_sync_ever_ok = true;
        if (s_current == PASSPORT_SCREEN_POINTS) points_refresh();
        if (s_current == PASSPORT_SCREEN_IDENTITY) passport_ui_show(PASSPORT_SCREEN_IDENTITY);
        if (s_current == PASSPORT_SCREEN_MENU) passport_ui_show(PASSPORT_SCREEN_MENU);
        break;
    case PASSPORT_UI_EVT_SYNC_FAIL:
        if (s_current == PASSPORT_SCREEN_POINTS) {
            lv_label_set_text(s_body2, "同步失败");
        }
        break;

    case PASSPORT_UI_EVT_TIME_VALID:
        if (s_current == PASSPORT_SCREEN_QR && !s_canvas) {
            passport_ui_show(PASSPORT_SCREEN_QR);   // 校时完成，重建可用的二维码页
        }
        break;
    case PASSPORT_UI_EVT_HEARTBEAT_OK:
        s_sync_ever_ok = true;
        if (s_current == PASSPORT_SCREEN_POINTS) points_refresh();
        if (s_current == PASSPORT_SCREEN_IDENTITY) passport_ui_show(PASSPORT_SCREEN_IDENTITY);
        if (s_current == PASSPORT_SCREEN_MENU) passport_ui_show(PASSPORT_SCREEN_MENU);
        break;
    case PASSPORT_UI_EVT_NET_IDLE:
        if (s_current == PASSPORT_SCREEN_POINTS) points_refresh();
        break;
    default:
        break;
    }
}

void passport_ui_tick(void)
{
    update_battery_display();

    if (s_current == PASSPORT_SCREEN_QR && s_canvas) {
        uint32_t ts = now_ts();
        if (!passport_time_is_valid(ts)) return;
        uint32_t window = passport_window(ts);
        if (window != s_qr_window) draw_qr(ts);
        if (s_body2) {
            int rem = (int)(PASSPORT_WINDOW_SECONDS - ts % PASSPORT_WINDOW_SECONDS);
            lv_label_set_text_fmt(s_body2, "%ds", rem);
        }
    }
}

void passport_ui_clear_sensitive(void)
{
    if (s_current == PASSPORT_SCREEN_QR && s_canvas && s_canvas_buf) {
        memset(s_canvas_buf + 8, 0, QR_STRIDE * QR_PX);   // 清码防断背光残影
        lv_obj_invalidate(s_canvas);
        s_qr_window = UINT32_MAX;
    }
}
