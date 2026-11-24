/* min_boot.c — 最小系统诊断入口（只保留 LCD + 触摸）
 *
 * 目的: 换板/换固件后仍发烫时，把固件裁到"只剩屏幕和触摸"，先判定
 *       "是硬件本身热，还是某个外设/服务在耗电"；再逐项加回定位。
 *
 * 进入: 正常系统串口 `minboot 1` → 重启（NVS: lintos_diag/minboot=1）
 * 退出: 最小系统串口 `minboot 0` → 重启
 * 加功能: `add <掩码>`（可叠加，只增不减；重启复位）
 *   0x01 WiFi · 0x02 音频(codec+I2S) · 0x04 SD · 0x08 GPS · 0x10 震动
 *
 * 刻意**不初始化**: 蓝牙、振动(默认)、GPS、音频、电池、OS/页面/应用中心/Lua/商店/
 *   屏保监督/性能监视/状态栏/快捷栏/文件网页服务 —— 它们在 app_main 里都在本函数之后。
 * app_board_init 顺带挂的 SD 会立刻卸载；音频只做 PA_EN 拉低。
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "app_board.h"
#include "st7305.h"
#include "input.h"
#include "sd_scan.h"
#include "audio_player.h"
#include "wifi_manager.h"
#include "vibrator.h"
#include "gps.h"
#include "remcon.h"        /* 最小系统里也开串口命令: tasks/pm/wifi status/add/minboot */
#include "min_boot.h"

static const char *TAG = "MINBOOT";
static uint32_t     s_mask = 0;
static app_board_t *s_b    = NULL;

/* 温度统一走 os 层 (os_temp.c): 性能监视器与串口 temp 共用同一个传感器实例,
 * 全局只 install 一次, 不会因为两处各装一遍而冲突。 */
int min_boot_temp10(void) { return os_temp_c10(); }
static int min_temp10(void) { return os_temp_c10(); }

bool min_boot_requested(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("lintos_diag", NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, "minboot", &v);
        nvs_close(h);
    }
    return v == 1;
}

void min_boot_set_flag(int on)
{
    nvs_handle_t h;
    if (nvs_open("lintos_diag", NVS_READWRITE, &h) != ESP_OK) {
        printf("MINBOOT NVS 打不开, 标志未写\r\n");
        return;
    }
    nvs_set_u8(h, "minboot", on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
    printf("MINBOOT flag=%d (重启生效)\r\n", on ? 1 : 0);
}

/* 触摸按钮: 拔掉 USB 用电池测时也能逐项加回 (不用串口) */
static const struct { uint32_t bit; int x0, x1, y0, y1; } BTN[] = {
    { 0x01,   8, 128, 228, 250 },   /* WiFi  */
    { 0x02, 136, 256, 228, 250 },   /* Audio */
    { 0x04, 264, 384, 228, 250 },   /* SD    */
    { 0x08,   8, 128, 254, 276 },   /* GPS   */
    { 0x10, 136, 256, 254, 276 },   /* Vib   */
    { 0x100,264, 384, 254, 276 },   /* Exit → 回正常 UI */
};

static bool min_hit_btn(int x, int y, uint32_t *bit)
{
    for (unsigned i = 0; i < sizeof(BTN) / sizeof(BTN[0]); i++) {
        if (x >= BTN[i].x0 && x <= BTN[i].x1 && y >= BTN[i].y0 && y <= BTN[i].y1) {
            *bit = BTN[i].bit;
            return true;
        }
    }
    return false;
}

static void min_draw(int tx, int ty, int taps)
{
    if (!s_b) return;
    char l[72];
    st7305_clear(&s_b->lcd, ST7305_COLOR_WHITE);
    st7305_draw_text(&s_b->lcd, 10, 10, "MIN BOOT  (LCD + TOUCH ONLY)");

    int t10 = min_temp10();
    if (t10 > -1000) {
        snprintf(l, sizeof l, "temp %d.%d C    max %d.%d C",
                 t10 / 10, t10 % 10, os_temp_max_c10() / 10, os_temp_max_c10() % 10);
    } else {
        snprintf(l, sizeof l, "temp n/a");
    }
    st7305_draw_text(&s_b->lcd, 10, 34, l);

    snprintf(l, sizeof l, "on=0x%02X", (unsigned)s_mask);
    st7305_draw_text(&s_b->lcd, 10, 58, l);
    snprintf(l, sizeof l, "touch x=%d y=%d  taps=%d", tx, ty, taps);
    st7305_draw_text(&s_b->lcd, 10, 82, l);
    st7305_draw_text(&s_b->lcd, 10, 106, "tap a box to add that part");

    st7305_draw_text(&s_b->lcd, 8,   234, (s_mask & 0x01) ? "[x] WiFi"  : "[ ] WiFi");
    st7305_draw_text(&s_b->lcd, 136, 234, (s_mask & 0x02) ? "[x] Audio" : "[ ] Audio");
    st7305_draw_text(&s_b->lcd, 264, 234, (s_mask & 0x04) ? "[x] SD"    : "[ ] SD");
    st7305_draw_text(&s_b->lcd, 8,   260, (s_mask & 0x08) ? "[x] GPS"   : "[ ] GPS");
    st7305_draw_text(&s_b->lcd, 136, 260, (s_mask & 0x10) ? "[x] Vib"   : "[ ] Vib");
    st7305_draw_text(&s_b->lcd, 264, 260, "Exit -> UI");
    st7305_flush(&s_b->lcd);
}

void min_boot_add(uint32_t want)
{
    want &= 0x1Fu;
    if (!s_b) {
        printf("MINBOOT 当前不是最小系统, add 无效\r\n");
        return;
    }
    if ((want & 0x01) && !(s_mask & 0x01)) {
        bool ok = wifi_manager_enable();
        printf("MINBOOT +WiFi %d\r\n", ok ? 1 : 0);
    }
    if ((want & 0x02) && !(s_mask & 0x02)) {
        int r = audio_player_init();
        printf("MINBOOT +Audio %d\r\n", r);
    }
    if ((want & 0x04) && !(s_mask & 0x04)) {
        esp_err_t r = sd_mount();
        printf("MINBOOT +SD %d\r\n", (int)r);
    }
    if ((want & 0x08) && !(s_mask & 0x08)) {
        app_board_init_gps(s_b);
        printf("MINBOOT +GPS (初始化后送待机)\r\n");
    }
    if ((want & 0x10) && !(s_mask & 0x10)) {
        vibrator_init();
        printf("MINBOOT +Vib (仅初始化, 不震)\r\n");
    }
    s_mask = want;
    printf("MINBOOT mask=0x%02X\r\n", (unsigned)s_mask);
}

void min_boot_run(app_board_t *b)
{
    s_b = b;
    ESP_LOGW(TAG, "===== 最小系统: 只保留 LCD + 触摸 =====");


    /* app_board_init 顺带做了 SD/输入/NVS/事件循环。这里把不用的立刻关掉。
     * 注意顺序: 先卸 SD, 再关功放 —— 功放 PA_EN 与 SD 无关, 但保持与深睡路径一致。 */
    sd_unmount();
    audio_pa_init();              /* PA_EN(GPIO46) 拉低, 功放断电 */
    wifi_manager_force_off();     /* 没 enable 过也安全 */

    /* 串口命令在最小系统里也要能用: tasks/pm/wifi status 是"确认真的关了"的证据,
     * add/minboot 是加功能与退出。remcon_init() 只建队列+任务, 不依赖 OS 页面。 */
    remcon_init();
    printf("MINBOOT 初态: wifi_enabled=%d sd_mounted=%d (都应为 0)\r\n",
           wifi_manager_is_enabled() ? 1 : 0, sd_is_mounted() ? 1 : 0);

    int taps = 0, tx = -1, ty = -1, lx = -999, ly = -999;
    bool down = false;
    uint32_t next_sd_chk = 0, next_temp = 0;
    min_draw(tx, ty, taps);

    for (;;) {
        uint32_t now = xTaskGetTickCount();

        /* SD 是 app_board_init 里的**后台任务**挂的 —— 上面那次 sd_unmount() 跑在它前面，
         * 所以卡最终还是会挂上（日志: MINBOOT 1087 → sd_mount 1182 → 成功 1333）。
         * 这里每 500ms 查一次，挂上就卸，保证"只剩屏幕和触摸"是真的。 */
        if (now >= next_sd_chk) {
            next_sd_chk = now + pdMS_TO_TICKS(500);
            if (sd_is_mounted()) {
                sd_unmount();
                printf("MINBOOT SD 已卸载 (后台挂载后)\r\n");
            }
        }

        /* 每 5 秒读一次片内温度: 串口一行 + 屏幕同步刷新 (拔线用电池时看屏幕) */
        if (now >= next_temp) {
            next_temp = now + pdMS_TO_TICKS(5000);
            int t10 = min_temp10();
            if (t10 > -1000) {
                printf("MINBOOT temp=%d.%dC max=%d.%dC mask=0x%02X\r\n",
                       t10 / 10, t10 % 10, os_temp_max_c10() / 10, os_temp_max_c10() % 10, (unsigned)s_mask);
            }
            min_draw(tx, ty, taps);
        }

        input_poll_touch();
        int x, y;
        if (input_get_touch_pos(&x, &y)) {
            if (!down) {                     /* 按下沿 */
                down = true;
                taps++;
                tx = x; ty = y;
                uint32_t bit = 0;
                if (min_hit_btn(x, y, &bit)) {
                    printf("MINBOOT btn 0x%02X @ %d,%d\r\n", (unsigned)bit, x, y);
                    if (bit == 0x100) {                 /* Exit: 清标志 + 重启回正常 UI */
                        printf("MINBOOT Exit -> 回正常 UI\r\n");
                        min_boot_set_flag(0);
                        vTaskDelay(pdMS_TO_TICKS(300));
                        esp_restart();
                    }
                    min_boot_add(s_mask | bit);
                } else {
                    printf("MINBOOT tap %d @ %d,%d\r\n", taps, x, y);
                }
                min_draw(tx, ty, taps);
            } else if (x != lx || y != ly) { /* 拖动 */
                lx = x; ly = y;
                tx = x; ty = y;
                min_draw(tx, ty, taps);
            }
        } else {
            down = false;
        }
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}
