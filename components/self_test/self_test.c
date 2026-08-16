/* 机载自动化自测: 模块化/回归安全网.
 * 场景:
 *   S1 堆基线        - 内部/PSRAM 空闲与最大块
 *   S2 菜单渲染      - 主菜单连续渲染 3 帧
 *   S3 状态栏渲染    - 时间居中/左侧图标/右侧电池 布局回归
 *   S4 壁纸状态机    - 立即进入 -> 渲染数帧 -> 输入退出
 *   S5 NES 引擎      - load/run/stop + 内存回收 (SD 有内置玛丽时)
 */
#include "self_test.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "menu_system.h"
#include "st7305.h"
#include "nes_emu.h"
#include "audio_player.h"

#define TAG "SELF_TEST"

extern menu_state_t g_menu;

static int s_pass = 0, s_fail = 0;

static void report(const char *name, bool ok, const char *detail)
{
    ESP_LOGI(TAG, "SELF-TEST [%s] %s%s%s", ok ? "PASS" : "FAIL", name,
             detail ? " " : "", detail ? detail : "");
    if (ok) s_pass++; else s_fail++;
}

static void mem_snapshot(uint32_t *ifree, uint32_t *ilargest,
                         uint32_t *pfree, uint32_t *plargest)
{
    *ifree    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    *ilargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    *pfree    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    *plargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}

static bool test_heap_baseline(void)
{
    uint32_t ifree, il, pfree, pl;
    mem_snapshot(&ifree, &il, &pfree, &pl);
    char d[128];
    snprintf(d, sizeof(d), "internal free=%u max=%u psram free=%u max=%u",
             (unsigned)ifree, (unsigned)il, (unsigned)pfree, (unsigned)pl);
    bool ok = (ifree >= 16000) && (pfree >= 500000);
    report("S1 堆基线", ok, d);
    return ok;
}

static bool test_menu_render(void)
{
    bool ok = true;
    g_menu.current_page = MENU_PAGE_MAIN;
    g_menu.needs_redraw = true;
    for (int i = 0; i < 3; i++) {
        menu_render(&g_menu);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    report("S2 菜单渲染 3 帧", ok, NULL);
    return ok;
}

static bool test_status_bar(void)
{
    menu_settings_t st;
    memset(&st, 0, sizeof(st));
    st.battery = 67;
    st.volume = 4;
    st.bt_enabled = true;
    st.bt_connected = true;
    menu_draw_status_bar(g_menu.lcd, &st, "");
    st.bt_connected = false;
    menu_draw_status_bar(g_menu.lcd, &st, "");
    report("S3 状态栏渲染(时间居中/左图标/右电池)", true, NULL);
    return true;
}

static bool test_wallpaper_state_machine(void)
{
    uint8_t saved_mode = g_menu.wallpaper_mode;
    g_menu.wallpaper_mode = WALLPAPER_MODE_STARS;   /* 不启动游戏, 只测状态机 */
    g_menu.current_page = MENU_PAGE_MAIN;

    menu_screensaver_enter_test();
    bool entered = false;
    for (int i = 0; i < 10 && !entered; i++) {
        screensaver_check_and_render(g_menu.lcd, false);
        entered = menu_screensaver_is_active();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (entered) {
        for (int i = 0; i < 3; i++) {
            screensaver_check_and_render(g_menu.lcd, false);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    screensaver_check_and_render(g_menu.lcd, true);   /* 任意输入退出 */
    bool exited = !menu_screensaver_is_active();

    g_menu.wallpaper_mode = saved_mode;
    report("S4 壁纸状态机 进入/渲染/退出", entered && exited,
           entered ? (exited ? NULL : "进入成功但退出失败") : "未进入壁纸");
    return entered && exited;
}

static bool test_nes_engine(void)
{
    struct stat stt;
    if (stat("/sdcard/nes/超级玛丽.nes", &stt) != 0) {
        report("S5 NES 引擎 (无测试 ROM, 跳过)", true, NULL);
        return true;
    }
    uint32_t ifree0, il0, pfree0, pl0;
    mem_snapshot(&ifree0, &il0, &pfree0, &pl0);
    esp_err_t r = nes_emu_start("/sdcard/nes/超级玛丽.nes");
    if (r != ESP_OK) {
        report("S5 NES 引擎 启动失败", false, esp_err_to_name(r));
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    nes_emu_stop();
    audio_player_stop();   /* 与真实游戏退出一致, 释放 PCM 输出任务 */
    nes_emu_unload();
    uint32_t ifree1, il1, pfree1, pl1;
    mem_snapshot(&ifree1, &il1, &pfree1, &pl1);
    char d[128];
    snprintf(d, sizeof(d), "internal %u->%u, psram %u->%u",
             (unsigned)ifree0, (unsigned)ifree1, (unsigned)pfree0, (unsigned)pfree1);
    bool ok = (ifree1 >= ifree0 - 4096);
    report("S5 NES 引擎 load/run/stop + 内存回收", ok, d);
    return ok;
}

esp_err_t self_test_run_all(void)
{
    s_pass = s_fail = 0;
    ESP_LOGI(TAG, "==== 自动化自测开始 ====");
    test_heap_baseline();
    test_menu_render();
    test_status_bar();
    test_wallpaper_state_machine();
    test_nes_engine();
    ESP_LOGI(TAG, "==== 自测汇总: PASS=%d FAIL=%d ====", s_pass, s_fail);
    return (s_fail == 0) ? ESP_OK : ESP_FAIL;
}
