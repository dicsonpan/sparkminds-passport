// main/passport_store.c —— "smpass" 命名空间的唯一读写模块 (ARCS-MINI lisa_kv 适配版)。
#include "passport_store.h"

#include "lisa_log.h"
#include "lisa_kv.h"
#include <string.h>

static const char *TAG = "smpass_store";

esp_err_t passport_store_init(void)
{
    // lisa_kv 已经在系统启动时初始化
    return ESP_OK;
}

esp_err_t passport_store_get_string(const char *key, char *buf, size_t len)
{
    char *out_str = NULL;
    int err = lisa_kv_get_string(key, &out_str);
    if (err != 0 || !out_str) return ESP_FAIL;
    strncpy(buf, out_str, len - 1);
    buf[len - 1] = '\0';
    lisa_kv_free(out_str);
    return ESP_OK;
}

esp_err_t passport_store_set_string(const char *key, const char *val)
{
    int err = lisa_kv_set_string(key, val);
    return (err == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t passport_store_get_i32(const char *key, int32_t *out)
{
    int val = 0;
    int err = lisa_kv_get_int(key, &val);
    if (err == 0) {
        *out = (int32_t)val;
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t passport_store_set_i32(const char *key, int32_t val)
{
    int err = lisa_kv_set_int(key, (int)val);
    return (err == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t passport_store_get_u8(const char *key, uint8_t *out)
{
    int val = 0;
    int err = lisa_kv_get_int(key, &val);
    if (err == 0) {
        *out = (uint8_t)val;
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t passport_store_set_u8(const char *key, uint8_t val)
{
    int err = lisa_kv_set_int(key, (int)val);
    return (err == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t passport_store_get_avatar(uint8_t **out_data, size_t *out_len)
{
    int len = 0;
    int err = lisa_kv_get_blob(PASSPORT_KEY_AVATAR_BLOB, out_data, &len);
    if (err != 0 || !*out_data) return ESP_FAIL;
    if (out_len) *out_len = len;
    return ESP_OK;
}

esp_err_t passport_store_set_avatar(const uint8_t *data, size_t len)
{
    int err = lisa_kv_set_blob(PASSPORT_KEY_AVATAR_BLOB, (uint8_t*)data, len);
    return (err == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t passport_store_del_avatar(void)
{
    lisa_kv_del(PASSPORT_KEY_AVATAR_BLOB);
    return ESP_OK;
}

void passport_store_free_avatar(uint8_t *data)
{
    if (data) lisa_kv_free(data);
}

bool passport_store_is_provisioned(void)
{
    char pid[32];
    uint8_t secret[PASSPORT_SECRET_LEN];
    bool ok = passport_store_get_string(PASSPORT_KEY_PID, pid, sizeof(pid)) == ESP_OK &&
              passport_store_get_secret(secret) == ESP_OK;
    memset(secret, 0, sizeof(secret));
    return ok;
}

esp_err_t passport_store_get_secret(uint8_t out[PASSPORT_SECRET_LEN])
{
    uint8_t *data = NULL;
    int len = 0;
    int err = lisa_kv_get_blob(PASSPORT_KEY_SECRET, &data, &len);
    if (err != 0 || !data) return ESP_FAIL;
    if (len != PASSPORT_SECRET_LEN) {
        lisa_kv_free(data);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, data, PASSPORT_SECRET_LEN);
    lisa_kv_free(data);
    return ESP_OK;
}

esp_err_t passport_store_provision(const char *pid,
                                   const uint8_t secret[PASSPORT_SECRET_LEN])
{
    int err = lisa_kv_set_string(PASSPORT_KEY_PID, pid);
    if (err == 0) {
        err = lisa_kv_set_blob(PASSPORT_KEY_SECRET, (uint8_t*)secret, PASSPORT_SECRET_LEN);
    }
    return (err == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t passport_store_erase_all(void)
{
    // lisa_kv doesn't have an erase_all for a namespace easily exposed here without iterating.
    // For now, delete known keys.
    lisa_kv_del(PASSPORT_KEY_PID);
    lisa_kv_del(PASSPORT_KEY_SECRET);
    lisa_kv_del(PASSPORT_KEY_PP_TOKEN);
    lisa_kv_del(PASSPORT_KEY_BALANCE);
    lisa_kv_del(PASSPORT_KEY_NAME);
    lisa_kv_del(PASSPORT_KEY_STUDENT_ID);
    lisa_kv_del(PASSPORT_KEY_AVATAR_URL);
    lisa_kv_del(PASSPORT_KEY_AVATAR_BLOB);
    lisa_kv_del(PASSPORT_KEY_IN_LAB);
    lisa_kv_del(PASSPORT_KEY_WIFI1_SSID);
    lisa_kv_del(PASSPORT_KEY_WIFI1_PASS);
    lisa_kv_del(PASSPORT_KEY_WIFI2_SSID);
    lisa_kv_del(PASSPORT_KEY_WIFI2_PASS);
    lisa_kv_del(PASSPORT_KEY_LAB_SSID);
    lisa_kv_del(PASSPORT_KEY_LAST_CHECKIN);
    LISA_LOGW(TAG, "已擦除已知键 (模拟恢复出厂)");
    return ESP_OK;
}
