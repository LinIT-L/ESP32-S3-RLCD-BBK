/* vid_tune.c — 画面优化器 (实现说明见 include/vid_tune.h) */
#include "vid_tune.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "input.h"
#include "svc_nvs.h"
#include "ui_common.h"

#define TAG "VIDTUNE"

/* ---------------- 参数与存取 ---------------- */
typedef struct { char id[8]; uint8_t p[VT_PARAM_N]; } vt_prof_t;
typedef struct {
    uint8_t  en;          /* 引擎优化里的总开关 */
    uint8_t  n;
    vt_prof_t e[8];
} vt_nvs_t;

#define VT_NVS_NS  "lintos"
#define VT_NVS_KEY "vidtune"

static vt_nvs_t s_store;
static vt_prof_t *s_cur = NULL;   /* 当前引擎档 (指向 s_store.e[i]); 非 const: 保存时要写回 */
static char     s_cur_id[8];
static uint8_t  s_p[VT_PARAM_N];        /* 工作副本 (取消要还原) */
static uint8_t  s_dots[4];              /* shade → 黑点数 */
static void   (*s_rebuild_cb)(void) = NULL;
static void vt_rebuild(void);   /* 前向: init 里要用 */

/* 默认档: 亮度 100 / 对比 120 / 白点 208 / 黑点 0 */
static const uint8_t s_def[VT_PARAM_N] = { 100, 120, 208, 0 };

static void store_flush(void)
{
    nvs_handle_t h;
    if (svc_nvs_open(VT_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        svc_nvs_set_blob(h, VT_NVS_KEY, &s_store, sizeof(s_store));
        svc_nvs_commit(h);
        svc_nvs_close(h);
    }
}

void vid_tune_init(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    memset(&s_store, 0, sizeof(s_store));
    nvs_handle_t h;
    if (svc_nvs_open(VT_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_store);
        if (svc_nvs_get_blob(h, VT_NVS_KEY, &s_store, &len) != ESP_OK) memset(&s_store, 0, sizeof(s_store));
        svc_nvs_close(h);
    }
    if (s_store.n > 8) s_store.n = 0;
    memcpy(s_p, s_def, sizeof(s_p));
    /* 启动时只用默认值; 具体参数由各引擎 start 时 vid_tune_attach("<id>") 载入,
     * 这样"不同引擎各自保存"成立: 没存过的引擎拿默认, 存过的拿自己那份。 */
    snprintf(s_cur_id, sizeof(s_cur_id), "all");
    vt_rebuild();
    ESP_LOGI(TAG, "init: en=%d 档数=%d id=%s", s_store.en, s_store.n, s_cur_id);
}

bool vid_tune_enabled(void) { vid_tune_init(); return s_store.en != 0; }
void vid_tune_set_enabled(bool on)
{
    vid_tune_init();
    s_store.en = on ? 1 : 0;
    store_flush();
    ESP_LOGI(TAG, "画面优化器 %s", on ? "开" : "关");
}

/* ---------------- 曲线 ---------------- */
static uint8_t vt_curve_one(int shade)
{
    /* shade 0=最白 … 3=最黑。先换成"亮度" 255..0，再套 对比/增益/白点/黑点。 */
    int v = (3 - shade) * 85;
    v = 128 + (v - 128) * (int)s_p[VT_CON] / 100;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    v = v * (int)s_p[VT_GAIN] / 100;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    if (s_p[VT_WHITE] && v >= s_p[VT_WHITE]) v = 255;   /* 白点: 够亮就是纯白 */
    if (s_p[VT_BLACK] && v <= s_p[VT_BLACK]) v = 0;     /* 黑点: 够暗就是纯黑 */
    int lv = (v < 51) ? 0 : (v < 102) ? 1 : (v < 153) ? 2 : (v < 204) ? 3 : 4;
    return (uint8_t)(4 - lv);
}

static void vt_rebuild(void)
{
    for (int s = 0; s < 4; s++) s_dots[s] = vt_curve_one(s);
    /* 必须单调: shade 越大黑点只能越多, 否则画面会出反向斑 */
    for (int s = 1; s < 4; s++) if (s_dots[s] < s_dots[s - 1]) s_dots[s] = s_dots[s - 1];
    if (s_rebuild_cb) s_rebuild_cb();
}

void vid_tune_get_dots(uint8_t dots[4]) { memcpy(dots, s_dots, 4); }
void vid_tune_set_rebuild_cb(void (*cb)(void)) { s_rebuild_cb = cb; if (cb) vt_rebuild(); }
void vid_tune_refresh(void) { vt_rebuild(); }

/* ---------------- 引擎档 ---------------- */
void vid_tune_attach(const char *engine_id)
{
    vid_tune_init();
    const char *id = engine_id ? engine_id : "all";
    snprintf(s_cur_id, sizeof(s_cur_id), "%s", id);
    s_cur = NULL;
    for (int i = 0; i < s_store.n; i++)
        if (strncmp(s_store.e[i].id, s_cur_id, sizeof(s_cur_id)) == 0) { s_cur = &s_store.e[i]; break; }
    if (s_cur) memcpy(s_p, s_cur->p, sizeof(s_p));
    else       memcpy(s_p, s_def, sizeof(s_p));
    vt_rebuild();
    ESP_LOGI(TAG, "attach %s (已有档=%d) 亮度%d 对比%d 白%d 黑%d dots=%d/%d/%d/%d",
             s_cur_id, s_cur != NULL, s_p[0], s_p[1], s_p[2], s_p[3],
             s_dots[0], s_dots[1], s_dots[2], s_dots[3]);
}

void vid_tune_detach(void) { s_cur = NULL; }

/* ---------------- 绘图小工具 ---------------- */
static void px(st7305_handle_t *lcd, int x, int y, int black)
{
    if (x < 0 || y < 0 || x >= ST7305_WIDTH || y >= ST7305_HEIGHT) return;
    st7305_draw_pixel(lcd, x, y, black ? ST7305_COLOR_BLACK : ST7305_COLOR_WHITE);
}
static void fill(st7305_handle_t *lcd, int x, int y, int w, int h, int black)
{
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(lcd, x + i, y + j, black);
}
static void frame(st7305_handle_t *lcd, int x, int y, int w, int h)
{
    for (int i = 0; i < w; i++) { px(lcd, x + i, y, 1); px(lcd, x + i, y + h - 1, 1); }
    for (int j = 0; j < h; j++) { px(lcd, x, y + j, 1); px(lcd, x + w - 1, y + j, 1); }
}

/* ---------------- 面板几何 ---------------- */
#define VT_ICON_SZ   28                     /* 2026-10-29 linit: 14→28, 宽高各放大一倍 */
#define VT_ICON_X    (ST7305_WIDTH - VT_ICON_SZ - 3)
#define VT_ICON_Y    3

#define VT_PANEL_H   96
#define VT_BTN_W     46
#define VT_BTN_H     18
#define VT_BTN_Y     3
#define VT_TRACK_Y0  34                      /* 滑条轨道上端 */
#define VT_TRACK_Y1  (VT_PANEL_H - 8)        /* 下端 */
#define VT_SLOT_X0   62
#define VT_SLOT_DX   70                      /* 4 条滑条间距 (62/132/202/272) */

static bool s_panel_open = false;
static int  s_drag = -1;                     /* 正在拖的滑杆序号 (-1 无) */
static bool s_prev_down = false;
static bool s_press_in_icon = false;
static uint8_t s_backup[VT_PARAM_N];

bool vid_tune_is_panel_open(void) { return s_panel_open; }

/* 由快捷栏等外部入口调用: 打开调节面板 (面板仍由引擎帧内的 overlay_tick 绘制) */
void vid_tune_open_panel(void) { if (vid_tune_enabled()) s_panel_open = true; }

/* ===== 2026-10-29: 给快捷栏的展开态提供参数读写 =====
 * 语义: pct 统一 0..100, 内部按各参数实际范围线性映射
 *   GAIN 60..140 / CON 100..200 / WHITE,BLACK 0..255 */
static float param_norm(int i);          /* 定义在后 */
static void  param_set_norm(int i, float t);

int vid_tune_get_pct(int i)
{
    if (i < 0 || i >= VT_PARAM_N) return 0;
    float t = param_norm(i);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return (int)(t * 100.0f + 0.5f);
}

void vid_tune_set_pct(int i, int pct)
{
    if (i < 0 || i >= VT_PARAM_N) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    param_set_norm(i, pct / 100.0f);
    vt_rebuild();                       /* 立即生效: 重建抖动表 */
}

void vid_tune_save(void)
{
    if (!s_cur) {                       /* 首次保存: 建新档 */
        if (s_store.n < 8) {
            vt_prof_t *e = &s_store.e[s_store.n++];
            memset(e, 0, sizeof(*e));
            snprintf(e->id, sizeof(e->id), "%s", s_cur_id);
            s_cur = e;
        }
    }
    if (s_cur) memcpy(s_cur->p, s_p, sizeof(s_p));
    store_flush();
    ESP_LOGI(TAG, "保存 %s: 亮%d 对%d 白%d 黑%d", s_cur_id, s_p[0], s_p[1], s_p[2], s_p[3]);
}

void vid_tune_restore(void)
{
    memcpy(s_p, s_backup, sizeof(s_p));
    vt_rebuild();
    ESP_LOGI(TAG, "取消 (还原)");
}

/* 展开调节前先备份当前参数, 供"取消"还原 */
void vid_tune_begin_edit(void) { memcpy(s_backup, s_p, sizeof(s_p)); }


static int slot_x(int i) { return VT_SLOT_X0 + i * VT_SLOT_DX; }

/* 参数 → 0..1 (用于画球) */
static float param_norm(int i)
{
    switch (i) {
    case VT_GAIN:  return (s_p[i] - 60) / 80.0f;
    case VT_CON:   return (s_p[i] - 100) / 100.0f;
    default:       return s_p[i] / 255.0f;
    }
}
static void param_set_norm(int i, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    if (i == VT_GAIN)      s_p[i] = (uint8_t)(60 + t * 80 + 0.5f);
    else if (i == VT_CON)  s_p[i] = (uint8_t)(100 + t * 100 + 0.5f);
    else                   s_p[i] = (uint8_t)(t * 255 + 0.5f);
}

static void draw_panel(st7305_handle_t *lcd)
{
    fill(lcd, 0, 0, ST7305_WIDTH, VT_PANEL_H, 0);            /* 白底 */
    for (int x = 0; x < ST7305_WIDTH; x++) px(lcd, x, VT_PANEL_H - 1, 1);   /* 下边线 */

    frame(lcd, 4, VT_BTN_Y, VT_BTN_W, VT_BTN_H);
    ui_text16c(lcd, 4 + VT_BTN_W / 2, VT_BTN_Y + 1, "\xe5\x8f\x96\xe6\xb6\x88", false);   /* 取消 */
    frame(lcd, ST7305_WIDTH - 4 - VT_BTN_W, VT_BTN_Y, VT_BTN_W, VT_BTN_H);
    ui_text16c(lcd, ST7305_WIDTH - 4 - VT_BTN_W / 2, VT_BTN_Y + 1, "\xe4\xbf\x9d\xe5\xad\x98", false); /* 保存 */

    static const char *const names[VT_PARAM_N] = { "亮", "对", "白", "黑" };
    for (int i = 0; i < VT_PARAM_N; i++) {
        int cx = slot_x(i);
        /* 轨道: 3px 宽细条 (比 MP3 音量条更瘦) */
        fill(lcd, cx - 1, VT_TRACK_Y0, 3, VT_TRACK_Y1 - VT_TRACK_Y0, 1);
        /* 已选段 (从下往上) */
        int ky = VT_TRACK_Y1 - (int)(param_norm(i) * (VT_TRACK_Y1 - VT_TRACK_Y0));
        fill(lcd, cx - 3, ky, 7, VT_TRACK_Y1 - ky, 1);
        /* 球: 7x7 方块 (瘦版) */
        fill(lcd, cx - 3, ky - 3, 7, 7, 1);
        fill(lcd, cx - 2, ky - 2, 5, 5, 0);
        /* 标签 */
        ui_text16c(lcd, cx, VT_TRACK_Y0 - 18, names[i], false);
    }
}

static void draw_icon(st7305_handle_t *lcd)
{
    /* 白底黑框 + 三根滑条 (画出来的, 不占资源); 尺寸随 VT_ICON_SZ 等比放大 */
    fill(lcd, VT_ICON_X, VT_ICON_Y, VT_ICON_SZ, VT_ICON_SZ, 0);
    frame(lcd, VT_ICON_X, VT_ICON_Y, VT_ICON_SZ, VT_ICON_SZ);
    for (int k = 0; k < 3; k++) {
        int x = VT_ICON_X + 6 + k * 8;
        fill(lcd, x, VT_ICON_Y + 6, 2, VT_ICON_SZ - 12, 1);      /* 轨道 */
        fill(lcd, x - 2, VT_ICON_Y + 8 + k * 6, 6, 4, 1);        /* 滑柄 */
    }
}

/* ---------------- 浮标 / 面板 主循环 ---------------- */
bool vid_tune_overlay_tick(st7305_handle_t *lcd)
{
    if (!lcd) return false;
    if (!vid_tune_enabled()) { s_panel_open = false; return false; }

    int x = 0, y = 0;
    bool down = input_get_touch_pos(&x, &y);
    bool press = down && !s_prev_down;    /* 按下沿 */
    bool release = !down && s_prev_down;  /* 抬起沿 */
    s_prev_down = down;

    if (!s_panel_open) {
        /* ★ 2026-10-29 linit: 入口已移到快捷栏 (Wi-Fi 右侧), 这里不再画右上角浮标,
         *   否则会出现两个入口。面板仍然由快捷栏调用 vid_tune_open_panel() 打开。 */
        if (press) {
            s_press_in_icon = (x >= VT_ICON_X && x < VT_ICON_X + VT_ICON_SZ &&
                               y >= VT_ICON_Y && y < VT_ICON_Y + VT_ICON_SZ);
        }
        if (release) {
            bool hit = s_press_in_icon && (x >= VT_ICON_X - 4 && x < VT_ICON_X + VT_ICON_SZ + 4 &&
                                           y >= VT_ICON_Y - 4 && y < VT_ICON_Y + VT_ICON_SZ + 4);
            s_press_in_icon = false;
            if (hit) {
                memcpy(s_backup, s_p, sizeof(s_p));   /* 取消要还原 */
                s_panel_open = true;
                s_drag = -1;
                draw_panel(lcd);
                return true;
            }
        }
        return false;
    }

    /* ---- 面板打开中 ---- */
    draw_panel(lcd);

    if (press) {
        /* 左键 = 取消(还原) / 右键 = 保存(写 NVS) */
        if (y < VT_BTN_Y + VT_BTN_H) {
            if (x < 4 + VT_BTN_W) {                       /* 取消 */
                memcpy(s_p, s_backup, sizeof(s_p));
                s_panel_open = false;
                vt_rebuild();
                ESP_LOGI(TAG, "取消 (还原)");
            } else if (x >= ST7305_WIDTH - 4 - VT_BTN_W) { /* 保存 */
                if (!s_cur) {                              /* 首次保存: 建一个新档 */
                    if (s_store.n < 8) {
                        vt_prof_t *e = &s_store.e[s_store.n++];
                        memset(e, 0, sizeof(*e));
                        snprintf(e->id, sizeof(e->id), "%s", s_cur_id);
                        s_cur = e;
                    }
                }
                if (s_cur) memcpy(s_cur->p, s_p, sizeof(s_p));
                store_flush();
                s_panel_open = false;
                ESP_LOGI(TAG, "保存 %s: 亮%d 对%d 白%d 黑%d", s_cur_id,
                         s_p[0], s_p[1], s_p[2], s_p[3]);
            }
            s_drag = -1;
            draw_panel(lcd);
            return true;
        }
        /* 选中最接近的那条滑条 */
        int best = -1, bestd = 24;
        for (int i = 0; i < VT_PARAM_N; i++) {
            int d = x - slot_x(i); if (d < 0) d = -d;
            if (d < bestd) { bestd = d; best = i; }
        }
        s_drag = best;
    }

    if (down && s_drag >= 0 && y >= VT_TRACK_Y0 - 6) {
        float t = 1.0f - (float)(y - VT_TRACK_Y0) / (float)(VT_TRACK_Y1 - VT_TRACK_Y0);
        param_set_norm(s_drag, t);
        vt_rebuild();          /* 实时生效 */
        draw_panel(lcd);
    }
    if (release) s_drag = -1;
    return true;
}
