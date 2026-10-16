/**
 * os_hid.c — 系统 USB HID 键鼠 API 实现 (包装 usb_hid + 管家硬件调度).
 *
 * 与 os_wifi.c 一致: start 内 os_hw_request(OS_HW_USB_HID) + usb_hid_start,
 * 失败则回退 release; stop 内 usb_hid_stop + os_hw_release. 页面只调 os_hid_*,
 * 不再手动管理 USB 复合设备硬件生命周期, 避免绕过管家导致的状态残留.
 */
#include "os_hid.h"
#include "usb_hid.h"
#include "os_hw.h"

esp_err_t os_hid_start(void) {
    os_hw_request(OS_HW_USB_HID);
    esp_err_t e = usb_hid_start();
    if (e != ESP_OK) os_hw_release(OS_HW_USB_HID);  /* 启动失败回退, 不泄漏引用 */
    return e;
}

void os_hid_stop(void) {
    usb_hid_stop();
    os_hw_release(OS_HW_USB_HID);
}

bool os_hid_is_running(void) {
    return usb_hid_is_running();
}

bool os_hid_connected(void) {
    return usb_hid_connected();
}

void os_hid_key_tap(uint8_t modifier, uint8_t keycode) {
    usb_hid_key_tap(modifier, keycode);
}

void os_hid_key_release(void) {
    usb_hid_key_release();
}

void os_hid_mouse_move(int8_t dx, int8_t dy) {
    usb_hid_mouse_move(dx, dy);
}

void os_hid_mouse_click(uint8_t buttons) {
    usb_hid_mouse_click(buttons);
}

void os_hid_mouse_release(uint8_t buttons) {
    usb_hid_mouse_release(buttons);
}

bool os_hid_cdc_connected(void) {
    return usb_hid_cdc_connected();
}

size_t os_hid_cdc_read(uint8_t *data, size_t len) {
    return usb_hid_cdc_read(data, len);
}
