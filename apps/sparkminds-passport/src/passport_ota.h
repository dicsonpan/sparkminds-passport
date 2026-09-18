#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 触发安全的 OTA 固件更新
 * @param url 固件下载直链 (支持 HTTP / HTTPS)
 * @param expected_md5_hex 期望的 MD5 校验和 (32位十六进制小写字符串，若为 NULL 则跳过 MD5 校验)
 * @param expected_size 期望的固件字节数 (若为 0 则按 Content-Length 判定)
 * @return ESP_OK 成功启动后台任务，其他为错误码
 */
esp_err_t passport_ota_start(const char *url, const char *expected_md5_hex, uint32_t expected_size);

/**
 * @brief 本地/快速测试入口（携带 v1.0.1 固件包专属校验指纹）
 */
esp_err_t passport_ota_start_local_test(const char *url);

/**
 * @brief 查询 OTA 是否处于下载/写入处理中
 */
bool passport_ota_is_busy(void);

/**
 * @brief 获取当前下载状态与进度
 */
uint32_t passport_ota_get_downloaded(void);
uint32_t passport_ota_get_total(void);
int passport_ota_get_progress_percent(void);
const char *passport_ota_get_status_text(void);

/**
 * @brief 云端固件版本元数据
 */
typedef struct {
    char version[32];
    char url[256];
    char md5[33];
    uint32_t size;
    char notes[64];
    bool checked;       // 是否已成功获取云端元数据
    bool checking;      // 是否正在向云端发起查询
    bool has_update;    // 是否有新版本可用
} passport_ota_info_t;

/**
 * @brief 获取云端版本检测信息缓存
 */
const passport_ota_info_t *passport_ota_get_info(void);

/**
 * @brief 异步向云端检查最新固件版本
 */
esp_err_t passport_ota_check_version_async(void);

/**
 * @brief 启动从云端探测到的最新版本升级
 */
esp_err_t passport_ota_start_latest(void);

#ifdef __cplusplus
}
#endif
