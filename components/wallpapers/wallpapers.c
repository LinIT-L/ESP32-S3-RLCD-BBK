/* 内置壁纸程序 (V4, 2026-08-26): 仅保留星空.
 * 星空由 menu_system 屏保 (screensaver_render_stars) 实现;
 * 本库仅保留帧缓冲管理与接口, 其余 7 个动态壁纸 (楼梯/粒子钟/二进制/生命/
 * 克苏鲁/电路板/麦浪) 与天气时钟已按用户要求全部删除. */
#include "wallpapers.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#define WP_W ST7305_WIDTH
#define WP_H ST7305_HEIGHT
#define WP_ROW ((WP_W + 7) / 8)          /* 50 */
#define WP_FB_BYTES (WP_ROW * WP_H)

static uint8_t *s_fb = NULL;

static void wp_ensure_fb(void) {
    if (!s_fb)
        s_fb = heap_caps_malloc(WP_FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

const char *wp_prog_name(int id) {
    (void)id;
    return "\xe6\x98\x9f\xe7\xa9\xba";   /* 星空 */
}

void wp_program_render(st7305_handle_t *lcd, int prog, uint32_t now_ms) {
    (void)lcd; (void)prog; (void)now_ms;
    /* 仅保留星空: 实际星空渲染由 menu_system 屏保负责.
     * 此处仅确保帧缓冲存在以保持接口兼容 (壁纸页/屏保调用方). */
    wp_ensure_fb();
}

void wp_release_buffers(void) {
    if (s_fb) {
        free(s_fb);
        s_fb = NULL;
    }
}
