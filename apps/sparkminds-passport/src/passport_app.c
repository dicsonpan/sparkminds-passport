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
    { "季煦", "SM-2026-005", "4bcff7b8136df551cf5bf175f03cc94c6979d1572f442bc8a2594d5fcacd81bc" },
    { "王之谦", "SM-2026-006", "d3df7c02ca477729de047b2c576a4b03274c0c80957bce3599e58e51a5d1841b" },
    { "梁子锟 Lewis", "SM-2026-008", "3cf6af1662675f95fda1b7b1b840acf3cbaa6c750c4e2166094cb04a21dfaf07" },
    { "林光磊", "SM-2026-009", "0a49ef8fd2c6bee4c4c15fddc506c5db84da3b482eebb7393735d13923844516" },
    { "李晋桥", "SM-2026-010", "9af9cc7ffa56cd185c275f2facfe373d883242effdd23d36db0d5f91be43e74d" },
    { "彭涂文轩", "SM-2026-011", "3c2cdabb2215e891464c8a6c28455b8cf3e0caf1974e1b9cd60fe2c9d3895282" },
    { "赵玮麒", "SM-2026-012", "6796be335fcb854f1aa1582d9afc1e0279a3f78af2079a6ee4929af8b1096c24" },
    { "徐一潇（Jeremy）", "SM-2026-013", "155e787acf59d130ce453fe22a498281098f0abce690d7049f0f8ee6c0c2aab1" },
    { "金骏轩", "SM-2026-014", "5f5bb9834eab94af35cd96e802ef8a84caa7070f2d149a9ae4c9cc470f0aebd6" },
    { "李泓毅", "SM-2026-015", "882e81ace2b87e7281e82180ab70f3efec00fcc36e7d5d8ddab50efcbf8c6c84" },
    { "邓远涛", "SM-2026-016", "4f3a7e3fb4ce04b96ed2c8321b7f7b54c686bb22569065bd3495822272671dd1" },
    { "曾顺鑫", "SM-2026-017", "c0e3ae1d0513979faa41141f40875aa2fdf76b0034b4cb1ad3c1d3c00ebd9bd0" },
    { "张楚越", "SM-2026-018", "80209617cf4eb0593c3bc7d315c86e2a7966b56d47586693f400e9e3259bf7c1" },
    { "杨聿筠", "SM-2026-019", "363fe72fe36a945fda9c960dbeb62e1f8a6e0deba0d903af79a8066d1817a922" },
    { "赵可昕", "SM-2026-020", "2b5fc42c3d2da74c099b60aeac5e4bd2e37ed015a9e0d0d7943f0cf529c25610" },
    { "陆思行", "SM-2026-021", "1efd80320d60855cbef140d0f4d314543a06508fbede9611417e9da15a7f06c2" },
    { "陈家熠", "SM-2026-022", "edd31c61bd786d14c0f7d11ac86086916bc5f75a5bc6769700fd612851a21557" },
    { "汪楚轩", "SM-2026-023", "b9d7e574d5721f533bdd631808c9c560cb0862d40a60a7a64b3650c9ac5e7434" },
    { "叶文湛", "SM-2026-024", "2216937317fc46003b801854e96179f14c19539a00fd41c65a8401a4ad331cfe" },
    { "肖臻耀", "SM-2026-025", "cb9c4ecb3f19cdcf0e57d43e9f6a49ed8168b5afd77e6d0dcc4072eecf95f9c3" },
    { "陈昭阳", "SM-2026-026", "3b58dc00d068ae0db8b8a20719396b2e7ee6425223041278a21e69b04dc114d5" },
    { "陈昭坤", "SM-2026-027", "d79d1e08fcaf1bfda0f85ba8e49538c8f7baa590cf6a2d1f4e07616d8ded4b7e" },
    { "陈昭信", "SM-2026-028", "188b4b79a5bac539188e853a4eae5c5210555b088cdd997e15b47283af0f2026" },
    { "韩瑞光", "SM-2026-029", "98d0dbdd5401f9525a258924896636c5b4ab5feeae44ec8010f89228ddecc9ce" },
    { "李子澍", "SM-2026-030", "5f0f0b6fd8bbfed8111392d607049479c4402695d5a0d7d3b8bfc07d33f74221" },
    { "杨明晗", "SM-2026-032", "c325ae53c6f5d78266561c72f058beca6c6c36c14f78c655a1dd347ea65ae500" },
    { "孙子恒", "SM-2026-033", "4f6851146d433b072f5b93ae0dc12f0264c508c93445248050b17f49e0a38b46" },
    { "敬彦博", "SM-2026-034", "bf06e86d28e8c3c2c35007c11c6df89a63894fbefc1c1f36c5d3f2fe3ab958cb" },
    { "Aiden(郑皓羽)", "SM-2026-035", "c397d99d51ded99f632c23495d86fae323e9e466b52dce68a613d880a87d37bd" },
    { "傅韦锜", "SM-2026-036", "fa1c45856533d7e8addd79a36db2f03dd1d38960a7f6fdc2520c6e25a6c2e839" },
    { "邱泰宁", "SM-2026-037", "5bf49dd2d0ae7c0a78d489920362d2e25a314c10b632486373c02f8ab8548f72" },
    { "张哲宁", "SM-2026-038", "8d804deb9b442774461054e4ebb113f2fb88025eabf0dd060d95a1134f6d1191" },
    { "郭光正", "SM-2026-039", "7ba30249778d5a312adae0ce567b4d15fc0b69447d49e106b5ea97035769ff49" },
    { "冼可晴", "SM-2026-040", "857a5914b5610fe159f9196178874d6a11f7701b738450009217b6b74c476a97" },
    { "贺祺珺", "SM-2026-041", "bb55fa16803da1af211db5946064bf799061bf01564942531c826b8f18a3f501" },
    { "陈隽扬", "SM-2026-042", "5ac1b95a4e5142e05d1a50ecc34babb6d0a3c5c345589885451909ed264eb7c9" },
    { "陈合雅", "SM-2026-043", "4660672885ee12d228759417b0457b2c1928a90778445b6a9449efcb90eff9d8" },
    { "翁晞然", "SM-2026-044", "0b24c526f41e83c65e6e47021c9760b2e71851d92418335ae9dca4900f466e2e" },
    { "严韬捷", "SM-2026-045", "be78c7e331705d6f0f4a2fda219f1e3ed620521c19cad957f3c2bedd5e8fe29c" },
    { "吴京熹", "SM-2026-046", "1dde93eaf50a2eab7223f720414a514179a5bc05de025816949be8cd72ac86d5" },
    { "潘悦", "SM-2026-047", "7e4e66eaca5571a374c098159b3611d62fc4c02cdf1b39f0b2cb29fc641f740c" },
    { "叶林熹", "SM-2026-048", "7975da5989305cf2a0e51d6919011e321a2debc39888d5da1358fac7428f8ece" },
    { "苏梓铭", "SM-2026-049", "fb7b4bf8f833deab5a64315229e15b90b24fd6348ee1c2519777eb79fc634ff8" },
    { "邓沛恒", "SM-2026-050", "66f31513eda3b88c0f715049bed3a34cefb67d18e9b8acd43b0435131ff409d5" },
    { "王宥森", "SM-2026-051", "2f2c7908e132801fb4bab69398c8d39e37192aa24af5a3a819c2e653d1a4ebed" },
    { "赖祉言", "SM-2026-052", "4837cdfcf37b0a10f5e7d1b9055abcf5e508c2fc9d4fb9f859eada12c7af4982" },
    { "曾兆骐", "SM-2026-053", "0e84507889e552a54b3663e9ac4ad4417ca3cdea3390d49d9d0e5137ac5ff771" },
    { "曾承浠", "SM-2026-054", "c5bad6e903a82a988569ff55ecb1c3e077192e69ffa8e375a2e869b5d523c220" },
    { "罗煜哲", "SM-2026-055", "ce5bae604acd328ed944bf813e188d51fb8807526ce382cc5cd794c7553eb243" },
    { "廖浩铖Jacob", "SM-2026-056", "82f4b02350f89d8d9a2b427a9fe4cae3e656573bb853e37fc568f5f336a4c134" },
    { "廖浩竣", "SM-2026-057", "506b7c482ef1fa4975fa9491747ab08f3cba4b1f48b2b1a088148bb1443cd6bb" },
    { "曹浩林", "SM-2026-058", "44ddb613a35c0ea41b0692e7d08e254efc59521d6508ab6b79a3aba776d9b9b5" },
    { "李彬佑", "SM-2026-059", "1f1a0de0f230548895c9bb00f2042ed4ddd7da68afb885d09ada83b0505a57e5" },
    { "陈乐哲", "SM-2026-060", "d07dce6e93c47a0a2e271c83f9d71dc7cf40751620d4b222717f85effcc34e51" },
    { "刘馥瑞", "SM-2026-062", "361ba10868fad5227a2fd59a301c2eafb69d6673f8e95144966b071eadd3228c" },
    { "曾汝元", "SM-2026-065", "35af7674f4d9adf6162c5c72eaf4c9eb60ea173bde5ff432ca595f36a13bfa51" },
    { "吴宸合", "SM-2026-066", "bdbfee211fcf0932611965cf3ed1c5d31cb0ebb68d12d3780775e6dfab33c792" },
    { "李骊铭", "SM-2026-067", "a40294488185fb8071370a2bd9e8607e3738c2dbf98ca4e744acab8e81b08dbb" },
    { "测试学生", "SM-2026-068", "45bbc92831e5ed6c521de4fe234cee4270887cdc395e976872d9a42be027b083" },
    { "郭涵若 Cavin", "SM-2026-070", "4a043814904f8e31668012d1997f5d2ba313ad1f39a7b021553341b266b9678c" },
    { "劲宝", "SM-2026-071", "850aa2ca415026ca4bda38ff8bc253a1cae3056f3fe97a4c7e95896fe5d1f9c3" },
    { "吴所谓", "SM-2026-072", "247f1fda84686fb24431e6bfd3b004f8e5601cc1a08bfb8f62bada2cf6e4dc97" },
    { "陈家祺", "SM-2026-073", "fbbba8da06593ca62619d9c2d13bffe19a82a912854c41236c101160b27db637" },
    { "陈品源 Berry", "SM-2026-074", "e36451dc249053d7aa110cf56f8b7ad70462429379140ea23fd43fe9f815ef3a" },
    { "罗格", "SM-2026-075", "5a6f0684306e69b22ce6e4d4da0040a2fbdea5c6211d7725f640a2321b8876c5" },
    { "叶祖睿", "SM-2026-077", "3d10ac276e0fb287c1435f3ecc5ecd813d526299a6ba75a36ff44819dbaac116" },
    { "马逸竣", "SM-2026-078", "43c28f57c7e6bacc8e758d21fee52594684a9df32d38fb55dc8952dfd10cddce" },
    { "徐朗", "SM-2026-079", "5ee9a2ae724426e35bff082a6d59622788ea2c190a6488768b639738e647851a" },
    { "王英权", "SM-2026-080", "204edd16d05d6b944db460e385e408283fe239b7049f95acccecc9d1210256f2" },
    { "王英帅", "SM-2026-081", "763106562acdcce64ed93ca41a1c3201ef55b282afd51600f1f3fc968780d5ce" },
    { "马明睿", "SM-2026-082", "63f0717bb9862e5d3fef41b9ada4e3e3d29a576744f05ea79f684418349b7708" },
    { "Baiting Wu", "SM-2026-083", "a2cbe0db8043465fa88695ee03970dfa256417689d63f0d675c91300a78fb2f2" },
    { "Baimei Wu", "SM-2026-084", "b14ae50e120a6bb3e736fe2685e77c8914e2df67fbd6a27e9905269d4758bac2" },
    { "彭涂文轩", "SM-2026-085", "ccd3e6fd6ea4becd7dad853fec17618709ef765b7dd3f8403a42a2e76d9af44d" },
    { "曹浩林", "SM-2026-143", "ec597fb1e4a34a28f869292fea5ddce0c16571177bcb78d9202e25d74eebbed5" },
    { "Aiden Kwok", "SM-2026-193", "93fc265de7a4f6bb21d6f24dc65a502d861d2bb4d5dd1c3b979064bcd702f747" },
    { "Charlie", "SM-2026-197", "424e7255ff883931dc334f2ef8b32303625935856b640ac65907293f86179f85" },
    { "转化测试", "SM-2026-564", "dd34af5e266c7208797e2b2d3648af9790b0917dbda58b7067358e3029eb9342" },
    { "Eric 金家熠", "SM-2026-565", "4cc41779c7a70d6475de87bb0f1267f5dfbceede55e39c137ae36c9b6aac5e3d" },
    { "许赞", "SM-2026-590", "57df3e137ba7806f184d86d9638a564d8fd0b01014aea929856421a7afa6ad56" },
    { "Ewan赵亦宏", "SM-2026-591", "4e457adfda790dc91fe91ab6f392765d358bf5ed5f18fd4cca064242cef8a14d" },
    { "侯思远", "SM-2026-592", "d73c9562f637aad5146298a0e348e9632e0fca096a581ec76ba720e12960b68d" },
    { "浠浠", "SM-2026-593", "0eea2457d7e28cda7b0ec85bf35aa620a7c8096f1924fdff8e125e3eed6075d8" },
    { "林俊宇", "SM-2026-718", "3983033faf33269d4325b614e49511b50940fe7bc369c517f09a96423fce77ff" },
    { "向子珅", "SM-2026-790", "0a4fd74feb9fc69f797ea8e74f2da1cf7838bd7f833852199589e87471cb8065" },
    { "郭怀骏", "SM-2026-884", "843a0039f1c2d626e7b0a048918a56639aed59c127a1b2017338b84e51e8846d" },
    { "李政贤", "SM-2026-956", "0efdcb3ffac0869f4978bd6b74335d03357d15d3b21dccfcd51dfb9b3d49a93b" },
    { "唐嘉著", "SM-2026-967", "fee19d80753c1131c06a2368ae12a4db824032ab98183e7514e349891a5576ab" },
    { "创才学霸演示生", "SM-2026-CCXB-DEMO", "a9fd3e15284dc834272d9944e641555cd46f5b9ca30391872003094f298e27c9" },
    { "演示同学", "SM-2026-DEMO", "f719d638e1823c84f9d2a447951b181ec139eb1895f74f49b5cca65d7844fd3a" },
    { "Woody Du", "SM-2026-NEW", "248645c47b2a4a779c4333dd7f5bf8942df924aa41ec61ef64490dd6128fbf4a" },
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

