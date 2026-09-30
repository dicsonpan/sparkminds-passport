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
    { "梁根润", "SM-2026-001", "8900f114af58cb2989c7856e0c93a49d39ad1f8e2f756d682540a7d6266ee37d" },
    { "陆昭闻（Owen）", "SM-2026-002", "9d6a4ea3a3f9802a40969b4676c65b73035e6cdcc1c4b970661c4f61b79c2819" },
    { "仇绍恒", "SM-2026-003", "32fa28d5be83704fd8eb77d19b6b8f9f1061fa77449f20d1d56a381ecea5aade" },
    { "梁根珹", "SM-2026-004", "913240fb10d0009522253bb392764b6ec1453c57fd328cd178ae7bc3a7eb9e65" },
    { "季煦", "SM-2026-005", "8bda00bab6bb56312e331e81850e5feb3cafe9334d225c89859954c893e93797" },
    { "王之谦", "SM-2026-006", "d3df7c02ca477729de047b2c576a4b03274c0c80957bce3599e58e51a5d1841b" },
    { "梁子锟 Lewis", "SM-2026-008", "2705b81ed5b97d43f747083ca7f2ae26026dda0551556edd95bbcf429c87718b" },
    { "林光磊", "SM-2026-009", "969ab850c1f6ea4790b37678f52c91b27669d36e91c9d5c2de67583a81253506" },
    { "李晋桥", "SM-2026-010", "1027d47a7915bb3f3d994257f3bec1507f3cbac52f981a21c5b2236471a406ea" },
    { "彭涂文轩", "SM-2026-011", "cdc50c859dcfd973d1e8d4174f12ddd7cf015cbc6d65cbf043fffd0ab9ba8b46" },
    { "赵玮麒", "SM-2026-012", "18887fba0e15447f6be86be817b6efaff41f5a6e4d05855867ed0feb25b7dda5" },
    { "徐一潇（Jeremy）", "SM-2026-013", "155e787acf59d130ce453fe22a498281098f0abce690d7049f0f8ee6c0c2aab1" },
    { "金骏轩", "SM-2026-014", "6b17ed40ce56324164556dfbd260cbe2bc4c81b0b212887c420a0a2d1c3af479" },
    { "李泓毅", "SM-2026-015", "29e716d804fc5b1544ae9905b818465397b118e5323fab3a26e26a46d1e64185" },
    { "邓远涛", "SM-2026-016", "e5e782a043e681cc7e775a4b725f2cad817241709040976bc0a0fa2dd0659fd7" },
    { "曾顺鑫", "SM-2026-017", "5c7de29d95200827e8fdf1800873fb5da56c71ce25a01cd0583422731507f73d" },
    { "张楚越", "SM-2026-018", "484ef858d525170c4cdac1b9c8d5c7bd79bf5bd2279ca354d0b4dd1818cda28d" },
    { "杨聿筠", "SM-2026-019", "9c513884dcc619bb23bffc3a79321f402fa417646f897cf23f5927edaec0ed6f" },
    { "赵可昕", "SM-2026-020", "7306eee006293851d8163f7e535ad0fb25edb08383bbdae412f44f117a115299" },
    { "陆思行", "SM-2026-021", "1719415dbd359ab711cd6c277bcf494f2814cffd6838e829fe25dcdb3cc543ea" },
    { "陈家熠", "SM-2026-022", "823984ae1dd8de9b00031589cb519220324d571baed298198f45c384a39204e1" },
    { "汪楚轩", "SM-2026-023", "2a99531e835a9c8c8c93ca6e72212f03163a1e54717f4dbd1cf70ced935bdd36" },
    { "叶文湛", "SM-2026-024", "8fa3f95ec221a414e9e562058bec48b67e86412392a519e05b01bd745d3c9efc" },
    { "肖臻耀", "SM-2026-025", "081377a101bb6c7fc8d865647bf505785d937b848bf7a6dba0006ea8ce5f7762" },
    { "陈昭阳", "SM-2026-026", "7b31e78de08c59a34b7f667d8e7d933d908d21de82045f733b41dd5332893681" },
    { "陈昭坤", "SM-2026-027", "766cc3b0fa006707c74f1e4f82778bbdda4ebcd501943df6af0c844b16a2ee82" },
    { "陈昭信", "SM-2026-028", "eaef784f404093f2669da08e7268bfe7ddb19c911ae35adddf155ed03e267cfb" },
    { "韩瑞光", "SM-2026-029", "2d6f922d4aaae4d3aa1df96af6cff8affb6b61b2b373b8fc04e1e4cf528f4a55" },
    { "李子澍", "SM-2026-030", "1e504e586cc6288517590541aea925bbbcad325a2fd8de35a17acd28197d49bd" },
    { "杨明晗", "SM-2026-032", "bc6da98e969f802fd569224b46fe4522fc254014cf3cbf4db46b399ab00865a0" },
    { "孙子恒", "SM-2026-033", "2a74c40df098eeb75e36d29f50a2dd7ce15b41fbf841710555b793a55dae37e6" },
    { "敬彦博", "SM-2026-034", "4cb640849e06611dc23372b2926d113a934dae9079ff7e589f1ec16ca4073fae" },
    { "Aiden(郑皓羽)", "SM-2026-035", "45b13d7021320fe6c89b302b6c32c1d5582e0d4558aa9d8ee948a90cdf3eb72b" },
    { "傅韦锜", "SM-2026-036", "b5cd34288e8fe76484ee8566b19cb3dffca753199fec1d747449d77eea83c3ce" },
    { "邱泰宁", "SM-2026-037", "9583a30d6c3d883e0493817fdfa752edffc2ade18cb072cf04dc307db8614e33" },
    { "张哲宁", "SM-2026-038", "1fc42adb5b3b2706fb90aef61bcbaea533e074b88366523810dbc8bd0b00763e" },
    { "郭光正", "SM-2026-039", "f813b5a9a1656d5f0f30561e485f8d5872d5dd831cf84a41584e633c06340e75" },
    { "冼可晴", "SM-2026-040", "efb2d9a2941b86532af860c12cc19ccae71ac487486d9dfe6b3f43c5d00a0ec7" },
    { "贺祺珺", "SM-2026-041", "02756b5fbb8da67b4afa220616fc33d0338ae5dedef7d3e30f56453359211a19" },
    { "陈隽扬", "SM-2026-042", "39fc939a35f0eff3243ce90bcfbffa5a8db80e9ec4564d3e6dec340736469f40" },
    { "陈合雅", "SM-2026-043", "8a7fe66244866b64010d15f674fd5511144142bb08fe30b82564995d47fdacc2" },
    { "翁晞然", "SM-2026-044", "9842a76ec7afcf9e27757c493c74bfc63139559733c588f94072920af8b50d71" },
    { "严韬捷", "SM-2026-045", "88e52debe3ac628e5f249e833233d2e3b3eb593a5b6e0fba3f32f8d060f10084" },
    { "吴京熹", "SM-2026-046", "108d2c11c54a0f37b75c26a713cb3dbc34470d9dec4097d4e17fb3c96749267a" },
    { "潘悦", "SM-2026-047", "b7b3010b3f7b22d4d62ae4bdaa5eef9fba899c79cd89308267fee1b0786181e8" },
    { "叶林熹", "SM-2026-048", "7975da5989305cf2a0e51d6919011e321a2debc39888d5da1358fac7428f8ece" },
    { "苏梓铭", "SM-2026-049", "7d219c78d34aebb283855156443cfa8f7ebd126a0526fecacdac19e592b05fa4" },
    { "邓沛恒", "SM-2026-050", "b93e66307dd278bc978bfffd15c02ccfa7d2f85348b1d09f921abd3e9cf4fd7a" },
    { "王宥森", "SM-2026-051", "2f2c7908e132801fb4bab69398c8d39e37192aa24af5a3a819c2e653d1a4ebed" },
    { "赖祉言", "SM-2026-052", "ffaab721da472a1704b9293caa431603d84bfa33ed06153e491520642a7a3999" },
    { "曾兆骐", "SM-2026-053", "f36653d8da756274040a22a13ad888055c8ca58fea91628ebfd227a0abadf2c9" },
    { "曾承浠", "SM-2026-054", "ad5df25773083ad19bac2a951ac16e12f25d96f8270f89da16e9e9e02ad5e626" },
    { "罗煜哲", "SM-2026-055", "56f1a328f8b78c7aae03823217405fb4af374f74e1d03a496b3abc00a56cdf83" },
    { "廖浩铖Jacob", "SM-2026-056", "e4488697bdad9e08bb2d47d2188d561e818e932dde92eb31d21b9331f0dc995e" },
    { "廖浩竣", "SM-2026-057", "c58e5ec8ca58e7db8f050f1541727997cb3d9c0787f3b500520d9f23b117ced3" },
    { "曹浩林", "SM-2026-058", "9047d0fad9ac14868473023b632f07d8d994ce018521458388901816b302e1fd" },
    { "李彬佑", "SM-2026-059", "031342e5f35e9e9936bd47acb359a3bda206dd0d32cca470315d6ef074c2e9fe" },
    { "陈乐哲", "SM-2026-060", "03230191069e4215d66029beafccf9c77cb8aa95e056bc3d7f2e3955f1117997" },
    { "刘馥瑞", "SM-2026-062", "9907a04bb6e972804252e1c864891ab388049d2998419a977ab529d36f997e6a" },
    { "曾汝元", "SM-2026-065", "b3c34a93c5d5476f6c349fd923df6e2d067d8324d30d732dc639211db24847fd" },
    { "吴宸合", "SM-2026-066", "a4d61819f4204c0193c69696602c9c2d373f8852226f0c55355065941c3424c8" },
    { "李骊铭", "SM-2026-067", "e93708d39a65534d848050254500a989a027033eab0afbff2f429c27d1d48cae" },
    { "测试学生", "SM-2026-068", "fd97846c3015f542058b2cb343428f97f088c0f0bb4c42e5afc827b7045d7dd7" },
    { "郭涵若 Cavin", "SM-2026-070", "4a043814904f8e31668012d1997f5d2ba313ad1f39a7b021553341b266b9678c" },
    { "劲宝", "SM-2026-071", "24e920f32367ed16b6bdd3cab755be224d979523bdaa191e033abe3ca6382027" },
    { "吴所谓", "SM-2026-072", "09153e8395c9c471d9bc431e753069e5aab6f2418ab69a58dcc4f22d79b075b2" },
    { "陈家祺", "SM-2026-073", "ec4b6706fd160039c44705b13b2ed73126dd4cd1ab5169431283cc650955004d" },
    { "陈品源 Berry", "SM-2026-074", "2c1603a784312b3af30631101583ed30cfe91d15b85085f424b875086a265bcd" },
    { "罗格", "SM-2026-075", "5bd4347c2241079767a2b70601591f69419b4d235ff5b66679c5d9a37ccf2689" },
    { "叶祖睿", "SM-2026-077", "a08d8a39aff77065ea6601ed017722cb1378e2afa3de4c65fc426cd58b409aca" },
    { "马逸竣", "SM-2026-078", "5cb499ffc3c3dd3e83c138d25f5f3fe7d58ec136e8a5ba96f66a9f65d191054c" },
    { "徐朗", "SM-2026-079", "09514a343500f885762ce26567fe797a3a6eacbe82590891cb82ebaca6eec7c0" },
    { "王英权", "SM-2026-080", "29721f97b1fa75ec2ff80907fb9fedc49bf1958077000eec0bb69909605b9952" },
    { "王英帅", "SM-2026-081", "3afbf2610b8cf96338e35a3632ebef4d8fe93d2643d7c4a307fbbf41d22d480e" },
    { "马明睿", "SM-2026-082", "5bad8d1ef5e4e8ff3dfc86a08585b00980d0b2f4e068cbe551aa07699aa25df2" },
    { "Baiting Wu", "SM-2026-083", "9cdf33b797be592fc5a5a6b0d501d2d639a4b4ed83da20aea5a806d41b719c5a" },
    { "Baimei Wu", "SM-2026-084", "5ebede2c5c4366bf67df9d2715fda539b592a0217d5d92aca6b93a5e451e697b" },
    { "彭涂文轩", "SM-2026-085", "cdaa455a8f92f071b8302e73f94e55b5a6e48188d71b58f40078f6c770da5857" },
    { "曹浩林", "SM-2026-143", "7456f9deb4a484ffeb394c2e6385b3f5b800c74961d58b6159bb1e3fd8488cd7" },
    { "Aiden Kwok", "SM-2026-193", "93fc265de7a4f6bb21d6f24dc65a502d861d2bb4d5dd1c3b979064bcd702f747" },
    { "Charlie", "SM-2026-197", "985c83e2c853103c95b0287e6ff0a9faca4b0e250cd53cb6d141c2812d88feda" },
    { "转化测试", "SM-2026-564", "752144a940571a30acdd61c84a6756dcad98b88c8e511223c89767f45da7ab9d" },
    { "Eric 金家熠", "SM-2026-565", "4cc41779c7a70d6475de87bb0f1267f5dfbceede55e39c137ae36c9b6aac5e3d" },
    { "许赞", "SM-2026-590", "1d9b4cf2c7efe65bad11a7c4d2ac31983f92d4c88161de6e68758330a39b9601" },
    { "Ewan赵亦宏", "SM-2026-591", "8610f8a3be32a35a5c0e152f43d0104182be7e2871137288fe31d01571ab6eed" },
    { "侯思远", "SM-2026-592", "31e24eec598d930f6f071646b7a85d9933ef0304fb06198b2a76bccfd9ddd443" },
    { "浠浠", "SM-2026-593", "db35e5197e933c03e080fbd283f3efc85f2b568100a9553fea687bf25de0474a" },
    { "林俊宇", "SM-2026-718", "e3c82b9ffe0f76006936a61383a9faaa54628a6ceffb1e815b577de97435e789" },
    { "向子珅", "SM-2026-790", "510e5b745d6d67d57112093f3c5c307a057f7058b995e2923f0cdaf0b3f9db6f" },
    { "郭怀骏", "SM-2026-884", "fd909e7f4235a754b2fd033c1e98c36adc24aff9b9509b4dcae0413921d0f4d5" },
    { "李政贤", "SM-2026-956", "6ec6e23ded152e3268f7b106b349a733642ecc7ca9ae91b2c67dec7a4ac2ba42" },
    { "唐嘉著", "SM-2026-967", "464b0ae668d229810f25219574900760cff7aaff0d249c078ecf49f92de41ca8" },
    { "创才学霸演示生", "SM-2026-CCXB-DEMO", "f39ae694c139c16066c0bca4e905e1f5919ab273ccdd973dc92a9d0a1fbdf5aa" },
    { "演示同学", "SM-2026-DEMO", "a65f481f2b0ae37b4cfe65a01b334c124659ea0ea3294dc3bca5e81a8e15662d" },
    { "Woody Du", "SM-2026-NEW", "7fc09c34ec7c2728b8bd37758ef64ba7c8d0ec0145b23b0b4a275473b73781ea" },
};

#define CURRENT_TARGET_STUDENT_ID "SM-2026-067"

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

