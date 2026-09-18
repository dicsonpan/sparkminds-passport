// main/passport_app.c —— 创智学员 Passport 应用主体 (ARCS-MINI 适配版)。
// 移除了依赖于 ESP-IDF 的独立电源管理和休眠逻辑，因为 ARCS-MINI 具备完整的电池/屏幕/唤醒管理服务。
#include "passport_app.h"

#include "passport_core.h"
#include "passport_net.h"
#include "passport_store.h"
#include "passport_ui.h"

#include "lisa_log.h"
#include "lisa_log.h"
#include "lisa_device.h"
#include "lisa_display.h"
#include "sys_wifi.h"
#include "sys_network_manager.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "service_brightness.h"
#include "battery.h"

static const char *TAG = "smpass_app";

#define SCREEN_OFF_TIMEOUT_S 60           // 1 分钟无操作自动熄屏

static uint32_t s_idle_seconds = 0;
static bool     s_screen_off = false;
static int      s_saved_brightness = 100;
static bool     s_last_usb = false;

#define KEY_QUEUE_LEN      8
#define UI_QUEUE_LEN       8
#define HEARTBEAT_PERIOD_S 120           // 2 分钟心跳（服务端超时15分钟，提供充分容错）
#define SYNC_FALLBACK_S    3600          // 每小时兜底同步
#define BOOT_SYNC_DELAY_S  3

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} key_evt_t;

typedef enum {
    MODE_PASSPORT = 0,
    MODE_LEGACY_DEMO,
} app_mode_t;

static QueueHandle_t s_key_queue;
static QueueHandle_t s_ui_queue;
static app_mode_t    s_mode = MODE_PASSPORT;
static int64_t       s_last_heartbeat_s;
static int64_t       s_last_sync_s;
static bool          s_boot_sync_done;

// 替换原有的 bsp_lvgl_lock。在 arcs_mini 中，LVGL 有独立的互斥锁或通过 LISA_UI_INVOKE_UI 投递。
// 简易起见，此处假设 LVGL_MUTEX 可用，或者假设回调已经在 LVGL 线程中。
// 为了编译通过并且不至于立刻 crash，先提供桩函数，实际生产需对接 lisa_ui_mutex。
bool bsp_lvgl_lock(int timeout_ms) { return true; }
void bsp_lvgl_unlock(void) {}

// ---------------------------------------------------------------------------
// dispatch 定时器
// ---------------------------------------------------------------------------
static void enter_legacy_demo(void)
{
    s_mode = MODE_LEGACY_DEMO;
    legacy_menu_enter();
}

static void handle_key(key_evt_t evt)
{
    if (s_mode == MODE_LEGACY_DEMO) {
        return;
    }

    s_idle_seconds = 0;

    // 若当前处于熄屏状态，点亮屏幕并忽略该次按键（防误触）
    if (s_screen_off) {
        s_screen_off = false;
        int target = s_saved_brightness > 0 ? s_saved_brightness : 100;
        service_brightness_set_temp(target);
        if (s_mode == MODE_PASSPORT) {
            passport_ui_show(passport_ui_current());
            passport_ui_tick();
        }
        LISA_LOGI(TAG, "Screen woke up by key press");
        return;
    }

    // 全局：OK 长按返回菜单
    if (evt.btn == BSP_BTN_OK && evt.ev == BSP_BTN_LONG &&
        passport_ui_current() != PASSPORT_SCREEN_MENU) {
        passport_ui_show(PASSPORT_SCREEN_MENU);
        return;
    }
    if (evt.btn == BSP_BTN_OK && evt.ev == BSP_BTN_LONG &&
        passport_ui_current() == PASSPORT_SCREEN_MENU) {
        return;
    }

    passport_ui_handle_key(evt.btn, evt.ev);
}

static uint32_t now_ts(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)tv.tv_sec;
}


static void second_tick(void)
{
    int64_t now_s = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;

    static bool s_time_was_valid = false;
    if (!s_time_was_valid && passport_time_is_valid(now_ts())) {
        s_time_was_valid = true;
        passport_ui_evt_t evt = { .type = PASSPORT_UI_EVT_TIME_VALID, .arg1 = 0 };
        if (s_ui_queue) xQueueSend(s_ui_queue, &evt, 0);
    }

    // USB 插拔事件检测：插拔时唤醒屏幕并刷新
    bool cur_usb = battery_usb_plugged_stable_get();
    if (cur_usb != s_last_usb) {
        s_last_usb = cur_usb;
        s_idle_seconds = 0;
        if (s_screen_off) {
            s_screen_off = false;
            int target = s_saved_brightness > 0 ? s_saved_brightness : 100;
            service_brightness_set_temp(target);
            if (s_mode == MODE_PASSPORT) {
                passport_ui_show(passport_ui_current());
                passport_ui_tick();
            }
            LISA_LOGI(TAG, "Screen woke up by USB change");
        }
    }

    // 无操作 60 秒（1 分钟）自动熄屏
    if (!s_screen_off) {
        if (++s_idle_seconds >= SCREEN_OFF_TIMEOUT_S) {
            s_screen_off = true;
            s_saved_brightness = service_brightness_get();
            if (s_saved_brightness <= 0) s_saved_brightness = 100;
            service_brightness_set_temp(0);
            passport_ui_clear_sensitive();
            LISA_LOGI(TAG, "Auto screen off after %d seconds idle", SCREEN_OFF_TIMEOUT_S);
        }
    }

    if (!s_screen_off && s_mode == MODE_PASSPORT) passport_ui_tick();

    // 开机一次性同步
    if (!s_boot_sync_done && now_s >= BOOT_SYNC_DELAY_S) {
        s_boot_sync_done = true;
        s_last_sync_s = now_s;
        if (passport_store_is_provisioned()) {
            passport_net_request(PASSPORT_NET_REQ_SYNC);
        }
    }

    // 每小时兜底同步
    if (s_boot_sync_done && now_s - s_last_sync_s >= SYNC_FALLBACK_S &&
        !passport_net_is_busy()) {
        s_last_sync_s = now_s;
        if (passport_store_is_provisioned()) {
            passport_net_request(PASSPORT_NET_REQ_SYNC);
        }
    }
    // 维持 10 分钟心跳节奏
    if (now_s - s_last_heartbeat_s >= HEARTBEAT_PERIOD_S &&
        !passport_net_is_busy()) {
        s_last_heartbeat_s = now_s;
        if (passport_store_is_provisioned()) {
            passport_net_request(PASSPORT_NET_REQ_HEARTBEAT);
        }
    }
    
    // 如果还没校时，强制触发一次 NTP 更新（假设网络可能是好但 NTP 没运行）
    // if (!sys_time_is_sync()) {
    //     sys_time_sync();
    // }
}

static void dispatch_cb(lv_timer_t *timer)
{
    (void)timer;
    static int s_tick_div;
    static bool s_first_run = true;

    if (s_first_run) {
        s_first_run = false;
        // 恢复：显示主菜单
        passport_ui_show(PASSPORT_SCREEN_MENU);
    }

    key_evt_t key;
    while (xQueueReceive(s_key_queue, &key, 0) == pdTRUE) {
        handle_key(key);
    }
    
    // UI 队列事件处理 (暂无网络模块，但可接收内部UI事件)
    passport_ui_evt_t evt;
    while (xQueueReceive(s_ui_queue, &evt, 0) == pdTRUE) {
        if (s_mode == MODE_PASSPORT) {
            passport_ui_on_net_event(&evt);
        }
    }

    if (++s_tick_div >= 10) {   // 100ms × 10 = 1s
        s_tick_div = 0;
        second_tick();
    }
}

// ---------------------------------------------------------------------------
// 供 main.c (arcs_mini) 调用的按键入口
// ---------------------------------------------------------------------------
void passport_app_on_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_key_queue) return;
    key_evt_t evt = { .btn = btn, .ev = ev };
    xQueueSend(s_key_queue, &evt, 0);
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 学员身份花名册（官方后台绑定凭证）
// ---------------------------------------------------------------------------
typedef struct {
    const char *name;
    const char *student_id;
    const char *secret_hex;
} student_entry_t;

static const student_entry_t S_STUDENTS[] = {
    { "梁根润",          "SM-2026-001", "8900f114af58cb2989c7856e0c93a49d39ad1f8e2f756d682540a7d6266ee37d" },
    { "陆昭闻（Owen）",  "SM-2026-002", "9d6a4ea3a3f9802a40969b4676c65b73035e6cdcc1c4b970661c4f61b79c2819" },
    { "仇绍恒",          "SM-2026-003", "32fa28d5be83704fd8eb77d19b6b8f9f1061fa77449f20d1d56a381ecea5aade" },
    { "梁根珹",          "SM-2026-004", "913240fb10d0009522253bb392764b6ec1453c57fd328cd178ae7bc3a7eb9e65" },
    { "王之谦",          "SM-2026-006", "d3df7c02ca477729de047b2c576a4b03274c0c80957bce3599e58e51a5d1841b" },
    { "徐一潇（Jeremy）","SM-2026-013", "155e787acf59d130ce453fe22a498281098f0abce690d7049f0f8ee6c0c2aab1" },
    { "王宥森",          "SM-2026-051", "2f2c7908e132801fb4bab69398c8d39e37192aa24af5a3a819c2e653d1a4ebed" },
    { "郭涵若 Cavin",    "SM-2026-070", "4a043814904f8e31668012d1997f5d2ba313ad1f39a7b021553341b266b9678c" },
    { "Aiden Kwok",      "SM-2026-193", "93fc265de7a4f6bb21d6f24dc65a502d861d2bb4d5dd1c3b979064bcd702f747" },
    { "Eric 金家熠",     "SM-2026-565", "4cc41779c7a70d6475de87bb0f1267f5dfbceede55e39c137ae36c9b6aac5e3d" },
};

#define CURRENT_TARGET_STUDENT_ID "SM-2026-004"

static bool student_hex_to_bytes(const char *hex, uint8_t *bytes, size_t byte_len) {
    if (!hex || strlen(hex) != byte_len * 2) return false;
    for (size_t i = 0; i < byte_len; i++) {
        char byte_str[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
        bytes[i] = (uint8_t)strtoul(byte_str, NULL, 16);
    }
    return true;
}

static bool passport_provision_student(const char *student_id) {
    for (size_t i = 0; i < sizeof(S_STUDENTS)/sizeof(S_STUDENTS[0]); i++) {
        if (strcmp(S_STUDENTS[i].student_id, student_id) == 0 ||
            strstr(S_STUDENTS[i].student_id, student_id) != NULL) {
            uint8_t secret[32];
            if (student_hex_to_bytes(S_STUDENTS[i].secret_hex, secret, 32)) {
                LISA_LOGI(TAG, "正在写入学员身份：%s (%s)", S_STUDENTS[i].name, S_STUDENTS[i].student_id);
                lisa_kv_del(PASSPORT_KEY_PP_TOKEN);
                lisa_kv_del(PASSPORT_KEY_BALANCE);
                lisa_kv_del(PASSPORT_KEY_BALANCE_TS);
                lisa_kv_del(PASSPORT_KEY_NAME);
                lisa_kv_del(PASSPORT_KEY_STUDENT_ID);
                lisa_kv_del(PASSPORT_KEY_AVATAR_URL);
                passport_store_del_avatar();

                passport_store_provision(S_STUDENTS[i].student_id, secret);
                passport_store_set_string(PASSPORT_KEY_NAME, S_STUDENTS[i].name);
                passport_store_set_string(PASSPORT_KEY_STUDENT_ID, S_STUDENTS[i].student_id);
                LISA_LOGI(TAG, "写入成功！学员身份已就绪：%s (%s)", S_STUDENTS[i].name, S_STUDENTS[i].student_id);
                return true;
            }
        }
    }
    LISA_LOGE(TAG, "未在花名册中找到学员 ID: %s", student_id);
    return false;
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
void passport_app_start(void)
{
    char existing_pid[32] = {0};
    int pid_ret = passport_store_get_string(PASSPORT_KEY_PID, existing_pid, sizeof(existing_pid));
    
    // 如果板内未绑定该学员或为出厂初始态，对齐写入指定学员身份
    if (pid_ret != ESP_OK || strlen(existing_pid) == 0 || strcmp(existing_pid, CURRENT_TARGET_STUDENT_ID) != 0) {
        LISA_LOGI(TAG, "初始化/对齐指定学员 -> %s", CURRENT_TARGET_STUDENT_ID);
        passport_provision_student(CURRENT_TARGET_STUDENT_ID);
    } else {
        LISA_LOGI(TAG, "检测到已有学员身份: %s，予以严格保护保留！", existing_pid);
    }

    // 确保开机屏幕亮度正常点亮 (100%)，清除可能残留的 0 亮度，并唤醒屏幕
    s_screen_off = false;
    s_idle_seconds = 0;
    s_saved_brightness = 100;
    service_brightness_set(100);
    lisa_device_t *disp = lisa_device_get("display");
    if (disp) {
        lisa_display_blanking_off(disp);
    }

    s_key_queue = xQueueCreate(KEY_QUEUE_LEN, sizeof(key_evt_t));
    s_ui_queue = xQueueCreate(UI_QUEUE_LEN, sizeof(passport_ui_evt_t));

    // 初始化网络
    passport_net_init(s_ui_queue);

    // 初始化 UI 组件和资源
    passport_ui_init(s_ui_queue);

    s_last_heartbeat_s = 0;
    s_last_sync_s = 0;

    lv_timer_create(dispatch_cb, 100, NULL);

    LISA_LOGI(TAG, "创智学员 Passport 就绪 (UI Only)");
}

#include "shell.h"

static int shell_provision_cmd(int argc, char *argv[])
{
    if (argc < 2) {
        LISA_LOGI(TAG, "用法: provision <学号后缀或完整学号，例如 070, 006, 013, 565, 051>");
        for (size_t i = 0; i < sizeof(S_STUDENTS)/sizeof(S_STUDENTS[0]); i++) {
            LISA_LOGI(TAG, "  - %s (%s)", S_STUDENTS[i].name, S_STUDENTS[i].student_id);
        }
        return -1;
    }
    bool ok = passport_provision_student(argv[1]);
    return ok ? 0 : -1;
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 provision, shell_provision_cmd, "provision <student_id_or_suffix>");

