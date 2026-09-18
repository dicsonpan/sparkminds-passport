// main/passport_net.c —— 网络任务实现 (ARCS-MINI 适配版)。
// 突发式工作流（实现文档 7.1/7.5 节）的极简版，依赖系统自动连接 Wi-Fi 和同步时间。
#include "passport_net.h"

#include "passport_core.h"
#include "passport_store.h"
#include "passport_ui.h"

#include "lisa_log.h"
#include "lisa_mem.h"
#include "sys_wifi.h"
#include "HTTPCUsr_api.h"
#include "lisa_http.h"
#include "mbedtls/base64.h"
#include "cJSON.h" // 替换 jsmn，因为 SDK 内置了 cJSON
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "semphr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "smpass_net";

#ifndef CONFIG_PASSPORT_API_BASE
#define CONFIG_PASSPORT_API_BASE "https://api.sparkminds.io/api"
#endif

#define HTTP_TIMEOUT_MS          10000
#define HTTP_BUF_SIZE            8192
#define NET_TASK_STACK           8192
#define NET_REQ_QUEUE_LEN        4

static QueueHandle_t   s_ui_queue;
static QueueHandle_t   s_req_queue;
static TaskHandle_t    s_task;
static volatile bool   s_busy;

static SemaphoreHandle_t s_history_lock;
static passport_point_item_t s_history[PASSPORT_HISTORY_MAX];
static int                 s_history_count;

static char *s_http_buf; // 突发期间分配

// UI 事件投递
static void post_ui(passport_ui_evt_type_t type, int32_t arg1)
{
    if (!s_ui_queue) return;
    passport_ui_evt_t evt = { .type = type, .arg1 = arg1 };
    xQueueSend(s_ui_queue, &evt, 0);
}

bool passport_net_is_busy(void)
{
    return s_busy;
}

int passport_net_history(passport_point_item_t out[PASSPORT_HISTORY_MAX])
{
    int n = 0;
    if (xSemaphoreTake(s_history_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        n = s_history_count;
        if (n > 0) memcpy(out, s_history, sizeof(passport_point_item_t) * n);
        xSemaphoreGive(s_history_lock);
    }
    return n;
}

// ---------------------------------------------------------------------------
// HTTPS 客户端 (适配 lisa_http)
// ---------------------------------------------------------------------------
typedef struct {
    char  *buf;
    size_t len;
} http_sink_t;

static char *g_auth_bearer = NULL;
static void *http_headers_cb(void)
{
    static char header_buf[256];
    if (g_auth_bearer) {
        snprintf(header_buf, sizeof(header_buf), "Content-Type:application/json&Authorization:Bearer %s", g_auth_bearer);
    } else {
        snprintf(header_buf, sizeof(header_buf), "Content-Type:application/json");
    }
    return (void *)header_buf;
}

// 成功（2xx）返回 ESP_OK 且 sink.buf 为响应体；401 返回 ESP_ERR_INVALID_STATE
// 以便调用方重走 auth；其他返回 ESP_FAIL。
static esp_err_t http_json(HTTP_VERB method, const char *path,
                           const char *bearer, const char *body,
                           http_sink_t *sink)
{
    HTTPParameters *http_param = lisa_mem_calloc(1, sizeof(HTTPParameters));
    if (!http_param) return ESP_ERR_NO_MEM;

    snprintf(http_param->Uri, sizeof(http_param->Uri), "%s%s", CONFIG_PASSPORT_API_BASE, path);
    http_param->HttpVerb = method;
    http_param->nTimeout = HTTP_TIMEOUT_MS;
    if (body) {
        http_param->pData = (void *)body;
        http_param->pLength = strlen(body);
    }

    g_auth_bearer = (char *)bearer; // 供 http_headers_cb 使用

    int ret = HTTPC_open(http_param);
    if (ret != 0) {
        lisa_mem_free(http_param);
        return ESP_FAIL;
    }

    ret = HTTPC_request(http_param, http_headers_cb);
    if (ret != 0) {
        HTTPC_close(http_param);
        lisa_mem_free(http_param);
        return ESP_FAIL;
    }

    HTTP_CLIENT http_client = {0};
    if (HTTPC_get_request_info(http_param, &http_client) != 0) {
        HTTPC_close(http_param);
        lisa_mem_free(http_param);
        return ESP_FAIL;
    }

    int status = (int)http_client.HTTPStatusCode;

    sink->len = 0;
    if (sink->buf) sink->buf[0] = '\0';

    if (http_client.TotalResponseBodyLength > 0 && sink->buf) {
        UINT32 received = 0;
        unsigned int readsize = 0;
        do {
            unsigned int to_read = http_client.TotalResponseBodyLength - readsize;
            if (to_read > (HTTP_BUF_SIZE - 1 - sink->len)) {
                to_read = HTTP_BUF_SIZE - 1 - sink->len;
            }
            if (to_read == 0) break;

            if (HTTPC_read(http_param, sink->buf + sink->len, to_read, (void *)&received) != 0) {
                sink->len += received;
                break;
            }
            if (received == 0) break;
            sink->len += received;
            readsize += received;
        } while (readsize < http_client.TotalResponseBodyLength);
        sink->buf[sink->len] = '\0';
    }

    HTTPC_close(http_param);
    lisa_mem_free(http_param);

    if (status == 401) return ESP_ERR_INVALID_STATE;
    if (status < 200 || status >= 300) {
        LISA_LOGW(TAG, "HTTP %d -> %d", method, status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// 业务调用
// ---------------------------------------------------------------------------

// auth 签名换 token
static esp_err_t do_auth(void)
{
    char pid[32];
    uint8_t secret[PASSPORT_SECRET_LEN];
    esp_err_t err = passport_store_get_string(PASSPORT_KEY_PID, pid, sizeof(pid));
    if (err != ESP_OK) return err;
    err = passport_store_get_secret(secret);
    if (err != ESP_OK) return err;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint32_t ts = (uint32_t)tv.tv_sec;
    char sig[PASSPORT_SIG_HEX_LEN + 1];
    passport_auth_sig(secret, pid, ts, sig);

    char body[160];
    snprintf(body, sizeof(body),
             "{\"passport_id\":\"%s\",\"ts\":%lu,\"sig\":\"%s\"}",
             pid, (unsigned long)ts, sig);
    memset(secret, 0, sizeof(secret));

    http_sink_t sink = { .buf = s_http_buf, .len = 0 };
    err = http_json(VerbPost, "/passport/auth", NULL, body, &sink);
    memset(body, 0, sizeof(body));
    memset(sig, 0, sizeof(sig));
    if (err != ESP_OK) return err;

    cJSON *root = cJSON_Parse(s_http_buf);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    cJSON *token = cJSON_GetObjectItem(root, "token");
    if (cJSON_IsString(token)) {
        passport_store_set_string(PASSPORT_KEY_PP_TOKEN, token->valuestring);
    } else {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *balance = cJSON_GetObjectItem(root, "points_balance");
    if (cJSON_IsNumber(balance)) passport_store_set_i32(PASSPORT_KEY_BALANCE, balance->valueint);

    cJSON *student = cJSON_GetObjectItem(root, "student");
    if (cJSON_IsObject(student)) {
        cJSON *name = cJSON_GetObjectItem(student, "name");
        if (cJSON_IsString(name)) {
            passport_store_set_string(PASSPORT_KEY_NAME, name->valuestring);
        }
        cJSON *sid = cJSON_GetObjectItem(student, "student_id");
        if (cJSON_IsString(sid)) {
            if (strcmp(sid->valuestring, pid) != 0) {
                LISA_LOGE(TAG, "student_id 与 pid 不一致，停止同步");
                cJSON_Delete(root);
                return ESP_ERR_INVALID_RESPONSE;
            }
            passport_store_set_string(PASSPORT_KEY_STUDENT_ID, sid->valuestring);
        }

        cJSON *avatar = cJSON_GetObjectItem(student, "avatar_url");
        if (cJSON_IsString(avatar) && avatar->valuestring && avatar->valuestring[0]) {
            passport_store_set_string(PASSPORT_KEY_AVATAR_URL, avatar->valuestring);
            const char *b64 = strstr(avatar->valuestring, "base64,");
            if (b64) b64 += 7; else b64 = avatar->valuestring;
            size_t b64_len = strlen(b64);
            size_t out_len = 0;
            unsigned char *jpg_buf = lisa_mem_alloc(b64_len); // max output is < b64_len
            if (jpg_buf) {
                if (mbedtls_base64_decode(jpg_buf, b64_len, &out_len, (const unsigned char *)b64, b64_len) == 0) {
                    passport_store_set_avatar(jpg_buf, out_len);
                }
                lisa_mem_free(jpg_buf);
            }
        } else if (cJSON_IsNull(avatar)) {
            passport_store_set_string(PASSPORT_KEY_AVATAR_URL, "");
            passport_store_del_avatar();
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

// 带 Bearer 的调用；401 时重 auth 并重试一次
static esp_err_t http_authed(HTTP_VERB method, const char *path,
                             const char *body, http_sink_t *sink)
{
    char token[96];
    if (passport_store_get_string(PASSPORT_KEY_PP_TOKEN, token, sizeof(token)) != ESP_OK ||
        token[0] == '\0') {
        esp_err_t err = do_auth();
        if (err != ESP_OK) return err;
        if (passport_store_get_string(PASSPORT_KEY_PP_TOKEN, token, sizeof(token)) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    esp_err_t err = http_json(method, path, token, body, sink);
    if (err == ESP_ERR_INVALID_STATE) {   // 401：token 失效
        err = do_auth();
        if (err != ESP_OK) return err;
        if (passport_store_get_string(PASSPORT_KEY_PP_TOKEN, token, sizeof(token)) != ESP_OK) {
            return ESP_FAIL;
        }
        err = http_json(method, path, token, body, sink);
    }
    return err;
}

static void stamp_balance_time(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm *tm_now = localtime(&now);
    if (!tm_now) return;
    char stamp[64];
    snprintf(stamp, sizeof(stamp), "%02d-%02d %02d:%02d",
             tm_now->tm_mon + 1, tm_now->tm_mday, tm_now->tm_hour, tm_now->tm_min);
    passport_store_set_string(PASSPORT_KEY_BALANCE_TS, stamp);
}

static void cache_balance(int32_t balance)
{
    passport_store_set_i32(PASSPORT_KEY_BALANCE, balance);
    stamp_balance_time();
}

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t max_len;
} avatar_buffer_t;

static void avatar_http_on_data(lisa_http_data_t *data)
{
    avatar_buffer_t *ab = (avatar_buffer_t *)data->user;
    if (ab && ab->buf && (ab->len + data->len <= ab->max_len)) {
        memcpy(ab->buf + ab->len, data->buf, data->len);
        ab->len += data->len;
    }
}

static esp_err_t download_avatar(const char *url)
{
    LISA_LOGI(TAG, "Downloading avatar with lisa_http: %s", url);

    avatar_buffer_t ab = {
        .buf = lisa_mem_alloc(65536),
        .len = 0,
        .max_len = 65536
    };
    if (!ab.buf) return ESP_ERR_NO_MEM;

    lisa_http_request_t req = {
        .method = LISA_HTTP_GET,
        .url = (uint8_t *)url,
        .timeout = 10000,
        .body = NULL,
        .body_len = 0,
        .headers = NULL,
        .on_data = avatar_http_on_data,
        .user = &ab,
    };

    lisa_http_t *http = lisa_http_init(&req);
    if (!http) {
        LISA_LOGE(TAG, "lisa_http_init failed for avatar download");
        lisa_mem_free(ab.buf);
        return ESP_FAIL;
    }

    lisa_http_err_e http_err = lisa_http_download(http);
    lisa_http_cleanup(http);

    esp_err_t err = ESP_FAIL;
    if (http_err == LISA_HTTP_OK && ab.len > 0) {
        passport_store_set_avatar(ab.buf, ab.len);
        LISA_LOGI(TAG, "Avatar successfully saved: %u bytes", ab.len);
        post_ui(PASSPORT_UI_EVT_SYNC_OK, 0);
        err = ESP_OK;
    } else {
        LISA_LOGW(TAG, "Avatar download failed: err=%d, len=%u", http_err, ab.len);
    }
    lisa_mem_free(ab.buf);
    return err;
}

// /passport/me
static esp_err_t do_me(void)
{
    http_sink_t sink = { .buf = s_http_buf, .len = 0 };
    esp_err_t err = http_authed(VerbGet, "/passport/me", NULL, &sink);
    if (err != ESP_OK) return err;

    cJSON *root = cJSON_Parse(s_http_buf);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    cJSON *bi = cJSON_GetObjectItem(root, "points_balance");
    if (cJSON_IsNumber(bi)) cache_balance(bi->valueint);

    cJSON *ni = cJSON_GetObjectItem(root, "name");
    if (cJSON_IsString(ni)) {
        passport_store_set_string(PASSPORT_KEY_NAME, ni->valuestring);
    }
    cJSON *ii = cJSON_GetObjectItem(root, "in_lab");
    if (cJSON_IsBool(ii) || cJSON_IsNumber(ii)) {
        uint8_t in_lab = cJSON_IsTrue(ii) || (cJSON_IsNumber(ii) && ii->valueint) ? 1 : 0;
        passport_store_set_u8(PASSPORT_KEY_IN_LAB, in_lab);
    }

    cJSON *ai = cJSON_GetObjectItem(root, "avatar_url");
    if (cJSON_IsString(ai) && ai->valuestring && ai->valuestring[0]) {
        passport_store_set_string(PASSPORT_KEY_AVATAR_URL, ai->valuestring);
        if (strncmp(ai->valuestring, "http", 4) == 0) {
            download_avatar(ai->valuestring);
        } else {
            char full_url[256];
            snprintf(full_url, sizeof(full_url), "https://api.sparkminds.io%s%s", ai->valuestring[0] == '/' ? "" : "/", ai->valuestring);
            download_avatar(full_url);
        }
    } else {
        // Fallback: 如果服务端未返回 avatar_url 或为空，尝试通过当前 PID 默认路径下载
        char pid[32] = {0};
        if (passport_store_get_string(PASSPORT_KEY_PID, pid, sizeof(pid)) == ESP_OK && pid[0]) {
            char full_url[256];
            snprintf(full_url, sizeof(full_url), "https://api.sparkminds.io/api/files/avatars/%s.jpg", pid);
            LISA_LOGI(TAG, "avatar_url is empty, fallback to: %s", full_url);
            if (download_avatar(full_url) == ESP_OK) {
                passport_store_set_string(PASSPORT_KEY_AVATAR_URL, full_url);
            }
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// /passport/points/history?limit=5
static esp_err_t do_history(void)
{
    http_sink_t sink = { .buf = s_http_buf, .len = 0 };
    esp_err_t err = http_authed(VerbGet, "/passport/points/history?limit=5", NULL, &sink);
    if (err != ESP_OK) return err;

    cJSON *root = cJSON_Parse(s_http_buf);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    cJSON *arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "items");
    if (!arr) arr = cJSON_GetObjectItem(root, "history");
    
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (xSemaphoreTake(s_history_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        cJSON_Delete(root);
        return ESP_OK;
    }
    s_history_count = 0;
    int items = cJSON_GetArraySize(arr);
    for (int i = 0; i < items && i < PASSPORT_HISTORY_MAX; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(item)) break;

        cJSON *di = cJSON_GetObjectItem(item, "delta");
        if (!di) di = cJSON_GetObjectItem(item, "points");
        cJSON *ri = cJSON_GetObjectItem(item, "reason");
        if (!ri) ri = cJSON_GetObjectItem(item, "title");

        s_history[i].delta = cJSON_IsNumber(di) ? di->valueint : 0;
        s_history[i].text[0] = '\0';
        if (cJSON_IsString(ri)) {
            strlcpy(s_history[i].text, ri->valuestring, sizeof(s_history[i].text));
        }
        s_history_count++;
    }
    xSemaphoreGive(s_history_lock);
    cJSON_Delete(root);
    return ESP_OK;
}

static void today_str(char out[64])
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm *tm_now = localtime(&now);
    if (!tm_now) {
        out[0] = '\0';
        return;
    }
    snprintf(out, 64, "%04d-%02d-%02d",
             tm_now->tm_year + 1900, tm_now->tm_mon + 1, tm_now->tm_mday);
}



// 在馆心跳
static esp_err_t do_heartbeat(void)
{
    http_sink_t sink = { .buf = s_http_buf, .len = 0 };
    esp_err_t err = http_authed(VerbPost, "/passport/heartbeat", "{}", &sink);
    if (err != ESP_OK) return err;

    cJSON *root = cJSON_Parse(s_http_buf);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    cJSON *ii = cJSON_GetObjectItem(root, "in_lab");
    if (cJSON_IsBool(ii) || cJSON_IsNumber(ii)) {
        uint8_t in_lab = cJSON_IsTrue(ii) || (cJSON_IsNumber(ii) && ii->valueint) ? 1 : 0;
        passport_store_set_u8(PASSPORT_KEY_IN_LAB, in_lab);
    }
    cJSON *bi = cJSON_GetObjectItem(root, "points_balance");
    if (cJSON_IsNumber(bi)) cache_balance(bi->valueint);
    
    cJSON_Delete(root);
    return ESP_OK;
}

// POST /passport/check-in
static esp_err_t do_checkin(void)
{
    http_sink_t sink = { .buf = s_http_buf, .len = 0 };
    esp_err_t err = http_authed(VerbPost, "/passport/check-in", "{}", &sink);
    if (err != ESP_OK) return err;

    cJSON *root = cJSON_Parse(s_http_buf);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    cJSON *awarded = cJSON_GetObjectItem(root, "awarded");
    cJSON *balance_after = cJSON_GetObjectItem(root, "balance_after");
    cJSON *reason = cJSON_GetObjectItem(root, "reason");

    if (cJSON_IsNumber(balance_after)) {
        cache_balance(balance_after->valueint);
    }

    if (cJSON_IsNumber(awarded) && awarded->valueint > 0) {
        LISA_LOGI(TAG, "每日打卡成功！获得 %d 积分", awarded->valueint);
    } else if (cJSON_IsString(reason)) {
        LISA_LOGI(TAG, "每日打卡结果: %s", reason->valuestring);
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm *tm_now = localtime(&now);
    if (tm_now) {
        char today_buf[16];
        snprintf(today_buf, sizeof(today_buf), "%04d-%02d-%02d",
                 tm_now->tm_year + 1900, tm_now->tm_mon + 1, tm_now->tm_mday);
        passport_store_set_string(PASSPORT_KEY_LAST_CHECKIN, today_buf);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// 任务循环
// ---------------------------------------------------------------------------
static void run_burst(passport_net_req_t req)
{
    s_busy = true;
    s_http_buf = lisa_mem_alloc(HTTP_BUF_SIZE);
    if (!s_http_buf) {
        post_ui(PASSPORT_UI_EVT_NET_IDLE, 0);
        s_busy = false;
        return;
    }

    if (!sys_wifi_is_ready()) {
        LISA_LOGW(TAG, "sys_wifi_is_ready() == false, 放弃突发请求");
        if (req == PASSPORT_NET_REQ_SYNC) post_ui(PASSPORT_UI_EVT_SYNC_FAIL, 0);
        goto done;
    }

    esp_err_t err = ESP_OK;
    switch (req) {
    case PASSPORT_NET_REQ_SYNC:
        // 开机或连网同步时，先发一次心跳确立/续期在馆状态
        do_heartbeat();
        err = do_me();
        if (err == ESP_OK) {
            do_checkin();
            do_history();
        }
        post_ui(err == ESP_OK ? PASSPORT_UI_EVT_SYNC_OK : PASSPORT_UI_EVT_SYNC_FAIL, 0);
        break;

    case PASSPORT_NET_REQ_HEARTBEAT:
        err = do_heartbeat();
        if (err == ESP_OK) post_ui(PASSPORT_UI_EVT_HEARTBEAT_OK, 0);
        break;

    case PASSPORT_NET_REQ_CHECKIN:
        err = do_checkin();
        if (err == ESP_OK) do_history();
        post_ui(err == ESP_OK ? PASSPORT_UI_EVT_SYNC_OK : PASSPORT_UI_EVT_SYNC_FAIL, 0);
        break;

    default:
        break;
    }

done:
    lisa_mem_free(s_http_buf);
    s_http_buf = NULL;
    post_ui(PASSPORT_UI_EVT_NET_IDLE, 0);
    s_busy = false;
}

static void net_task(void *arg)
{
    for (;;) {
        passport_net_req_t req = 0;
        if (xQueueReceive(s_req_queue, &req, portMAX_DELAY) != pdTRUE) continue;
        
        passport_net_req_t extra;
        while (xQueueReceive(s_req_queue, &extra, 0) == pdTRUE) {
            if (extra != req) {
                if (extra == PASSPORT_NET_REQ_SYNC && req == PASSPORT_NET_REQ_HEARTBEAT) req = extra;
            }
        }
        
        if (!passport_store_is_provisioned()) continue;
        run_burst(req);
    }
}

esp_err_t passport_net_init(QueueHandle_t ui_queue)
{
    s_ui_queue = ui_queue;
    s_req_queue = xQueueCreate(NET_REQ_QUEUE_LEN, sizeof(passport_net_req_t));
    s_history_lock = xSemaphoreCreateMutex();
    if (!s_req_queue || !s_history_lock) return ESP_ERR_NO_MEM;
    if (xTaskCreate(net_task, "pp_net", NET_TASK_STACK / sizeof(StackType_t), NULL, 5, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void passport_net_request(passport_net_req_t req)
{
    if (s_req_queue) xQueueSend(s_req_queue, &req, 0);
}

#include "shell.h"
static int shell_avatar_cmd(int argc, char *argv[])
{
    const char *url = (argc > 1) ? argv[1] : "https://api.sparkminds.io/api/files/avatars/SM-2026-070.jpg?t=1789449846979";
    printf("Starting avatar test download: %s\n", url);
    esp_err_t err = download_avatar(url);
    printf("download_avatar returned: %d\n", err);
    return (err == ESP_OK) ? 0 : -1;
}
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 avatar_dl, shell_avatar_cmd, "avatar_dl [url]");


