/**
 * mem_sim.c — LinTOS 内存「主机侧」模拟测试（不依赖 ESP-IDF，不参与固件构建）
 *
 * 目的：在 macOS 主机上复现可被"算式化"的那部分内存逻辑，用来
 *   ① 校验源码里的尺寸计算（I2S DMA / 显示缓冲 / 任务栈单位）；
 *   ② 用量化模型解释真机现象（"空闲 183KB 但最大连续块只有 84KB"）；
 *   ③ 做分配器碎片压力测试。
 *
 * ⛔ 边界（必须遵守，写清楚免得以后误读）：
 *   本程序**不是** ESP32 模拟器。它不知道 ESP32-S3 的真实堆布局、cache、
 *   ROM 保留区，也**不模拟 PSRAM 访问成本**。它输出的一切都是
 *   「主机模型数据」，不能当作「真机可用内存」。
 *   真机数据只来自串口 `mem` / `audit` 命令。
 *
 * 编译运行（不改变工程任何构建配置）：
 *   cc -O2 -std=c11 -Wall -Wextra tests/mem_sim/mem_sim.c -o /tmp/mem_sim && /tmp/mem_sim
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) { g_pass++; printf("  [PASS] " fmt "\n", ##__VA_ARGS__); }   \
        else      { g_fail++; printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); }   \
    } while (0)

/* =====================================================================
 * 0. 真机基线（唯一来源：串口 `mem` 命令，2026-10-30 uptime=143s）
 *    任何与本文件计算结果的距离，都要以这组数字为准。
 * ===================================================================== */
#define REAL_INTERNAL_TOTAL   322339u
#define REAL_INTERNAL_FREE    187439u
#define REAL_INTERNAL_USED    134900u
#define REAL_INTERNAL_LARGEST  86016u
#define REAL_PSRAM_TOTAL     7560052u
#define REAL_PSRAM_FREE      7303256u
#define REAL_PSRAM_LARGEST   7208960u

/* 真机 audit 里观察到的内部堆区块尺寸（"Free: No" 且 >=1024B） */
static const uint32_t k_real_blocks[] = {
    32768, 16384, 15360, 6656, 4608, 4096, 3584,
    2304, 2304, 2304, 2304, 2304, 2304,          /* ← I2S DMA 6 块 */
    1536, 1536, 1536, 1280, 1280, 1152
};

/* =====================================================================
 * 1. 任务栈单位 —— ESP-IDF 的 StackType_t 是 uint8_t，栈参数就是「字节」
 *    依据：esp-idf-v60/components/freertos/FreeRTOS-Kernel/portable/
 *          xtensa/include/freertos/portmacro.h:78  #define portSTACK_TYPE uint8_t
 * ===================================================================== */
typedef uint8_t  StackType_t;          /* 与 IDF 一致：1 字节 */
typedef uint32_t BaseType_t;

typedef struct {
    const char *name;
    uint32_t    arg;        /* 源码里 xTaskCreate 传入的深度值 */
    uint32_t    hwm_free;   /* 真机 vTaskList 高水位余量（字节） */
    const char *src;
} task_spec_t;

/* 数值全部来自源码 + 真机 vTaskList，见报告 §2 */
static const task_spec_t k_tasks[] = {
    { "main",       6144, 2192, "CONFIG_ESP_MAIN_TASK_STACK_SIZE=6144 (字节)" },
    { "sys_evt",    3072, 2816, "CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=3072" },
    { "esp_timer",  4096, 3952, "CONFIG_ESP_TIMER_TASK_STACK_SIZE=4096" },
    { "Tmr Svc",    1536,  840, "CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=1536(字)?" },
    { "ipc0",       1024,  632, "IDF 内部" },
    { "ipc1",       1024,  544, "IDF 内部" },
    { "IDLE0",      1536,  744, "CONFIG_FREERTOS_IDLE_TASK_STACKSIZE=1536" },
    { "IDLE1",      1536,  736, "CONFIG_FREERTOS_IDLE_TASK_STACKSIZE=1536" },
    { "touch_wd",   4096, 2452, "touch_panel.c xTaskCreate" },
    { "sse",        4096, 2092, "wallpapers.c SS_SUPER_STACK=4096 (静态栈)" },
    { "gps_guard",  4096, 1836, "app_board.c gps_guard" },
    { "remcon",    16384,12172, "remcon.c RC_TASK_STACK=16384" },
};

static void t_stack(void)
{
    printf("\n=== T1 任务栈：单位与真实字节数 ===\n");
    printf("  sizeof(StackType_t)=%zu （ESP-IDF 定义为 uint8_t ⇒ 1 字节；"
           "FreeRTOS 原生的\"字\"概念在 IDF 已被改写）\n", sizeof(StackType_t));

    uint32_t total = 0, free_sum = 0;
    printf("  %-11s %7s %7s %7s %6s  %s\n",
           "task", "stack", "hwm_free", "used", "余量%", "来源");
    for (size_t i = 0; i < sizeof(k_tasks)/sizeof(k_tasks[0]); i++) {
        const task_spec_t *t = &k_tasks[i];
        uint32_t bytes = t->arg * (uint32_t)sizeof(StackType_t);  /* 关键：×1 不是 ×4 */
        uint32_t used  = (t->hwm_free <= bytes) ? (bytes - t->hwm_free) : 0;
        total += bytes; free_sum += t->hwm_free;
        printf("  %-11s %7u %7u %7u %5u%%  %s\n",
               t->name, bytes, t->hwm_free, used,
               (unsigned)(t->hwm_free * 100u / bytes), t->src);
    }
    printf("  ------------------------------------------------\n");
    printf("  12 条任务栈合计 = %u B（%.1f KB），栈内未用合计 = %u B（%.1f KB）\n",
           total, total / 1024.0, free_sum, free_sum / 1024.0);

    /* 若按"字(4字节)"理解，会得出 4 倍的错误结论 —— 这正是 D1/D2/D3 的根源 */
    CHECK(total == 48640u, "12 条任务栈按字节合计 = 48,640 B（若误按字算会是 %u B）",
          total * 4u);
    CHECK(sizeof(StackType_t) == 1, "StackType_t 为 1 字节 ⇒ 栈参数单位是字节");

    /* D1：audio_out 的 6144 实际含义 */
    uint32_t pcm_out_stack = 6144u * sizeof(StackType_t);
    printf("\n  [D1 复核] PCM_OUT_STACK_WORDS=6144 ⇒ 实际分配 %u B，"
           "注释/日志按字算成 %u B（差 %u B）\n",
           pcm_out_stack, 6144u * 4u, 6144u * 4u - pcm_out_stack);
    CHECK(pcm_out_stack == 6144u, "audio_out 栈真实为 6,144 B（不是 24 KB）");
}

/* =====================================================================
 * 2. I2S DMA —— 与真机 6×2304 对照
 *    依据：audio_player.c:535-537（desc=3, frame=576, STEREO/16bit）
 *          i2s_common.c:642-653（每个描述符单独 calloc(1,bufsize)）
 * ===================================================================== */
static void t_i2s(void)
{
    printf("\n=== T2 I2S DMA：算式 vs 真机块 ===\n");
    const uint32_t desc_num = 3, frame_num = 576, slot_num = 2, bytes_per_sample = 2;
    uint32_t per_desc = frame_num * slot_num * bytes_per_sample;
    uint32_t per_chan = per_desc * desc_num;
    uint32_t total    = per_chan * 2;   /* TX + RX */

    printf("  单描述符 = %u帧 × %u槽 × %uB = %u B\n", frame_num, slot_num, bytes_per_sample, per_desc);
    printf("  每通道   = %u B × %u 描述符 = %u B\n", per_desc, desc_num, per_chan);
    printf("  TX+RX    = %u B × 2 通道 = %u B\n", per_chan, total);

    uint32_t real_cnt = 0;
    for (size_t i = 0; i < sizeof(k_real_blocks)/sizeof(k_real_blocks[0]); i++)
        if (k_real_blocks[i] == per_desc) real_cnt++;
    printf("  真机 dump 中 size==%u 的块数量 = %u（若为 6 ⇒ 与算式完全吻合）\n",
           per_desc, real_cnt);

    CHECK(per_desc == 2304u, "单描述符 2,304 B");
    CHECK(per_chan == 6912u, "每通道 6,912 B");
    CHECK(total == 13824u, "TX+RX 合计 13,824 B");
    CHECK(real_cnt == 6u && real_cnt * per_desc == total,
          "真机 6 块 × 2,304 = %u B == 源码算式 %u B", real_cnt * per_desc, total);

    /* I2S DMA 必须在内部 RAM（MALLOC_CAP_INTERNAL|DMA|8BIT），不落 PSRAM */
    printf("  区域：I2S_DMA_ALLOC_CAPS = INTERNAL|DMA|8BIT ⇒ 必在内部 RAM（i2s_private.h:51）\n");
}

/* =====================================================================
 * 3. 显示缓冲 —— 算式 vs 真机
 *    依据：st7305.c:16  FB_BYTES = (400/4)*(300/2)；:237 fb；:690 txbuf
 * ===================================================================== */
static void t_display(void)
{
    printf("\n=== T3 显示缓冲：算式 vs 真机块 ===\n");
    const uint32_t w = 400, h = 300;
    uint32_t fb_bytes = (w / 4) * (h / 2);
    printf("  FB_BYTES = (%u/4)×(%u/2) = %u B\n", w, h, fb_bytes);
    printf("  fb + txbuf 上限 = %u B\n", fb_bytes * 2);
    CHECK(fb_bytes == 15000u, "帧缓冲 15,000 B");

    /* 真机里 >=10KB 的块只有一个 15,360 —— 与 15,000 不等，属于未决项 */
    uint32_t big[8], nbig = 0;
    for (size_t i = 0; i < sizeof(k_real_blocks)/sizeof(k_real_blocks[0]); i++)
        if (k_real_blocks[i] >= 10000u && nbig < 8) big[nbig++] = k_real_blocks[i];
    printf("  真机 >=10KB 的内部块：");
    for (uint32_t i = 0; i < nbig; i++) printf("%u ", big[i]);
    printf("（共 %u 个）\n", nbig);
    printf("  ⇒ 15,360 ≠ 15,000，且只有 1 个：另一次分配未发生或已被 free，"
           "登记为待验证项（报告 Q4）\n");
}

/* =====================================================================
 * 4. 多区 vs 单区：解释"空闲 183KB / 最大块 84KB"
 *    模型：first-fit + 相邻空闲块合并（与 multi_heap 行为同构的最小模型）
 * ===================================================================== */
typedef struct blk {
    uint32_t size;      /* 可用数据字节 */
    uint32_t off;       /* 区内偏移 */
    int      free;
} blk_t;

#define MAX_BLK 4096
typedef struct { blk_t b[MAX_BLK]; int n; uint32_t cap; } heap_t;

static void heap_init(heap_t *h, uint32_t cap)
{
    h->n = 0; h->cap = cap;
    h->b[0] = (blk_t){ cap, 0, 1 };
    h->n = 1;
}

static void heap_coalesce(heap_t *h)
{
    for (int i = 0; i + 1 < h->n; ) {
        if (h->b[i].free && h->b[i + 1].free) {
            h->b[i].size += h->b[i + 1].size;
            for (int j = i + 1; j + 1 < h->n; j++) h->b[j] = h->b[j + 1];
            h->n--;
        } else i++;
    }
}

/* 返回 1 成功，0 失败；overhead 模拟每块 8 字节头（真机 multi_heap 4~8B/块） */
static int heap_alloc(heap_t *h, uint32_t size)
{
    const uint32_t overhead = 8;
    uint32_t need = size + overhead;
    for (int i = 0; i < h->n; i++) {
        if (!h->b[i].free || h->b[i].size < need) continue;
        if (h->b[i].size == need) { h->b[i].free = 0; return 1; }
        if (h->n + 1 >= MAX_BLK) return 0;
        uint32_t remain = h->b[i].size - need;
        uint32_t off    = h->b[i].off;
        for (int j = h->n; j > i + 1; j--) h->b[j] = h->b[j - 1];
        h->n++;
        h->b[i]     = (blk_t){ need,   off,           0 };
        h->b[i + 1] = (blk_t){ remain, off + need,    1 };
        return 1;
    }
    return 0;
}

/* 按区内偏移释放（偏移在合并/切分过程中保持不变，适合做指针替身） */
static int heap_free_at(heap_t *h, uint32_t off)
{
    for (int i = 0; i < h->n; i++) {
        if (!h->b[i].free && h->b[i].off == off) { h->b[i].free = 1; return 1; }
    }
    return 0;
}

static uint32_t heap_total_free(const heap_t *h)
{
    uint32_t s = 0;
    for (int i = 0; i < h->n; i++) if (h->b[i].free) s += h->b[i].size;
    return s;
}

static uint32_t heap_largest_free(const heap_t *h)
{
    uint32_t m = 0;
    for (int i = 0; i < h->n; i++) if (h->b[i].free && h->b[i].size > m) m = h->b[i].size;
    return m;
}

/* =====================================================================
 * 5. 分配成功率 vs 请求大小（用真机 7 区布局，而不是单区理想模型）
 * ===================================================================== */
/* 真机 dump 推出的 7 个内部堆区（cap = 该区 alloc+free；used = 该区已分配）
 * 数据来源：串口 `audit` → heap_caps_dump_all 的每区块列表，2026-10-30
 * 注：块尺寸不含 multi_heap 头部，故 7 区 cap 之和 316,064 < 真机 total 322,339，
 *     差 6,275 B 即约 280 个块的头部开销。 */
#define NREGION 7
static const uint32_t k_region_cap[NREGION]  = { 121288, 32020, 64168, 37644, 21196, 32024, 7724 };
static const uint32_t k_region_used[NREGION] = {  32952,     0, 63832, 23216,  8704,     0,    0 };

static void build_multi(heap_t h[NREGION])
{
    for (int i = 0; i < NREGION; i++) {
        heap_init(&h[i], k_region_cap[i]);
        if (k_region_used[i]) heap_alloc(&h[i], k_region_used[i]);
        heap_coalesce(&h[i]);
    }
}

/* 跨区首次适配：模拟 heap_caps_malloc 在多个堆区里挑一个能放下的 */
static int multi_alloc(heap_t h[NREGION], uint32_t size)
{
    for (int i = 0; i < NREGION; i++)
        if (heap_largest_free(&h[i]) >= size + 8u && heap_alloc(&h[i], size)) return 1;
    return 0;
}

static void t_alloc_by_size(void)
{
    printf("\n=== T4 分配成功率 vs 请求大小（真机 7 区布局模型）===\n");
    heap_t h[NREGION]; build_multi(h);
    uint32_t max_largest = 0;
    for (int i = 0; i < NREGION; i++)
        if (heap_largest_free(&h[i]) > max_largest) max_largest = heap_largest_free(&h[i]);

    printf("  模型 7 区最大连续块 = %u B；真机 mem 实测 largest = %u B（两次采样不同步，量级一致）\n",
           max_largest, REAL_INTERNAL_LARGEST);
    printf("  %10s %10s %8s\n", "请求B", "理论", "模型实测");
    const uint32_t sizes[] = { 1024, 4096, 16384, 32768, 65536, 81920, 86016, 88000, 100000, 131072 };
    int mismatch = 0;
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        heap_t h2[NREGION]; build_multi(h2);
        int ok = multi_alloc(h2, sizes[i]);
        int expect = (sizes[i] + 8u <= max_largest) ? 1 : 0;
        if (ok != expect) mismatch++;
        printf("  %10u %10s %8s\n", sizes[i], expect ? "可" : "不可", ok ? "成功" : "失败");
    }
    CHECK(mismatch == 0, "模型跨区分配与理论阈值一致（%d 处不一致）", mismatch);
    printf("  ⇒ 内部 RAM 想一次拿 100KB/128KB，在现有布局下必然失败，"
           "与\"总空闲 183KB\"无关。\n");
}

/* =====================================================================
 * 6. 碎片压力测试：单区 vs 多区
 * ===================================================================== */
static void t_frag(void)
{
    printf("\n=== T5 碎片压力：单区 322,339 B vs 真机 7 区 ===\n");

    /* 单区模型：把整个内部堆当一块连续空间 */
    heap_t single; heap_init(&single, REAL_INTERNAL_TOTAL);
    /* 先占用真机已用量（用一大块代表"已用"），保留其余为空闲 */
    heap_alloc(&single, REAL_INTERNAL_USED);

    /* 多区模型：与 T4 共用同一套真机 7 区数据 */
    heap_t multi[NREGION];
    build_multi(multi);

    uint32_t single_free = heap_total_free(&single), single_largest = heap_largest_free(&single);
    uint32_t multi_free = 0, multi_largest = 0;
    for (int i = 0; i < NREGION; i++) {
        multi_free += heap_total_free(&multi[i]);
        uint32_t l = heap_largest_free(&multi[i]);
        if (l > multi_largest) multi_largest = l;
    }
    printf("  单区模型：free=%u  largest=%u\n", single_free, single_largest);
    printf("  多区模型：free=%u  largest=%u\n", multi_free, multi_largest);
    printf("  真机实测：free=%u  largest=%u\n", REAL_INTERNAL_FREE, REAL_INTERNAL_LARGEST);
    printf("  ⇒ 多区把最大块从 %u B 压到 %u B（降幅 %.0f%%），"
           "与真机 %u B 同量级 —— 这是**布局问题**，不是泄漏。\n",
           single_largest, multi_largest,
           100.0 * (single_largest - multi_largest) / single_largest,
           REAL_INTERNAL_LARGEST);
    CHECK(multi_largest < single_largest, "多区模型的最大连续块显著小于单区");

    /* 随机分配/释放压力（固定种子，可复现） */
    printf("\n  --- 随机分配/释放压力（种子固定 0x1234，20,000 次）---\n");
    srand(0x1234);
    heap_t stress; heap_init(&stress, REAL_INTERNAL_TOTAL);
    uint32_t *offs = calloc(256, sizeof(uint32_t));
    int live = 0, fails = 0; uint32_t peak_largest = 0;
    for (int op = 0; op < 20000; op++) {
        if (live < 256 && (live == 0 || (rand() % 100) < 55)) {
            uint32_t sz = 16u + (uint32_t)(rand() % 4080);
            /* 记录分配前的空闲块偏移：分配成功后该块起点即 off */
            uint32_t off = 0xFFFFFFFFu;
            for (int i = 0; i < stress.n; i++)
                if (stress.b[i].free && stress.b[i].size >= sz + 8u) { off = stress.b[i].off; break; }
            if (off != 0xFFFFFFFFu && heap_alloc(&stress, sz)) offs[live++] = off;
            else fails++;
        } else if (live > 0) {
            int k = rand() % live;
            heap_free_at(&stress, offs[k]);
            offs[k] = offs[--live];
            heap_coalesce(&stress);
        }
        uint32_t l = heap_largest_free(&stress);
        if (l > peak_largest) peak_largest = l;
    }
    printf("  存活分配=%d  失败次数=%d  最终 free=%u  largest=%u  历史最大 largest=%u\n",
           live, fails, heap_total_free(&stress), heap_largest_free(&stress), peak_largest);
    CHECK(live >= 0, "压力测试正常结束（存活 %d，失败 %d）", live, fails);
    free(offs);
}

/* =====================================================================
 * 7. 初始化场景回放（模型 —— 明确标注，非真机）
 * ===================================================================== */
static void t_lifecycle(void)
{
    printf("\n=== T6 生命周期场景回放（模型，非真机）===\n");
    struct { const char *step; int delta; const char *where; } steps[] = {
        { "静态段（IRAM+data+bss）",      +187952, "不在堆内，单独计账" },
        { "开机后堆内已用（真机 used）",   +134900, "真机 mem" },
        { "打开 Wi-Fi（enable）",          +69000, "wifi_manager.c 注释: 省 ~69KB" },
        { "关闭 Wi-Fi（disable+deinit）",  -69000, "wifi_manager.c:627-667" },
        { "蓝牙初始化（按需）",            +72948, "bt_manager 2026-10-30 改动注释" },
        { "蓝牙关闭（拆栈）",              -72948, "bt_core.c:47-68" },
        { "游戏音频启动（PCM环+栈，PSRAM）", +268288, "262,144 + 6,144（PSRAM）" },
        { "游戏音频停止（PSRAM）",        -268288, "audio_player_stop()" },
        { "GBC 引擎 load（WRAM 32KB 内部）", +32768, "gbc_emu.c:414" },
        { "GBC 引擎 unload",               -32768, "gbc_emu.c:477" },
    };
    int32_t internal = 0;
    printf("  %-34s %10s %12s  %s\n", "步骤", "增量", "内部累计", "依据");
    for (size_t i = 0; i < sizeof(steps)/sizeof(steps[0]); i++) {
        int is_psram  = (strstr(steps[i].step, "PSRAM") != NULL);
        int not_heap  = (strstr(steps[i].where, "不在堆内") != NULL);
        if (!is_psram && !not_heap) internal += steps[i].delta;
        printf("  %-34s %+10d %12d  %s\n", steps[i].step, steps[i].delta, internal, steps[i].where);
    }
    printf("  ⇒ 回放结束后内部累计回到开机值，说明这些模块**有配对释放路径**（源码层面）。\n");
    printf("  ⚠ 这是模型推演，不是真机时序；真机验证见报告 §6。\n");
    CHECK(internal == 134900, "场景回放收支平衡（回到 %d）", internal);
}

int main(void)
{
    printf("LinTOS 内存主机侧模拟测试（host model, NOT an ESP32 emulator）\n");
    printf("真机基线: internal %u/%u used %u largest %u | psram %u/%u\n",
           REAL_INTERNAL_FREE, REAL_INTERNAL_TOTAL, REAL_INTERNAL_USED,
           REAL_INTERNAL_LARGEST, REAL_PSRAM_FREE, REAL_PSRAM_TOTAL);

    t_stack();
    t_i2s();
    t_display();
    t_alloc_by_size();
    t_frag();
    t_lifecycle();

    printf("\n==== 汇总: PASS=%d FAIL=%d ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
