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
#include "tusb_tasks.h"
#include "class/msc/msc_device.h"
#include "class/hid/hid.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "USBH_MSC";
static bool s_running = false;
static bool s_jtag_was_running = false;

/* ---- USB MSC 配置描述符 (仅 MSC 接口) ----
 * 关键: 固件启用了 CONFIG_TINYUSB_HID_COUNT=2 (仿真键鼠).
 * esp_tinyusb 的 descriptors_control.c 在 CFG_TUD_HID>0 时要求调用方必须
 * 显式提供 configuration_descriptor, 否则 tinyusb_driver_install 返回
 * ESP_ERR_INVALID_ARG ("Configuration descriptor must be provided for this device")
 * → USB 存储模式提示"挂载失败". 原项目 HID_COUNT=0 走默认描述符所以正常.
 * 这里显式提供仅含 MSC 接口的描述符, 与 usb_hid 的 HID 描述符互不干扰. */
#define MSC_EP_IN   0x82
#define MSC_EP_OUT  0x01
#define MSC_IF_NUM  0

#define MSC_CONFIG_DESC_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t msc_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, MSC_CONFIG_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),
    TUD_MSC_DESCRIPTOR(MSC_IF_NUM, 0, MSC_EP_OUT, MSC_EP_IN, 64),
};

/* 字符串描述符: [0]=langid, [1]=manufacturer, [2]=product, [3]=serial */
static const char *msc_string_desc_arr[] = {
    (char[]){0x09, 0x04}, "Espressif Systems", "BBK SD Card Reader", "123456",
};

/* 启动 USB MSC (PC 看到 SD 卡)
 *
 * 重要: ESP32-S3 的 USB Serial/JTAG 和 USB OTG 共用 GPIO 19/20
 * 启动 OTG MSC 之前必须先卸掉 Serial/JTAG, 释放引脚
 * 退出 MSC 时再装回来, 这样默认还能从 USB 串口看日志 */
esp_err_t usbh_msc_start(void) {
    if (s_running) return ESP_OK;
    ESP_LOGI(TAG, "[USB MSC] 准备启动 tinyusb...");

    /* 若仿真键鼠 (USB HID) 正在运行, 先停止它: HID 与 MSC 都走 tinyusb + OTG,
     * 二者不能并存. 且 HID 运行时会先卸载 Serial/JTAG, 若不先停 HID,
     * 下面再次 uninstall Serial/JTAG 会报 "uninstall without install called". */
    extern bool usb_hid_is_running(void);
    extern void usb_hid_stop(void);
    if (usb_hid_is_running()) {
        ESP_LOGI(TAG, "[USB MSC] 检测到 USB HID 运行, 先停止以释放 OTG");
        usb_hid_stop();
    }

    /* Serial/JTAG 在系统启动时已默认安装. 但经 HID/MSC 切换 + 重启后未必在装.
     * 这里无条件调用 uninstall: 驱动已装则真正卸载释放 GPIO; 未装时 IDF 实现
     * 只打 warning 并返回 ESP_OK, 不会报错. (旧版用 is_driver_installed 判断,
     * 因日志走 ROM secondary console 而误判"未安装"跳过卸载, 导致 ROM console
     * 仍占用 USB PHY, tinyusb 切 OTG 失败 → "挂载失败".) */
    ESP_LOGI(TAG, "[USB MSC] 卸载 USB Serial/JTAG (释放 GPIO 19/20)");
    s_jtag_was_running = usb_serial_jtag_is_driver_installed();
    esp_err_t jret = usb_serial_jtag_driver_uninstall();
    if (jret != ESP_OK) {
        ESP_LOGE(TAG, "卸载 Serial/JTAG 失败: %s (0x%x)", esp_err_to_name(jret), jret);
        return jret;
    }

    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
        .self_powered = true,
        .vbus_monitor_io = -1,
        .device_descriptor = NULL,                 /* 用 esp_tinyusb 默认设备描述符 */
        .configuration_descriptor = msc_config_desc, /* 必须显式提供 (HID_COUNT>0 时) */
        .string_descriptor = msc_string_desc_arr,
        .string_descriptor_count = 4,
    };
    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install 失败: %s (0x%x)", esp_err_to_name(ret), ret);
        /* 失败时把 Serial/JTAG 装回来 */
        if (s_jtag_was_running) {
            usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
            usb_serial_jtag_driver_install(&cfg);
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
    /* tusb_stop_task + tusb_deinit: 彻底释放 tusb 栈 (任务/DCD/队列/互斥锁).
     * 不调则 tud_inited() 仍为 true, 下次 tinyusb_driver_install 跳过 DCD 重配
     * → 二次打开键鼠/MSC 失效. 与 usb_hid_stop 保持一致. */
    tusb_stop_task();
    tusb_deinit(0);
    tinyusb_driver_uninstall();
    s_running = false;
    /* 关键修复 (esp-idf #9826/#15912, 对齐 usb_hid 已验证模式):
     * 旧版在这里显式 usb_new_phy(USB_PHY_CTRL_SERIAL_JTAG) 去"切回"PHY 控制器,
     * 但拿到的 jtag_phy_hdl 只丢弃从不 usb_del_phy 释放, 会把 IDF 的 fsls PHY 单例
     * 永久占成 IN_USE. 一旦用过一次 USB 存储, 之后仿真键鼠 (tinyusb_driver_install
     * 内部 usb_new_phy(OTG)) 就会报 "selected PHY is in use" 而失败闪退.
     * v5.5.5 的 tinyusb_driver_uninstall() 内部已 usb_del_phy 正确释放 OTG PHY,
     * 这里只需安全地重装 USB Serial/JTAG 驱动恢复串口即可, 不能再手动切 PHY. */
    /* 同一引导内恢复串口: 重装 USB-Serial/JTAG 驱动即可, 无需重启 */
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t sj_ret = usb_serial_jtag_driver_install(&cfg);
    if (sj_ret == ESP_OK) {
        ESP_LOGI(TAG, "[USB MSC] USB Serial/JTAG 驱动已重装, 串口已恢复 (无需重启)");
    } else {
        ESP_LOGW(TAG, "[USB MSC] 重装 USB Serial/JTAG 驱动失败: %s", esp_err_to_name(sj_ret));
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