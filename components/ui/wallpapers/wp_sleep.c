/**
 * wp_sleep.c — 深睡（休眠到点）时留在屏幕上的那张图。
 *
 * 为什么需要它:
 *   ST7305 是记忆屏 —— 断电后**画面还留着**。原来关眼前是把屏刷白, 于是休眠时
 *   屏幕一片白; 现在换成"黑底 + 居中一幅图", 黑底在反射屏上看着就是关机, 图是唯一亮点。
 *
 * 两条路径的区别（由调用方 sleep.c 判断, 本文件只负责画）:
 *   · 休眠到点 (AUTO)     → 画这张图
 *   · 长按手动关机 (MANUAL) → 一律刷白, 什么都不显示
 *
 * ⛔ 图本体 (wp_sleep_default.h) 由 tools/gen_wp_1bpp.py 生成, 别手工改数据。
 *    (该工具支持 --rle 压缩到 31%, 需要时再开; 现按 linit 要求存未压缩原图。)
 * ⛔ 深睡期间不可能有动画: 画完这一帧 CPU 就断电了, "动态"只能在睡前这一帧做。
 */
#include "wallpapers.h"
#include "wp_sleep_default.h"
#include "esp_log.h"

void wp_sleep_wallpaper_draw(st7305_handle_t *lcd)
{
    if (!lcd) return;

    /* 整屏黑: 图以外的地方全黑 (反射屏上等于"没内容") */
    st7305_clear(lcd, ST7305_COLOR_BLACK);

    /* 图按原始像素 1:1 贴, 只做居中, 不缩放 (尺寸由生成器保证) */
    const int x = (ST7305_WIDTH  - WP_SLEEP_DEFAULT_W) / 2;
    const int y = (ST7305_HEIGHT - WP_SLEEP_DEFAULT_H) / 2;
    st7305_blit_1bit_white(lcd, x, y, WP_SLEEP_DEFAULT_W, WP_SLEEP_DEFAULT_H,
                           wp_sleep_default);

    st7305_set_full_window(lcd);        /* 保证刷整屏 (局部窗口可能被上一页留着) */
    st7305_flush_override(lcd);
    ESP_LOGI("PWR", "休眠壁纸已铺屏 (%dx%d @ %d,%d)", WP_SLEEP_DEFAULT_W, WP_SLEEP_DEFAULT_H, x, y);
}
