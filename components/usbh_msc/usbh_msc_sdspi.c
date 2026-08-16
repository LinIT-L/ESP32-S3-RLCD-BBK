/*
 * SPDX-FileCopyrightText: 2026
 *
 * USB MSC device 适配层 - 通过 SDSPI 直接读写 SD 卡扇区.
 *
 * 调用方负责先 unmount VFS (释放 SPI 总线), 然后调 usbh_msc_start();
 * 退出时调 usbh_msc_stop() 重新挂载 VFS.
 *
 * esp_tinyusb 启动时已自动创建 tinyusb task, 我们直接实现
 * tud_msc_*_cb 弱符号回调.
 */
#include "usbh_msc_sdspi.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "class/msc/msc_device.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "USBH_MSC";
static bool s_running = false;
static bool s_jtag_was_running = false;

/* 启动 USB MSC (PC 看到 SD 卡)
 *
 * 重要: ESP32-S3 的 USB Serial/JTAG 和 USB OTG 共用 GPIO 19/20
 * 启动 OTG MSC 之前必须先卸掉 Serial/JTAG, 释放引脚
 * 退出 MSC 时再装回来, 这样默认还能从 USB 串口看日志 */
esp_err_t usbh_msc_start(void) {
    if (s_running) return ESP_OK;
    ESP_LOGI(TAG, "[USB MSC] 准备启动 tinyusb...");

    /* Serial/JTAG 在系统启动时已默认安装, 这里直接卸掉即可 */
    ESP_LOGI(TAG, "[USB MSC] 卸载 USB Serial/JTAG (释放 GPIO 19/20)");
    esp_err_t jret = usb_serial_jtag_driver_uninstall();
    if (jret != ESP_OK) {
        ESP_LOGE(TAG, "卸载 Serial/JTAG 失败: %s (0x%x)", esp_err_to_name(jret), jret);
        return jret;
    }
    s_jtag_was_running = true;

    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
        .self_powered = true,
        .vbus_monitor_io = -1,
    };
    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install 失败: %s (0x%x)", esp_err_to_name(ret), ret);
        /* 失败时把 Serial/JTAG 装回来 */
        if (s_jtag_was_running) {
            usb_serial_jtag_driver_install(NULL);
        }
        return ret;
    }
    s_running = true;
    ESP_LOGI(TAG, "USB MSC 已启动, 请连接电脑 USB (D+/D- = GPIO 20/19)");
    return ESP_OK;
}

/* 停止 USB MSC (释放 OTG, 恢复 Serial/JTAG) */
void usbh_msc_stop(void) {
    if (!s_running) return;
    ESP_LOGI(TAG, "[USB MSC] 停止 tinyusb...");
    tinyusb_driver_uninstall();
    s_running = false;
    /* 把 Serial/JTAG 装回来, 这样日志又能从 USB 串口看 */
    if (s_jtag_was_running) {
        ESP_LOGI(TAG, "[USB MSC] 恢复 USB Serial/JTAG 驱动");
        usb_serial_jtag_driver_install(NULL);
        s_jtag_was_running = false;
    }
}

bool usbh_msc_is_running(void) { return s_running; }

/* ----------- tinyusb MSC 回调 ----------- */

extern sdmmc_card_t *sd_get_card(void);
#define CARD (sd_get_card())

/* 设备就绪: 有 SD 卡 */
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    return CARD != NULL;
}

/* 容量: 512 字节扇区, 总扇区数由 sdmmc 提供 */
void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    if (CARD) {
        *block_size = (uint16_t)CARD->csd.sector_size;
        if (*block_size == 0) *block_size = 512;
        *block_count = CARD->csd.capacity;  /* 实际是 512B 扇区数 */
    } else {
        *block_size = 512;
        *block_count = 0;
    }
}

/* SCSI Inquiry: 厂商 + 产品 */
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    const char vid[] = "ESP32S3";
    const char pid[] = "SD Card Reader";
    const char rev[] = "1.0";
    memcpy(vendor_id, vid, sizeof(vid) > 8 ? 8 : sizeof(vid));
    memcpy(product_id, pid, sizeof(pid) > 16 ? 16 : sizeof(pid));
    memcpy(product_rev, rev, sizeof(rev) > 4 ? 4 : sizeof(rev));
}

/* 读扇区: lba 起始扇区, offset/bufsize 用于跨扇区 */
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    if (!CARD) return -1;
    if (offset != 0) {
        /* 跨扇区读: 大部分 MSC 控制器会先对齐 bufsize. 这里简化只支持 aligned. */
        return -1;
    }
    esp_err_t ret = sdmmc_read_sectors(CARD, buffer, lba, bufsize / 512);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read10 lba=%u 失败: %s", (unsigned)lba, esp_err_to_name(ret));
        return -1;
    }
    return (int32_t)bufsize;
}

/* 写扇区 */
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    if (!CARD) return -1;
    if (offset != 0) return -1;
    esp_err_t ret = sdmmc_write_sectors(CARD, buffer, lba, bufsize / 512);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "write10 lba=%u 失败: %s", (unsigned)lba, esp_err_to_name(ret));
        return -1;
    }
    return (int32_t)bufsize;
}

/* SCSI Start/Stop: 接受弹出/收回 */
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    return true;
}

/* SCSI 自定义命令分发: 我们不实现, 返回 -1 让 tinyusb STALL */
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void)lun; (void)scsi_cmd; (void)buffer; (void)bufsize;
    return -1;
}

/* 默认 lun 数 = 1 */
uint8_t tud_msc_get_maxlun_cb(void) { return 1; }

/* 可选: 允许介质移除 */
bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun, uint8_t prohibit_removal, uint8_t control) {
    (void)lun; (void)prohibit_removal; (void)control;
    return true;
}

/* 可写 */
bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return CARD != NULL;
}
