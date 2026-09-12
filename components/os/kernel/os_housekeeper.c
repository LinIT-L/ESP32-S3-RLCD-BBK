/**
 * os_housekeeper.c — 资源管家 (V1.5.x, 高权限版).
 *
 * 回到主菜单 / 应用管家时统一清扫瞬态内存占用. 分两级:
 *   1) 显式登记级: 各模块在开启瞬态资源时 os_housekeeper_add() 登记的清理回调.
 *   2) 默认级(高权限, 无需登记): 只要退到主菜单/管家就安全执行的通用清理:
 *       - 停掉残留的瞬态声音/震动 (tone/vibrator);
 *       - 若正在 Wi-Fi 扫描则停掉 (不影响已联网);
 *       - 内部堆遗检: 记录最大可分配块, 低于阈值告警, 内存垃圾立刻现形.
 * 原则 (保守): 只清瞬态; 不动蓝牙/手柄连接、Wi-Fi 联网、背景音乐等"后台运行".
 */
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "tone_player.h"
#include "vibrator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef CONFIG_HEAP_TRACING
#include "esp_heap_trace.h"
#endif

static const char *TAG = "HOUSEKEEP";

#define HK_MAX 16
static os_hk_fn s_hk[HK_MAX];
static int  s_hk_n;

void os_housekeeper_add(os_hk_fn fn)
{
    if (!fn) return;
    for (int i = 0; i < s_hk_n; i++) if (s_hk[i] == fn) return;
    if (s_hk_n >= HK_MAX) return;
    s_hk[s_hk_n++] = fn;
}

int os_housekeeper_pending(void){ return s_hk_n; }

void os_housekeeper_sync(void)
{
    /* 1) 显式登记的瞬态清理 */
    for (int i = 0; i < s_hk_n; i++) { os_hk_fn f = s_hk[i]; if (f) f(); }
    s_hk_n = 0;

    /* 2) 默认级高权限清扫: 一律安全可执行 */
    tone_stop();                 /* 残留按键音/效果音 (音乐走 audio_player, 不受影响) */
    vibrator_stop();             /* 残留震动                                     */
    esp_wifi_scan_stop();        /* 若在扫描则停 (不影响已联网 WiFi)             */

    /* 3) 内部堆遗检: 任何 App 漏清理都会在这里现形 */
    multi_heap_info_t hi;
    heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "sweep done: int_free=%d largest=%d nblock=%d",
             (int)hi.total_free_bytes, (int)hi.largest_free_block, (int)hi.free_blocks);
    if (hi.largest_free_block < (32 * 1024) && hi.total_free_bytes < (40 * 1024))
        ESP_LOGW(TAG, "内部堆紧张! free=%d largest=%d — 某模块可能未释放",
                 (int)hi.total_free_bytes, (int)hi.largest_free_block);
}

/* ==== 内存审计 (V1.6.x): 定位"谁占用内部 RAM" ====
 * 分为 4 块: ①整机 heap 分类 ②内部/PSRAM 明细 ③所有任务栈水位 ④(启用时)heap 块明细. */
void os_housekeeper_mem_audit(void)
{
    const uint32_t caps[][2] = {
        { MALLOC_CAP_INTERNAL, MALLOC_CAP_INTERNAL },
        { MALLOC_CAP_SPIRAM,   MALLOC_CAP_SPIRAM },
        { MALLOC_CAP_8BIT,     MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL },
        { MALLOC_CAP_DMA,      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL },
    };
    const char *capname[] = { "internal", "psram", "int-8bit", "int-dma" };
    for (int i = 0; i < 4; i++) {
        multi_heap_info_t hi;
        heap_caps_get_info(&hi, caps[i][1]);
        printf("[AUDIT] %-9s total=%u free=%u used=%u largest=%u nblocks=%u\n",
               capname[i],
               (unsigned)heap_caps_get_total_size(caps[i][0]),
               (unsigned)hi.total_free_bytes,
               (unsigned)(heap_caps_get_total_size(caps[i][0]) - hi.total_free_bytes),
               (unsigned)hi.largest_free_block, (unsigned)hi.free_blocks);
    }

    /* 所有任务栈高水位 (高水位越小=栈越紧张) */
    char *buf = malloc(3072);
    if (buf) {
        vTaskList(buf);
        printf("---[AUDIT] tasks (Name State Prio Stack Free Task#) ---\n");
        char *save = NULL, *tok = strtok_r(buf, "\n", &save);
        while (tok) { printf("        %s\n", tok); tok = strtok_r(NULL, "\n", &save); }
        free(buf);
    }

#ifdef CONFIG_HEAP_TRACING
    /* standalone 模式: 列出内部堆所有块 (含已分配大块, 便于对 size 归属) */
    printf("---[AUDIT] internal heap blocks ---\n");
    /* esp_heap_trace_dump 需已 init+start; 若未起, 退化为 heap_caps_dump_all */
    heap_caps_dump_all();
#endif
}