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
#include "esp_system.h"      /* esp_restart: 频谱仪启用/释放 64KB 需重启生效 */
#include "dsps_fft2r.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
/* ★ 2026-10-14 新增: 冻帧 / 上方拖动调频(跟手滚动) / 内联设置页 */
static bool    s_freeze;                  /* 冻帧: 停 DSP 更新, 画面定住 */
static bool    s_panning;                 /* 正在拖 */
static int     s_pan_x;                   /* 上次触摸 x */
static float   s_pan_v;                   /* 惯性速度 (px/帧) */
static float   s_pan_res;                 /* ★ 拖动调频的**欠账** (MHz, 含小数!) —— 见 sdr_poll 注释 */
static int     s_pan_shift;               /* 当前下发的像素偏移 (显示/诊断用) */
static int64_t s_pan_ms;                  /* 上次真换频的时刻 (节流到 DSP 节拍) */
static int     s_tr_disp = -999;          /* 上一次下发给渲染层的像素偏移 (去重, 少一次重绘) */
static int     s_anim_to = -99999;        /* 松手后的弹性归位目标 (0=回原位 / ±400=翻页) */
static int     s_set_sel;                 /* 设置项选中行 */
static int     s_slide;                   /* 下方横拖位移 (跟手翻页, 0=已吸附) */
static bool    s_sliding;                 /* 正在拖页 (render 走"合成"路径) */
static bool    s_offscreen;               /* 正在往离屏缓冲渲染 (不画底栏) */

/* ===== 整页跟手滑动: 离屏预渲染 + 带偏移像素合成 =====
 * ⛔ 为什么必须这么做: 面板帧缓冲是"每字节 2 列 × 4 行"的压缩排布, 不能像普通位图那样
 *   memmove 平移; 而给 sdr_view 全部绘制加 xoff 要改 ~50 处。
 *   做法: 拖动开始时把 **上一项 / 当前 / 下一项** 各渲染一份到离屏缓冲(预加载),
 *   拖动期间只做带偏移的像素合成 ⇒ **1:1 跟手、不白屏**、数据不刷新(松手才恢复实时)。 */
/* ⛔ 帧缓冲排布 = 面板原生"2 列 × 4 行 / 字节": idx = (x>>1)*75 + ((299-y)>>2),
 *   bit = 7 - ((((299-y)&3)<<1)|(x&1)); 位=1 白 / 0 黑。**stride 是 75 不是 200, y 还是翻转的**
 *   (2026-10-14 我先按 200 写 ⇒ 索引最大 39874 > 15000 **写越界**, 已修)。 */
#define SDR_FB_STRIDE  75
#define SDR_FB_BYTES   (200 * 75)                   /* 15000 */
static uint8_t *s_scr[3];
static bool     s_scr_ok[3];

/* ---- 判读 / 菜单 (2026-10-02) ---- */
static bool     s_hold_on = true;         /* peak-hold 开关 (底栏"保持"槽) */
/* 页号: 0 = 模式菜单, 1 = 全景, 2 = 信道, 3 = 蓝牙, 4 = 干扰 (一屏只干一件事) */
static int      s_page = 1;
/* ⛔ 2026-10-14 删除「四个选项」模式菜单页: 它只吃按键不吃触摸 (linit 点了没反应),
 *   现在**默认直接进频谱页**, 换视图改在**下方左右拖**。 */
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

    /* 5. 指数平均 (avg_n=1 时不平滑) + peak-hold
     * ★ 拖动调频期间必须**不平滑** (a=1): 换频后旧谱还占着 3/4 权重时, 新中心是
     *   "按新频率重画"的 —— 画面位置不动、内容也不变 ⇒ 看起来就是"拖了波形不跟着"。
     *   (linit: "波形没有跟着我左右拖动显示实际波形") */
    float a = (s_panning || s_avg_n <= 1) ? 1.0f : 1.0f / (float)s_avg_n;
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

/* ================= 音频页: 麦克风频谱 (2026-10-14 新增, 视图名「音频」) =================
 * 数据源 = ES7210 双麦阵列 (16kHz 立体声, 取左声道 = MIC1), 走 audio_player_mic_*。
 * 复用射频那套 FFT 表与 s_fft / s_bin / s_win 缓冲 —— 同一时刻只有一条路径在跑
 * (第 4 页**跳过射频采集**, 见 sdr_poll), 所以不会互相踩。
 * 显示: **对数频率轴** 30Hz ~ 8kHz, 368 列 → 每列取落在该频段内的 bin 最大值。 */
#define MIC_FS      16000
#define MIC_F_LO    30.0f
#define MIC_BIN_HZ  ((float)MIC_FS / (float)FFT_N)      /* 7.8125 Hz */
#define MIC_HIST_N  161                                 /* dBFS 直方图: 1dB 一格, [-160, 0] */

static float   *s_mic_avg;                 /* 每列 dBFS (指数平均) */
static float   *s_mic_hold;                /* peak-hold */
static uint8_t *s_mic_wf;                  /* 瀑布位图 (1 bit/px, 行优先) */
static int16_t *s_mic_pcm;                 /* I2S 读缓冲: FFT_N 帧立体声 */
static int16_t  s_mic_b0[COLS], s_mic_b1[COLS];   /* 每列对应的 bin 区间 (预计算) */
static float    s_mic_tilt[COLS];          /* 每列的显示补偿 (见下) */
static bool     s_mic_bufs, s_mic_on, s_mic_ok, s_mic_tried;
static float    s_mic_floor;
static bool     s_mic_floor_valid;
static int      s_mic_marker_x = COLS / 2;
static int      s_mic_peak_hz;
static int      s_mic_level;               /* 电平表 0..100 */
static int      s_mic_rms_last, s_mic_pk_last;   /* 诊断: mic_step 上一帧实际算到的值 */
static int      s_mic_avg_n = 4;
static int      s_mic_f_hi  = 8000;        /* 对数轴右端 (2000/4000/8000) */
static bool     s_mic_hold_on = true;
static int      s_mic_fail_n;
static uint16_t s_mic_hist[MIC_HIST_N];

/* 列 → 频率 (对数轴): f(c) = 30 · (HI/30)^(c/COLS), c ∈ [0, COLS] */
static float mic_col_hz(int c)
{
    return MIC_F_LO * powf((float)s_mic_f_hi / MIC_F_LO, (float)c / (float)COLS);
}
/* 频率 → 屏幕 x (与列映射同源) */
static int mic_hz_x(float hz)
{
    float t = logf(hz / MIC_F_LO) / logf((float)s_mic_f_hi / MIC_F_LO);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return SDRV_PLOT_X0 + (int)(t * (float)(SDRV_PLOT_W - 1) + 0.5f);
}

/* 重算"列 → bin 区间" (换频率上限时要重算) */
static void sdr_mic_map_cols(void)
{
    for (int i = 0; i < COLS; i++) {
        int b0 = (int)(mic_col_hz(i)     / MIC_BIN_HZ + 0.5f);
        int b1 = (int)(mic_col_hz(i + 1) / MIC_BIN_HZ + 0.5f);
        if (b0 < 1) b0 = 1;
        if (b1 <= b0) b1 = b0 + 1;                 /* 低频段一格 bin 盖好几列 ⇒ 不允许空列 */
        if (b1 > FFT_N / 2) b1 = FFT_N / 2;
        if (b1 <= b0) b1 = b0 + 1;
        s_mic_b0[i] = (int16_t)b0;
        s_mic_b1[i] = (int16_t)b1;
        /* ★ 显示补偿 (+3dB/倍频程, 拐点 150Hz, **限幅 ±8dB**): 麦克风前端是 1/f(粉噪) 本底,
         *   原始 dB 曲线一路下坡(实测 ~3.7dB/倍频程: 50Hz 处比 8kHz 高 ~20dB),
         *   瀑布会糊成左边一整块黑、峰值 marker 永远停在最低频。
         *   ⚠ **必须限幅**: 2kHz 以上原谱本就是平的(白噪), 无上限地补会把它抬成"右高左低",
         *     瀑布右边反而糊黑。限 ±8dB 后整条曲线基本水平。
         *   刻度仍是"高于本底多少 dB"; 想看不补偿的原图把这里改成 0 即可。 */
        float fc = mic_col_hz(i);
        float t  = 10.0f * log10f(fc / 150.0f);
        if (t >  8.0f) t =  8.0f;
        if (t < -8.0f) t = -8.0f;
        s_mic_tilt[i] = t;
    }
}

static void sdr_mic_free(void)
{
    if (s_mic_avg)  { heap_caps_free(s_mic_avg);  s_mic_avg  = NULL; }
    if (s_mic_hold) { heap_caps_free(s_mic_hold); s_mic_hold = NULL; }
    if (s_mic_wf)   { heap_caps_free(s_mic_wf);   s_mic_wf   = NULL; }
    if (s_mic_pcm)  { heap_caps_free(s_mic_pcm);  s_mic_pcm  = NULL; }
    s_mic_bufs = false;
}

static void sdr_mic_alloc(void)
{
    if (s_mic_bufs) return;
    s_mic_avg  = heap_caps_malloc(sizeof(float) * COLS, MALLOC_CAP_SPIRAM);
    s_mic_hold = heap_caps_malloc(sizeof(float) * COLS, MALLOC_CAP_SPIRAM);
    s_mic_wf   = heap_caps_malloc((size_t)SDRV_WF_ROW_B * SDRV_WF_H, MALLOC_CAP_SPIRAM);
    s_mic_pcm  = heap_caps_malloc(sizeof(int16_t) * FFT_N * 2, MALLOC_CAP_SPIRAM);
    if (!s_mic_avg || !s_mic_hold || !s_mic_wf || !s_mic_pcm) { sdr_mic_free(); return; }
    for (int i = 0; i < COLS; i++) { s_mic_avg[i] = SDRV_DB_NODATA; s_mic_hold[i] = SDRV_DB_NODATA; }
    memset(s_mic_wf, 0, (size_t)SDRV_WF_ROW_B * SDRV_WF_H);
    sdr_mic_map_cols();
    s_mic_bufs = true;
}

/* 麦克风开关 (进第 4 页开, 离开停) */
static void sdr_mic_enable(bool on)
{
    if (on) {
        sdr_mic_alloc();
        if (!s_mic_bufs) return;
        s_mic_floor_valid = false;
        s_mic_fail_n = 0;
        s_mic_tried = true;
        if (audio_player_mic_start(MIC_FS) != 0) { s_mic_ok = false; return; }
        s_mic_on = true;
        s_mic_ok = true;
    } else if (s_mic_on) {
        audio_player_mic_stop();
        s_mic_on = false;
    }
}

/* 一趟麦克风采集 + FFT (热路径, 不打日志) */
static bool sdr_mic_step(void)
{
    if (!s_mic_on || !s_mic_bufs || !s_fft || !s_bin || !s_win) return false;
    /* ⚠ i2s_channel_read 是"有多少给多少"(xRingbufferReceiveUpTo), 2048 帧要 128ms
     *   而单次超时只给几十 ms ⇒ **必须循环凑满**, 否则尾部是上一帧的旧数据。 */
    int got = 0;
    int64_t t0 = esp_timer_get_time() / 1000;
    while (got < FFT_N) {
        int n = audio_player_mic_read(s_mic_pcm + (size_t)got * 2, FFT_N - got, 60);
        if (n <= 0) break;
        got += n;
        if (esp_timer_get_time() / 1000 - t0 > 250) break;
    }
    if (got < FFT_N / 2) {
        /* ⛔ 失败计数**不能只增不减**: 喇叭那条路(beep/播报)会重配 I2S 时钟, 可能把共用
         *   的 RX 打断 —— 一旦卡住页面就永远显示"麦克风未就绪"。所以连续读不到就**自愈重启**。 */
        if (++s_mic_fail_n > 20) {                 /* 约 2 秒读不到 → 重启一次麦克风链路 */
            s_mic_fail_n = 0;
            audio_player_mic_stop();
            if (audio_player_mic_start(MIC_FS) != 0) { s_mic_ok = false; s_mic_on = false; }
            else                                       s_mic_ok = true;
        }
        return false;
    }
    s_mic_fail_n = 0;
    if (got < FFT_N) memset(s_mic_pcm + (size_t)got * 2, 0, (size_t)(FFT_N - got) * 2 * sizeof(int16_t));
    s_mic_ok = true;

    /* 1. 去 DC + Hann 窗 (16kHz/2048 ⇒ 128ms 一帧, bin 7.8125Hz)
     *    ⚠ 电平表用**双声道峰值**: 安静房间里 RMS 会低到看不出变化, 峰值稳且直观;
     *      FFT 仍只用左声道 (两颗麦等电平, 取一路即可, 避免叠加噪声)。 */
    float mi = 0.0f, sumsq = 0.0f;
    int   pk2 = 0;
    for (int k = 0; k < FFT_N; k++) mi += (float)s_mic_pcm[2 * k];
    mi /= (float)FFT_N;
    for (int k = 0; k < FFT_N; k++) {
        float v = (float)s_mic_pcm[2 * k] - mi;
        sumsq += v * v;
        int al = s_mic_pcm[2 * k]     < 0 ? -s_mic_pcm[2 * k]     : s_mic_pcm[2 * k];
        int ar = s_mic_pcm[2 * k + 1] < 0 ? -s_mic_pcm[2 * k + 1] : s_mic_pcm[2 * k + 1];
        if (al > pk2) pk2 = al;
        if (ar > pk2) pk2 = ar;
        s_fft[2 * k]     = v * s_win[k];
        s_fft[2 * k + 1] = 0.0f;
    }
    /* 电平表: -70dBFS → 0%, 0dBFS → 100% */
    float rms = sqrtf(sumsq / (float)FFT_N);
    s_mic_rms_last = (int)lrintf(rms);
    s_mic_pk_last  = pk2;
    int lvl = (int)lrintf((20.0f * log10f((float)pk2 / 32768.0f + 1e-9f) + 70.0f) * 100.0f / 70.0f);
    if (lvl < 0) lvl = 0;
    if (lvl > 100) lvl = 100;
    if (lvl > s_mic_level) s_mic_level = lvl;                       /* attack 立即 */
    else s_mic_level -= (s_mic_level - lvl + 3) / 4;                /* decay 平缓 */

    /* 2. FFT + 归一化: 满幅正弦(A=32768) 过 Hann 窗后 |X| = A/2·N·0.5 = N·32768/4 ⇒ 0dBFS */
    dsps_fft2r_fc32(s_fft, FFT_N);
    dsps_bit_rev_fc32(s_fft, FFT_N);
    const float norm = 20.0f * log10f((float)FFT_N * 32768.0f / 4.0f);
    for (int k = 0; k < FFT_N / 2; k++) {
        float re = s_fft[2 * k], im = s_fft[2 * k + 1];
        s_bin[k] = 10.0f * log10f(re * re + im * im + 1e-6f) - norm;
    }

    /* 3. bin → 列 (取区间最大值, 窄带峰不会被抽掉) + 粉噪斜率补偿 */
    for (int i = 0; i < COLS; i++) {
        float mx = -200.0f;
        for (int b = s_mic_b0[i]; b < s_mic_b1[i]; b++) if (s_bin[b] > mx) mx = s_bin[b];
        s_col[i] = mx + s_mic_tilt[i];
    }
    float a = (s_mic_avg_n <= 1) ? 1.0f : 1.0f / (float)s_mic_avg_n;
    for (int i = 0; i < COLS; i++) {
        if (s_mic_avg[i] <= SDRV_DB_NODATA + 1.0f) s_mic_avg[i] = s_col[i];   /* 首帧直接落位 */
        else s_mic_avg[i] += (s_col[i] - s_mic_avg[i]) * a;
        if (s_mic_hold_on && s_mic_avg[i] > s_mic_hold[i]) s_mic_hold[i] = s_mic_avg[i];
    }

    /* 4. 本底 (中位数, 稳健) */
    memset(s_mic_hist, 0, sizeof(s_mic_hist));
    for (int i = 0; i < COLS; i++) {
        int q = (int)(s_mic_avg[i] + 160.0f);
        s_mic_hist[q < 0 ? 0 : (q >= MIC_HIST_N ? MIC_HIST_N - 1 : q)]++;
    }
    int acc = 0, q50 = 0;
    for (int q = 0; q < MIC_HIST_N; q++) { acc += s_mic_hist[q]; if (acc >= COLS / 2) { q50 = q; break; } }
    float nf = -160.0f + (float)q50 + 0.5f;
    if (!s_mic_floor_valid) { if (q50 > 2) { s_mic_floor = nf; s_mic_floor_valid = true; } }
    else s_mic_floor += (nf - s_mic_floor) * 0.1f;

    /* 5. marker 跟最高峰 (低频残留会让 marker 永远贴在最左边 ⇒ 从第 4 列起找) */
    int pk = 4;
    for (int i = 4; i < COLS - 2; i++) if (s_mic_avg[i] > s_mic_avg[pk]) pk = i;
    s_mic_marker_x = SDRV_PLOT_X0 + pk;
    s_mic_peak_hz  = (int)(mic_col_hz(pk) + 0.5f);

    /* 6. 瀑布: 阈值 = 本底 +8dB (射频页是 +4 —— 那边是平噪声; 这边补偿后曲线本身
     *    还有几 dB 起伏, 阈值太紧会整片黑、太松则全白), 保留"最多 40% 列变黑"的兜底。 */
    int thr_q = q50 + 8;
    int acc2 = 0, q_lim = MIC_HIST_N - 1;
    for (int q = MIC_HIST_N - 1; q >= 0; q--) {
        acc2 += s_mic_hist[q];
        if (acc2 >= COLS * 2 / 5) { q_lim = q; break; }
    }
    if (thr_q < q_lim) thr_q = q_lim;
    memmove(s_mic_wf, s_mic_wf + SDRV_WF_ROW_B, (size_t)SDRV_WF_ROW_B * (SDRV_WF_H - 1));
    uint8_t *row = s_mic_wf + (size_t)SDRV_WF_ROW_B * (SDRV_WF_H - 1);
    memset(row, 0, SDRV_WF_ROW_B);
    for (int i = 0; i < COLS; i++) {
        int q = (int)(s_mic_avg[i] + 160.0f);
        if (q >= thr_q) row[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
    }
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
    sdr_mic_free();
}

static void sdr_reset_view(void)
{
    if (!s_avg || !s_hold) return;
    s_pan_res   = 0.0f;                    /* 换频/改档 ⇒ 拖动欠账清零, 谱线偏移归零 */
    s_pan_shift = 0;
    s_tr_disp   = 0;
    sdrv_set_trace_shift(0);
    for (int i = 0; i < COLS; i++) { s_avg[i] = SDRV_DB_NODATA; s_hold[i] = SDRV_DB_NODATA; }
    s_marker_rel  = 0;
    s_floor_valid = false;                    /* 换频/改档后重新估本底 */
    if (s_wf) memset(s_wf, 0, (size_t)SDRV_WF_ROW_B * SDRV_WF_H);
}

/* ★ 频谱仪 64KB 采集区: 默认不占用(开机已回收进堆), 只有"启用标记"才保留。
 *   与 USB 网卡同范式 —— 启用/释放都写 NVS 后重启生效 (IDF 无 heap remove_region)。
 *   s_need_enable : 进入时未启用 → 弹"启用并重启"
 *   s_need_release: 退出时已启用 → 弹"退出并重启释放 64KB" */
static bool s_need_enable  = false;
static bool s_need_release = false;
static bool s_notouch_msg  = false;   /* 无触摸: 待弹一次"不可用"提示 */

static void sdr_enable_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result == 0) {                       /* 0=确定 1/-1=取消 */
        espsdr_set_enabled(true);
        ESP_LOGW(TAG, "频谱仪: 已写启用标记, 重启后保留 64KB");
        vTaskDelay(pdMS_TO_TICKS(600));      /* 让提示可见 */
        esp_restart();                       /* 不返回 */
    }
    /* ★ 2026-10-28 linit: **取消 = 不启用 ⇒ 直接退出频谱仪**。
     *   原来只置脏, 页面停在"射频未就绪 / 返回 菜单"的空页上, 用户点了取消
     *   看着就像"没反应", 还得再按一次返回才能走。
     *   走 os_request_pop: 弹窗回调里不能直接 os_pop(那只弹掉弹窗层, 页面原地不动),
     *   由 os_tick 在弹窗真正关闭后的下一帧执行。 */
    ESP_LOGI(TAG, "频谱仪: 取消启用 ⇒ 退出本页");
    if (ctx) { ctx->needs_redraw = true; os_request_pop(ctx); }
}
static void sdr_release_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result == 0) {                       /* 0=退出并重启释放 */
        espsdr_set_enabled(false);
        ESP_LOGW(TAG, "频谱仪: 已清启用标记, 重启后释放 64KB");
        vTaskDelay(pdMS_TO_TICKS(600));
        esp_restart();                       /* 不返回 */
    }
    s_need_release = false;                  /* 取消: 留在本页, 返回键可再提示 */
    if (ctx) ctx->needs_redraw = true;
}

static void sdr_on_enter(ui_ctx_t *ctx)
{
    /* ★ 频谱仪未启用: 采集区 64KB 已于开机回收进堆, 这里绝不碰它。
     *   ① 本机无触摸 → 频谱仪全触屏操作没法用: 只提示"不可用", 不给启用入口 (图标仍显示);
     *   ② 有触摸 → 由 poll 弹一次"启用并重启"; 取消则停在空页, 按返回离开。 */
    if (!espsdr_usable()) {
        if (!espsdr_touch_ok()) {
            s_ok = false;
            s_notouch_msg = true;   /* poll 弹一次"不可用" */
            ESP_LOGW(TAG, "无触摸屏: 频谱仪不可用 (仅提示)");
            return;   /* 用户按返回离开 */
        }
        s_need_enable = true;
        s_ok = false;
        ESP_LOGW(TAG, "频谱仪未启用(64KB已回收): 提示启用");
        return;
    }
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
    sdr_mic_alloc();     /* 音频页缓冲 (PSRAM); 没进第 4 页也不占内部 RAM */

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
    sdr_mic_enable(false);                     /* 先松麦克风/codec, 再交射频 */
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

/* 前向声明: render 用到, 定义在下方 */
static void sdr_retune(int mhz);
static void sdr_pan_to(int mhz);
static void sdr_band_text(char *b, int n);
static void sdr_set_adj(int dir);

/* 底栏统一出口: 扫描中 = 设置/继续 + 进度; 定格后 = 设置/冻结 + 视图序号 */
static const char *const s_vname[4] = { "波形", "信道", "蓝牙", "音频" };
#define SDR_NPAGE 4
/* 换视图 (唯一入口): 进出第 4 页要开关麦克风 —— 不重扫射频 (三视图同源) */
static void sdr_goto(int p)
{
    if (p < 1) p = SDR_NPAGE;
    if (p > SDR_NPAGE) p = 1;
    if (p == s_page) return;
    s_page = p;
    sdr_mic_enable(s_page == 4);
}
static void sdr_softbar_bottom(st7305_handle_t *lcd, bool scanning, int pct)
{
    (void)pct;
    if (s_offscreen) return;               /* 离屏页不画底栏(底栏由实时合成层画) */
    /* ★ 绘制顺序很关键: 先轮播 → 再把凹口外涂白 → 最后画软键栏(线与键盖在文字上)
     *   ⇒ "超过线条的部分被线条挡住"(linit 要求)。 */
    sdrv_draw_carousel(lcd, s_vname, SDR_NPAGE,
                       (s_page >= 1 && s_page <= SDR_NPAGE) ? s_page - 1 : 0, s_slide);
    fill_rect(lcd, 0, SDRV_SOFT_LID + 1, 95, SDRV_SOFT_VALLEY, ST7305_COLOR_WHITE);
    fill_rect(lcd, 305, SDRV_SOFT_LID + 1, SDRV_W - 1, SDRV_SOFT_VALLEY, ST7305_COLOR_WHITE);
    sdrv_draw_softbar(lcd, "设置", scanning ? "继续" : (s_freeze ? "继续" : "冻结"), NULL);
}

/* ---------------- 音频页 (麦克风): 对数频率轴 + 电平条 + 瀑布 ---------------- */
static void sdr_draw_audio_page(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    char b[24];
    sdrv_draw_topbar(lcd, "音频", s_mic_f_hi >= 8000 ? "16kHz" : s_mic_f_hi >= 4000 ? "4kHz" : "2kHz");
    if (!s_mic_tried) { sdrv_draw_notice(lcd, "麦克风启动中", NULL); return; }        /* 离屏预加载 */
    if (!s_mic_ok)    { sdrv_draw_notice(lcd, "麦克风未就绪", "返回 退出"); return; }
    if (!s_mic_avg || s_mic_avg[0] <= SDRV_DB_NODATA + 1.0f) {   /* 第一帧还没到: 别画假平线 */
        sdrv_draw_notice(lcd, "麦克风启动中", NULL);
        return;
    }

    /* 量程窗口固定 70dB, 上沿 = 本底 + 60 ⇒ 左侧刻度 60/40/20/本底 才与谱线对齐
     * (⛔ 全局一直没调 sdrv_range_set, 用默认 [-100,0] 时刻度与谱线是错位的) */
    if (s_mic_floor_valid) sdrv_range_set(s_mic_floor - 10.0f, s_mic_floor + 60.0f);
    sdrv_draw_scale(lcd);
    sdrv_draw_grid(lcd, NULL);
    sdrv_draw_trace(lcd, s_mic_avg, COLS);
    if (s_mic_hold_on) sdrv_draw_hold(lcd, s_mic_hold, COLS);

    /* 对数频率轴 (30Hz ~ 8k) */
    draw_hline(lcd, 0, SDRV_W - 1, SDRV_AXIS_LINE, ST7305_COLOR_BLACK);
    static const int   hzt[8] = { 50, 100, 200, 500, 1000, 2000, 4000, 8000 };
    static const char *lbt[8] = { "50", "100", "200", "500", "1k", "2k", "4k", "8k" };
    for (int i = 0; i < 8; i++) {
        if (hzt[i] > s_mic_f_hi) break;
        int x = mic_hz_x((float)hzt[i]);
        draw_vline(lcd, x, SDRV_AXIS_TICK, SDRV_AXIS_TICK + 3, ST7305_COLOR_BLACK);
        sdrv_ascii_center(lcd, x, SDRV_AXIS_TXT_Y, lbt[i], false);
    }

    /* 电平条 (轴下 168..178, 瀑布分隔线在 180/181) */
    sdrv_ascii(lcd, 2, 168, "LVL", false);
    draw_rect_outline(lcd, 32, 167, SDRV_W - 2, 179, ST7305_COLOR_BLACK);
    int w = (SDRV_PLOT_W - 4) * s_mic_level / 100;
    if (w > 0) fill_rect(lcd, 34, 169, 33 + w, 177, ST7305_COLOR_BLACK);

    /* marker: 峰值列 + 频率读数 */
    if (s_mic_peak_hz >= 1000) snprintf(b, sizeof b, "%.1fkHz", s_mic_peak_hz / 1000.0f);
    else                       snprintf(b, sizeof b, "%dHz", s_mic_peak_hz);
    sdrv_draw_marker(lcd, s_mic_marker_x, b);

    sdrv_draw_waterfall(lcd, s_mic_wf);
}

/* BLE 三条广播信道 (37/38/39), 画在瀑布带里 (下半栏, 与信道页同构) */
static void sdr_draw_ble_band(st7305_handle_t *lcd, const sdrv_report_t *r)
{
    (void)r;
    const int base = SDRV_WF_Y0 + SDRV_WF_H - 14;
    const int hmax = SDRV_WF_H - 28;
    static const char *const nm[3] = { "37", "38", "39" };
    int mx = 15;
    for (int i = 0; i < 3; i++) if (s_ble_db[i] > mx) mx = s_ble_db[i];
    for (int i = 0; i < 3; i++) {
        int x = SDRV_PLOT_X0 + 56 + i * 88;
        int h = (s_ble_db[i] * hmax) / mx;
        if (h < 3) h = 3;
        fill_rect(lcd, x, base - h, x + 52, base, ST7305_COLOR_BLACK);
        char b[20]; snprintf(b, sizeof(b), "ch%s %ddB", nm[i], s_ble_db[i]);
        sdrv_ascii(lcd, x - 4, base + 3, b, false);
    }
}

/* 13 根信道柱, 画在瀑布带里 (下半栏) */
static void sdr_draw_bars_band(st7305_handle_t *lcd, const sdrv_report_t *r)
{
    const int base = SDRV_WF_Y0 + SDRV_WF_H - 12;
    const int hmax = SDRV_WF_H - 26;
    int mx = 20;
    for (int i = 0; i < 13; i++) if (r->pct[i] > mx) mx = r->pct[i];
    for (int i = 0; i < 13; i++) {
        int x = SDRV_PLOT_X0 + i * 28;
        int h = (r->pct[i] * hmax) / mx;
        if (h < 3) h = 3;
        fill_rect(lcd, x, base - h, x + 22, base, ST7305_COLOR_BLACK);
        char b[4]; snprintf(b, sizeof(b), "%d", i + 1);
        sdrv_ascii(lcd, x + 7, base + 3, b, false);
    }
}
static void sdr_open_settings(ui_ctx_t *ctx);
static void sdr_draw_bars_band(st7305_handle_t *lcd, const sdrv_report_t *r);
static void sdr_draw_ble_band(st7305_handle_t *lcd, const sdrv_report_t *r);
static void sdr_softbar_bottom(st7305_handle_t *lcd, bool scanning, int pct);

static void sdr_render(ui_ctx_t *ctx);   /* 前向声明: 离屏渲染递归调用 */
static void sdr_scr_render(ui_ctx_t *ctx, int slot, int page);
static int  sdr_px(const uint8_t *fb, int x, int y);
static ui_ctx_t *s_ctx;                  /* 最近一次 render 的 ctx (供串口测试钩子) */

/* ===== 串口测试钩子 (remcon: `sdrpan <dx>` / `sdrslide <dx>`) =====
 * remcon 只有 tap 没有 drag ⇒ 用它们注入"拖动"来验证跟手路径。
 * sdrpan: 走"谱线跟手平移"; sdrslide: 走"整页预渲染+合成"(dx=0 结束并恢复实时)。 */
/* 模拟一次**真实拖动**: 位移进欠账(MHz) → 按 DSP 节拍换频 → 欠账折算成像素平移。
 * 与 sdr_poll 同一条路径 (只省掉 45ms 的节流, 便于串口一步步注入验证)。 */
void sdr_test_pan(int dx)
{
    s_pan_res += (float)dx * (float)s_span / (float)SDRV_PLOT_W;
    if (s_pan_res >= 1.0f || s_pan_res <= -1.0f) {
        int m = (int)lrintf(s_pan_res);
        if (m == 0) m = (s_pan_res > 0.0f) ? 1 : -1;
        s_pan_res -= (float)m;
        sdr_pan_to(s_freq - m);
    }
    int disp = (int)lrintf(s_pan_res * (float)SDRV_PLOT_W / (float)s_span);
    if (disp >  32) disp =  32;
    if (disp < -32) disp = -32;
    sdrv_set_trace_shift(disp);
    s_pan_shift = disp;
    if (s_ctx) s_ctx->needs_redraw = true;
}
/* 设备侧自检: 直接数帧缓冲里的黑像素 (串口 fbdump 会被日志插行打断, 不可靠) */
void sdr_test_stat(void)
{
    if (!s_ctx || !s_ctx->lcd || !s_ctx->lcd->fb) { printf("SDRSTAT no-ctx\r\n"); return; }
    st7305_handle_t *l = s_ctx->lcd;
    int black = 0, plot = 0, bot = 0;
    for (int y = 0; y < 300; y++)
        for (int x = 0; x < 400; x++)
            if (!sdr_px(l->fb, x, y)) {
                black++;
                if (y >= 22 && y <= 150) plot++;      /* 波形区 */
                if (y >= 270) bot++;                  /* 底栏区 */
            }
    printf("SDRSTAT page=%d slide=%d sliding=%d latch=%d freq=%d span=%d panres=%d panshift=%d panning=%d black=%d plot=%d bottom=%d\r\n",
           s_page, s_slide, (int)s_sliding, (int)s_latch, s_freq, s_span,
           (int)lrintf(s_pan_res * 1000.0f), s_pan_shift,
           (int)s_panning, black, plot, bot);

    /* 音频页诊断: 直接抢读一帧 I2S RX, 看样点是不是"全 0"(硬件通了但没数据) */
    if (s_mic_on && s_mic_pcm) {
        int got = audio_player_mic_read(s_mic_pcm, 256, 80);
        int pk = 0, mn = 0, mx = 0; long ss = 0;
        for (int k = 0; k < (got > 0 ? got * 2 : 0); k++) {
            int v = s_mic_pcm[k];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            int a = v < 0 ? -v : v;
            if (a > pk) pk = a;
            ss += (long)v * (long)v;
        }
        printf("SDRMIC on=%d ok=%d fail=%d got=%d peak=%d min=%d max=%d rms=%ld lvl=%d avg0=%.1f floor=%.1f step_rms=%d step_pk=%d\r\n",
               (int)s_mic_on, (int)s_mic_ok, s_mic_fail_n, got, pk, mn, mx,
               got > 0 ? (long)sqrtf((float)ss / (float)(got * 2)) : -1L,
               s_mic_level, s_mic_avg ? s_mic_avg[0] : -999.0f, s_mic_floor,
               s_mic_rms_last, s_mic_pk_last);
    } else {
        printf("SDRMIC off tried=%d bufs=%d on=%d\r\n", (int)s_mic_tried, (int)s_mic_bufs, (int)s_mic_on);
    }
}

void sdr_test_slide(int dx)
{
    if (!s_ctx) return;
    if (dx == 0) { s_sliding = false; s_slide = 0; s_ctx->needs_redraw = true; return; }
    s_sliding = true;
    s_slide = dx;
    sdr_scr_render(s_ctx, 1, s_page);
    sdr_scr_render(s_ctx, 0, (s_page <= 1) ? SDR_NPAGE : s_page - 1);
    sdr_scr_render(s_ctx, 2, (s_page >= SDR_NPAGE) ? 1 : s_page + 1);
    s_ctx->needs_redraw = true;
}

/* 把某一视图渲染到离屏缓冲 (预加载) */
static void sdr_scr_render(ui_ctx_t *ctx, int slot, int page)
{
    if (slot < 0 || slot > 2) return;
    if (!s_scr[slot]) {
        s_scr[slot] = heap_caps_malloc(SDR_FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_scr[slot]) { s_scr_ok[slot] = false; return; }
    }
    st7305_handle_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.fb = s_scr[slot];
    st7305_handle_t *save_lcd = ctx->lcd;
    int  save_page = s_page;
    ctx->lcd    = &dev;
    s_page      = page;
    s_offscreen = true;
    sdr_render(ctx);                    /* 递归: 只画内容, 底栏被 s_offscreen 拦掉 */
    s_offscreen = false;
    s_page      = save_page;
    ctx->lcd    = save_lcd;
    s_scr_ok[slot] = true;
}

static int sdr_px(const uint8_t *fb, int x, int y)
{
    int yy = 299 - y;
    return (fb[(x >> 1) * SDR_FB_STRIDE + (yy >> 2)] >> (7 - (((yy & 3) << 1) | (x & 1)))) & 1u;
}
static void sdr_setpx(uint8_t *fb, int x, int y, int white)
{
    int yy = 299 - y;
    uint8_t m = (uint8_t)(1u << (7 - (((yy & 3) << 1) | (x & 1))));
    uint8_t *p = &fb[(x >> 1) * SDR_FB_STRIDE + (yy >> 2)];
    if (white) *p |= m; else *p &= (uint8_t)~m;
}

/* 合成: 当前页在 offset=dx, 邻页贴在其外侧; 再画实时底栏(轮播跟手) */
static void sdr_slide_render(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd || !lcd->fb) return;
    if (!s_scr_ok[0] || !s_scr_ok[1] || !s_scr_ok[2]) { s_offscreen = false; sdr_render(ctx); return; }
    const uint8_t *src[2];
    int off[2];
    if (s_slide >= 0) {                    /* 手指右拖: 上一项从左边进来 */
        src[0] = s_scr[0]; off[0] = s_slide - 400;
        src[1] = s_scr[1]; off[1] = s_slide;
    } else {                               /* 手指左拖: 下一项从右边进来 */
        src[0] = s_scr[1]; off[0] = s_slide;
        src[1] = s_scr[2]; off[1] = s_slide + 400;
    }
    for (int y = 0; y < 300; y++) {
        for (int x = 0; x < 400; x++) {
            const uint8_t *s = NULL; int sx = 0;
            for (int k = 0; k < 2; k++)
                if (x >= off[k] && x < off[k] + 400) { s = src[k]; sx = x - off[k]; break; }
            sdr_setpx(lcd->fb, x, y, s ? sdr_px(s, sx, y) : 1);
        }
    }
    sdr_softbar_bottom(lcd, false, 0);     /* 底栏(含轮播)实时画在最上层 */
}

static void sdr_render(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    s_ctx = ctx;                                                          /* 供测试钩子用 */
    /* ★ 未启用: 采集区已回收进堆, 各缓冲均为 NULL → 直接清屏返回 (不画频谱, 防空指针)。
     *   此时 poll 正在弹"启用并重启"确认框。 */
    if (!espsdr_usable()) { st7305_clear(lcd, ST7305_COLOR_WHITE); return; }
    if (s_sliding && !s_offscreen) { sdr_slide_render(ctx); return; }   /* 拖动中: 合成两页 */
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    /* ★ 4 个视图 (轮播): 波形 / 信道 / 蓝牙 / 音频; 顶栏右 = 频段(定格后) / 扫描进度(扫描中) */
    if (s_page < 1 || s_page > SDR_NPAGE) s_page = 1;
    static const char *const s_view_title[4] = { "2.4G 波形", "WiFi 信道", "蓝牙广播", "音频" };
    const char *ttl = s_view_title[s_page - 1];

    /* ★ 第 4 页 = 音频 (麦克风), 与射频无关: 单独一条路径 (不定格 / 不重扫) */
    if (s_page == 4) {
        sdr_draw_audio_page(ctx);
        sdr_softbar_bottom(lcd, false, 0);
        return;
    }
    /* ★ 扫描中**不再整屏遮住** —— 先出空白框架, 进度放凹口, 右软键变「继续」
     *   (linit: "优先显示空白数据"; 继续 = 不等了, 用现有数据定格)。 */
    bool scanning = s_offscreen ? false : !s_latch;   /* 离屏预加载要画"有数据"的样子 */
    int  scan_pct = 0;
    if (scanning) {
        int64_t now = esp_timer_get_time() / 1000;
        scan_pct = 100 - (int)((s_scan_until - now) * 100 / SDR_SCAN_MS);
        if (scan_pct < 0) scan_pct = 0;
        if (scan_pct > 100) scan_pct = 100;
    }

    char right[24];
    if (scanning) snprintf(right, sizeof(right), "扫描 %d%%", scan_pct);
    else          sdr_band_text(right, sizeof(right));

    sdrv_info_t   in;  sdr_fill_info(&in);
    sdrv_report_t r;   sdr_fill_report(&r);

    /* ★ 量程: 窗口固定 70dB, 上沿 = 本底 + 60 ⇒ 左侧刻度 60/40/20/本底 才与谱线对齐。
     *   ⛔ 全工程一直没调过 sdrv_range_set() ⇒ 一直用默认 [-100,0] dB, 刻度与谱线错位、
     *     每 dB 只占 1.28px 显得"起伏太小" —— 那段动态量程代码从来没被接上。 */
    if (s_floor_valid) sdrv_range_set(s_floor - 10.0f, s_floor + 60.0f);

    if (s_page == 1) {                       /* 全景: 只有图 */
        sdrv_draw_topbar(lcd, ttl, right);
        if (!s_ok)         sdrv_draw_notice(lcd, "射频未就绪", "返回 菜单");
        else if (scanning) sdrv_draw_panorama(lcd, &in, NULL, NULL, COLS, NULL);
        else               sdrv_draw_panorama(lcd, &in, s_avg, s_hold, COLS, s_wf);
        { char vw[24]; snprintf(vw, sizeof(vw), "%d/4 %s", s_page, s_freeze ? "已定格" : "实时");
          sdr_softbar_bottom(lcd, scanning, scan_pct); }
        return;
    }
    if (s_page == 2) {                       /* 信道: **上半波形 + 下半 13 根柱** */
        sdrv_draw_topbar(lcd, ttl, right);
        if (s_ok && !scanning) sdrv_draw_panorama(lcd, &in, s_avg, NULL, COLS, NULL);
        if (!scanning) sdr_draw_bars_band(lcd, &r);
        sdr_softbar_bottom(lcd, scanning, scan_pct);
        return;
    }
    if (s_page == 3) {                       /* 蓝牙: **上半波形 + 下半 3 条广播信道** (同信道页) */
        sdrv_draw_topbar(lcd, ttl, right);
        if (s_ok && !scanning) sdrv_draw_panorama(lcd, &in, s_avg, NULL, COLS, NULL);
        if (!scanning) sdr_draw_ble_band(lcd, &r);
        sdr_softbar_bottom(lcd, scanning, scan_pct);
        return;
    }
    /* 页 4: 干扰 */
    sdrv_draw_topbar(lcd, ttl, right);
    sdrv_draw_jampage(lcd, &r);
    sdr_softbar_bottom(lcd, scanning, scan_pct);
}

/* ---------------- 交互 ---------------- */
/* 频段文字: "2400-2485MHz" */
static void sdr_band_text(char *b, int n)
{
    int lo = s_freq - s_span / 2, hi = s_freq + s_span / 2;
    snprintf(b, (size_t)n, "%d-%dMHz", lo, hi);
}

/* 拖动调频专用: 只调频, **不**清平均(否则每像素清一次, 画面闪)。
 * 但要清 peak-hold —— 换了频率, 旧包络挂在新坐标上是错的。 */
static void sdr_pan_to(int mhz)
{
    if (mhz < (int)ESPSDR_FREQ_MIN_MHZ) mhz = (int)ESPSDR_FREQ_MIN_MHZ;
    if (mhz > (int)ESPSDR_FREQ_MAX_MHZ) mhz = (int)ESPSDR_FREQ_MAX_MHZ;
    if (mhz == s_freq) return;
    s_freq = mhz;
    if (s_hold) for (int i = 0; i < COLS; i++) s_hold[i] = SDRV_DB_NODATA;
    if (s_ok) espsdr_tune((unsigned)s_freq);
}

/* 设置页: 调当前选中项 (dir = +1/-1) */
static void sdr_set_adj(int dir)
{
    switch (s_set_sel) {
    case 0: sdr_retune(s_freq + dir * (s_span >= 40 ? 5 : 1)); break;
    case 1: {
        static const int up[4]   = { 40, 20, 10, 80 };   /* 80→40→20→10→80 */
        static const int down[4] = { 10, 80, 40, 20 };
        int idx = (s_span == 80) ? 0 : (s_span == 40) ? 1 : (s_span == 20) ? 2 : 3;
        s_span = (dir > 0) ? up[idx] : down[idx];
        sdr_reset_view();
        break;
    }
    case 2: s_avg_n = (s_avg_n == 1) ? 4 : (s_avg_n == 4) ? 16 : 1; sdr_reset_view(); break;
    default: {
        s_gain = (s_gain == ESPSDR_GAIN_AUTO) ? 40 : ESPSDR_GAIN_AUTO;
        if (s_ok) espsdr_set_gain(s_gain);
        break;
    }
    }
}

static void sdr_retune(int mhz)
{
    if (mhz < (int)ESPSDR_FREQ_MIN_MHZ) mhz = (int)ESPSDR_FREQ_MIN_MHZ;
    if (mhz > (int)ESPSDR_FREQ_MAX_MHZ) mhz = (int)ESPSDR_FREQ_MAX_MHZ;
    s_freq = mhz;
    if (s_ok) espsdr_tune((unsigned)s_freq);
    sdr_reset_view();                       /* 换频 → 清平均与 peak-hold (旧包络已无意义) */
}

/* ---- 设置: 走**二级弹窗**(os_dialog_list, 350×250), 不用全屏页 ----
 * 每行显示"项名 + 当前值"; 点一行 = 换下一档并重开弹窗刷新。 */
static char s_set_line[4][28];
static const char *s_set_ptr[4];

static void sdr_set_cb(ui_ctx_t *ctx, int sel, void *ud);
static void sdr_open_settings(ui_ctx_t *ctx);

/* ---- 音频页的设置 (同样走二级弹窗, 3 项) ---- */
static char s_mset_line[3][28];
static const char *s_mset_ptr[3];
static int  s_mset_sel;

static void sdr_micset_cb(ui_ctx_t *ctx, int sel, void *ud);

static void sdr_open_micset(ui_ctx_t *ctx)
{
    static const char *const nm[3] = { "平均次数", "频率上限", "峰保持" };
    char v[12];
    if (!s_mic_bufs) return;
    for (int i = 0; i < 3; i++) {
        switch (i) {
        case 0: snprintf(v, sizeof v, s_mic_avg_n == 1 ? "关" : "%d次", s_mic_avg_n); break;
        case 1: snprintf(v, sizeof v, "%dkHz", s_mic_f_hi / 1000); break;
        default: snprintf(v, sizeof v, s_mic_hold_on ? "开" : "关"); break;
        }
        snprintf(s_mset_line[i], sizeof(s_mset_line[i]), "%s    %s", nm[i], v);
        s_mset_ptr[i] = s_mset_line[i];
    }
    os_dialog_list(ctx, "设置", s_mset_ptr, 3, s_mset_sel, sdr_micset_cb, NULL);
}

static void sdr_micset_cb(ui_ctx_t *ctx, int sel, void *ud)
{
    (void)ud;
    if (sel < 0 || sel > 2 || !s_mic_bufs) return;
    s_mset_sel = sel;
    switch (sel) {
    case 0:
        s_mic_avg_n = (s_mic_avg_n == 1) ? 4 : (s_mic_avg_n == 4) ? 16 : 1;
        for (int i = 0; i < COLS; i++) s_mic_avg[i] = SDRV_DB_NODATA;      /* 重新积累 */
        break;
    case 1:
        s_mic_f_hi = (s_mic_f_hi == 8000) ? 4000 : (s_mic_f_hi == 4000) ? 2000 : 8000;
        sdr_mic_map_cols();
        for (int i = 0; i < COLS; i++) { s_mic_avg[i] = SDRV_DB_NODATA; s_mic_hold[i] = SDRV_DB_NODATA; }
        memset(s_mic_wf, 0, (size_t)SDRV_WF_ROW_B * SDRV_WF_H);
        s_mic_floor_valid = false;
        break;
    default:
        s_mic_hold_on = !s_mic_hold_on;
        if (s_mic_hold_on) for (int i = 0; i < COLS; i++) s_mic_hold[i] = SDRV_DB_NODATA;
        break;
    }
    sdr_open_micset(ctx);
}

static void sdr_open_settings(ui_ctx_t *ctx)
{
    if (s_page == 4) { sdr_open_micset(ctx); return; }   /* 音频页: 麦克风参数 */
    static const char *const nm[4] = { "中心频率", "跨度", "平均次数", "增益" };
    char v[12];
    for (int i = 0; i < 4; i++) {
        switch (i) {
        case 0: snprintf(v, sizeof(v), "%dMHz", s_freq); break;
        case 1: snprintf(v, sizeof(v), "%dMHz", s_span); break;
        case 2: snprintf(v, sizeof(v), s_avg_n == 1 ? "关" : "%d次", s_avg_n); break;
        default: snprintf(v, sizeof(v), s_gain == ESPSDR_GAIN_AUTO ? "自动" : "手动"); break;
        }
        snprintf(s_set_line[i], sizeof(s_set_line[i]), "%s    %s", nm[i], v);
        s_set_ptr[i] = s_set_line[i];
    }
    os_dialog_list(ctx, "设置", s_set_ptr, 4, s_set_sel, sdr_set_cb, NULL);
}

static void sdr_set_cb(ui_ctx_t *ctx, int sel, void *ud)
{
    (void)ud;
    if (sel >= 0 && sel <= 3) {
        s_set_sel = sel;
        sdr_set_adj(+1);
        sdr_open_settings(ctx);
    }
}

/* 交互 (linit 2026-10-02 定案: "不需要左右上下浮动, 选中某个频率, 选项太多没必要")
 *  → 全部砍掉: 不调中心频率、不调增益、不选频率、没有菜单。
 *  只留两个键: 确认 = 在"频谱页 / 判读页"之间切; 返回 = 退出。
 *  跨度/平均/增益固定为自动最优值 (80MHz / 4 次 / AGC), 不再暴露。 */
static void sdr_action(ui_ctx_t *ctx, os_action_t a)
{
    /* ★ 未启用: 缓冲全为 NULL, 只允许"返回退出", 其它动作一律忽略 (防空指针)。 */
    if (!espsdr_usable()) {
        if (a == OS_ACTION_BACK) os_request_exit_confirm(ctx);
        return;
    }
    switch (a) {
    case OS_ACTION_CONFIRM:
        sdr_scan_start();                      /* 点确认 = 重新测一次 (结果才更新) */
        break;
    case OS_ACTION_LEFT:                       /* ←→ 换视图: **同样不要重扫** (否则白屏几秒) */
        sdr_goto(s_page - 1);
        break;
    case OS_ACTION_RIGHT:
        sdr_goto(s_page + 1);
        break;
    case OS_ACTION_BACK:
        if (espsdr_usable()) {
            /* 已占用 64KB: 退出前提示"重启释放"。选取消则留在本页 (返回键可再提示)。 */
            if (!s_need_release) {
                s_need_release = true;
                os_dialog_confirm_ex(ctx, "退出并重启?\n将释放频谱仪占用的64KB内部内存", 0, 0, sdr_release_cb, NULL);
            }
        } else {
            os_request_exit_confirm(ctx);      /* 未占用: 普通退出确认 */
        }
        return;
    default: break;
    }
    ctx->needs_redraw = true;
}

/* 顶栏按**同样等分槽**响应 —— 看到的四个槽就是能点的四个参数 (槽位定义见 sdr_view.h) */
static bool sdr_touch(ui_ctx_t *ctx, int x, int y)
{
    if (!espsdr_usable()) return true;     /* 未启用: 空页, 不吃触摸 (防呆) */
    /* ---- 底栏软键 (与全项目同套): 左「设置」 右「冻结/返回」 ---- */
    if (y >= SDRV_SOFT_LID) {
        if (x < 90) {                                   /* 左软键 = 设置(二级弹窗) */
            sdr_open_settings(ctx);
            return true;
        }
        if (x > 310) {                                  /* 右软键 = 继续 / 冻结 */
            if (s_page != 4 && !s_latch) sdr_scan_latch();   /* 音频页没有"扫描定格" */
            else                         s_freeze = !s_freeze;
            ctx->needs_redraw = true; return true;
        }
        return true;
    }
    if (s_page == 4) return true;      /* 音频页: 顶栏/谱区不做射频参数 (拖动手势在 poll 里) */
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
    } else if (y >= SDRV_PLOT_Y0 && y <= SDRV_PLOT_Y1) {
        /* ★ 谱区改为**上方左右拖动调频**(跟手滚动, 在 poll 里跟手), 点一下不再跳频 */
        return true;
    } else {
        return false;
    }
    ctx->needs_redraw = true;
    return true;
}

static void sdr_poll(ui_ctx_t *ctx)
{
    /* ★ 频谱仪未启用(默认): 唯一动作 = 弹一次"启用并重启"; 已启用才走正常采集路径。
     *   (此时 s_ok=false 且所有缓冲为 NULL, 绝不能往下走。) */
    if (s_notouch_msg) {
        s_notouch_msg = false;
        os_dialog_toast(ctx, "本机无触摸屏, 频谱仪不可用");
        /* ★ 2026-10-28: 提示完直接退出 —— 不能用的功能没必要把用户留在空页上 */
        os_request_pop(ctx);
        return;
    }
    if (s_need_enable) {
        s_need_enable = false;
        os_dialog_confirm_ex(ctx, "频谱仪需占用64KB内部内存\n启用后需重启生效\n是否启用?", 0, 0, sdr_enable_cb, NULL);
        return;
    }
    if (!espsdr_usable()) return;   /* 未启用: 空页, 等用户按返回离开 */
    /* ⛔ 开头**不能**是 `if (!s_ok) return`: 第 4 页(音频)走麦克风, 与射频无关,
     *   射频没接管也要能用; 电池读数与拖动手势两条也得照跑。 */
    int64_t now = esp_timer_get_time() / 1000;

    if (now - s_batt_ms > 1000) {                /* 电池读数 (接口自身 60s 节流) */
        s_batt_ms = now;
        board_battery_status_t bs;
        if (board_battery_read_cached(&bs) == ESP_OK) s_batt = bs.percent;
    }
    if (s_ok && !s_latch && now >= s_scan_until && s_page != 0) {   /* 扫描时间到 → 定格 */
        sdr_scan_latch();
        ctx->needs_redraw = true;
    }
    /* ---- 上方拖 = 调频(**跟手滚动**) ; 下方拖 = 左右换视图 ---- */
    {
        int tx, ty;
        bool down = input_get_touch_pos(&tx, &ty);
        bool in_lower = down && s_page >= 1 && s_page <= SDR_NPAGE && ty > SDRV_AXIS_LINE;
        if (in_lower) {                             /* ★ 下方横滑 = **跟手翻页** */
            static int sw_x0;
            if (!s_panning) {                        /* 拖动开始: 预加载相邻三页 */
                s_panning = true; sw_x0 = tx; s_slide = 0; s_pan_v = 0.0f;
                s_sliding = true;
                sdr_scr_render(ctx, 1, s_page);
                sdr_scr_render(ctx, 0, (s_page <= 1) ? SDR_NPAGE : s_page - 1);
                sdr_scr_render(ctx, 2, (s_page >= SDR_NPAGE) ? 1 : s_page + 1);
            }
            int sl = tx - sw_x0;
            if (sl > 399)  sl = 399;                 /* 上限 = 整屏宽 (原来 120 ⇒ 拖一定距离就顶住) */
            if (sl < -399) sl = -399;
            if (sl != s_slide) { s_slide = sl; ctx->needs_redraw = true; }
            return;
        }
        if (s_panning && !down) {                   /* 松手: **弹性归位动画** (不立刻刷新) */
            s_panning = false;
            s_anim_to = (s_slide <= -38 || s_slide >= 38)   /* 过半 ⇒ 滑出到下一屏 */
                      ? ((s_slide < 0) ? -400 : 400) : 0;   /* 否则弹回原位 */
        }
        if (!s_panning && s_anim_to != -99999) {    /* 动画中: 每帧走 35% 剩余 ⇒ 有弹性的归位 */
            int d = s_anim_to - s_slide;
            s_slide += (d * 35) / 100;
            if (d > -4 && d < 4) {
                bool flip = (s_anim_to != 0);
                s_slide = 0;
                s_anim_to = -99999;
                s_sliding = false;
                if (flip) {
                    sdr_goto((d < 0) ? ((s_page >= SDR_NPAGE) ? 1 : s_page + 1)
                                     : ((s_page <= 1) ? SDR_NPAGE : s_page - 1));
                    /* ⛔ 不重扫: 三个射频视图同源, 换页立刻用现有数据显示 (白屏根因) */
                }
            }
            ctx->needs_redraw = true;
        }
        bool in_plot = down && s_page >= 1 && s_page <= 3 &&      /* 音频页没有"谱区调频" */
                       ty >= SDRV_PLOT_Y0 && ty <= SDRV_AXIS_LINE;
        if (in_plot) {
            if (!s_panning) { s_panning = true; s_pan_x = tx; }
            int dx = tx - s_pan_x;
            s_pan_x = tx;
            bool dirty = (dx != 0);
            /* ★★★ 拖动调频的记账 (2026-10-15 重做, 三条血泪):
             *  ① **欠账用浮点 MHz 记, 不能用像素来回折算** —— 原来 "px→MHz→px" 每换一次频
             *     都要取整两次, 实测拖 240px 会被吃掉 2MHz (系统性漂移, 拖久了越差越远)。
             *     现在: 手指位移 → 直接加进 `s_pan_res`(MHz, 小数保留), 换频只扣整数 MHz。
             *  ② **换频节流到 DSP 节拍** (45ms): 换得比 DSP 快, 画面就只剩糊。
             *  ③ 欠账**不截断** —— 拖得快的部分留到后面继续换, 保证**总位移 == 手指总位移**。
             *     显示只取欠账的 ±32px 做像素平移 ⇒ 快拖时画面最多落后半格, 随后自己追平。
             * ★ 方向: 手指往右拖 = 把图往右挪 ⇒ 左边缘露出来的是**更低**的频率 ⇒
             *   中心频率必须**减小**。原来写成 `s_freq + mhz` 且谱线偏移取了反号, 两个都反。 */
            if (dx) s_pan_res += (float)dx * (float)s_span / (float)SDRV_PLOT_W;   /* px → MHz */
            if ((s_pan_res >= 1.0f || s_pan_res <= -1.0f) && now - s_pan_ms >= 45) {
                int m = (int)lrintf(s_pan_res);
                if (m == 0) m = (s_pan_res > 0.0f) ? 1 : -1;    /* 至少换 1MHz (PHY 的粒度) */
                s_pan_res -= (float)m;
                sdr_pan_to(s_freq - m);
                s_pan_ms = now;
                dirty = true;
            }
            int disp = (int)lrintf(s_pan_res * (float)SDRV_PLOT_W / (float)s_span);
            if (disp >  32) disp =  32;
            if (disp < -32) disp = -32;
            if (disp != s_tr_disp) { sdrv_set_trace_shift(disp); s_tr_disp = disp; }
            s_pan_shift = disp;
            if (dirty) ctx->needs_redraw = true;   /* 正号 = 内容跟着手指往右走 */
        } else if (s_panning && !down) {
            /* 松手: 把欠账结清 (方向同上, 内容跟手) */
            s_panning = false;
            if (s_pan_res >= 0.5f || s_pan_res <= -0.5f) {
                int m = (int)lrintf(s_pan_res);
                s_pan_res = 0.0f;
                sdrv_set_trace_shift(0); s_tr_disp = 0; s_pan_shift = 0;
                sdr_retune(s_freq - m);
            } else {
                s_pan_res = 0.0f;
                sdrv_set_trace_shift(0); s_tr_disp = 0; s_pan_shift = 0;
            }
            ctx->needs_redraw = true;
        }
    }

    if (s_freeze) return;                               /* 冻帧: 停 DSP, 画面定住 */

    /* ★ 第 4 页 = 音频: 只跑麦克风采集, **跳过射频** —— 两路共用同一套 FFT
     *   /s_fft/s_bin 缓冲, 同一帧里不能并行 (也省掉射频那份 CPU 占用) */
    if (s_page == 4) {
        if (now - s_last_ms < STEP_MS) return;
        s_last_ms = now;
        if (sdr_mic_step()) ctx->needs_redraw = true;
        return;
    }

    if (!s_ok) return;
    /* ★ 拖动调频期间把 DSP 节拍从 10Hz 提到 20Hz: 换频后新数据要尽快顶上来,
     *   否则画面比手指慢半秒("拖了没反应")。松手即回 10Hz。 */
    if (now - s_last_ms < (s_panning ? 50 : STEP_MS)) return;
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
