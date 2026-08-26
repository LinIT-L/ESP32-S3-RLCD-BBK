/*
 * boot_switch.h — 双系统(双固件)开机切换
 *
 * 通过 OTA otadata 在 factory(BBK 掌机) 与 ota_0(Meshtastic 独立固件) 之间
 * 切换下次启动分区, 供两个独立固件互相"重启切系统"用:
 *   - BBK 主菜单 Meshtastic 快捷方式: boot_switch_to_meshtastic()
 *   - Meshtastic 固件「退出回步步高」:  boot_switch_to_bbk()
 *
 * 依赖 ESP-IDF 分区表同时存在 factory + ota_0 + otadata(见 partitions.csv).
 */
#ifndef BOOT_SWITCH_H
#define BOOT_SWITCH_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ota_0 (Meshtastic) 是否已烧入有效启动镜像 */
bool boot_switch_meshtastic_ready(void);

/* 重启切到 Meshtastic (ota_0). 未烧录/校验失败返回错误, 不重启 */
esp_err_t boot_switch_to_meshtastic(void);

/* 重启切回 BBK (factory). 供 Meshtastic 固件退出时调用 */
esp_err_t boot_switch_to_bbk(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_SWITCH_H */