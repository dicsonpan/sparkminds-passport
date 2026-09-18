#include "FreeRTOS.h"
#include "task.h"
#include "lisa_log.h"
#include "voice_msg.h"
#include "bsp_button.h"
#include "passport_app.h"

#define TAG "main"

#include "button_blecfg.h"
#include "passport_net.h"
#include "passport_ota.h"

extern void passport_app_on_key(bsp_btn_t btn, bsp_btn_ev_t ev);

static void voice_msg_button_evt_handler(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data) {
    const voice_msg_button_evt_t *evt = (const voice_msg_button_evt_t *)data;
    if (evt == NULL || len < sizeof(*evt)) return;

    bsp_btn_t btn;
    bsp_btn_ev_t ev;

    // 单按键逻辑替代（纯净开发）：
    // 短按 -> 相当于按 DOWN（用于移动光标/翻页）
    // 双击 -> 相当于按 OK（用于确认选择）
    // 长按 -> 相当于 OK 长按（用于返回菜单）
    // 5次及以上连击 -> 进入 BLE 配网
    switch (evt->action) {
        case VOICE_MSG_BUTTON_ACTION_CLICK:
            btn = BSP_BTN_DOWN;
            ev = BSP_BTN_CLICK;
            break;
        case VOICE_MSG_BUTTON_ACTION_DOUBLE_CLICK:
            btn = BSP_BTN_OK;
            ev = BSP_BTN_CLICK;
            break;
        case VOICE_MSG_BUTTON_ACTION_LONG_HOLD:
            btn = BSP_BTN_OK;
            ev = BSP_BTN_LONG;
            break;
        case VOICE_MSG_BUTTON_ACTION_QUINTUPLE_CLICK:
        case VOICE_MSG_BUTTON_ACTION_SEXTUPLE_CLICK:
        case VOICE_MSG_BUTTON_ACTION_SEPTUPLE_CLICK:
        case VOICE_MSG_BUTTON_ACTION_REPEAT_CLICK:
            button_blecfg_start();
            return;
        default:
            return;
    }

    passport_app_on_key(btn, ev);
}

static void voice_msg_network_evt_handler(void *unused, uint32_t msg_id, void *data, uint32_t len, void *user_data) {
    if (msg_id == VOICE_MSG_WIFI_CONNECTED || 
        msg_id == VOICE_MSG_WIFI_IP_GOT || 
        msg_id == VOICE_MSG_SYSTEM_NETWORK_PROBE_SUCCESS) {
        LISA_LOGI(TAG, "Network is ready! Triggering SYNC and checking OTA version...");
        passport_net_request(PASSPORT_NET_REQ_SYNC);
        passport_ota_check_version_async();
    }
}

int main(int argc, char **argv)
{
    extern int boot_watchdog_feed(void);
    boot_watchdog_feed();

    LISA_LOGI(TAG, "Starting sparkminds-passport on ARCS-MINI");

    extern int service_led_init(void);
    service_led_init();
    extern int service_volume_init(void);
    service_volume_init();
    extern int service_brightness_init(void);
    service_brightness_init();
    extern int service_button_init(void);
    service_button_init();
    extern int service_image_init(void);
    service_image_init();
    extern int service_camera_init(void);
    service_camera_init();
    
    // app_button_init(); // Optional if not used

#if CONFIG_APPLICATION_UI
    extern int lisa_ui_init(void);
    lisa_ui_init();
#endif

    extern int battery_init(void);
    battery_init();

    button_blecfg_init();
    voice_msg_sub(VOICE_MSG_BUTTON_CHANGE, voice_msg_button_evt_handler, NULL);
    voice_msg_sub(VOICE_MSG_WIFI_CONNECTED, voice_msg_network_evt_handler, NULL);
    voice_msg_sub(VOICE_MSG_WIFI_IP_GOT, voice_msg_network_evt_handler, NULL);
    voice_msg_sub(VOICE_MSG_SYSTEM_NETWORK_PROBE_SUCCESS, voice_msg_network_evt_handler, NULL);

    // Init passport application
    passport_app_start();

    while (1) {
        boot_watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return 0;
}
