/**
 * sdr_view.h — 频谱仪**渲染层**（与采集/DSP 解耦）
 * ----------------------------------------------------------------------------
 * 版式（400×300 横屏，参考 tinySA）：
 *   ① 顶栏(反白)  0..19   **纯状态**: 中心 | 跨度 | RBW | 本底 (+电池 / 越写告警)
 *   ② 谱图       22..150  左侧"本/20/40/60"刻度 + 点阵网格 + 细线谱线
 *                          + 点线 peak-hold + marker(▼ + 紧跟的反白读数框)
 *   ③ 频率轴    152..166  每 10MHz 一个 tick, **每 20MHz 才标数字**（别堆数字）
 *   ④ 信道标注  167..181  WiFi ch1..13 + BLE 广播 2402/2426/2480
 *   ⑤ 瀑布      183..271  1bpp 位图(1=黑=强) + 左侧"强/弱"图例
 *   ⑥ 底栏(反白)274..299  **5 个功能槽**: 分析 | 保持 | 跨度 | 平均 | 增益
 *                          常态显示各功能当前值; 菜单模式下选中槽反白, 可左右选、确认执行
 *
 * 文字策略: 中文标签走 `ui_text16`（中文16px / 数字8px 混排）, 纯数字刻度用 8px 小字。
 * 单位(dB/MHz)按中文排版惯例保留拉丁字母。
 * ==========================================================================*/
#pragma once

#include "os_app_api.h"
#include <stdbool.h>
#include <stdint.h>

/* ---------------- 版式几何 ---------------- */
#define SDRV_W            400
#define SDRV_H            300

#define SDRV_TOP_Y        0
#define SDRV_TOP_H        20

#define SDRV_PLOT_Y0      22
#define SDRV_PLOT_Y1      150
#define SDRV_PLOT_X0      32          /* 与瀑布/信道条对齐, 且是 8 的整数倍 */
#define SDRV_PLOT_X1      399
#define SDRV_PLOT_W       368

#define SDRV_AXIS_LINE    152
#define SDRV_AXIS_TICK    153
#define SDRV_AXIS_TXT_Y   155

#define SDRV_CH_Y0        167
#define SDRV_CH_H         15

#define SDRV_WF_Y0        183
#define SDRV_WF_H         89
#define SDRV_WF_ROW_B     46          /* SDRV_PLOT_W / 8 */

#define SDRV_BOT_Y        274
#define SDRV_BOT_H        26
#define SDRV_BOT_TXT_Y    281

/* 顶栏: 只留"中心频率 + 电量"两件事 (其余参数全自动, 不显示) */
#define SDRV_SLOT_X0      2
#define SDRV_SLOT_W       88


/* 频率轴: 每 10MHz 一个 tick, 每 20MHz 才标数字 */
#define SDRV_TICK_STEP_MHZ 10
#define SDRV_LABEL_EVERY   2

/* 量程窗口固定 70dB, 上下沿由页面按"有没有信号"动态定 (见 sdrv_range_set 的调用处):
 *   安静 → 上沿 = 本底+30 (把本底的噪声起伏撑开, 不会变成一条贴边的直线)
 *   有信号 → 上沿 = 峰顶+8  (峰顶铺满, 不顶天花板)
 * 刻度仍是 0/20/40/60 = 高于本底多少 dB (0 = 本底, 画实线)。 */
#define SDRV_RANGE_SPAN   70.0f
#define SDRV_QUIET_TOP    30.0f      /* 安静时上沿 = 本底 + 30dB */
#define SDRV_PEAK_MARGIN  8.0f       /* 有信号时峰顶上方留 8dB */
#define SDRV_SIG_MIN      6.0f       /* 高出本底不足 6dB 视为"没信号"(marker 不画) */
#define SDRV_DB_NODATA    (-200.0f)

/* ---------------- 一帧信息 ---------------- */
typedef struct {
    unsigned center_mhz;
    float    span_mhz;
    int      rbw_khz;
    int      floor_db;       /* 本底 (整数 dB, 仅显示用) */
    int      batt_pct;       /* -1 = 未知 */
    int      overrun_word;   /* >=0 → 顶栏提示采集区越写 */
    int      cap_ok;
    int      marker_x;
    int      marker_rel;     /* 高于本底 dB */
    const char *marker_txt;  /* 紧跟 marker 的反白小框文字; NULL = 不画 */
} sdrv_info_t;

/* ---------------- 底栏: 只留一行操作提示 ----------------
 * linit 2026-10-02: "选项说明还是太多了, 都是没必要的" → 底栏不再放功能槽,
 * 只写当前能按的两个键。跨度/平均/增益全部改成自动, 不再暴露给用户。 */

/* ---------------- 扫描判读报告 (覆盖整屏) ---------------- */
typedef struct {
    const char *title;       /* 页面标题 (如 "2.4G 扫描判读") */
    int   floor_db;
    int   span_mhz;
    const float *trace;      /* 实时谱线 (trace_n 列, dB) —— 中间"浮动频谱"小条用 */
    int   trace_n;
    int   pct[13];           /* WiFi ch1..13 占用度 0..100; -1 = 不在当前跨度内 */
    int   worst_ch;          /* 最挤 (1..13), 0 = 未知 */
    int   best_ch;           /* 最空 (1..13), 0 = 未知 */
    int   ble_mhz;           /* 检到的 BLE 广播频率, 0 = 未检到 */
    int   ble_db[3];         /* 2402/2426/2480 三处高出本底多少 dB (可为负) */
    int   n_sig;             /* 谱线里检到几个明显的峰 (高于本底 12dB 的局部极大) */
    int   broadband;         /* 1 = 检到宽带抬高 */
    const char *verdict;     /* 结论 (中文) */
    const char *advice;      /* 建议 (中文) */
} sdrv_report_t;

/* ---------------- 量程 ---------------- */
void  sdrv_range_set(float bot_db, float top_db);
float sdrv_range_top(void);
float sdrv_range_bot(void);

/* ---------------- 坐标换算 (唯一来源) ---------------- */
int   sdrv_y_of_db(float db);
int   sdrv_x_of_mhz(float mhz, unsigned center_mhz, float span_mhz);
float sdrv_mhz_of_x(int x, unsigned center_mhz, float span_mhz);
float sdrv_mhz_of_col(int idx, unsigned center_mhz, float span_mhz);

/* ---------------- 分区绘制 (按此顺序) ---------------- */
void sdrv_draw_top(st7305_handle_t *lcd, const sdrv_info_t *in);
void sdrv_draw_scale(st7305_handle_t *lcd);
void sdrv_draw_grid(st7305_handle_t *lcd, const sdrv_info_t *in);
void sdrv_draw_trace(st7305_handle_t *lcd, const float *db, int n);
void sdrv_draw_hold(st7305_handle_t *lcd, const float *hold, int n);
void sdrv_draw_marker(st7305_handle_t *lcd, int x, const char *label);
void sdrv_draw_axis(st7305_handle_t *lcd, const sdrv_info_t *in);
void sdrv_draw_channels(st7305_handle_t *lcd, const sdrv_info_t *in);
void sdrv_draw_waterfall(st7305_handle_t *lcd, const uint8_t *bmp);
void sdrv_draw_hint(st7305_handle_t *lcd, const char *left, const char *right);

/* ★ 2026-10-14 新视觉: 顶栏**无标题无分隔线**(左=当前源/页名, 右=频段), 底栏改全局软键栏。
 *   软键栏几何与全项目一致 (同 page_netdect / page_logdiag): 盖子线 273 / 谷线 294 /
 *   左右键贴边距 16px 纯文字无框; 中间凹口放状态字。 */
#define SDRV_SOFT_LID     273
#define SDRV_SOFT_VALLEY  294
#define SDRV_SOFT_KEYY    277
#define SDRV_SOFT_MARGIN  16
void sdrv_draw_topbar(st7305_handle_t *lcd, const char *title, const char *right);
void sdrv_draw_softbar(st7305_handle_t *lcd, const char *left, const char *right, const char *mid);

/* ★ 2026-10-14 视图轮播: 3 个视图名横向排, 中间放大、两侧缩小; dx = 跟手位移(px)。
 *   这是"拖全屏卡片"的视觉载体 —— 拖动时名字按 1:1 平移, 松手吸附到最近一项。 */
void sdrv_draw_carousel(st7305_handle_t *lcd, const char *const *nm, int n, int sel, int dx);
/* ★ 谱线水平偏移(px): 上方拖动调频时让**曲线跟手平移**(1:1), 松手才真正换频+更新数据。
 *   雷达/SDR 通用做法: 先"图形平移"给即时反馈, 再重采数据。 */
void sdrv_set_trace_shift(int px);

/* ---------------- 按功能分屏 (一屏只干一件事, 不拼合) ----------------
 * linit 2026-10-02: "根据不同的功能显示不同的频谱 UI, 不要上面太多字和频谱拼在一起"。 */
void sdrv_draw_modebar(st7305_handle_t *lcd, const char *title);   /* 顶部标题 + 3px 横线 */

/* ① 模式菜单页: 4 项, 每项"主标题 + 一句说明", 选中整行反白 */
void sdrv_draw_modemenu(st7305_handle_t *lcd, int sel, const char *const *names,
                        const char *const *notes, int n);

/* ② 全景页: 只有频谱 + 瀑布 (纯图, 不放统计/建议) */
void sdrv_draw_panorama(st7305_handle_t *lcd, const sdrv_info_t *in,
                        const float *trace, const float *hold, int n, const uint8_t *wf);

/* ③ 信道页: 13 根大柱 + 柱顶百分比 + 一行建议 */
void sdrv_draw_chanbars(st7305_handle_t *lcd, const sdrv_report_t *r);

/* ④ 蓝牙页: 三条 BLE 广播信道横条 + 一句结论 */
void sdrv_draw_blepage(st7305_handle_t *lcd, const sdrv_report_t *r);

/* ⑤ 干扰页: 3 行指示 + 一句结论 */
void sdrv_draw_jampage(st7305_handle_t *lcd, const sdrv_report_t *r);
void sdrv_draw_scanning(st7305_handle_t *lcd, const char *title, int pct);
void sdrv_draw_report(st7305_handle_t *lcd, const sdrv_report_t *r);
void sdrv_draw_notice(st7305_handle_t *lcd, const char *line1, const char *line2);

int  sdrv_axis_ticks(float span_mhz);
int  sdrv_axis_tick_x(int i, int n);

/* 小字体 (8×12) 文本 */
void sdrv_ascii(st7305_handle_t *lcd, int x, int y, const char *s, bool inv);
void sdrv_ascii_center(st7305_handle_t *lcd, int cx, int y, const char *s, bool inv);
int  sdrv_ascii_w(const char *s);
