/* 机载自动化自测: 模块化/回归安全网.
 * 场景:
 *   S1 堆基线        - 内部/PSRAM 空闲与最大块
 *   S5 NES 引擎      - load/run/stop + 内存回收 (SD 有内置玛丽时)
 *
 *  注: 原 S2 菜单渲染 / S3 状态栏 / S4 壁纸 基于旧 menu_system(已移除),
 *      待新 os 稳定后可用 os 渲染 API 重写.
 */
#include "self_test.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nes_emu.h"
#include "audio_player.h"

#define TAG "SELF_TEST"

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
    audio_player_stop();   /* 与真实游戏退出一致, 释放 PCM 输出任务 */
    nes_emu_unload();
    uint32_t ifree1, il1, pfree1, pl1;
    mem_snapshot(&ifree1, &il1, &pfree1, &pl1);
    char d[128];
    snprintf(d, sizeof(d), "internal %u->%u, psram %u->%u",
             (unsigned)ifree0, (unsigned)ifree1, (unsigned)pfree0, (unsigned)pfree1);
    /* 内部 RAM 复原 (容差 4KB) + PSRAM 复原 (容差 64KB, 帧缓冲/共享池释放) */
    bool ok = (ifree1 >= ifree0 - 4096) && (pfree1 >= pfree0 - (64 * 1024));
    report("S5 NES 引擎 load/run/stop + 内存回收(含PSRAM)", ok, d);
    return ok;
}

esp_err_t self_test_run_all(void)
{
    s_pass = s_fail = 0;
    ESP_LOGI(TAG, "==== 自动化自测开始 ====");
    test_heap_baseline();
    test_nes_engine();
    ESP_LOGI(TAG, "==== 自测汇总: PASS=%d FAIL=%d ====", s_pass, s_fail);
    return (s_fail == 0) ? ESP_OK : ESP_FAIL;
}
