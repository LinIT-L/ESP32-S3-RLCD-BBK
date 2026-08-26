#include "bbk_boot.h"
#include <string.h>
#include <math.h>
#include "esp_attr.h"

#define LOGO_W 400
#define LOGO_H 300

/* V1.0.46: 开机画面使用"电子词典"图片 (400x300 1bpp, 由 tools 脚本从 PNG 生成).
 * 数组放在 flash (const), 不再占用 PSRAM. */
#include "bbk_boot_logo.inc"

const int bbk_boot_logo_w = LOGO_W;
const int bbk_boot_logo_h = LOGO_H;

const uint8_t *bbk_boot_logo = s_boot_logo_img;
