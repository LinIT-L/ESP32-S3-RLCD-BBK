/*
 * SPDX-FileCopyrightText: 2026
 *
 * USB MSC 挂载到电脑 - 纯 MSC (标准 U盘) + esp_tinyusb v2 官方 storage API.
 *
 * 架构 ("切换"方案, 与虚拟键鼠一致):
 *   - 正常模式: USB Serial/JTAG 串口控制台, 不装 TinyUSB.
 *   - 挂载时: 先暂停串口控制台 (log_console_suspend, 只写文件日志) → 切到纯 MSC
 *     设备 (usb_composite_enter_msc) → tinyusb_msc_new_storage_sdmmc 注册 TF 卡.
 *   - 退出时: tinyusb_msc_delete_storage → usb_composite_exit 切回正常模式 → 恢复串口.
 *   - 挂载期间串口控制台被暂停, 日志只写 /sdcard/log/bbk.log, 彻底避免写已卸载串口.
 *   - 调用方 (page_storage) 负责: 挂载前 sd_unmount_vfs_keep_card (释放 VFS),
 *     卸载后 sd_remount_vfs_from_card (重挂本机).
 */
#include "usbh_msc_sdspi.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"
#include "tinyusb_msc.h"
#include "usb_hid.h"

extern sdmmc_card_t *sd_get_card(void);   /* sd_scan 提供: 当前已初始化的卡 */
extern void log_console_suspend(bool suspend);   /* main.c: 挂载期间暂停串口控制台 */

static const char *TAG = "USBH_MSC";
static bool s_running = false;                       /* 是否已挂载到电脑 */
static bool s_driver_installed = false;
static tinyusb_msc_storage_handle_t s_storage = NULL;

/* 启动 USB MSC (PC 看到 U盘): 暂停串口 → 切纯 MSC 设备 → 创建 storage (USB 模式).
 * 调用方负责先 sd_unmount_vfs_keep_card() (释放 VFS, 保留 card). */
esp_err_t usbh_msc_start(void) {
    if (s_running) return ESP_OK;

    log_console_suspend(true);   /* 挂载期间只写文件日志, 不碰串口 */

    /* 1) 切换到纯 MSC (U盘) 设备 (卸载 Serial/JTAG, 装 TinyUSB, 全程压制日志) */
    esp_err_t ret = usb_composite_enter_msc();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "进入挂载设备失败: %s", esp_err_to_name(ret));
        log_console_suspend(false);
        return ret;
    }

    /* 2) 安装 MSC storage driver (关闭自动切换, 由 UI 显式控制) */
    if (!s_driver_installed) {
        const tinyusb_msc_driver_config_t dc = {
            .user_flags = { .auto_mount_off = 1 },
        };
        ret = tinyusb_msc_install_driver(&dc);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "tinyusb_msc_install_driver 失败: %s", esp_err_to_name(ret));
            usb_composite_exit();
            log_console_suspend(false);
            return ret;
        }
        s_driver_installed = true;
    }

    /* 3) 创建 storage: 直接暴露给 PC, 不本地挂载 */
    if (s_storage == NULL) {
        const tinyusb_msc_storage_config_t cfg = {
            .medium.card = sd_get_card(),
            .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        };
        ret = tinyusb_msc_new_storage_sdmmc(&cfg, &s_storage);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "tinyusb_msc_new_storage_sdmmc 失败: %s", esp_err_to_name(ret));
            usb_composite_exit();
            log_console_suspend(false);
            return ret;
        }
    }

    s_running = true;
    ESP_LOGI(TAG, "[USB MSC] 已挂载到电脑 (纯 MSC U盘, 串口已暂停)");
    return ESP_OK;
}

/* 停止 USB MSC: 删除 storage, 切回正常模式, 恢复串口.
 * 调用方随后 sd_remount_vfs_from_card(). */
void usbh_msc_stop(void) {
    if (!s_running) return;
    if (s_storage) {
        esp_err_t r = tinyusb_msc_delete_storage(s_storage);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "tinyusb_msc_delete_storage 失败: %s", esp_err_to_name(r));
        }
        s_storage = NULL;
    }
    s_running = false;
    /* 切回正常模式 (卸 TinyUSB, 装回 Serial/JTAG) */
    usb_composite_exit();
    log_console_suspend(false);   /* 恢复串口控制台 */
    ESP_LOGI(TAG, "[USB MSC] 已卸载 (已恢复 Serial/JTAG 控制台)");
}

bool usbh_msc_is_running(void) { return s_running; }
