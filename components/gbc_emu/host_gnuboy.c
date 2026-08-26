/* host_gnuboy.c — esp-box-emu gnuboy 所需的宿主函数 (原由 espp/BoxEmu 提供).
 * 本项目自己驱动帧循环/音频/显示, 这些钩子全部降级为空实现. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gnuboy/gnuboy.h"

static const char *TAG = "gnuboy_host";

void die(char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[160];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "die: %s", buf);
    /* 与 esp-box-emu 一致: 不终止模拟, 避免坏 ROM 直接重启设备 */
}

void doevents(void) {}
void ev_poll(void) {}

void vid_close(void) {}
void vid_preinit(void) {}
void vid_begin(void) {}
void vid_end(void) {}
void vid_setpal(int i, int r, int g, int b)
{
    (void)i; (void)r; (void)g; (void)b;
}
void vid_settitle(char *title) { (void)title; }

void sys_sleep(int us)
{
    if (us > 0) vTaskDelay(pdMS_TO_TICKS((uint32_t)((us + 999) / 1000)));
}

void *sys_timer(void)
{
    return (void *)(intptr_t)esp_timer_get_time();
}

int sys_elapsed(void *in_ptr)
{
    int64_t t0 = (int64_t)(intptr_t)in_ptr;
    return (int)(esp_timer_get_time() - t0);
}

void sys_checkdir(char *path, int wr) { (void)path; (void)wr; }
void sys_sanitize(char *s) { (void)s; }
void sys_initpath(char *exe) { (void)exe; }

void pcm_init(void) {}
int  pcm_submit(void) { return 0; }
void pcm_close(void) {}
