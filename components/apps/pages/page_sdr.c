/**
 * page_sdr.c — 频谱仪 (2.4G 原始 I/Q → 本机 FFT → 屏上频谱 + 瀑布)
 *
 * 为什么能这么干: 官方 ESP-SDR 的瓶颈是"把 2.56 Gbit/s 的原始流搬出去",
 * 而频谱/瀑布图只需要**片段**做 FFT, 所以宁可本机算完只画一张图 ——
 * 数据一步都不出芯片, 占空比不再是问题。
 *
 * 分工 (2026-10-01 拆开):
 *   本文件   = 采集 + DSP (取 I/Q / 去 DC / 加窗 / FFT / 分列 / 平均 / peak-hold / 瀑布行)
 *   sdr_view = 版式 + 绘制 (五分区, 见 sdr_view.h)。坐标换算只有 sdr_view 一处来源。
 *
 * ⚠ 射频独占: 进入本页要接管 PHY, **不能与正常 WiFi/BT 收发同时使用**。
 *   实测 (2026-10-01): 手柄正在连着时直接抢射频 → 约 2.4s 后
 *   `rst:0x8 TG1WDT_SYS_RST` 复位。所以进入前**先让蓝牙让出** (拆栈),
 *   退出后恢复 (bt_manager 会自动回连)。顺带还回 ~97KB 内部 RAM。
 * ⚠ 采集窗口内不要写 flash (NVS/日志) —— 本页热路径刻意不打 ESP_LOGI。
 */
#include "os_app_api.h"
#include "sdr_view.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "dsps_fft2r.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TAG "SDR"

/* ---------------- DSP 参数 ---------------- */
#define FFT_N     2048                /* 分辨率 = 80MSa/s / 2048 ≈ 39 kHz */
#define FS_MHZ    80.0f               /* divider=0 → 80 MSa/s */
#define SPAN_DEF  80                  /* 默认跨度 MHz (全带宽: 一次看到 2412/2437/2462 三条信道) */
#define STEP_MS   100                 /* 刷新节拍 (10 Hz) */
#define COLS      SDRV_PLOT_W         /* 显示列数 (368) */

/* 本底直方图: 1dB 一格, 覆盖 [HIST_LO, HIST_LO + HIST_N) */
#define HIST_LO   (-120.0f)
#define HIST_N    125

/* ---------------- 页面状态 (static, 不出本文件) ---------------- */
static float   *s_fft;                    /* 2*FFT_N 交错 I/Q, PSRAM */
static float   *s_bin;                    /* N 个 bin 的 dB, PSRAM */
static float   *s_win;                    /* Hann 窗, PSRAM */
static uint8_t *s_wf;                     /* 瀑布位图 PSRAM */
/* 每帧热数组也放 PSRAM (linit 2026-10-01: "内存可以用很多, 但别占核心内存") */
static float   *s_col;                    /* 本帧显示列 (dB) */
static float   *s_avg;                    /* 指数平均 (显示用) */
static float   *s_hold;                   /* peak-hold 包络 */
static int      s_freq = 2437;
static int      s_span = SPAN_DEF;        /* 跨度 MHz: 80/40/20/10, 顶栏点一下循环 */
static int      s_gain = ESPSDR_GAIN_AUTO;
static int      s_avg_n = 4;
static int      s_marker_x = COLS / 2;    /* tracking marker: 自动跟最高峰 */
static int      s_marker_rel;             /* 峰值强度: 高于本底多少 dB (显示口径) */
static float    s_floor = -80.0f;         /* 本底 (dB) —— 显示量程以它为 0 */
static bool     s_floor_valid;

/* ---- 判读 / 菜单 (2026-10-02) ---- */
static bool     s_hold_on = true;         /* peak-hold 开关 (底栏"保持"槽) */
/* 页号: 0 = 模式菜单, 1 = 全景, 2 = 信道, 3 = 蓝牙, 4 = 干扰 (一屏只干一件事) */
static int      s_page;
static int      s_menu_sel;               /* 模式菜单选中项 */
static const char *const s_mode_name[4] = { "看全景", "看信道", "看蓝牙", "看干扰" };
static const char *const s_mode_note[4] = { "哪儿有信号", "该用哪条", "有无广播", "有无干扰" };
static float    s_median_db;              /* 本底中位数 (判读用) */
static int      s_ch_pct[13];             /* WiFi ch1..13 占用度 0..100; -1 = 不在当前跨度 */
static int      s_ble_mhz;                /* 检到的 BLE 广播频率; 0 = 未检到 */
static int      s_ble_db[3];              /* 2402/2426/2480 三处高出本底 dB */
static int      s_n_sig;                  /* 明显信号个数 */

/* ---- 判读快照 (linit 2026-10-02: "给了判断结果不需要一直跳动, 点确认才进行下一步判断") ----
 * 每帧算的是"实时量", 但**上屏的结论必须定格**: 进内容页先扫 SDR_SCAN_MS 毫秒, 扫完把
 * 结果**拷进快照**并锁住, 之后不管频谱怎么动, 数字/结论都不再变; 按"确认"才重新扫一次。 */
#define SDR_SCAN_MS 1500
typedef struct {
    int  pct[13];
    int  ble_db[3];
    int  n_sig;
    int  worst_ch, best_ch, ble_mhz, broadband;
    char verdict[48], advice[48];
} sdr_snap_t;
static sdr_snap_t s_snap;
static bool       s_latch;          /* 快照有效 (定格中) */
static int64_t    s_scan_until;     /* 扫描结束时刻 ms */
static int      s_broadband;              /* 1 = 检到宽带抬高 (微波炉之类) */
static int      s_worst_ch, s_best_ch;    /* 最挤 / (1·6·11 里)最空 */
static char     s_verdict[48];
static char     s_advice[48];
static int      s_batt = -1;
static bool     s_ok;
static bool     s_wifi_owned;             /* 本页把 WiFi 开起来的 → 退出负责关回去 */
static bool     s_bt_owned;               /* 本页把蓝牙让出去的 → 退出负责恢复 */
static int64_t  s_last_ms, s_batt_ms, s_hint_until;
static int      s_cap_n, s_fail_n;
static uint32_t s_elapsed_us;


/* ---------------- 判读快照 ---------------- */
static void sdr_scan_start(void)
{
    s_latch      = false;
    s_scan_until = esp_timer_get_time() / 1000 + SDR_SCAN_MS;
}
static void sdr_scan_latch(void)
{
    for (int i = 0; i < 13; i++) s_snap.pct[i] = s_ch_pct[i];
    for (int i = 0; i < 3;  i++) s_snap.ble_db[i] = s_ble_db[i];
    s_snap.n_sig     = s_n_sig;
    s_snap.worst_ch  = s_worst_ch;
    s_snap.best_ch   = s_best_ch;
    s_snap.ble_mhz   = s_ble_mhz;
    s_snap.broadband = s_broadband;
    snprintf(s_snap.verdict, sizeof s_snap.verdict, "%s", s_verdict);
    snprintf(s_snap.advice,  sizeof s_snap.advice,  "%s", s_advice);
    s_latch = true;
}

/* ---------------- 扫描判读 ----------------
 * 判据 (与 docs/频谱仪应用-完整方案与计划.md 的表一致):
 *   WiFi: 信道中心 2412+5(n-1), 取 ±10MHz 内的最大值, 高出本底多少 → 占用度
 *   BLE : 广播只在 2402/2426/2480, 取 ±2MHz 内突起 ≥15dB 即认为在广播
 *   宽带: 频谱中位数整体高于本底 ≥10dB (微波炉/无绳电话这类没有信道形状) */
static void sdr_analyze(void)
{
    if (!s_floor_valid) return;

    float f_lo = (float)s_freq - (float)s_span * 0.5f;
    float f_hi = (float)s_freq + (float)s_span * 0.5f;

    s_worst_ch = 0;
    int worst = -1;
    for (int n = 1; n <= 13; n++) {
        float c = 2412.0f + 5.0f * (float)(n - 1);
        if (c + 10.0f < f_lo || c - 10.0f > f_hi) { s_ch_pct[n - 1] = -1; continue; }
        int i0 = (int)lrintf((c - 10.0f - f_lo) / (float)s_span * (float)(COLS - 1));
        int i1 = (int)lrintf((c + 10.0f - f_lo) / (float)s_span * (float)(COLS - 1));
        if (i0 < 0) i0 = 0;
        if (i1 > COLS - 1) i1 = COLS - 1;
        float mx = SDRV_DB_NODATA;
        for (int i = i0; i <= i1; i++) if (s_avg[i] > mx) mx = s_avg[i];
        int over = (int)lrintf(mx - s_floor);
        if (over < 0) over = 0;
        int pct = over * 100 / 30;                 /* 高于本底 30dB 记作 100% (实测 45dB 太宽, 全是低值) */
        if (pct > 100) pct = 100;
        s_ch_pct[n - 1] = pct;
        if (pct > worst) { worst = pct; s_worst_ch = n; }
    }

    s_ble_mhz = 0;
    static const float ble[3] = { 2402.0f, 2426.0f, 2480.0f };
    for (int k = 0; k < 3; k++) {
        int i0 = (int)lrintf((ble[k] - 2.0f - f_lo) / (float)s_span * (float)(COLS - 1));
        int i1 = (int)lrintf((ble[k] + 2.0f - f_lo) / (float)s_span * (float)(COLS - 1));
        if (i0 < 0) i0 = 0;
        if (i1 > COLS - 1) i1 = COLS - 1;
        float mx = SDRV_DB_NODATA;
        for (int i = i0; i <= i1; i++) if (s_avg[i] > mx) mx = s_avg[i];
        s_ble_db[k] = (int)lrintf(mx - s_floor);              /* 三处都算, 蓝牙页要画三条 */
        if (!s_ble_mhz && (mx - s_floor) >= 15.0f) s_ble_mhz = (int)ble[k];
    }
    /* 明显信号个数: 高于本底 12dB 的局部极大 (5 列窗口) */
    s_n_sig = 0;
    for (int i = 2; i < COLS - 2; i++) {
        if (s_avg[i] - s_floor < 12.0f) continue;
        if (s_avg[i] >= s_avg[i - 1] && s_avg[i] >= s_avg[i + 1] &&
            s_avg[i] >  s_avg[i - 2] && s_avg[i] >  s_avg[i + 2]) s_n_sig++;
    }

    s_broadband = ((s_median_db - s_floor) >= 10.0f) ? 1 : 0;

    /* 推荐只在**不重叠的 1/6/11** 里挑最空的 (这是现实里选信道的那三条) */
    s_best_ch = 0;
    int best = 101;
    static const int trio[3] = { 1, 6, 11 };
    for (int k = 0; k < 3; k++) {
        int p = s_ch_pct[trio[k] - 1];
        if (p < 0) continue;
        if (p < best) { best = p; s_best_ch = trio[k]; }
    }

    if (worst >= 40)      snprintf(s_verdict, sizeof s_verdict, "有 WiFi 路由器在 ch%d", s_worst_ch);
    else if (s_ble_mhz)   snprintf(s_verdict, sizeof s_verdict, "未见 WiFi, 有 BLE 广播");
    else if (s_broadband) snprintf(s_verdict, sizeof s_verdict, "整段抬高, 疑似宽带干扰");
    else                  snprintf(s_verdict, sizeof s_verdict, "这段基本干净");
    if (s_broadband)      snprintf(s_advice, sizeof s_advice, "建议离开此段或查微波炉类设备");
    else if (s_best_ch)   snprintf(s_advice, sizeof s_advice, "路由器建议用 ch%d", s_best_ch);
    else                  snprintf(s_advice, sizeof s_advice, "当前跨度看不到 1/6/11");
}

/* ---------------- 一趟采集 + 处理 (热路径, 不打日志) ---------------- */
static bool sdr_step(void)
{
    int n = espsdr_capture(FFT_N, ESPSDR_DIV_80M);
    s_elapsed_us = espsdr_last_elapsed_us();
    if (n <= 0) { s_fail_n++; return false; }
    s_cap_n++;

    /* 1. 取 I/Q + 去 DC (消掉零中频的直流尖峰) */
    float mi = 0, mq = 0;
    for (int k = 0; k < FFT_N; k++) {
        float I, Q; espsdr_iq(k, &I, &Q);
        s_fft[2 * k] = I; s_fft[2 * k + 1] = Q;
        mi += I; mq += Q;
    }
    mi /= FFT_N; mq /= FFT_N;
    for (int k = 0; k < FFT_N; k++) {
        s_fft[2 * k]     = (s_fft[2 * k]     - mi) * s_win[k];
        s_fft[2 * k + 1] = (s_fft[2 * k + 1] - mq) * s_win[k];
    }

    /* 2. FFT (esp-dsp, S3 向量指令) + 自然序 */
    dsps_fft2r_fc32(s_fft, FFT_N);
    dsps_bit_rev_fc32(s_fft, FFT_N);

    /* 3. 每个 bin 的 dB (带 fftshift: bin k → k - N/2) */
    const float norm = 1.0f / (float)FFT_N;
    for (int k = 0; k < FFT_N; k++) {
        float re = s_fft[2 * k] * norm, im = s_fft[2 * k + 1] * norm;
        float p  = re * re + im * im;
        s_bin[k] = 10.0f * log10f(p + 1e-12f);
    }

    /* 4. FFT_N bin → COLS 显示列 (每列取区间最大值, 窄带信号不会被抽掉)
     *    列 ↔ 频率一律走 sdr_view 的换算, 保证与刻度/marker/触摸同源 */
    const int half = FFT_N / 2;
    for (int i = 0; i < COLS; i++) {
        /* ⚠ 必须是**相对中心**的偏移, 不是绝对频率 —— FFT 是对复基带做的,
         *   bin k ↔ 偏移 (k - N/2)·FS/N。用绝对频率算索引会直接越界 (2437MHz
         *   会算出 6 万多的下标), 结果每列都取不到值 → 画面永远空白。 */
        float f0 = sdrv_mhz_of_col(i,     (unsigned)s_freq, (float)s_span) - (float)s_freq;
        float f1 = sdrv_mhz_of_col(i + 1, (unsigned)s_freq, (float)s_span) - (float)s_freq;
        int b0 = half + (int)floorf(f0 / FS_MHZ * FFT_N);
        int b1 = half + (int)ceilf (f1 / FS_MHZ * FFT_N);
        if (b0 < 0) b0 = 0;
        if (b1 > FFT_N) b1 = FFT_N;
        if (b1 <= b0) b1 = b0 + 1;
        float mx = -200.0f;
        for (int b = b0; b < b1 && b < FFT_N; b++) if (s_bin[b] > mx) mx = s_bin[b];
        s_col[i] = mx;
    }
    /* 中心 ±2 列做 DC 修补 (用外侧列平均盖掉残留直流尖峰) */
    const int c = (COLS - 1) / 2;
    for (int k = -2; k <= 2; k++) {
        float l = s_col[c + k - 3], r = s_col[c + k + 3];
        s_col[c + k] = 0.5f * (l + r);
    }

    /* 5. 指数平均 (avg_n=1 时不平滑) + peak-hold */
    float a = (s_avg_n <= 1) ? 1.0f : 1.0f / (float)s_avg_n;
    for (int i = 0; i < COLS; i++) {
        s_avg[i] += (s_col[i] - s_avg[i]) * a;
        if (s_hold_on && s_avg[i] > s_hold[i]) s_hold[i] = s_avg[i];
    }

    /* 6. 本底估计: 1dB 直方图取中位数 (稳健), 慢速平滑避免画面"呼吸"。
     *    显示量程 = 本底-20 .. 本底+60, 于是刻度恒为 0/20/40/60 (dB 高于本底)。 */
    static uint16_t hist[HIST_N];   /* ⚠ 必须 uint16: 368 列会往同一 bin 累加, uint8_t 直接回绕 */
    memset(hist, 0, sizeof(hist));
    for (int i = 0; i < COLS; i++) {
        int q = (int)(s_avg[i] - HIST_LO);
        hist[q < 0 ? 0 : q >= HIST_N ? HIST_N - 1 : q]++;
    }
    int acc = 0, q50 = 0;
    for (int q = 0; q < HIST_N; q++) { acc += hist[q]; if (acc >= COLS / 2) { q50 = q; break; } }
    float nf = HIST_LO + (float)q50 + 0.5f;
    s_median_db = nf;                        /* 判读要用: 中位数抬高就是宽带干扰 */
    if (!s_floor_valid) {
        if (q50 > 4) { s_floor = nf; s_floor_valid = true; }   /* 等数据脱离哨兵区再定本底 */
    } else {
        s_floor += (nf - s_floor) * 0.05f;                     /* 慢速跟随 */
    }

    /* 7. tracking marker: 跟最高峰 (两端各排除 6 列 —— 边缘毛刺会把 marker 拽到屏幕边上) */
    const int edge = 6;
    float pk = SDRV_DB_NODATA; int pk_i = c;
    for (int i = edge; i < COLS - edge; i++) if (s_avg[i] > pk) { pk = s_avg[i]; pk_i = i; }
    s_marker_x   = SDRV_PLOT_X0 + pk_i;
    s_marker_rel = (int)lrintf(pk - s_floor);

    /* 8. 瀑布: 阈值取"本底+8dB", 但**最多允许 40% 的列变黑** ——
     *    只按本底+8dB 判, 本底稍有偏差就会整行都算"有信号", 瀑布糊成一块黑 (实测踩过)。
     *    夹逼的做法: 从最响的一格往下累加, 累到 40% 的那一格就是黑的上限。 */
    int thr_q = q50 + 4;              /* 4dB: 让本底噪声纹理也显示 (安静时瀑布才不会全白) */
    int acc2 = 0, q_lim = HIST_N - 1;
    for (int q = HIST_N - 1; q >= 0; q--) {
        acc2 += hist[q];
        if (acc2 >= COLS * 2 / 5) { q_lim = q; break; }
    }
    if (thr_q < q_lim) thr_q = q_lim;

    memmove(s_wf, s_wf + SDRV_WF_ROW_B, (size_t)SDRV_WF_ROW_B * (SDRV_WF_H - 1));
    uint8_t *row = s_wf + (size_t)SDRV_WF_ROW_B * (SDRV_WF_H - 1);
    memset(row, 0, SDRV_WF_ROW_B);
    for (int i = 0; i < COLS; i++) {
        int q = (int)(s_avg[i] - HIST_LO);
        if (q >= thr_q) row[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
    }
    sdr_analyze();                           /* 每帧更新判读结果 (13 信道 × 20 列, 开销可忽略) */
    return true;
}

/* ---------------- 生命周期 ---------------- */
static void sdr_free_bufs(void)
{
    if (s_fft) { heap_caps_free(s_fft); s_fft = NULL; }
    if (s_bin) { heap_caps_free(s_bin); s_bin = NULL; }
    if (s_win) { heap_caps_free(s_win); s_win = NULL; }
    if (s_wf)  { heap_caps_free(s_wf);  s_wf  = NULL; }
    if (s_col) { heap_caps_free(s_col); s_col = NULL; }
    if (s_avg) { heap_caps_free(s_avg); s_avg = NULL; }
    if (s_hold){ heap_caps_free(s_hold);s_hold= NULL; }
}

static void sdr_reset_view(void)
{
    if (!s_avg || !s_hold) return;
    for (int i = 0; i < COLS; i++) { s_avg[i] = SDRV_DB_NODATA; s_hold[i] = SDRV_DB_NODATA; }
    s_marker_rel  = 0;
    s_floor_valid = false;                    /* 换频/改档后重新估本底 */
    if (s_wf) memset(s_wf, 0, (size_t)SDRV_WF_ROW_B * SDRV_WF_H);
}

static void sdr_on_enter(ui_ctx_t *ctx)
{
    ESP_LOGI(TAG, "enter 频谱仪");
    s_ok = false; s_fail_n = 0; s_cap_n = 0; s_last_ms = 0;
    s_wifi_owned = false; s_bt_owned = false;
    s_hint_until = esp_timer_get_time() / 1000 + 5000;   /* 提示显示 5 秒 */

    s_fft = heap_caps_malloc(sizeof(float) * 2 * FFT_N, MALLOC_CAP_SPIRAM);
    s_bin = heap_caps_malloc(sizeof(float) * FFT_N,     MALLOC_CAP_SPIRAM);
    s_win = heap_caps_malloc(sizeof(float) * FFT_N,     MALLOC_CAP_SPIRAM);
    s_wf  = heap_caps_malloc((size_t)SDRV_WF_ROW_B * SDRV_WF_H, MALLOC_CAP_SPIRAM);
    s_col  = heap_caps_malloc(sizeof(float) * COLS, MALLOC_CAP_SPIRAM);
    s_avg  = heap_caps_malloc(sizeof(float) * COLS, MALLOC_CAP_SPIRAM);
    s_hold = heap_caps_malloc(sizeof(float) * COLS, MALLOC_CAP_SPIRAM);
    if (!s_fft || !s_bin || !s_win || !s_wf || !s_col || !s_avg || !s_hold) {
        os_dialog_toast(ctx, "内存不足");
        sdr_free_bufs();
        return;
    }
    for (int k = 0; k < FFT_N; k++)                        /* Hann 窗 */
        s_win[k] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * k / (FFT_N - 1));
    if (dsps_fft2r_init_fc32(NULL, FFT_N) != ESP_OK) {
        os_dialog_toast(ctx, "FFT 表初始化失败");
        sdr_free_bufs();
        return;
    }
    sdr_reset_view();

    /* ---- 射频征用 (顺序有讲究, 三条教训都在这) ----
     * ① **先让蓝牙让出射频**: 它和我们抢同一根天线。实测 (2026-10-01) 手柄正在连着时
     *    直接抢 → 约 2.4s 后 TG1WDT 复位。disable 会拆栈, 顺带还回 ~97KB 内部 RAM。
     * ② 再确保 WiFi 处于 init+start (PHY 靠它拉起来)。**不能先 wifi_manager_disable()**——
     *    它会 esp_wifi_deinit(), 而采集区已从堆里挖走一块, 重新 init 极易 NO_MEM。
     * ③ **不用 os_hw_request(OS_HW_WIFI)**: 管家那条路的 stop 就是 wifi_manager_disable(),
     *    而用户自己开的 WiFi 没走过管家 (ref 从 0 起) → 退出时会把用户的 WiFi 关掉。
     *    所以这里自己做"开了才关"的存档/还原。 */
    if (bt_manager_is_enabled()) {
        ESP_LOGI(TAG, "让出射频: 暂停蓝牙 (退出后自动恢复/回连)");
        bt_manager_disable();
        s_bt_owned = true;
    }
    if (!wifi_manager_is_enabled()) {
        if (!wifi_manager_enable()) {
            os_dialog_toast(ctx, "开启 WiFi 失败");
            goto fail;
        }
        s_wifi_owned = true;
    }
    espsdr_tune((unsigned)s_freq);      /* 先设好中心频率, 再 prepare_rx (日志里才是对的频率) */
    if (espsdr_enter() != ESP_OK) {
        os_dialog_toast(ctx, "射频接管失败");
        goto fail;
    }
    espsdr_set_gain(s_gain);
    s_ok = true;
    ESP_LOGI(TAG, "就绪: 中心 %dMHz 跨度 %dMHz 采集区 %u 字", s_freq, s_span,
             espsdr_aperture_words());
    return;

fail:
    if (s_wifi_owned) { wifi_manager_disable(); s_wifi_owned = false; }
    if (s_bt_owned)   { bt_manager_enable();    s_bt_owned = false; }
    sdr_free_bufs();
}

static void sdr_on_exit(ui_ctx_t *ctx)
{
    (void)ctx;
    /* 顺序要紧: espsdr_exit() 要把射频交回 STA (此时 WiFi 必须还活着), 再逐个还原 */
    if (s_ok) { espsdr_exit(); s_ok = false; }
    if (s_wifi_owned) { wifi_manager_disable(); s_wifi_owned = false; }
    if (s_bt_owned)   { bt_manager_enable();    s_bt_owned = false; }
    sdr_free_bufs();
    ESP_LOGI(TAG, "exit 频谱仪 (采集 %d 帧, 失败 %d, 越写字=%d)",
             s_cap_n, s_fail_n, espsdr_overrun_word());
}

/* ---------------- 渲染 (按页分发: 一屏只干一件事) ---------------- */
static void sdr_fill_info(sdrv_info_t *in)
{
    memset(in, 0, sizeof(*in));
    in->center_mhz   = (unsigned)s_freq;
    in->span_mhz     = (float)s_span;
    in->rbw_khz      = (int)(FS_MHZ * 1000.0f / (float)FFT_N + 0.5f);
    in->floor_db     = (int)lrintf(s_floor);
    in->batt_pct     = s_batt;
    in->overrun_word = espsdr_overrun_word();
    in->cap_ok       = (s_cap_n > 0);
    in->marker_x     = s_marker_x;
    in->marker_rel   = s_marker_rel;
    in->marker_txt   = NULL;
}

static void sdr_fill_report(sdrv_report_t *r)
{
    memset(r, 0, sizeof(*r));
    r->title     = "扫描判读";
    r->trace     = s_avg;
    r->trace_n   = COLS;
    r->floor_db  = (int)lrintf(s_floor);
    r->span_mhz  = s_span;
    r->worst_ch  = s_snap.worst_ch;
    r->best_ch   = s_snap.best_ch;
    r->ble_mhz   = s_snap.ble_mhz;
    r->broadband = s_snap.broadband;
    r->n_sig     = s_snap.n_sig;
    r->verdict   = s_snap.verdict;
    r->advice    = s_snap.advice;
    for (int i = 0; i < 13; i++) r->pct[i] = s_snap.pct[i];
    for (int i = 0; i < 3;  i++) r->ble_db[i] = s_snap.ble_db[i];
}

static void sdr_render(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    /* ---- 页 0: 模式菜单 (启动先看这个) ---- */
    if (s_page == 0) {
        sdrv_draw_modebar(lcd, "频谱仪");
        sdrv_draw_modemenu(lcd, s_menu_sel, s_mode_name, s_mode_note, 4);
        sdrv_draw_hint(lcd, "确认 进入", "返回 退出");
        return;
    }

    /* ---- 内容页共用: 自动量程 + 判读数据 ---- */
    float peak_db = s_floor + (float)s_marker_rel;
    float top_db  = s_floor + SDRV_QUIET_TOP;
    if (peak_db + SDRV_PEAK_MARGIN > top_db) top_db = peak_db + SDRV_PEAK_MARGIN;
    sdrv_range_set(top_db - SDRV_RANGE_SPAN, top_db);

    const char *ttl = (s_page == 1) ? "2.4G 全景" : (s_page == 2) ? "WiFi 信道占用"
                    : (s_page == 3) ? "蓝牙广播" : "干扰检查";
    /* 没定格 → 只显示扫描进度, 不出任何数字 (数字一跳就没法看) */
    if (!s_latch) {
        int64_t now = esp_timer_get_time() / 1000;
        int pct = 100 - (int)((s_scan_until - now) * 100 / SDR_SCAN_MS);
        sdrv_draw_scanning(lcd, ttl, pct);
        sdrv_draw_hint(lcd, "确认 重扫", "返回 菜单");
        return;
    }

    sdrv_info_t   in;  sdr_fill_info(&in);
    sdrv_report_t r;   sdr_fill_report(&r);

    if (s_page == 1) {                       /* 全景: 只有图 */
        sdrv_draw_modebar(lcd, ttl);
        if (!s_ok) sdrv_draw_notice(lcd, "射频未就绪", "返回 菜单");
        else       sdrv_draw_panorama(lcd, &in, s_avg, s_hold, COLS, s_wf);
        sdrv_draw_hint(lcd, "确认 换页", "返回 菜单");
        return;
    }
    if (s_page == 2) {                       /* 信道: 只有 13 根柱 */
        sdrv_draw_modebar(lcd, ttl);
        sdrv_draw_chanbars(lcd, &r);
        sdrv_draw_hint(lcd, "确认 换页", "返回 菜单");
        return;
    }
    if (s_page == 3) {                       /* 蓝牙: 只有三条 */
        sdrv_draw_modebar(lcd, ttl);
        sdrv_draw_blepage(lcd, &r);
        sdrv_draw_hint(lcd, "确认 换页", "返回 菜单");
        return;
    }
    /* 页 4: 干扰 */
    sdrv_draw_modebar(lcd, ttl);
    sdrv_draw_jampage(lcd, &r);
    sdrv_draw_hint(lcd, "确认 换页", "返回 菜单");
}

/* ---------------- 交互 ---------------- */
static void sdr_retune(int mhz)
{
    if (mhz < (int)ESPSDR_FREQ_MIN_MHZ) mhz = (int)ESPSDR_FREQ_MIN_MHZ;
    if (mhz > (int)ESPSDR_FREQ_MAX_MHZ) mhz = (int)ESPSDR_FREQ_MAX_MHZ;
    s_freq = mhz;
    if (s_ok) espsdr_tune((unsigned)s_freq);
    sdr_reset_view();                       /* 换频 → 清平均与 peak-hold (旧包络已无意义) */
}

/* 交互 (linit 2026-10-02 定案: "不需要左右上下浮动, 选中某个频率, 选项太多没必要")
 *  → 全部砍掉: 不调中心频率、不调增益、不选频率、没有菜单。
 *  只留两个键: 确认 = 在"频谱页 / 判读页"之间切; 返回 = 退出。
 *  跨度/平均/增益固定为自动最优值 (80MHz / 4 次 / AGC), 不再暴露。 */
static void sdr_action(ui_ctx_t *ctx, os_action_t a)
{
    if (s_page == 0) {                       /* 模式菜单 */
        switch (a) {
        case OS_ACTION_UP:    s_menu_sel = (s_menu_sel + 3) & 3; break;
        case OS_ACTION_DOWN:  s_menu_sel = (s_menu_sel + 1) & 3; break;
        case OS_ACTION_CONFIRM:
        case OS_ACTION_RIGHT: s_page = s_menu_sel + 1; sdr_scan_start(); break;
        case OS_ACTION_BACK:  os_pop(ctx); return;
        default: break;
        }
    } else {
        switch (a) {
        case OS_ACTION_CONFIRM:
            sdr_scan_start();                  /* 点确认 = 重新测一次 (结果才更新) */
            break;
        case OS_ACTION_LEFT:                   /* 左右仍可换页看别的 */
            s_page = (s_page <= 1) ? 4 : s_page - 1; sdr_scan_start();
            break;
        case OS_ACTION_RIGHT:
            s_page = (s_page >= 4) ? 1 : s_page + 1; sdr_scan_start();
            break;
        case OS_ACTION_BACK:
            s_page = 0;                        /* 回模式菜单 */
            break;
        default: break;
        }
    }
    ctx->needs_redraw = true;
}

/* 顶栏按**同样等分槽**响应 —— 看到的四个槽就是能点的四个参数 (槽位定义见 sdr_view.h) */
static bool sdr_touch(ui_ctx_t *ctx, int x, int y)
{
    if (y <= SDRV_TOP_H) {
        if (x < SDRV_SLOT_X0 + SDRV_SLOT_W) {              /* 槽1 中心频率 */
            sdr_retune(2437);
            os_dialog_toast(ctx, "中心 2437MHz");
        } else if (x < SDRV_SLOT_X0 + 2 * SDRV_SLOT_W) {    /* 槽2 跨度 */
            s_span = (s_span == 80) ? 40 : (s_span == 40) ? 20 : (s_span == 20) ? 10 : 80;
            sdr_reset_view();
            os_dialog_toast(ctx, s_span == 80 ? "跨度 80MHz" : s_span == 40 ? "跨度 40MHz"
                               : s_span == 20 ? "跨度 20MHz" : "跨度 10MHz");
        } else if (x < SDRV_SLOT_X0 + 3 * SDRV_SLOT_W) {    /* 槽3 平均档 */
            s_avg_n = (s_avg_n == 1) ? 4 : (s_avg_n == 4) ? 16 : 1;
            sdr_reset_view();
            os_dialog_toast(ctx, s_avg_n == 1 ? "平均 关" : s_avg_n == 4 ? "平均 4 次" : "平均 16 次");
        } else {                                            /* 槽4 增益 自动/手动 */
            s_gain = (s_gain == ESPSDR_GAIN_AUTO) ? 40 : ESPSDR_GAIN_AUTO;
            if (s_ok) espsdr_set_gain(s_gain);
            os_dialog_toast(ctx, s_gain == ESPSDR_GAIN_AUTO ? "增益 自动" : "增益 手动");
        }
    } else if (y >= SDRV_PLOT_Y0 && y <= SDRV_PLOT_Y1) {   /* 点谱图 → 中心频率移到该处 */
        sdr_retune((int)lrintf(sdrv_mhz_of_x(x, (unsigned)s_freq, (float)s_span)));
    } else {
        return false;
    }
    ctx->needs_redraw = true;
    return true;
}

static void sdr_poll(ui_ctx_t *ctx)
{
    if (!s_ok) return;
    int64_t now = esp_timer_get_time() / 1000;

    if (now - s_batt_ms > 1000) {                /* 电池读数 (接口自身 60s 节流) */
        s_batt_ms = now;
        board_battery_status_t bs;
        if (board_battery_read_cached(&bs) == ESP_OK) s_batt = bs.percent;
    }
    if (!s_latch && now >= s_scan_until && s_page != 0) {   /* 扫描时间到 → 定格 */
        sdr_scan_latch();
        ctx->needs_redraw = true;
    }
    if (now - s_last_ms < STEP_MS) return;
    s_last_ms = now;
    if (sdr_step() || s_fail_n == 1) ctx->needs_redraw = true;
}

static const os_module_t s_mod_sdr = {
    .name      = "sdr",
    .page_id   = OS_PAGE_SDR,
    .on_enter  = sdr_on_enter,
    .on_exit   = sdr_on_exit,
    .render    = sdr_render,
    .action    = sdr_action,
    .touch     = sdr_touch,
    .poll      = sdr_poll,
    .fullscreen = true,
};

void os_page_sdr_register(void) { os_register(&s_mod_sdr); }
