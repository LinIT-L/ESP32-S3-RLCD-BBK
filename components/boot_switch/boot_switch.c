/*
 * boot_switch.c — 双系统(双固件)开机切换
 *
 * 原理: 用 esp_ota_set_boot_partition 写入 otadata 选择下次启动分区, 再 esp_restart.
 *   - 切到 Meshtastic: esp_ota_set_boot_partition(ota_0) → 重启进 ota_0
 *   - 切回 BBK:       esp_ota_set_boot_partition(factory) → 重启进 factory
 * esp_ota_set_boot_partition 内部会做镜像校验 (image_validate), 分区为空/镜像无效会
 * 失败并返回错误, 不会进到 esp_restart —— 保证"未烧 Meshtastic 时点击只报错不跳转"。
 */
#include "boot_switch.h"

#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_log.h"

static const char *TAG = "BOOT_SWITCH";

/* 按 app 子类型找分区 */
static const esp_partition_t *boot_switch_find_app(esp_partition_subtype_t st) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, st, NULL);
}

bool boot_switch_meshtastic_ready(void) {
    /* 只判断 ota_0 分区是否存在; 是否真有有效镜像由 esp_ota_set_boot_partition 校验 */
    return boot_switch_find_app(ESP_PARTITION_SUBTYPE_APP_OTA_0) != NULL;
}

esp_err_t boot_switch_to_meshtastic(void) {
    const esp_partition_t *p = boot_switch_find_app(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    if (!p) return ESP_ERR_NOT_FOUND;
    esp_err_t e = esp_ota_set_boot_partition(p);   /* 内部校验镜像, 无效则失败返回 */
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "切到 ota_0 失败: %d (可能未烧 Meshtastic)", e);
        return e;
    }
    ESP_LOGI(TAG, "已设下次启动=ota_0(Meshtastic), 重启中...");
    esp_restart();
    return ESP_OK;   /* 不返回 */
}

esp_err_t boot_switch_to_bbk(void) {
    const esp_partition_t *p = boot_switch_find_app(ESP_PARTITION_SUBTYPE_APP_FACTORY);
    if (!p) return ESP_ERR_NOT_FOUND;
    esp_err_t e = esp_ota_set_boot_partition(p);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "切回 factory 失败: %d", e);
        return e;
    }
    ESP_LOGI(TAG, "已设下次启动=factory(BBK), 重启中...");
    esp_restart();
    return ESP_OK;   /* 不返回 */
}