#include "passport_ota.h"
#include "ota_flash.h"
#include "uboot_ota_api.h"
#include "power_manager.h"
#include "arcs_ap_base.h"
#include "lisa_http.h"
#include "cJSON.h"
#include "passport_ui.h"
#include "mbedtls/md5.h"

#include "lisa_log.h"
#include "lisa_mem.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "passport_ota";

static volatile bool s_ota_busy = false;
static volatile uint32_t s_downloaded = 0;
static volatile uint32_t s_total_size = 0;
static char s_status_text[64] = "空闲";
static char s_ota_url[256] = {0};
static char s_expected_md5[33] = {0};
static uint32_t s_expected_size = 0;

typedef struct {
    uint32_t expected_size;
    uint32_t downloaded;
    uint32_t last_log_kb;
    mbedtls_md5_context md5_ctx;
    bool has_expected_md5;
    uint8_t expected_md5_bytes[16];
    bool aborted;
    int abort_code;
} ota_download_ctx_t;

bool passport_ota_is_busy(void)
{
    return s_ota_busy;
}

uint32_t passport_ota_get_downloaded(void)
{
    return s_downloaded;
}

uint32_t passport_ota_get_total(void)
{
    return s_total_size;
}

int passport_ota_get_progress_percent(void)
{
    if (s_total_size == 0) {
        return 0;
    }
    uint32_t pct = (s_downloaded * 100) / s_total_size;
    return (pct > 100) ? 100 : (int)pct;
}

const char *passport_ota_get_status_text(void)
{
    return s_status_text;
}

static bool hex_to_bytes(const char *hex, uint8_t *bytes, size_t byte_len)
{
    if (!hex || strlen(hex) != byte_len * 2) {
        return false;
    }
    for (size_t i = 0; i < byte_len; i++) {
        char byte_str[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
        bytes[i] = (uint8_t)strtoul(byte_str, NULL, 16);
    }
    return true;
}

static void ota_http_on_data(lisa_http_data_t *data)
{
    ota_download_ctx_t *ctx = (ota_download_ctx_t *)data->user;
    if (!ctx || ctx->aborted) {
        return;
    }

    // 1. 首包校验：前 6 字节必须是合法 XZ 压缩包文件头魔数 (0xFD '7' 'z' 'X' 'Z' 0x00)
    if (ctx->downloaded == 0 && data->len >= 6) {
        const uint8_t xz_magic[6] = {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00};
        if (memcmp(data->buf, xz_magic, 6) != 0) {
            LISA_LOGE(TAG, "XZ Magic check failed! Not a valid .txz archive.");
            ctx->aborted = true;
            ctx->abort_code = -1;
            snprintf(s_status_text, sizeof(s_status_text), "固件格式非法");
            return;
        }
    }

    // 2. 写入 Flash 暂存区分区
    int ret = ota_flash_update_step(OTA_PART_APP_STAGING, ctx->downloaded, data->buf, data->len);
    if (ret < 0) {
        LISA_LOGE(TAG, "ota_flash_update_step failed at offset %u: %d", ctx->downloaded, ret);
        ctx->aborted = true;
        ctx->abort_code = ret;
        snprintf(s_status_text, sizeof(s_status_text), "Flash写入失败");
        return;
    }

    // 3. 实时计算 MD5
    if (ctx->has_expected_md5) {
        mbedtls_md5_update(&ctx->md5_ctx, (const unsigned char *)data->buf, data->len);
    }

    ctx->downloaded += data->len;
    s_downloaded = ctx->downloaded;

    uint32_t current_kb = ctx->downloaded / 1024;
    if (current_kb >= ctx->last_log_kb + 128) {
        ctx->last_log_kb = current_kb;
        LISA_LOGI(TAG, "OTA Download Progress: %u KB / %u KB", current_kb, ctx->expected_size / 1024);
        snprintf(s_status_text, sizeof(s_status_text), "下载中 %u%%", passport_ota_get_progress_percent());
    }
}

static void ota_worker_task(void *arg)
{
    (void)arg;
    s_ota_busy = true;
    s_downloaded = 0;
    s_total_size = s_expected_size;
    snprintf(s_status_text, sizeof(s_status_text), "连接下载中...");

    LISA_LOGI(TAG, "========================================");
    LISA_LOGI(TAG, "Starting Secure OTA Pipeline");
    LISA_LOGI(TAG, "URL: %s", s_ota_url);
    LISA_LOGI(TAG, "Expected Size: %u bytes", s_expected_size);
    if (strlen(s_expected_md5) > 0) {
        LISA_LOGI(TAG, "Expected MD5: %s", s_expected_md5);
    }
    LISA_LOGI(TAG, "========================================");

    ota_download_ctx_t ctx = {0};
    ctx.expected_size = s_expected_size;
    ctx.last_log_kb = 0;

    if (strlen(s_expected_md5) == 32) {
        if (hex_to_bytes(s_expected_md5, ctx.expected_md5_bytes, 16)) {
            ctx.has_expected_md5 = true;
            mbedtls_md5_init(&ctx.md5_ctx);
            mbedtls_md5_starts(&ctx.md5_ctx);
        }
    }

    int ret = ota_flash_update_begin(OTA_PART_APP_STAGING, OTA_FLASH_SIZE_UNKNOWN);
    if (ret < 0) {
        LISA_LOGE(TAG, "ota_flash_update_begin failed: %d", ret);
        snprintf(s_status_text, sizeof(s_status_text), "Flash初始化失败");
        s_ota_busy = false;
        vTaskDelete(NULL);
        return;
    }

    lisa_http_request_t req = {
        .method = LISA_HTTP_GET,
        .url = (uint8_t *)s_ota_url,
        .timeout = 30000,
        .body = NULL,
        .body_len = 0,
        .headers = NULL,
        .on_data = ota_http_on_data,
        .user = &ctx,
    };

    lisa_http_t *http = lisa_http_init(&req);
    if (!http) {
        LISA_LOGE(TAG, "lisa_http_init failed");
        ota_flash_update_abort(OTA_PART_APP_STAGING);
        snprintf(s_status_text, sizeof(s_status_text), "网络连接失败");
        s_ota_busy = false;
        vTaskDelete(NULL);
        return;
    }

    lisa_http_err_e http_err = lisa_http_download(http);
    lisa_http_cleanup(http);

    // 4. 传输完整性拦截校验
    if (http_err != LISA_HTTP_OK || ctx.aborted) {
        LISA_LOGE(TAG, "Download failed: http_err=%d, aborted=%d, got=%u bytes",
                  http_err, ctx.aborted, ctx.downloaded);
        ota_flash_update_abort(OTA_PART_APP_STAGING);
        snprintf(s_status_text, sizeof(s_status_text), "下载中断已取消");
        s_ota_busy = false;
        vTaskDelete(NULL);
        return;
    }

    // 5. 字节总数绝对匹配拦截
    if (ctx.expected_size > 0 && ctx.downloaded != ctx.expected_size) {
        LISA_LOGE(TAG, "Size mismatch: downloaded %u != expected %u! ABORTING to prevent brick.",
                  ctx.downloaded, ctx.expected_size);
        ota_flash_update_abort(OTA_PART_APP_STAGING);
        snprintf(s_status_text, sizeof(s_status_text), "大小不符已拦截");
        s_ota_busy = false;
        vTaskDelete(NULL);
        return;
    }

    // 6. MD5 指纹绝对匹配拦截
    if (ctx.has_expected_md5) {
        uint8_t actual_md5[16];
        mbedtls_md5_finish(&ctx.md5_ctx, actual_md5);

        if (memcmp(actual_md5, ctx.expected_md5_bytes, 16) != 0) {
            LISA_LOGE(TAG, "MD5 mismatch! Integrity check FAILED. ABORTING to prevent brick.");
            ota_flash_update_abort(OTA_PART_APP_STAGING);
            snprintf(s_status_text, sizeof(s_status_text), "MD5校验不符已拦截");
            s_ota_busy = false;
            vTaskDelete(NULL);
            return;
        }
        LISA_LOGI(TAG, "MD5 integrity check PASSED!");
    }

    // 7. 所有严格校验 100% 通过后，才允许提交给 U-Boot
    ret = ota_flash_update_finish(OTA_PART_APP_STAGING);
    if (ret < 0) {
        LISA_LOGE(TAG, "ota_flash_update_finish failed: %d", ret);
        snprintf(s_status_text, sizeof(s_status_text), "分区封包失败");
        s_ota_busy = false;
        vTaskDelete(NULL);
        return;
    }

    const void *mapped = NULL;
    ota_flash_get(OTA_PART_APP_STAGING, &mapped, NULL);
    uint32_t flash_offset = (uint32_t)mapped - CMN_FLASH_REGION;

    LISA_LOGI(TAG, "========================================");
    LISA_LOGI(TAG, "OTA Package 100%% Verified!");
    LISA_LOGI(TAG, "Total Size: %u bytes", ctx.downloaded);
    LISA_LOGI(TAG, "Flash Offset: 0x%08X", flash_offset);
    LISA_LOGI(TAG, "Registering request to U-Boot...");
    LISA_LOGI(TAG, "========================================");
    snprintf(s_status_text, sizeof(s_status_text), "校验通过，重启刷机");

    ret = uboot_ota_start_from_flash(flash_offset, ctx.downloaded);
    if (ret != 0) {
        LISA_LOGE(TAG, "uboot_ota_start_from_flash returned error: %d", ret);
        snprintf(s_status_text, sizeof(s_status_text), "Bootloader登记失败");
        s_ota_busy = false;
        vTaskDelete(NULL);
        return;
    }

    LISA_LOGI(TAG, "U-Boot request registered! Soft rebooting in 1.5s...");
    vTaskDelay(pdMS_TO_TICKS(1500));
    power_reboot_soft();

    s_ota_busy = false;
    vTaskDelete(NULL);
}

esp_err_t passport_ota_start(const char *url, const char *expected_md5_hex, uint32_t expected_size)
{
    if (s_ota_busy) {
        LISA_LOGW(TAG, "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    if (!url || strlen(url) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_ota_url, url, sizeof(s_ota_url) - 1);
    s_ota_url[sizeof(s_ota_url) - 1] = '\0';

    if (expected_md5_hex) {
        strncpy(s_expected_md5, expected_md5_hex, sizeof(s_expected_md5) - 1);
        s_expected_md5[sizeof(s_expected_md5) - 1] = '\0';
    } else {
        s_expected_md5[0] = '\0';
    }

    s_expected_size = expected_size;

    BaseType_t res = xTaskCreate(ota_worker_task, "pp_ota", 8192 / sizeof(StackType_t), NULL, 5, NULL);
    if (res != pdPASS) {
        LISA_LOGE(TAG, "Failed to create OTA task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t passport_ota_start_local_test(const char *url)
{
    // 测试固件专属安全指纹：
    // 文件大小：1,869,136 字节
    // MD5 指纹：20dcdd392f5fb1e037430d276312114e
    return passport_ota_start(url, "20dcdd392f5fb1e037430d276312114e", 1869136);
}

#define PASSPORT_OTA_INFO_URL "https://api.sparkminds.io/api/files/avatars/ota_info.jpg"

typedef struct {
    char buf[1024];
    size_t len;
} ota_info_buffer_t;

static passport_ota_info_t s_ota_info = {
    .version = "",
    .url = "",
    .md5 = "",
    .size = 0,
    .notes = "",
    .checked = false,
    .checking = false,
    .has_update = false,
};

static void ota_info_http_on_data(lisa_http_data_t *data)
{
    ota_info_buffer_t *ib = (ota_info_buffer_t *)data->user;
    if (!ib || !data || !data->buf || data->len == 0) {
        return;
    }
    if (ib->len + data->len < sizeof(ib->buf) - 1) {
        memcpy(ib->buf + ib->len, data->buf, data->len);
        ib->len += data->len;
        ib->buf[ib->len] = '\0';
    }
}

static void ota_check_worker_task(void *arg)
{
    (void)arg;
    s_ota_info.checking = true;

    char url_with_cb[160];
    uint32_t ts = (uint32_t)xTaskGetTickCount();
    snprintf(url_with_cb, sizeof(url_with_cb), "%s?t=%u", PASSPORT_OTA_INFO_URL, (unsigned int)ts);
    LISA_LOGI(TAG, "Checking cloud OTA version from: %s", url_with_cb);

    ota_info_buffer_t ib = {0};
    lisa_http_request_t req = {
        .method = LISA_HTTP_GET,
        .url = (uint8_t *)url_with_cb,
        .timeout = 10000,
        .body = NULL,
        .body_len = 0,
        .headers = NULL,
        .on_data = ota_info_http_on_data,
        .user = &ib,
    };

    lisa_http_t *http = lisa_http_init(&req);
    if (!http) {
        LISA_LOGE(TAG, "lisa_http_init failed for OTA version check");
        s_ota_info.checking = false;
        vTaskDelete(NULL);
        return;
    }

    lisa_http_err_e http_err = lisa_http_download(http);
    lisa_http_cleanup(http);

    if (http_err != LISA_HTTP_OK || ib.len == 0) {
        LISA_LOGW(TAG, "OTA version check request failed: err=%d, len=%u", http_err, ib.len);
        s_ota_info.checking = false;
        vTaskDelete(NULL);
        return;
    }

    LISA_LOGI(TAG, "OTA Cloud Info Response: %s", ib.buf);

    cJSON *root = cJSON_Parse(ib.buf);
    if (!root) {
        LISA_LOGE(TAG, "Failed to parse OTA info JSON");
        s_ota_info.checking = false;
        vTaskDelete(NULL);
        return;
    }

    cJSON *item_ver = cJSON_GetObjectItem(root, "version");
    cJSON *item_url = cJSON_GetObjectItem(root, "url");
    cJSON *item_md5 = cJSON_GetObjectItem(root, "md5");
    cJSON *item_size = cJSON_GetObjectItem(root, "size");
    cJSON *item_notes = cJSON_GetObjectItem(root, "notes");

    if (item_ver && cJSON_IsString(item_ver) && item_url && cJSON_IsString(item_url)) {
        strncpy(s_ota_info.version, item_ver->valuestring, sizeof(s_ota_info.version) - 1);
        strncpy(s_ota_info.url, item_url->valuestring, sizeof(s_ota_info.url) - 1);
        if (item_md5 && cJSON_IsString(item_md5)) {
            strncpy(s_ota_info.md5, item_md5->valuestring, sizeof(s_ota_info.md5) - 1);
        } else {
            s_ota_info.md5[0] = '\0';
        }
        if (item_size && cJSON_IsNumber(item_size)) {
            s_ota_info.size = (uint32_t)item_size->valueint;
        } else {
            s_ota_info.size = 0;
        }
        if (item_notes && cJSON_IsString(item_notes)) {
            strncpy(s_ota_info.notes, item_notes->valuestring, sizeof(s_ota_info.notes) - 1);
        }

        s_ota_info.checked = true;
        // 比较版本：云端版本与当前固件版本 PASSPORT_FW_VERSION 比较
        if (strcmp(s_ota_info.version, PASSPORT_FW_VERSION) != 0) {
            s_ota_info.has_update = true;
            LISA_LOGI(TAG, "Found new version on cloud: %s (Current: %s)", s_ota_info.version, PASSPORT_FW_VERSION);
        } else {
            s_ota_info.has_update = false;
            LISA_LOGI(TAG, "Firmware is up-to-date: %s", PASSPORT_FW_VERSION);
        }
    } else {
        LISA_LOGE(TAG, "Invalid OTA info schema");
    }

    cJSON_Delete(root);
    s_ota_info.checking = false;
    vTaskDelete(NULL);
}

const passport_ota_info_t *passport_ota_get_info(void)
{
    return &s_ota_info;
}

esp_err_t passport_ota_check_version_async(void)
{
    if (s_ota_info.checking) {
        LISA_LOGW(TAG, "OTA version check already running");
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t res = xTaskCreate(ota_check_worker_task, "ota_chk", 4096 / sizeof(StackType_t), NULL, 4, NULL);
    if (res != pdPASS) {
        LISA_LOGE(TAG, "Failed to create ota_check task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t passport_ota_start_latest(void)
{
    if (!s_ota_info.checked || strlen(s_ota_info.url) == 0) {
        LISA_LOGW(TAG, "No cloud OTA info available yet, checking first...");
        return passport_ota_check_version_async();
    }
    LISA_LOGI(TAG, "Starting OTA to cloud version: %s (%u bytes)", s_ota_info.version, s_ota_info.size);
    return passport_ota_start(s_ota_info.url, s_ota_info.md5, s_ota_info.size);
}

#include "shell.h"

static int shell_ota_cmd(int argc, char *argv[])
{
    if (argc > 1 && strcmp(argv[1], "check") == 0) {
        LISA_LOGI(TAG, "Shell: checking cloud OTA version");
        return passport_ota_check_version_async();
    } else if (argc > 1 && strcmp(argv[1], "update") == 0) {
        LISA_LOGI(TAG, "Shell: triggering update to latest version");
        return passport_ota_start_latest();
    }
    const char *url = (argc > 1) ? argv[1] : "https://api.sparkminds.io/api/files/avatars/ota_v101.jpg";
    LISA_LOGI(TAG, "Shell triggered OTA: %s", url);
    return passport_ota_start_local_test(url);
}

SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0) | SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN) | SHELL_CMD_DISABLE_RETURN,
                 ota, shell_ota_cmd, "ota [check|update|url]");

