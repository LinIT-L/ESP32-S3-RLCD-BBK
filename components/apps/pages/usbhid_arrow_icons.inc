/* ============================================================
 * usbhid_arrow_icons.inc — USB 键鼠页的方向箭头字形 (8×12 1bpp, MSB first, 1=黑)
 *
 * 原先内联在 page_usb_hid.c 里 (ARROW_UP/DOWN/LEFT/RIGHT, 全大写宏名、无尺寸标记)。
 * 抽成独立资源文件并按统一命名规范改名为 usbhid_arrow_<方向>_8x12。
 * 数据一个字节都没改, 只是搬家 + 改名。
 * ============================================================ */
#ifndef USBHID_ARROW_ICONS_INC
#define USBHID_ARROW_ICONS_INC

#include <stdint.h>

#define USBHID_ARROW_W 8
#define USBHID_ARROW_H 12

static const uint8_t usbhid_arrow_up_8x12[12] = {
    0x18,0x3C,0x7E,0x7E,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,
};
static const uint8_t usbhid_arrow_down_8x12[12] = {
    0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x7E,0x7E,0x3C,0x18,0x18,
};
static const uint8_t usbhid_arrow_left_8x12[12] = {
    0x00,0x00,0x18,0x18,0x38,0xF8,0xF8,0x38,0x18,0x18,0x00,0x00,
};
static const uint8_t usbhid_arrow_right_8x12[12] = {
    0x00,0x00,0x18,0x18,0x1C,0x1F,0x1F,0x1C,0x18,0x18,0x00,0x00,
};

#endif /* USBHID_ARROW_ICONS_INC */
