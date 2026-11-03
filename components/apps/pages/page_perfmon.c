/**
 * page_perfmon.c — 性能监视器（350×250 面板）
 *
 * 两种形态，**同一套绘制**（pm_draw_panel 是唯一实现，两边画面逐像素一致）：
 *   · 独立应用页 OS_PAGE_PERFMON —— 应用中心 → 应用 → 系统   ← **唯一 UI 入口**
 *   · 二级弹窗 perfmon_open()   —— 2026-11-03 设置里的入口已删除, 此函数暂留备用
 *                                  (没有调用方; 要清理就整段删掉, 连带 apps_pages.h 的声明)
 * 页面版把面板放在状态栏下方居中、整屏白底；弹窗版只画自己那块矩形、透出底层页面。
 *
 * 2026-11-02 收回过一版: 当时把它做成 400×300 全屏应用 + 常驻 1Hz 采集任务，两个问题:
 *   ① 看性能是"临时瞄一眼", 全屏应用会把当前页面整体顶掉, 关掉还得重新点回来;
 *   ② 为了偶尔打开的一个页面常驻一个任务 + 63KB PSRAM, 不合"没用绝不常驻"。
 * 现在两种形态都**没有常驻任务**: 采集在页面/弹窗的 poll 里按 1s 节拍做, 关掉即停;
 * PSRAM 历史缓冲进页面才分配、退出即释放。
 *
 * 三个页签 (精简后, 仍是原来那三块核心信息):
 *   趋势 — CPU / 内部 RAM / PSRAM 三条曲线 + 1/5/10/30 分钟窗口
 *   内存 — SRAM/PSRAM 用量、最大连续块、任务数、运行时间
 *   任务 — 按 CPU 或栈余排序的前 5 个任务
 *
 * CPU% 怎么算: 100 - Σ(IDLE 增量)/Σ(总增量), 两次采样差分。
 *   ulRunTimeCounter 是 uint32, esp_timer 基准下约 71 分钟回绕一次; 用无符号减法
 *   (cur - prev) 天然处理回绕, 只要两次采样间隔远小于回绕周期 (我们是 1s)。
 *   百分比用的是**同一段窗口**的实际增量和, 所以 UI 卡一下也不会把数字算歪。
 *
 * 内存规则: 历史缓冲放 PSRAM (heap_caps_malloc)。本文件没有栈上大数组。
 */
#include "os.h"
#include "ui_common.h"
#include "st7305.h"
#include "input.h"          /* input_touch_now(): 左右拖拉切页签 */
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>            /* strcasecmp: 任务视图按名称(A~Z)排序 */

#define PF_TAG "PERFMON"

/* ============================ 参数 ============================ */
#define PF_PERIOD_MS      1000u                         /* 采样周期 */
#define PF_MAX_SAMPLES    1800                          /* 30 分钟 × 60 */
#define PF_TASK_MAX       24                            /* uxTaskGetSystemState 一次抓多少 */
#define PF_ROWS_MAX       16                            /* 实时表最多多少行 */
/* 一屏显示几行任务：标题行/页签行删除后内容区多了 54px ⇒ 5 行变 7 行
 *   （7×22=154，从 PM_BODY_Y+26 起算，落在底栏线 by+PM_H-40=by+210 之内） */
#define PF_SHOW_ROWS      7

#define PF_RANGE_CNT      4
static const uint16_t s_range_sec[PF_RANGE_CNT] = { 60, 300, 600, 1800 };   /* 1/5/10/30 分 */
static const char *const s_range_name[PF_RANGE_CNT] = { "1分", "5分", "10分", "30分" };

/* ---- 弹窗几何 (与 os_dialog 的大窗一致: 350×250 居中) ---- */
#define PM_W        350
#define PM_H        250
#define PM_TAB_Y0   (by + 30)     /* 页签行（已停用：改为左右拖动切换） */
#define PM_TAB_H    22
#define PM_TAB_W    64
/* 内容区顶：2026-11-03 linit —— **标题行 + 页签行都删掉**，
 *   只要左右拖动内容区就切页。内容直接从面板顶部开始（by+6）。
 *   原来 by+60（标题 30 + 页签 30），现在省下的 54px 全给内容区。 */
#define PM_BODY_Y   (by + 6)
/* 趋势图高度 + 图例行 + chip 行（绘制与点击判定共用，避免两处数值对不上） */
#define PM_TREND_H  150
#define PM_LEGEND_DY 10
#define PM_CHIP_DY   22
#define PM_CHIP_Y    (PM_BODY_Y + PM_TREND_H + PM_LEGEND_DY + PM_CHIP_DY)

/* ============================ 类型 ============================ */
/* 趋势样本: 8B/组 × 1800 = 14400 B (PSRAM) */
typedef struct {
    uint32_t t_ms;
    uint8_t  cpu;
    uint8_t  sram;
    uint8_t  psram;
    uint8_t  rsv;
} pf_sample_t;

/* 实时表行 */
typedef struct {
    char     name[16];
    uint8_t  cpu;           /* 上一秒差分 0..100 */
    uint8_t  prio;          /* 保留备用 */
    uint16_t used_sram_kb;  /* 栈已用(在内部 RAM) KB */
    uint16_t used_psram_kb; /* 栈已用(在 PSRAM) KB */
} perf_row_t;

/* ============================ 状态 ============================ */
static pf_sample_t *s_buf;                  /* PSRAM 环形历史 */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint16_t s_head;                     /* 下一个写入位置 */
static uint16_t s_cnt;                      /* 已有样本数 */

/* 实时表 */
static EXT_RAM_BSS_ATTR perf_row_t s_rows[PF_ROWS_MAX];
static int s_rows_n;

/* 差分基线 (PSRAM: 24×约 40B ≈ 1KB) */
static EXT_RAM_BSS_ATTR TaskStatus_t s_ts[PF_TASK_MAX];
static EXT_RAM_BSS_ATTR uint32_t     s_pcnt[PF_TASK_MAX];
static EXT_RAM_BSS_ATTR char         s_pname[PF_TASK_MAX][16];
static int      s_pn;
static uint64_t s_ptotal;

/* UI 状态 */
static int  s_tab;              /* 0 趋势 1 内存 2 任务 */
static int  s_range = 1;        /* 默认 5 分钟 */
static int  s_sort;             /* 0=名称升序 1=CPU降序 2=SRAM降序 3=PSRAM降序 */
static bool s_bg_running;       /* 后台态: 退出时保留 PSRAM 历史, 重进可看波形 */

/* 左右拖拉切页签 (2026-11-02 linit): 与页签点击并存。
 * 用 input_touch_now(): 只读当前按压坐标, **不消费** tap 状态机 ——
 * 否则页签/关闭按钮的点击会被提前吃掉。 */
static bool s_drag;
static int  s_drag_x0, s_drag_xl;
#define PM_DRAG_PX  40      /* 拖动超过 40px 才算换页, 与系统手势返回阈值一致 */

/* 图表矩形 (每帧由 pm_draw_trend 设定, 供 pf_plot/pf_dot 用) */
static int s_cx0, s_cx1, s_cy0, s_cy1;

/* ============================ 小工具 ============================ */
static uint32_t pf_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void pf_mmss(uint32_t ms, char *b, size_t n)
{
    uint32_t sec = ms / 1000u;
    snprintf(b, n, "%u:%02u", (unsigned)(sec / 60u), (unsigned)(sec % 60u));
}

static void pf_hms(uint32_t ms, char *b, size_t n)
{
    uint32_t sec = ms / 1000u;
    snprintf(b, n, "%u:%02u:%02u", (unsigned)(sec / 3600u),
             (unsigned)((sec / 60u) % 60u), (unsigned)(sec % 60u));
}

/* 字节 → "1.2M / 320K / 512B" */
static void pf_fmt(char *b, size_t n, size_t bytes)
{
    if (bytes >= (size_t)1024 * 1024) snprintf(b, n, "%.1fM", (double)bytes / 1048576.0);
    else if (bytes >= 1024)           snprintf(b, n, "%uK", (unsigned)(bytes / 1024));
    else                              snprintf(b, n, "%uB", (unsigned)bytes);
}

/* ============================ PSRAM 缓冲 ============================ */
static void pf_free_buffers(void)
{
    portENTER_CRITICAL(&s_mux);
    pf_sample_t *b = s_buf;
    s_buf = NULL;
    s_head = s_cnt = 0;
    s_rows_n = 0;
    portEXIT_CRITICAL(&s_mux);
    if (b) heap_caps_free(b);
}

static esp_err_t pf_alloc(void)
{
    if (s_buf) return ESP_OK;                   /* 幂等 */

    pf_sample_t *b = heap_caps_malloc(sizeof(pf_sample_t) * PF_MAX_SAMPLES, MALLOC_CAP_SPIRAM);
    if (!b) {
        ESP_LOGE(PF_TAG, "PSRAM 分配失败 (%u B), 不启动采集",
                 (unsigned)(sizeof(pf_sample_t) * PF_MAX_SAMPLES));
        return ESP_ERR_NO_MEM;
    }
    memset(b, 0, sizeof(pf_sample_t) * PF_MAX_SAMPLES);

    portENTER_CRITICAL(&s_mux);
    s_buf = b;
    s_head = s_cnt = 0;
    s_rows_n = 0;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

/* ============================ 采样 ============================ */
/* 排序: 0=名称升序(A~Z) · 1=CPU 降序 · 2=SRAM已用降序 · 3=PSRAM已用降序 */
static void pf_sort_rows(perf_row_t *rows, int n, int mode)
{
    for (int i = 1; i < n; i++) {
        perf_row_t key = rows[i];
        int j = i - 1;
        while (j >= 0) {
            const perf_row_t *a = &rows[j];
            bool key_first;
            switch (mode) {
            case 0:  key_first = (strcasecmp(key.name, a->name) > 0); break;
            case 1:  key_first = (key.cpu > a->cpu); break;
            case 2:  key_first = (key.used_sram_kb > a->used_sram_kb); break;
            default: key_first = (key.used_psram_kb > a->used_psram_kb); break;
            }
            if (!key_first) break;
            rows[j + 1] = rows[j];
            j--;
        }
        rows[j + 1] = key;
    }
}

/**
 * 采一次。record=false 只用于建立差分基线（第一拍没有"上一秒"）。
 * 在 UI 任务里跑: 只读系统状态 + 写 PSRAM, 不碰 flash、不阻塞。
 */
static void pf_sample_once(bool record)
{
    uint32_t now = pf_now_ms();

    UBaseType_t n = uxTaskGetSystemState(s_ts, PF_TASK_MAX, NULL);
    uint64_t total = 0;
    for (UBaseType_t i = 0; i < n; i++) total += (uint64_t)s_ts[i].ulRunTimeCounter;

    /* ---- 差分窗口: 本拍到上一拍之间实际流逝了多少 CPU 时间 ---- */
    uint64_t dtotal = 0;
    if (s_ptotal && total > s_ptotal) dtotal = total - s_ptotal;

    uint64_t idle_d = 0;
    perf_row_t tmp[PF_ROWS_MAX];
    int rows = 0;

    for (UBaseType_t i = 0; i < n; i++) {
        uint32_t cur  = s_ts[i].ulRunTimeCounter;
        uint32_t prev = 0;
        bool found = false;
        for (int k = 0; k < s_pn; k++) {
            if (strncmp(s_pname[k], s_ts[i].pcTaskName, 15) == 0) { prev = s_pcnt[k]; found = true; break; }
        }
        /* 无符号减法: 32 位回绕也能算对（间隔 1s ≪ 71 分钟回绕周期） */
        uint32_t d = found ? (uint32_t)(cur - prev) : 0u;

        if (strncmp(s_ts[i].pcTaskName, "IDLE", 4) == 0) {
            idle_d += d;
            continue;                       /* 空闲不算"在跑的任务", 不进表 */
        }
        if (rows >= PF_ROWS_MAX) continue;

        perf_row_t *r = &tmp[rows++];
        memset(r, 0, sizeof(*r));
        snprintf(r->name, sizeof(r->name), "%s", s_ts[i].pcTaskName);
        uint32_t pct = dtotal ? (uint32_t)((uint64_t)d * 100u / dtotal) : 0u;
        r->cpu  = (uint8_t)(pct > 100u ? 100u : pct);
        r->prio = (uint8_t)s_ts[i].uxCurrentPriority;
        /* 每进程栈已用 = 栈分配总大小 − 高水位(剩余)。
         * ⚠ 两个坑（都踩过）：
         *   ① Xtensa 栈**向下**生长(portSTACK_GROWTH=-1)，TaskStatus_t 无 pxEndOfStack；
         *   ② heap_caps_get_allocated_size / _get_containing_block_size 对**非堆指针**
         *      (静态栈，如 IDLE/静态创建的任务)内部 `assert(heap)` → 直接 panic 重启！
         *   故必须先用 heap_caps_check_integrity_addr(...,false) 安全判定"是否在堆内"
         *   (非堆时它 return false, 不 assert)，再取分配大小；静态栈 total=0 →
         *   该任务不显示内存列(预期)；heap_caps 反查开销 1Hz×任务数，可接受。 */
        size_t total_b = 0;
        if (heap_caps_check_integrity_addr((intptr_t)s_ts[i].pxStackBase, false))
            total_b = heap_caps_get_allocated_size((void *)s_ts[i].pxStackBase);
        uint32_t free_b = (uint32_t)s_ts[i].usStackHighWaterMark * sizeof(StackType_t);
        uint32_t used_b = (total_b > free_b) ? (uint32_t)(total_b - free_b) : 0u;
        if (esp_ptr_external_ram(s_ts[i].pxStackBase))
            r->used_psram_kb = (uint16_t)(used_b / 1024u);
        else
            r->used_sram_kb  = (uint16_t)(used_b / 1024u);
    }

    /* ---- 系统级三项指标 ---- */
    uint32_t cpu_pct = 0;
    if (dtotal) {
        uint64_t busy = (idle_d < dtotal) ? (dtotal - idle_d) : 0;
        cpu_pct = (uint32_t)(busy * 100u / dtotal);
        if (cpu_pct > 100u) cpu_pct = 100u;
    }

    size_t ti = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t fi = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t tp = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t fp = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t sram_pct  = (ti && ti > fi) ? (uint32_t)((ti - fi) * 100u / ti) : 0u;
    uint32_t psram_pct = (tp && tp > fp) ? (uint32_t)((tp - fp) * 100u / tp) : 0u;

    /* ---- 发布: 排序在临界区外做完, 区内只做拷贝 ---- */
    pf_sort_rows(tmp, rows, s_sort);

    portENTER_CRITICAL(&s_mux);
    memcpy(s_rows, tmp, sizeof(perf_row_t) * (size_t)rows);
    s_rows_n = rows;

    if (record && s_buf) {
        pf_sample_t *s = &s_buf[s_head];
        s->t_ms  = now;
        s->cpu   = (uint8_t)cpu_pct;
        s->sram  = (uint8_t)sram_pct;
        s->psram = (uint8_t)psram_pct;
        s->rsv   = 0;
        s_head = (uint16_t)((s_head + 1) % PF_MAX_SAMPLES);
        if (s_cnt < PF_MAX_SAMPLES) s_cnt++;
    }
    portEXIT_CRITICAL(&s_mux);

    /* ---- 更新差分基线 ---- */
    s_pn = 0;
    for (UBaseType_t i = 0; i < n && s_pn < PF_TASK_MAX; i++) {
        snprintf(s_pname[s_pn], sizeof(s_pname[0]), "%s", s_ts[i].pcTaskName);
        s_pcnt[s_pn] = s_ts[i].ulRunTimeCounter;
        s_pn++;
    }
    s_ptotal = total;
}

/* ============================ 读缓冲 ============================ */
static int pf_sample_count(void)
{
    portENTER_CRITICAL(&s_mux);
    int n = s_cnt;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

/* age: 0 = 最新, 1 = 上一秒… */
static bool pf_get_sample(int age, pf_sample_t *out)
{
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (s_buf && age >= 0 && age < (int)s_cnt) {
        int i = (int)s_head - 1 - age;
        while (i < 0) i += PF_MAX_SAMPLES;
        *out = s_buf[i];
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

/* 生成一份"任务占用"表 (结果写进调用方缓冲区) */
static int perf_build(perf_row_t *out, int cap)
{
    int n;
    portENTER_CRITICAL(&s_mux);
    n = (s_rows_n < cap) ? s_rows_n : cap;
    if (n > 0) memcpy(out, s_rows, sizeof(perf_row_t) * (size_t)n);
    portEXIT_CRITICAL(&s_mux);
    pf_sort_rows(out, n, s_sort);
    return n;
}

/* ============================ 曲线 ============================ */
static void pf_dot(st7305_handle_t *lcd, int x, int y)
{
    if (x < s_cx0 || x > s_cx1 || y < s_cy0 || y > s_cy1) return;
    st7305_draw_pixel(lcd, x, y, ST7305_COLOR_BLACK);
}

/* 可见样本数 = min(所选时间范围秒数, 实际已采样本数) */
static int pf_visible(void)
{
    int cnt  = pf_sample_count();
    int want = (int)s_range_sec[s_range];
    return (cnt < want) ? cnt : want;
}

/**
 * 画一条曲线。
 *   style 0: 1px 实线      (CPU)
 *   style 1: 1px 虚线 2on1off (SRAM)
 *   style 2: 2px 虚线 3on2off (PSRAM)
 * 纵向用"上一列 y → 本列 y"的竖段补线, 相当于轻量 Bresenham —— 斜率再陡也不会断。
 */
static void pf_plot(st7305_handle_t *lcd, int want, int series, int style)
{
    if (want <= 0) return;
    const int W = s_cx1 - s_cx0;
    const int H = s_cy1 - s_cy0;
    int prev_y = -1;

    for (int i = 0; i <= W; i++) {
        int age = (want <= 1) ? 0 : (int)((long)(want - 1) * (W - i) / W);
        pf_sample_t s;
        int v = 0;
        if (pf_get_sample(age, &s)) v = (series == 0) ? s.cpu : (series == 1 ? s.sram : s.psram);
        if (v < 0) v = 0;
        if (v > 100) v = 100;

        int y = s_cy1 - H * v / 100;
        int x = s_cx0 + i;

        bool on;
        switch (style) {
        case 0:  on = true;                 break;
        case 1:  on = ((x % 3) != 0);       break;
        default: on = ((x % 5) < 3);        break;
        }
        if (on) {
            if (prev_y < 0) {
                pf_dot(lcd, x, y);
            } else {
                int a = (prev_y < y) ? prev_y : y;
                int b = (prev_y < y) ? y : prev_y;
                for (int yy = a; yy <= b; yy++) pf_dot(lcd, x, yy);
            }
            if (style == 2) pf_dot(lcd, x, y + 1);      /* 2px 粗: 相邻一行 */
        }
        prev_y = y;
    }
}

/* ============================ 弹窗绘制 ============================ */
static void pm_box(st7305_handle_t *lcd, int bx, int by)
{
    /* 只清弹窗自己这块: 弹窗之外是底层页面, 一个像素都不许动 */
    fill_rect(lcd, bx, by, bx + PM_W - 1, by + PM_H - 1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 2; k++) {
        draw_hline(lcd, bx + k, bx + PM_W - 1 - k, by + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, bx + k, bx + PM_W - 1 - k, by + PM_H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + k, by + k, by + PM_H - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + PM_W - 1 - k, by + k, by + PM_H - 1 - k, ST7305_COLOR_BLACK);
    }
}

/* 页签 chip 已删除（2026-11-03 linit：改为左右拖动内容区切页，不要页签行）。
   原来这里有 pm_tab_x() / pm_tab_chip()，随页签行一起去掉。 */

/* 时间窗口 chip (仅"趋势"页)。y 由调用方给 —— 原来固定在标题行 by+5，
 *   标题行删掉后改由趋势页自己定位（图表下方一行）。 */
#define PM_RANGE_W 34
#define PM_RANGE_GAP 4
static int pm_range_x(int bx, int i)
{
    int total = PF_RANGE_CNT * PM_RANGE_W + (PF_RANGE_CNT - 1) * PM_RANGE_GAP;
    return bx + PM_W - 10 - total + i * (PM_RANGE_W + PM_RANGE_GAP);
}

static void pm_range_chip(st7305_handle_t *lcd, int bx, int y, int i)
{
    int x = pm_range_x(bx, i);
    const char *t = s_range_name[i];
    if (i == s_range) {
        fill_rect(lcd, x, y, x + PM_RANGE_W - 1, y + 20, ST7305_COLOR_BLACK);
        ui_text16(lcd, x + (PM_RANGE_W - ui_text16w(t)) / 2, y + 2, t, true);
    } else {
        ui_text16(lcd, x + (PM_RANGE_W - ui_text16w(t)) / 2, y + 2, t, false);
    }
}

/* 图例: 前面一小段与曲线同样线型的样条, 一眼对得上 */
static void pm_legend_item(st7305_handle_t *lcd, int x, int y, int style, const char *txt)
{
    for (int i = 0; i < 12; i++) {
        bool on = (style == 0) ? true : (style == 1 ? ((i % 3) != 2) : ((i % 5) < 3));
        if (!on) continue;
        st7305_draw_pixel(lcd, x + i, y + 7, ST7305_COLOR_BLACK);
        if (style == 2) st7305_draw_pixel(lcd, x + i, y + 8, ST7305_COLOR_BLACK);
    }
    ui_text16(lcd, x + 16, y, txt, false);
}

static void pm_draw_trend(st7305_handle_t *lcd, int bx, int by)
{
    s_cx0 = bx + 12;  s_cx1 = bx + PM_W - 12;
    /* 标题行/页签行删除后内容区多了 54px：图表顺势加高。 */
    s_cy0 = PM_BODY_Y + 2;  s_cy1 = PM_BODY_Y + PM_TREND_H;

    draw_rect_outline(lcd, s_cx0 - 1, s_cy0 - 1, s_cx1 + 1, s_cy1 + 1, ST7305_COLOR_BLACK);
    /* 50% 参考虚线 (1 on 7 off, 比数据虚线更疏, 不会看混) */
    int y50 = s_cy1 - (s_cy1 - s_cy0) / 2;
    for (int x = s_cx0; x <= s_cx1; x += 8)
        st7305_draw_pixel(lcd, x, y50, ST7305_COLOR_BLACK);

    int want = pf_visible();
    if (want <= 0) {
        ui_text16c(lcd, (s_cx0 + s_cx1) / 2, (s_cy0 + s_cy1) / 2 - 8, "正在采集…", false);
        return;
    }
    /* 先粗线后细线: PSRAM(2px) → SRAM(1px 虚线) → CPU(1px 实线), 保证 CPU 最清楚 */
    pf_plot(lcd, want, 2, 2);
    pf_plot(lcd, want, 1, 1);
    pf_plot(lcd, want, 0, 0);

    /* 图例: 取最新一格的值。位置跟着内容区上移。 */
    pf_sample_t s;
    char b[16];
    int ly = s_cy1 + PM_LEGEND_DY;
    if (pf_get_sample(0, &s)) {
        snprintf(b, sizeof(b), "CPU %u%%",   (unsigned)s.cpu);   pm_legend_item(lcd, bx + 12,  ly, 0, b);
        snprintf(b, sizeof(b), "RAM %u%%",   (unsigned)s.sram);  pm_legend_item(lcd, bx + 130, ly, 1, b);
        snprintf(b, sizeof(b), "PSR %u%%",   (unsigned)s.psram); pm_legend_item(lcd, bx + 248, ly, 2, b);
    } else {
        ui_text16(lcd, bx + 12, ly, "等待采样", false);
    }

    /* 时间窗口 chip: 原来在标题行右侧，标题行删了 ⇒ 移到**图例下一行**。
     *   功能保留（1/5/10/30 分），只是不再占抬头那一排。 */
    for (int i = 0; i < PF_RANGE_CNT; i++) pm_range_chip(lcd, bx, PM_CHIP_Y, i);
}

static void pm_draw_mem(st7305_handle_t *lcd, int bx, int by)
{
    size_t ti = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t fi = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t mi = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t tp = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t fp = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t lp = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    char ti_s[12], ui_s[12], fi_s[12], mi_s[12];
    char tp_s[12], up_s[12], fp_s[12], lp_s[12];
    pf_fmt(ti_s, sizeof(ti_s), ti);
    pf_fmt(ui_s, sizeof(ui_s), (ti > fi) ? (ti - fi) : 0);
    pf_fmt(fi_s, sizeof(fi_s), fi);
    pf_fmt(mi_s, sizeof(mi_s), mi);
    pf_fmt(tp_s, sizeof(tp_s), tp);
    pf_fmt(up_s, sizeof(up_s), (tp > fp) ? (tp - fp) : 0);
    pf_fmt(fp_s, sizeof(fp_s), fp);
    pf_fmt(lp_s, sizeof(lp_s), lp);

    char up[16];
    pf_hms((uint32_t)(esp_timer_get_time() / 1000), up, sizeof(up));

    char line[4][64];
    snprintf(line[0], sizeof(line[0]), "内部RAM  %s / %s   余 %s", ui_s, ti_s, fi_s);
    snprintf(line[1], sizeof(line[1]), "         最低余 %s", mi_s);
    snprintf(line[2], sizeof(line[2]), "PSRAM    %s / %s   余 %s", up_s, tp_s, fp_s);
    snprintf(line[3], sizeof(line[3]), "         最大连续 %s", lp_s);

    int y = PM_BODY_Y + 4;
    for (int i = 0; i < 4; i++) { ui_text16(lcd, bx + 12, y, line[i], false); y += 24; }

    char tail[64];
    snprintf(tail, sizeof(tail), "任务 %u 个    运行 %s",
             (unsigned)uxTaskGetNumberOfTasks(), up);
    ui_text16(lcd, bx + 12, PM_BODY_Y + 100, tail, false);

    /* 一行结论, 省得看着一堆数字不知道正不正常 */
    const char *hint = "内存正常";
    if (mi < 8 * 1024)                 hint = "内部 RAM 余量偏低";
    else if (lp < 8 * 1024)            hint = "PSRAM 连续块偏低";
    ui_text16(lcd, bx + 12, PM_BODY_Y + 122, hint, false);
}

static void pm_draw_tasks(st7305_handle_t *lcd, int bx, int by)
{
    perf_row_t rows[PF_ROWS_MAX];
    int n = perf_build(rows, PF_ROWS_MAX);

    /* 表头（可点：名称/CPU/SRAM/PSRAM 切排序；名称升序，其余按已用降序） */
    const int x_name = bx + 8, x_cpu = bx + 150, x_sram = bx + 222, x_psram = bx + 292;
    ui_text16(lcd, x_name,  PM_BODY_Y, "名称",  false);
    ui_text16(lcd, x_cpu,   PM_BODY_Y, "CPU",   false);
    ui_text16(lcd, x_sram,  PM_BODY_Y, "SRAM",  false);
    ui_text16(lcd, x_psram, PM_BODY_Y, "PSRAM", false);
    /* 选中列不再划线 (linit 2026-11-03): 只保留表头分隔线 */
    draw_hline(lcd, bx + 10, bx + PM_W - 11, PM_BODY_Y + 21, ST7305_COLOR_BLACK);

    char buf[16];
    int y = PM_BODY_Y + 26;
    for (int i = 0; i < n && i < PF_SHOW_ROWS; i++) {
        const perf_row_t *r = &rows[i];
        ui_text16(lcd, x_name, y, r->name, false);
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)r->cpu);
        ui_text16(lcd, x_cpu, y, buf, false);
        snprintf(buf, sizeof(buf), r->used_sram_kb ? "%uK" : "0", (unsigned)r->used_sram_kb);
        ui_text16(lcd, x_sram, y, buf, false);
        snprintf(buf, sizeof(buf), r->used_psram_kb ? "%uK" : "0", (unsigned)r->used_psram_kb);
        ui_text16(lcd, x_psram, y, buf, false);
        y += 22;
    }
    if (n == 0) ui_text16(lcd, bx + 12, PM_BODY_Y + 30, "暂无数据", false);
}

/* 面板绘制（**唯一实现**）：弹窗与独立应用页共用，保证两处画面逐像素一致。
 * bx/by = 面板左上角；本函数只画 (bx,by)-(bx+PM_W-1,by+PM_H-1) 这块。 */
static void pm_draw_panel(st7305_handle_t *lcd, int bx, int by)
{
    pm_box(lcd, bx, by);

    /* ★ 2026-11-03 linit: **标题行整行删除** —— 不要「性能监视」那一排。
     *   原来这里是: "性能监视" + 时间窗口chip(趋势页) 或 页码(其它页)，
     *   下面还有一整行页签(趋势/内存/任务)。两行抬头把内容区挤到 by+60。
     *   现在只剩内容区，**左右拖动内容区**切页（pm_tick 里的 s_drag）。
     *   趋势页的时间窗口 chip 移到**内容区右下角**（pm_draw_trend 里画），
     *   功能不丢，但不再占抬头。 */
    switch (s_tab) {
    case 0:  pm_draw_trend(lcd, bx, by); break;
    case 1:  pm_draw_mem(lcd, bx, by);   break;
    default: pm_draw_tasks(lcd, bx, by); break;
    }

    /* 底栏: 左提示 · 右"关闭" */
    draw_hline(lcd, bx + 8, bx + PM_W - 9, by + PM_H - 40, ST7305_COLOR_BLACK);
    {
        char hint[24];
        if (s_tab == 0) {
            char t[12];
            pf_mmss((uint32_t)pf_sample_count() * PF_PERIOD_MS, t, sizeof(t));
            snprintf(hint, sizeof(hint), "已采 %s", t);
        } else if (s_tab == 1) {
            snprintf(hint, sizeof(hint), "按 1s 采样");
        } else {
            snprintf(hint, sizeof(hint), "按任务统计");
        }
        ui_text16(lcd, bx + 12, by + PM_H - 30, hint, false);
    }

    int cbx = bx + PM_W - 76;
    draw_rect_outline(lcd, cbx, by + PM_H - 34, cbx + 64, by + PM_H - 8, ST7305_COLOR_BLACK);
    ui_text16c(lcd, cbx + 32, by + PM_H - 30, "关闭", false);
}

static bool pm_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud)
{
    (void)d; (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return true;

    int bx = (ui_screen_w() - PM_W) / 2;
    int by = (ui_screen_h() - PM_H) / 2;
    pm_draw_panel(lcd, bx, by);
    return true;
}

/* ============================ 交互 ============================ */
static void pm_close(ui_ctx_t *ctx)
{
    /* 先停采集/放 PSRAM, 再关窗: 关窗后底层页面会整屏重绘 */
    pf_free_buffers();
    ctx->needs_redraw = true;
    os_dialog_pop(ctx);
}

/* 面板内点击处理（弹窗与页面共用）。返回 true = 点到了「关闭」。
 * 调用方负责坐标范围检查; 本函数假定 (x,y) 落在面板内。 */
static bool pm_hit_panel(ui_ctx_t *ctx, int bx, int by, int x, int y)
{
    /* 关闭按钮 */
    if (y >= by + PM_H - 36 && x >= bx + PM_W - 80) return true;

    /* 时间窗口 chip (仅趋势页)。位置与 pm_draw_trend 里画的**同一处**（共用 PM_CHIP_Y）。 */
    if (s_tab == 0) {
        if (y >= PM_CHIP_Y && y <= PM_CHIP_Y + 20) {
            for (int i = 0; i < PF_RANGE_CNT; i++) {
                int rx = pm_range_x(bx, i);
                if (x >= rx && x < rx + PM_RANGE_W) {
                    s_range = i;
                    ctx->needs_redraw = true;
                    return false;
                }
            }
        }
    }

    /* 页签行已删除（2026-11-03）—— 切页改为左右拖动内容区 / 左右方向键。
     * 原来这里按 PM_TAB_Y0 判定点击三个 chip，现已移除。 */

    /* 任务页表头: 点 名称/CPU/SRAM/PSRAM 切排序（名称升序，其余按已用降序） */
    if (s_tab == 2 && y >= PM_BODY_Y - 2 && y <= PM_BODY_Y + 20) {
        if      (x >= bx + 8   && x <= bx + 144) { s_sort = 0; ctx->needs_redraw = true; return false; }
        else if (x >= bx + 150 && x <= bx + 184) { s_sort = 1; ctx->needs_redraw = true; return false; }
        else if (x >= bx + 222 && x <= bx + 262) { s_sort = 2; ctx->needs_redraw = true; return false; }
        else if (x >= bx + 292 && x <= bx + 336) { s_sort = 3; ctx->needs_redraw = true; return false; }
    }
    return false;
}

static bool pm_touch(ui_ctx_t *ctx, os_dlg_stack_t *d, int x, int y, void *ud)
{
    (void)d; (void)ud;
    int bx = (ui_screen_w() - PM_W) / 2;
    int by = (ui_screen_h() - PM_H) / 2;

    /* 弹窗外: 吃掉, 不关闭 (与系统其他弹窗一致) */
    if (x < bx || x >= bx + PM_W || y < by || y >= by + PM_H) return true;

    if (pm_hit_panel(ctx, bx, by, x, y)) { pm_close(ctx); return true; }
    return true;
}

/* BACK 的"退出?"用小号确认窗 (os_dialog_confirm_ex = small 自适应样式, 不是 350×250 大窗)。
 * 确定: 连弹两层 (小确认 → 性能弹窗); 底层页面由 os_dialog_pop 的 os_invalidate_bg 整屏重绘恢复。 */
static void pm_exit_cb(ui_ctx_t *ctx, int result, void *ud)
{
    (void)ud;
    if (result != 0) return;          /* 取消/超时: 留在性能弹窗 */
    pf_free_buffers();
    os_dialog_pop(ctx);               /* 弹掉小确认 */
    os_dialog_pop(ctx);               /* 再弹掉性能监视弹窗 → 回到打开前的画面 */
}

static bool pm_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud)
{
    (void)d; (void)ud;
    switch (a) {
    case OS_ACTION_LEFT:
        s_tab = (s_tab + 2) % 3;
        ctx->needs_redraw = true;
        return true;
    case OS_ACTION_RIGHT:
        s_tab = (s_tab + 1) % 3;
        ctx->needs_redraw = true;
        return true;
    case OS_ACTION_UP:
        if (s_tab != 0) return false;
        s_range = (s_range + 1) % PF_RANGE_CNT;
        ctx->needs_redraw = true;
        return true;
    case OS_ACTION_DOWN:
        if (s_tab != 0) return false;
        s_range = (s_range + PF_RANGE_CNT - 1) % PF_RANGE_CNT;
        ctx->needs_redraw = true;
        return true;
    case OS_ACTION_CONFIRM:
        s_tab = (s_tab + 1) % 3;
        ctx->needs_redraw = true;
        return true;
    case OS_ACTION_BACK:
        /* 退出走**小号**确认窗: 误触返回不会直接把监视器关掉 */
        s_drag = false;
        os_dialog_confirm_ex(ctx, "退出性能监视?", 0, -1, pm_exit_cb, NULL);
        return true;
    default:
        return false;
    }
}

/* 每帧轮询: 到 1s 就采一拍并重绘。没有独立任务, 关窗即停。 */
static bool     s_fresh = true;      /* 刚打开: 第一拍只对齐节拍, 不采 (避免 0ms 窗口算出假值) */
static uint32_t s_last_ms;

/* 左右拖拉在 pm_poll 里处理 (见上方的 s_drag 声明) */

/* 每帧节拍（弹窗与页面共用）: 左右拖拉切页签 + 每 1s 采一拍。
 * 没有独立任务，页面/弹窗一关就停。 */
static void pm_tick(ui_ctx_t *ctx)
{
    /* ---- 左右拖拉切页签 ---- */
    {
        int tx = 0, ty = 0;
        if (input_touch_now(&tx, &ty)) {
            if (!s_drag) { s_drag = true; s_drag_x0 = tx; }
            s_drag_xl = tx;
        } else if (s_drag) {
            s_drag = false;
            int dx = s_drag_xl - s_drag_x0;
            if (dx <= -PM_DRAG_PX)      { s_tab = (s_tab + 1) % 3; ctx->needs_redraw = true; }
            else if (dx >= PM_DRAG_PX)  { s_tab = (s_tab + 2) % 3; ctx->needs_redraw = true; }
        }
    }

    uint32_t now = pf_now_ms();
    if (s_fresh) { s_fresh = false; s_last_ms = now; return; }
    if ((uint32_t)(now - s_last_ms) < PF_PERIOD_MS) return;
    s_last_ms = now;
    pf_sample_once(true);
    ctx->needs_redraw = true;
}

static void pm_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud)
{
    (void)d; (void)ud;
    pm_tick(ctx);
}

/* 弹窗关掉(含 BACK): 释放 PSRAM。os_dialog_pop 路径已自行释放过, 这里幂等。 */
static void pm_closed(ui_ctx_t *ctx, int result, void *ud)
{
    (void)ctx; (void)result; (void)ud;
    pf_free_buffers();
}

/* ============================ 入口 ============================ */
void perfmon_open(ui_ctx_t *ctx)
{
    if (!ctx) return;
    if (pf_alloc() != ESP_OK) {
        os_dialog_toast(ctx, "内存不足, 无法启动采集");
        return;
    }
    pf_sample_once(false);        /* 建立差分基线: 第一拍只记时间, 不出样本 */
    s_fresh = true;               /* 第一次 poll 只对齐节拍, 1 秒后才出第一个点 */

    s_tab     = 2;                /* ★ 默认进「任务」视图 (名称/CPU/SRAM/PSRAM 四列) */
    s_range   = 1;
    s_sort    = 0;                /* 默认按名称(A~Z)排序 */

    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.count     = 0;            /* 0 = 自定义整层渲染/触摸 */
    dlg.no_footer = true;         /* 自己的底栏 */
    dlg.on_render = pm_render;
    dlg.on_touch  = pm_touch;
    dlg.on_key    = pm_key;
    dlg.on_poll   = pm_poll;
    dlg.cb        = pm_closed;
    os_dialog_push(ctx, &dlg);
    ESP_LOGI(PF_TAG, "打开: 350x250 弹窗, 历史缓冲 %u B (PSRAM)",
             (unsigned)(sizeof(pf_sample_t) * PF_MAX_SAMPLES));
}

/* ============================================================================
 * 独立应用页 (2026-11-03 linit: "保持目前弹窗UI, 但独立出来做成 C 应用")
 * ----------------------------------------------------------------------------
 * 画面**完全沿用上面那块 350×250 面板**（pm_draw_panel，唯一实现），
 * 不另做一套 UI —— 弹窗和独立应用看起来必须是同一个东西。
 * 区别只有三点：
 *   · 作为页面时面板落在**状态栏下方的内容区**里居中（整体下移约 12px，不压时间/电量）；
 *   · 画面里那个「关闭」= 退出应用，走系统统一的「退出程序?」确认（同返回键）；
 *   · 硬件返回键由内核按 .exit_confirm 统一弹确认，页面不再自己弹一层。
 * 采集依旧是"进页面才分配、退出即释放"，没有常驻任务、没有开机占用。
 * 入口: 应用中心 → 应用 → 系统。
 * ========================================================================== */
#define PMG_TOP   24                                              /* 状态栏高度 */
#define PMG_BX    (((int)ui_screen_w() - PM_W) / 2)
#define PMG_BY    (PMG_TOP + ((int)ui_screen_h() - PMG_TOP - PM_H) / 2)

/* 前向声明: 返回确认回调 (pmg_action 的 BACK 分支引用) */
static void pmg_back_cb(ui_ctx_t *ctx, int result, void *ud);

static void pmg_enter(ui_ctx_t *ctx)
{
    if (pf_alloc() != ESP_OK) {
        os_dialog_toast(ctx, "内存不足, 无法启动采集");
        return;                       /* 退化成一张白底页, 不画半截面板 */
    }
    s_bg_running = false;             /* 重新进入 ⇒ 不再是后台态(保留的历史继续用) */
    pf_sample_once(false);            /* 建差分基线 */
    s_fresh = true;                   /* 第一帧只对齐节拍, 1 秒后出第一个点 */
    s_tab = 2; s_range = 1; s_sort = 0;   /* ★ 默认「任务」视图 + 名称序 */
    s_drag = false;
    ESP_LOGI(PF_TAG, "打开(独立应用): 350x250 面板, 历史缓冲 %u B (PSRAM)",
             (unsigned)(sizeof(pf_sample_t) * PF_MAX_SAMPLES));
}

static void pmg_exit(ui_ctx_t *ctx)
{
    (void)ctx;
    if (s_bg_running) return;         /* 后台态: 保留 PSRAM 历史, 暂不释放 */
    pf_free_buffers();                /* 退出即还 PSRAM (约 63KB) */
}

static void pmg_render(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    /* ★ 2026-11-03 linit: **弹窗形态** —— 不再整屏刷白。
     *   原来这里 st7305_clear(WHITE) 把整屏涂白，面板四周是白茫茫一片，
     *   看起来就是个全屏应用，而不是弹窗。
     *   现在只画面板自己那块矩形，四周**透出底层页面**（应用中心），
     *   和 os_dialog 的弹窗一致（dlg_render 同样不清屏、不重绘背景）。
     *   ⚠ 面板底色仍要自己填白，否则底下的应用图标会透进面板里。 */
    if (!s_buf) return;               /* 采集缓冲没拿到: 不画半截面板 */
    pm_draw_panel(lcd, PMG_BX, PMG_BY);
}

static void pmg_poll(ui_ctx_t *ctx)
{
    if (!s_buf) return;
    if (os_dialog_depth() > 0) return;   /* 确认框打开时不采样/不重绘, 避免与弹窗抢画导致白屏 */
    pm_tick(ctx);
}

/* 关闭性能监视（弹窗语义）：释放 PSRAM + 标记背景脏 + 退回底层页面。
 * ⚠ 必须调 os_invalidate_bg()：本模块是 fullscreen=false 的模态覆盖页，
 *   内核 pop 时只对 **fullscreen** 模态页置背景脏（os_core.c pop 里那行），
 *   不标记的话面板那块矩形会留残影在应用中心画面上。 */
static void pmg_close(ui_ctx_t *ctx)
{
    pf_free_buffers();
    os_invalidate_bg();
    os_pop(ctx);
}

/* 返回确认回调: 0=退出(释放 PSRAM+清历史) · 1=后台(保留历史) · -1=取消(留在监视器)
 * 复用修改机(page_modtool.c)的成熟收尾: os_dialog_clear_all → os_pop。 */
static void pmg_back_cb(ui_ctx_t *ctx, int result, void *ud)
{
    (void)ud;
    if (result < 0) return;           /* 取消: 什么都不做, 留在性能监视 */
    os_dialog_clear_all(ctx);         /* 先收掉小确认框 (同修改机) */
    s_bg_running = (result != 0);     /* 0=退出(彻底释放) · 1=后台(保留 s_buf 历史) */
    os_invalidate_bg();               /* 非全屏模态页: 标记背景脏, 防面板残影 */
    os_pop(ctx);                      /* 关监视器回底层; on_exit(pmg_exit) 按 s_bg_running 决定是否释放 */
}

static void pmg_action(ui_ctx_t *ctx, os_action_t a)
{
    switch (a) {
    case OS_ACTION_LEFT:    s_tab = (s_tab + 2) % 3; ctx->needs_redraw = true; break;
    case OS_ACTION_RIGHT:   s_tab = (s_tab + 1) % 3; ctx->needs_redraw = true; break;
    case OS_ACTION_UP:      if (s_tab == 0) { s_range = (s_range + 1) % PF_RANGE_CNT; ctx->needs_redraw = true; } break;
    case OS_ACTION_DOWN:    if (s_tab == 0) { s_range = (s_range + PF_RANGE_CNT - 1) % PF_RANGE_CNT; ctx->needs_redraw = true; } break;
    case OS_ACTION_CONFIRM: s_tab = (s_tab + 1) % 3; ctx->needs_redraw = true; break;
    case OS_ACTION_BACK: {
        /* 返回弹**小确认框** (同修改机: os_dialog_push + small=true)：
         * ★ 原来用 os_dialog_list 是 350×250 大窗，会和本面板叠在一起导致白屏卡死、
         *   无法退出(linit 2026-11-03 反馈)。改用标准小自适应确认模板。
         * 两选项「退出」(彻底释放) / 「后台」(保留采集与历史)。 */
        os_dlg_stack_t dlg;
        memset(&dlg, 0, sizeof(dlg));
        snprintf(dlg.title, sizeof(dlg.title), "退出性能监视？");
        dlg.count = 2;
        snprintf(dlg.items[0], sizeof(dlg.items[0]), "退出");
        snprintf(dlg.items[1], sizeof(dlg.items[1]), "后台");
        dlg.sel       = 1;         /* 默认高亮「后台」(非破坏) */
        dlg.no_footer = true;      /* 确认框: 两键, 无底部返回行 */
        dlg.small     = true;      /* 小自适应确认模板 (同修改机) */
        dlg.cb        = pmg_back_cb;
        os_dialog_push(ctx, &dlg);
        break;
    }
    default: break;
    }
}

static bool pmg_touch(ui_ctx_t *ctx, int x, int y)
{
    int bx = PMG_BX, by = PMG_BY;
    /* 面板外不消费: 让内核照常处理手势(上滑返回等) */
    if (x < bx || x >= bx + PM_W || y < by || y >= by + PM_H) return false;
    if (pm_hit_panel(ctx, bx, by, x, y)) {
        /* 「关闭」= 退出监视器, 回到底层页面（弹窗语义） */
        pmg_close(ctx);
    }
    return true;
}

static const os_module_t s_mod_perfmon = {
    .name         = "perfmon",
    .page_id      = OS_PAGE_PERFMON,
    .on_enter     = pmg_enter,
    .on_exit      = pmg_exit,
    .render       = pmg_render,
    .action       = pmg_action,
    .touch        = pmg_touch,
    .poll         = pmg_poll,
    .fullscreen   = false,   /* 顶部保留状态栏: 时间/电量对"系统"类工具是必要信息 */
    /* ★ 2026-11-03 linit: **弹窗形态** —— 底层应用中心画面保留，
     *   性能监视面板浮在上面，四周透出底层（不再整屏刷白）。
     *   modal=true 的语义（os_core.c push）：不退出底层、底层保持活动、pop 时不重进。
     *   exit_confirm 关掉：返回键由页面自己处理（见 pmg_action / pmg_touch 的关闭按钮）。 */
    .modal        = true,
    .exit_confirm = false,
    /* 看曲线是"放着观察"的行为: 空闲不能被弹回桌面, 否则刚攒的趋势窗白等。
     * 与收藏夹同一理由。息屏/屏保不受影响(那是 wallpaper 的空闲计时, 另一条路径)。 */
    .keep_on_idle = true,
};

void os_page_perfmon_register(void) { os_register(&s_mod_perfmon); }
