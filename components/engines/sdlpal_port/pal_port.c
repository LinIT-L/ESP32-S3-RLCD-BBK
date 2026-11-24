/* pal_port.c — 8bpp 调色板画面 -> ST7305 1bpp 单色：全屏 / 自适应灰度
 *
 * 关键点（踩过的坑都写在这）：
 *  · ST7305 的 framebuffer **不是行扫描**，而是"2 列 × 4 行 / 字节"：
 *        idx = (x>>1)*75 + ((299-y)>>2)
 *        bit = 7 - ((((299-y)&3)<<1) | (x&1))     黑=清位, 白=置位
 *    也就是说内存是**列优先**：同一字节列的 75 个字节是连续的。
 *    所以这里按"字节列 c（=2 个屏幕列）× 4 行"为单位一次拼出 1 字节，
 *    每 8 个像素只写一次内存（老版本是每像素读改写一次，慢 8 倍）。
 *  · 1bpp 屏没法表现灰阶，用 **4x4 Bayer 有序抖动**把亮度映射成黑白点阵。
 *  · **自适应灰度**：画面整体偏黑/偏白时，抖动层次会全挤在一头（画面发灰或发白）。
 *    开启后统计亮度分布的 2%~98% 分位，把这段线性拉满再抖动，并和原值半强度混合，
 *    避免过度增强出噪点。默认开，可用 pal_port_set_auto_level(false) 关掉。
 *  · **虚拟按键**不在这里画 —— 直接用系统现成的 components/ui/virtual_keys
 *    （GBC/NES 用的那套：左方向十字 + 右 A/B + 底 SEL/STA），present 末尾叠加。
 */
#include "pal_port.h"

#include "st7305.h"
#include "virtual_keys.h"
#include "input.h"   /* engine_overlay_quickbar */   /* 系统现成的虚拟按键（GBC 同款） */
#include "esp_log.h"
#include "esp_timer.h"

#include <string.h>
#include <stdio.h>      /* printf: 帧 dump */
#include <math.h>       /* powf: gamma 曲线表 */

static const char *TAG = "PALPORT";

#define SCR_W  ST7305_WIDTH      /* 400 */
#define SCR_H  ST7305_HEIGHT     /* 300 */
#define COL_BYTES (SCR_H / 4)    /* 75：一个字节列存 4 行 */

static const uint8_t *s_src = NULL;
static int      s_sw = 0, s_sh = 0;
static uint8_t  s_pal[256 * 4];
/* ★★★ 2026-10-25 linit: 「画面先优化再送屏，中间层用 GBC 的画面优化」。
 *   GBC/NES 在 board_shim 里用的是「4 级灰度 + 2x2 散点抖动」，出来的黑白干净、
 *   不像 8x8 Bayer 那样整片灰噪。这里把同一套算法搬成仙剑的**中间层**：
 *     调色板 →(分位拉伸)→ 亮度 →(量化)→ 4 级 shade →(2x2 散点)→ 1bpp
 *   与 GBC 的差异只在几何(仙剑 320x200→400x300 是 1.25 倍，不是整数倍)，
 *   抖动仍按屏幕像素奇偶取 2x2 象限，观感一致。 */
static uint8_t  s_shade[256];       /* 调色板索引 -> 5 级灰度 0..4 */
static volatile bool s_dirty = false;
static int      s_zoom = 0;         /* 0 = 全屏铺满 */
/* ★★★ 2026-10-25 linit「游戏整体太白 能不能关闭动态亮度或者动态对比度」⇒ **默认关**。
 *   自适应拉伸本身会让"亮场景更白、暗场景更黑", 在 1bpp 屏上很容易整片发白。
 *   现在是**固定曲线**: 只跟调色板的亮度线性映射, 不统计画面、不随时间变化。
 *   想试动态: remcon `palgray 1`。 */
static bool     s_auto = false;     /* 动态对比度(分位拉伸) —— 默认关 */
/* ★★★ 2026-10-25 linit「单独限制仙剑奇侠传帧率」。
 *   全工程别处(UI/其他引擎)都已不限速; 只有仙剑这一路单独限 20fps。
 *   做法是**跳过**, 不是 vTaskDelay —— 绝不阻塞主循环(pal_tick 在主任务里跑)。
 *   跳过时保留 s_dirty, 等窗口到了再画最新一帧(游戏画面是同一块缓冲原地更新)。 */
static int      s_max_fps = 20;     /* 0 = 不限 */
static volatile uint32_t s_frames_done = 0;   /* 已上屏帧数: 0 = 还在载入 (首帧未出) */
/* ★★★ 2026-10-25 linit「载入完成还是白屏了一会」:
 *   实测载入页 ~5% 黑, 切过去之后连续 2~3 秒是 **0~1% 黑 = 整片白**
 *   (SDLPal 首帧先播"白→正常"的淡入, 我在首帧就切, 正好切在白屏上)。
 *   所以判据不能是"首帧到了", 而是"画面里已经有真内容":
 *     · s_scene_pct  = 当前 8bpp 帧里暗像素(shade>=2)的占比
 *     · s_scene_hits = 连续多少帧达到阈值(>=6%) —— 连续 3 帧才认, 防抖
 *     · 白屏帧 pct≈0, 标题画面 pct≈90, 分得很开
 *   s_out_paused: 载入期间**不写 fb、不送屏** —— 屏幕整块留给载入页,
 *   游戏帧一帧都不会漏出去闪一下(之前 pal_tick 会送一帧, 白屏就是这么漏出来的)。 */
static volatile int  s_scene_pct   = 0;
static volatile int  s_scene_hits  = 0;
static bool          s_out_paused  = false;

/* 只在 paused 时调用: 从 8bpp 源图便宜地估"暗部占比"(每 2~3 像素取一个) */
static void scene_sample(void)
{
    const int step = (s_sh > 160) ? 3 : 2;
    int dark = 0, tot = 0;
    for (int y = 0; y < s_sh; y += step) {
        const uint8_t *row = s_src + (size_t)y * (size_t)s_sw;
        for (int x = 0; x < s_sw; x += step) {
            if (s_shade[row[x]] >= 2) dark++;
            tot++;
        }
    }
    if (!tot) return;
    s_scene_pct = dark * 100 / tot;
    if (s_scene_pct >= 6) { if (s_scene_hits < 100) s_scene_hits++; pal_port_note_stage(5); }
    else                  s_scene_hits = 0;
}

/* ★★★ 2026-10-25 linit「能不能真实显示进度条」的结论:
 *   仙剑的读盘全在 third_party 的 util.c 里直接 fopen/fread, 不拦 stdio 就拿不到字节数;
 *   能拿到的只有初始化各阶段的完成事件 ⇒ 只做**真实阶段**(下面 5 级), 不按时间瞎猜百分比。
 *   0=启动 1=视频就绪 2=建屏 3=音频就绪 4=首帧 5=首场景(可以进游戏) */
static volatile int s_stage = 0;
static int64_t s_stage_ms[6] = { 0 };       /* 各级到达时刻(相对第一次打点), 用来量真实耗时 */
extern int64_t pal_launch_us(void);
void pal_port_note_stage(int st)
{
    if (st <= s_stage) return;
    s_stage = st;
    if (st >= 0 && st < 6) {
        int64_t now = esp_timer_get_time();
        s_stage_ms[st] = (now - pal_launch_us()) / 1000;
        s_stage_ms[0] = s_stage_ms[0] ? s_stage_ms[0] : 0;
        ESP_LOGI(TAG, "STAGE %d @%dms", st, (int)s_stage_ms[st]);
    }
}
int  pal_port_load_stage(void)   { return s_stage; }

void pal_port_pause_output(bool on) { s_out_paused = on; }
bool pal_port_scene_ready(void)     { return s_scene_hits >= 3; }
int  pal_port_scene_dark_pct(void)  { return s_scene_pct; }
static int64_t  s_last_present_us = 0;

static uint16_t s_xmap[SCR_W];
static uint16_t s_ymap[SCR_H];

/* GBC 同款 2x2 散点: 每种 shade 在 2x2 里放几个黑点
 *   shade 0 → 0 点(纯白)  1 → 1 点  2 → 2 点(对角)  3 → 3 点  4 → 4 点(纯黑)
 * 象限顺序 q = (y&1)<<1 | (x&1) → 0=(0,0) 1=(0,1) 2=(1,0) 3=(1,1)
 * ★ 2026-10-25 linit「游戏整体太白」: 原来只有 0/1/2/4 四档(没有 3 点),
 *   中间调从 50% 黑直接跳到 100% 黑, 亮部又一步到白 ⇒ 观感"要么糊白要么死黑"。
 *   补上 3 点这一档, 中间调才有正常的过渡。 */
static const uint8_t s_dith[5][4] = {
    { 0, 0, 0, 0 },   /* shade 0: 全白 */
    { 1, 0, 0, 0 },   /* shade 1: 25% 黑 */
    { 1, 0, 0, 1 },   /* shade 2: 50% 黑(对角) */
    { 1, 1, 1, 0 },   /* shade 3: 75% 黑 */
    { 1, 1, 1, 1 },   /* shade 4: 全黑 */
};

/* ★ 性能: 把"一对像素 + 行奇偶"压成一次查表 (照 GBC board_shim 的 rowbits 做法)。
 *   内层每 2 个像素只做 1 次查表 + 1 次位与, 比原来两次 if 判断快很多。
 *   mask 里置 1 的位 = 该像素要画黑 (清位)。 */
static uint8_t s_pairclr[2][5][5];   /* [y&1][左shade][右shade] → 需要清零的位 */
static void build_pairclr(void)
{
    for (int qy = 0; qy < 2; qy++)
        for (int a = 0; a < 5; a++)
            for (int b = 0; b < 5; b++) {
                uint8_t m = 0;
                if (s_dith[a][(qy << 1) | 0]) m |= 0x01;   /* 左像素 (由调用方移位) */
                if (s_dith[b][(qy << 1) | 1]) m |= 0x02;   /* 右像素 */
                s_pairclr[qy][a][b] = m;
            }
}

/* ---------------------------------------------------------------- 基础 */

static uint8_t  s_lum[256];            /* 调色板索引 → 亮度 (BT.601) */
static int      s_lo = 0, s_hi = 255;  /* 当前生效的拉伸端点 (稳定值, 不跟淡入淡出跳) */
/* ★ 2026-10-25 linit 实机定档: palgam 52 / palcon 150 / palwhite 149 / palbr 81 / palgray 0 */
static int      s_gain = 81;           /* 额外亮度增益 % (线性微调, 主旋钮是 gamma) */

/* ★★★ 2026-10-25 linit「不要简简单单调高亮度, 而是调整策略」:
 *   1bpp 屏的观感不是"整体亮度"决定的, 而是**哪些区域算白**。
 *   固定线性阈值(51/102/153/204)对仙剑这种偏暗的手绘调色板等于"几乎全黑";
 *   单纯乘增益又不分场合(一亮就死白)。所以改成 **gamma 曲线 + 5 档散点**:
 *     · gamma<1: 把中暗部整体抬起来(该算白的浅色区域真的变白), 黑位和高光不动;
 *     · 5 档散点(0/1/2/3/4 点): 中间调有过渡, 不会 50% 直接跳 100% 黑。
 *   全程**静态曲线**: 不统计画面、不随时间变化 ⇒ 不会忽白忽黑。
 *   现场调: remcon `palgam 60` (越大越暗, 100=线性) / `palbr 110`。 */
static int      s_gamma_x100 = 52;     /* gamma = 0.52 (越小越白) */
/* ★★★ 2026-10-25 linit「继续加强对比度 黑的黑一点 白的白一点」:
 *   gamma 只负责"整体抬高", 抬完以后画面大量像素挤在中间档(50%~75% 黑点) ⇒ 发灰。
 *   再叠一条 **S 曲线**(以 128 为支点做线性扩张): 比中点亮的推得更白、比中点暗的压得更黑,
 *   中间调只保留过渡。和 gamma 一起构成"亮而不灰"的曲线, 仍然**全程静态**, 不忽明忽暗。
 *   现场调: remcon `palcon 180` (100=不扩张, 越大对比越强)。 */
static int      s_contrast_x100 = 150; /* S 曲线强度 (100=关, 200=两倍扩张) */
static int      s_white_pt = 149;      /* 白点: 曲线值 ≥ 它 ⇒ 纯白 (0=关闭) */
static int      s_black_pt = 0;        /* 黑点: 曲线值 ≤ 它 ⇒ 纯黑 (0=关闭) */
static int      s_pivot = 128;         /* 当前 S 曲线支点 (按调色板均值算, 见 rebuild_shade) */
static uint8_t  s_gamma_lut[256];
static bool     s_gamma_ready = false;
static void     rebuild_shade(void);   /* 前向: 改旋钮后立即重建曲线 */

static void build_gamma(void)
{
    const float g = (float)s_gamma_x100 / 100.0f;
    for (int i = 0; i < 256; i++) {
        float x = (float)i / 255.0f;
        float y = powf(x, g) * 255.0f + 0.5f;
        s_gamma_lut[i] = (uint8_t)(y < 0 ? 0 : (y > 255 ? 255 : y));
    }
    s_gamma_ready = true;
}

void pal_port_set_gamma(int x100)
{
    if (x100 < 30)  x100 = 30;
    if (x100 > 100) x100 = 100;
    s_gamma_x100 = x100;
    s_gamma_ready = false;
    rebuild_shade();
    s_dirty = true;
    ESP_LOGI(TAG, "gamma = %.2f", (double)(s_gamma_x100 / 100.0));
}
int pal_port_get_gamma(void) { return s_gamma_x100; }

/* S 曲线强度 (对比度): 100 = 不扩张, 越大黑白越分明 */
void pal_port_set_contrast(int x100)
{
    if (x100 < 100) x100 = 100;
    if (x100 > 300) x100 = 300;
    s_contrast_x100 = x100;
    rebuild_shade();
    s_dirty = true;
    ESP_LOGI(TAG, "对比度 = %d%%", s_contrast_x100);
}
int pal_port_get_contrast(void) { return s_contrast_x100; }

/* 白点: 曲线值 ≥ pt 一律当纯白 (0 = 关闭)。只影响"接近白"的区域, 不动黑位。 */
void pal_port_set_white_point(int pt)
{
    if (pt < 0)   pt = 0;
    if (pt > 255) pt = 255;
    s_white_pt = pt;
    rebuild_shade();
    s_dirty = true;
    ESP_LOGI(TAG, "白点 = %d %s", s_white_pt, s_white_pt ? "" : "(关)");
}
int pal_port_get_white_point(void) { return s_white_pt; }

/* 黑点: 曲线值 ≤ pt 一律当纯黑 (0 = 关闭)。 */
void pal_port_set_black_point(int pt)
{
    if (pt < 0)   pt = 0;
    if (pt > 128) pt = 128;
    s_black_pt = pt;
    rebuild_shade();
    s_dirty = true;
    ESP_LOGI(TAG, "黑点 = %d %s", s_black_pt, s_black_pt ? "" : "(关)");
}
int pal_port_get_black_point(void) { return s_black_pt; }
static int64_t  s_pal_us  = 0;         /* 调色板**内容**最后一次变化的时刻 */
static int64_t  s_tone_us = 0;         /* 最近一次重建色调曲线时刻 */
static uint32_t s_pal_hash = 0;        /* 调色板内容指纹 (区分"真变了"和"每帧重设同一张") */

static uint32_t pal_hash(void)
{
    uint32_t h = 0;
    for (int i = 0; i < 256 * 4; i += 4) {
        uint32_t w;
        memcpy(&w, s_pal + i, 4);
        h = ((h << 1) | (h >> 31)) ^ w;
    }
    return h;
}

static void lum_from_pal(void)
{
    for (int i = 0; i < 256; i++) {
        int r = s_pal[i * 4 + 0], g = s_pal[i * 4 + 1], b = s_pal[i * 4 + 2];
        s_lum[i] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
    }
}

/* 用 s_lum + 当前 s_lo/s_hi + s_gain 生成查询表 s_shade[]。
 * 中间层顺序: 亮度 →(分位拉伸)→(亮度增益)→ 量化 4 级 →(2x2 散点)→ 1bpp
 * ⚠ 不要在这里加"<64 全黑 / >192 全白"的硬阈值 —— 试过, 标题页会整片压成黑底白点。 */
static void rebuild_shade(void)
{
    if (!s_gamma_ready) build_gamma();
    /* ★ S 曲线的支点必须落在"这一屏的中等亮度"上, 不能写死 128 ——
     *   仙剑的调色板整体偏暗, gamma 之后内容大多在 90~140; 支点定在 128 时
     *   一加对比就等于**整体压黑**(实测 palcon 200 变成 79% 全黑、纯白 0%)。
     *   支点改成"该调色板的平均亮度(过 gamma 后)", 加对比才是真正的两头拉开:
     *   比平均暗的压到黑、比平均亮的推到白, 整体亮度基本不变。
     *   只依赖调色板 ⇒ 静态、不随时间变 ⇒ 不会忽明忽暗。 */
    uint32_t sum = 0;
    for (int i = 0; i < 256; i++) sum += s_lum[i];
    int mean = (int)(sum / 256);
    int pivot = s_gamma_lut[mean];
    if (pivot < 48)  pivot = 48;
    if (pivot > 208) pivot = 208;
    s_pivot = pivot;

    const int span = (s_hi > s_lo) ? (s_hi - s_lo) : 255;
    for (int i = 0; i < 256; i++) {
        int v = s_lum[i];
        if (v <= s_lo) v = 0;
        else if (v >= s_hi) v = 255;
        else v = (v - s_lo) * 255 / span;
        v = s_gamma_lut[v];                  /* ★ 主旋钮: gamma 抬中暗部, 决定"哪些区域算白" */
        {                                     /* ★ S 曲线: 以本屏中位亮度为支点拉开明暗 */
            int d = v - pivot;
            d = d * s_contrast_x100 / 100;
            v = pivot + d;
            if (v < 0) v = 0;
            if (v > 255) v = 255;
        }
        if (s_gain != 100) {                 /* 亮度增益(线性微调) */
            v = v * s_gain / 100;
            if (v > 255) v = 255;
        }
        /* ★★★ 2026-10-25 linit「黑的够黑 白的不够白」⇒ 单独加"白点":
         *   曲线抬到白点的亮度一律当纯白(0 个黑点), 不再留 25% 的散点灰。
         *   和黑边完全解耦 —— 黑的还是黑, 只把"接近白"的区域一口气推成纯白。
         *   现场调: remcon `palwhite 170` (0=关闭该功能)。 */
        if (s_white_pt > 0 && v >= s_white_pt) v = 255;
        if (s_black_pt > 0 && v <= s_black_pt) v = 0;
        /* 5 档等分 (0..255 → 5 段), fb 位语义"1=白/0=黑" ⇒ 亮度高 shade 小(黑点少) */
        int lv = (v < 51 ? 0 : v < 102 ? 1 : v < 153 ? 2 : v < 204 ? 3 : 4);
        s_shade[i] = (uint8_t)(4 - lv);
    }
}

/* 按当前画面统计 2%~98% 分位 → 更新 s_lo/s_hi → 重建曲线。
 * ⚠ 只在**画面稳定**时调用 (见 pal_port_present 的门槛), 所以:
 *   · 淡入淡出期间调色板一直在变 ⇒ 不会进来 ⇒ 曲线冻结, 不会和淡出对着干;
 *   · 场景切换/进游戏后画面稳定 ⇒ 拉一次, 把暗部铺满量程 ⇒ 不再"太黑"。
 * 值没变就不重建, 避免每 400ms 白算一次。 */
static void tone_retune(void)
{
    s_tone_us = esp_timer_get_time();
    int lo = 0, hi = 255;
    if (s_src && s_sw > 0 && s_sh > 0) {
        uint32_t hist[64]; memset(hist, 0, sizeof(hist));
        const int step = (s_sh > 200) ? 2 : 1;
        uint32_t n = 0;
        for (int y = 0; y < s_sh; y += step) {
            const uint8_t *row = s_src + (size_t)y * (size_t)s_sw;
            for (int x = 0; x < s_sw; x += step) { hist[s_lum[row[x]] >> 2]++; n++; }
        }
        if (n) {
            const uint32_t lo_t = n * 2 / 100, hi_t = n * 98 / 100;
            uint32_t acc = 0;
            for (int i = 0; i < 64; i++) { acc += hist[i]; if (acc >= lo_t) { lo = i << 2; break; } }
            acc = 0;
            for (int i = 0; i < 64; i++) { acc += hist[i]; if (acc >= hi_t) { hi = ((i + 1) << 2) - 1; break; } }
            if (hi > 255) hi = 255;
            if (hi - lo < 24) { lo = 0; hi = 255; }   /* 几乎纯色: 不拉伸 */
        }
    }
    if (lo == s_lo && hi == s_hi) return;             /* 没变: 不动 */
    s_lo = lo; s_hi = hi;
    rebuild_shade();
    ESP_LOGD(TAG, "色调重算 lo=%d hi=%d gain=%d%%", s_lo, s_hi, s_gain);
}

void pal_port_set_auto_level(bool on) { s_auto = on; s_tone_us = 0; s_dirty = true; }
bool pal_port_get_auto_level(void) { return s_auto; }

/* 亮度增益: 100 = 只做分位拉伸后的原样; 120 = 再整体提亮 20%。
 * 分位拉伸已经把暗部铺满了, 仍然偏暗时再抬这个。 */
void pal_port_set_brightness(int pct)
{
    if (pct < 50)  pct = 50;
    if (pct > 200) pct = 200;
    s_gain = pct;
    rebuild_shade();
    s_dirty = true;
    ESP_LOGI(TAG, "亮度增益 = %d%%", s_gain);
}
int pal_port_get_brightness(void) { return s_gain; }

/* 诊断: 把当前游戏画面(未量化前)的亮度分布打到串口 —— 调曲线不用靠猜。
 * 输出: 各分位亮度 + 4 个量化档位分别占多少像素(按 gain=100 算)。 */
void pal_port_dump_stats(void)
{
    if (!s_src || s_sw <= 0 || s_sh <= 0) { ESP_LOGW(TAG, "stats: 没有画面"); return; }
    uint32_t hist[64]; memset(hist, 0, sizeof(hist));
    uint32_t n = 0;
    for (int y = 0; y < s_sh; y++) {
        const uint8_t *row = s_src + (size_t)y * (size_t)s_sw;
        for (int x = 0; x < s_sw; x++) { hist[s_lum[row[x]] >> 2]++; n++; }
    }
    int pct[5] = { 2, 25, 50, 75, 98 }, pv[5] = { 0 };
    for (int k = 0; k < 5; k++) {
        uint32_t t = n * (uint32_t)pct[k] / 100, acc = 0;
        for (int i = 0; i < 64; i++) { acc += hist[i]; if (acc >= t) { pv[k] = i << 2; break; } }
    }
    /* 按**实际生效的曲线**统计 5 档占比 (s_shade 就是最终查表) */
    uint32_t bucket[5] = { 0 };
    for (int i = 0; i < 64; i++) bucket[4 - s_shade[i << 2]] += hist[i];
    ESP_LOGI(TAG, "PALSTAT n=%u p2=%d p25=%d p50=%d p75=%d p98=%d gamma=%d con=%d pivot=%d auto=%d",
             (unsigned)n, pv[0], pv[1], pv[2], pv[3], pv[4],
             s_gamma_x100, s_contrast_x100, s_pivot, (int)s_auto);
    ESP_LOGI(TAG, "PALSTAT 全黑=%u%% 3点=%u%% 2点=%u%% 1点=%u%% 全白=%u%%  (像素占比)",
             (unsigned)(bucket[0] * 100 / n), (unsigned)(bucket[1] * 100 / n),
             (unsigned)(bucket[2] * 100 / n), (unsigned)(bucket[3] * 100 / n),
             (unsigned)(bucket[4] * 100 / n));
}

/* 诊断: 把当前 8bpp 画面 + 调色板原样 dump 到串口 (base64)。
 * 用途: 抓真机帧回主机做"离线调曲线"(网页模拟器), 再不用靠猜参数。 */
static void b64_out(const uint8_t *p, size_t n)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[77];
    int  k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n) v |= (uint32_t)p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        line[k++] = T[(v >> 18) & 63];
        line[k++] = T[(v >> 12) & 63];
        line[k++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        line[k++] = (i + 2 < n) ? T[v & 63] : '=';
        if (k >= 76) { line[k] = 0; printf("%s\n", line); k = 0; }
    }
    if (k) { line[k] = 0; printf("%s\n", line); }
}

void pal_port_dump_frame(void)
{
    if (!s_src || s_sw <= 0 || s_sh <= 0) { ESP_LOGW(TAG, "frame: 没有画面"); return; }
    printf("PALFRAME %d %d %d\n", s_sw, s_sh, 0);
    b64_out(s_src, (size_t)s_sw * (size_t)s_sh);   /* 索引 */
    b64_out(s_pal, 256 * 4);                        /* 调色板 r,g,b,x */
    printf("PALFRAME_END\n");
    fflush(stdout);
}

/* 载入进度条用: 首帧是否已经上屏 */
bool pal_port_first_frame_done(void) { return s_frames_done > 0; }
uint32_t pal_port_frame_count(void) { return s_frames_done; }

void pal_port_set_max_fps(int fps) {
    s_max_fps = (fps <= 0) ? 0 : (fps > 60 ? 60 : fps);
    ESP_LOGI(TAG, "帧率上限 = %d fps", s_max_fps);
}
int pal_port_get_max_fps(void) { return s_max_fps; }

/* ---------------------------------------------------------------- 对外 */

void pal_port_set_source(const uint8_t *pixels, int w, int h, const uint8_t *palette)
{
    s_src = pixels;
    s_sw  = w;
    s_sh  = h;
    if (palette) memcpy(s_pal, palette, sizeof(s_pal));
    static bool pairclr_ready = false;
    if (!pairclr_ready) { build_pairclr(); pairclr_ready = true; }
    lum_from_pal();
    s_pal_us = esp_timer_get_time();
    s_tone_us = 0;              /* 新场景: 允许立刻按这一屏重定曲线 */
    s_scene_hits = 0;           /* 新一轮: 重新判"画面里有没有真内容" */
    s_scene_pct  = 0;
    pal_port_note_stage(2);     /* 建屏完成 */
    tone_retune();              /* 场景起点就是稳定画面, 直接算一次(不必等 200ms) */
    s_dirty = true;
    ESP_LOGI(TAG, "source %dx%d (中间层: GBC 4级+2x2)", w, h);
}

void pal_port_set_palette(const uint8_t *palette)
{
    if (!palette) return;
    memcpy(s_pal, palette, sizeof(s_pal));
    lum_from_pal();
    /* ★ 只有**内容真的变了**才刷新"变化时刻"。SDL_SetPalette 在仙剑里可能每帧都
     *   被调用(同一张表), 按时间戳判断的话"画面稳定"永远不成立 ⇒ 永远不重算 ⇒ 太黑。
     *   用内容指纹才能区分: 每帧重设同一张表 = 稳定; 淡入淡出 = 每帧内容都在变。 */
    uint32_t h = pal_hash();
    if (h != s_pal_hash) { s_pal_hash = h; s_pal_us = esp_timer_get_time(); }
    /* ⚠ 这里**不**重算 s_lo/s_hi —— 淡入淡出/闪光就是靠改调色板做的,
     *   跟着重算就会和淡出对着干(忽白忽黑)。只用新亮度重建曲线, 端点保持不变;
     *   等调色板 200ms 不再变化后, 由 present 里的门限补一次重算。 */
    rebuild_shade();
    s_dirty = true;
}

void pal_port_mark_dirty(void) { s_dirty = true; }
void pal_port_set_zoom_pct(int pct) { if (pct >= 0 && pct <= 200) { s_zoom = pct; s_dirty = true; } }
int  pal_port_get_zoom_pct(void) { return s_zoom; }

void pal_port_present(void *lcd_handle)
{
    if (!lcd_handle) return;
    /* 载入阶段: 只采样画面内容, 不写 fb、不送屏 (屏幕留给载入页) */
    if (s_out_paused) {
        if (s_src && s_sw > 0 && s_sh > 0) scene_sample();
        s_dirty = false;
        return;
    }
    if (!s_dirty || !s_src || !s_sw || !s_sh) return;
    /* ★ 单独给仙剑限帧 (见 s_max_fps 注释): 距上一帧不足就**跳过本轮**,
     *   保留 s_dirty —— 游戏缓冲是原地更新的, 下一次过来直接画最新内容,
     *   中间那些帧丢掉无所谓(面板只有 16~24Hz, 本来也显示不出来)。 */
    if (s_max_fps > 0) {
        const int64_t now = esp_timer_get_time();
        const int64_t min_us = 1000000LL / s_max_fps;
        if (s_last_present_us && (now - s_last_present_us) < min_us) return;
        s_last_present_us = now;
    }
    /* ★ 色调曲线: 只有"画面稳定"时才重算 —— 调色板 200ms 没动 + 距上次重算 400ms。
     *   淡入淡出/闪烁效果期间调色板每一帧都在变 ⇒ 永远进不来 ⇒ 曲线冻结,
     *   画面该暗就暗(和电影一样); 场景稳定后补一次, 暗部才不会被压成一片黑。 */
    if (s_auto) {
        const int64_t now = esp_timer_get_time();
        if ((now - s_pal_us) > 200000 && (now - s_tone_us) > 400000) tone_retune();
    }
    s_dirty = false;

    st7305_handle_t *lcd = (st7305_handle_t *)lcd_handle;
    if (!lcd->fb) return;

    const int64_t t0 = esp_timer_get_time();

    /* 目标尺寸 */
    int dw, dh, ox, oy;
    if (s_zoom == 0) {
        dw = SCR_W; dh = SCR_H; ox = 0; oy = 0;              /* 全屏铺满 */
    } else {
        dw = s_sw * s_zoom / 100;
        dh = s_sh * s_zoom / 100;
        if (dw > SCR_W) { dw = SCR_W; dh = s_sh * dw / s_sw; }
        if (dh > SCR_H) { dh = SCR_H; dw = s_sw * dh / s_sh; }
        if (dw <= 0 || dh <= 0) return;
        ox = (SCR_W - dw) / 2;
        oy = (SCR_H - dh) / 2;
    }

    static int map_dw = -1, map_dh = -1, map_sw = -1, map_sh = -1;
    if (dw != map_dw || dh != map_dh || s_sw != map_sw || s_sh != map_sh) {
        for (int x = 0; x < dw; x++) s_xmap[x] = (uint16_t)(x * s_sw / dw);
        for (int y = 0; y < dh; y++) s_ymap[y] = (uint16_t)(y * s_sh / dh);
        map_dw = dw; map_dh = dh; map_sw = s_sw; map_sh = s_sh;
    }

    /* ★★★ 2026-10-25 linit「画面整体动一下白一些、动一下黑一些」的根因:
     *   原来**每帧**都在这里调 refresh_shade(), 而它内部按"2%~98% 分位"重新拉伸
     *   亮度 —— 画面一变, 统计就变, 整屏对比度跟着跳 ⇒ 一帧偏白一帧偏黑。
     *   现在只在**调色板/画面尺寸变化**时更新(见 set_palette / set_source),
     *   播放过程中固定不动, 就不会再闪。 */
    uint8_t *fb = lcd->fb;
    memset(fb, 0xFF, (size_t)SCR_W * SCR_H / 8);           /* 全白 */

    const int ytop = oy;
    const int ybot = oy + dh - 1;

    /* 按字节列（2 屏幕列）遍历：同一列的 75 字节在内存里连续，写起来最省 */
    for (int c = 0; c < SCR_W / 2; c++) {
        const int x0 = c * 2, x1 = x0 + 1;
        if (x0 < ox || x0 >= ox + dw) continue;            /* 非全屏时左右留白 */
        const int sx0 = s_xmap[x0 - ox];
        const int sx1 = s_xmap[x1 - ox];
        uint8_t *col = fb + (size_t)c * COL_BYTES;

        const int qx0 = x0 & 1, qx1 = x1 & 1;     /* 2x2 象限的 x 分位 */
        for (int by = 0; by < COL_BYTES; by++) {
            uint8_t b = 0xFF;
            for (int k = 0; k < 4; k++) {
                const int sy = SCR_H - 1 - (by * 4 + k);   /* 屏幕 y（布局上下翻转） */
                if (sy < ytop || sy > ybot) continue;      /* 留白区保持白 */
                const uint8_t *row = s_src + (size_t)s_ymap[sy - oy] * (size_t)s_sw;
                /* GBC 同款: 一对像素只查一次表 → 得到要清零的两位 */
                const uint8_t clr = s_pairclr[sy & 1][s_shade[row[sx0]]][s_shade[row[sx1]]];
                /* 位布局: 左像素在 bit(7-k*2), 右像素在 bit(6-k*2) */
                b &= (uint8_t)~((clr & 1) ? (1u << (7 - (k << 1))) : 0u);
                b &= (uint8_t)~((clr & 2) ? (1u << (6 - (k << 1))) : 0u);
            }
            col[by] = b;
        }
    }

    /* ★ 系统虚拟按键画在游戏帧之上（框线式，不挡画面），然后一起 flush */
    if (virtual_keys_is_enabled()) virtual_keys_draw(lcd);
    engine_overlay_quickbar(lcd);   /* ★ 游戏内下拉快捷栏 (画在游戏帧上, flush 前) */

    /* 送屏失败 = 这一帧没上屏。连续失败到一定程度就打一行, 免得
     * "画面不动但后台正常"时无从判断是画错了还是根本没送出去。 */
    {
        static int fail = 0;
        if (st7305_flush_from(lcd, fb) != ESP_OK) {
            if (++fail == 20 || (fail % 200) == 0)
                ESP_LOGW(TAG, "连续 %d 帧送屏失败 (LCD 忙/超时)", fail);
        } else {
            fail = 0;
        }
    }

    s_frames_done++;                       /* 已真正上屏的帧数 */
    pal_port_note_stage(4);                /* 首帧已上屏 */
    static uint32_t cnt = 0;
    static int64_t  acc = 0;
    acc += esp_timer_get_time() - t0;
    if ((++cnt % 60) == 0) {
        ESP_LOGI(TAG, "present avg=%dus (%dx%d %s)",
                 (int)(acc / 60), dw, dh, s_zoom ? "zoom" : "fullscreen");
        acc = 0;
    }
}
