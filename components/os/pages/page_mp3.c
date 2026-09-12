/**
 * page_mp3.c — MP3 播放器 页面模块 (P3 迁移).
 *
 * 从 menu_system.c 的 render_mp3_player / mp3_* 迁移:
 *   - 左: 播放列表 (中文 24px 每行, 当前歌【】标记)
 *   - 右: 旋转光盘 (播放时旋转)
 *   - 底部: 循环模式按钮(单/列/随, 24x24) + 全宽进度条
 *   - 按键: 上下=选曲, 确认=播放/暂停/切歌, 左右=上一/下一曲,
 *           左键长按=切循环模式, 返回=停止退出
 *   - 触摸: 列表=点歌 / 光盘=播放暂停 / 进度条=点击跳转(seek) / 循环按钮=切模式
 *   - 播完按循环模式自动切歌 (单曲=重播本曲 / 列表=下一首 / 随机=随机一首)
 *   - 私有 state 全部 static 留本文件.
 *
 * 注: 文件列表用 opendir 直读 /sdcard/mp3 (POSIX), 不依赖 menu 组件.
 */
#include "os.h"
#include "ui_common.h"
#include "audio_player.h"
#include "vibrator.h"       /* V1.4.x: 随音乐震动 */
#include "input.h"          /* input_get_touch_multi: 按钮"按下即触发" */
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "mp3_icons_auto.h" /* V1.5.x: 完美图标/音乐 5 组无框图标 (32x32) */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <strings.h>
#include <stdlib.h>

#define TAG "MP3"

#define MP3_DIR         "/sdcard/mp3"
#define MAX_MP3_FILES   64
#define MP3_FNAME_LEN   128

/* ============ 私有状态 ============ */
EXT_RAM_BSS_ATTR static char (*s_files)[MP3_FNAME_LEN] = NULL;   /* 播放列表 (PSRAM) */
static int  s_count   = 0;
static int  s_current = 0;
static int  s_sel     = 0;         /* 列表选中 */
static bool s_in_player = false;   /* 已在播放 */
static int  s_last_cd_angle = 0;
static bool s_scanned = false;

/* V1.5.x: 音量竖滑条状态 — 点击音量图标弹出竖条, 拖动圆点调音量.
 * s_vol_slider 仅在触碰滑条时置位 (render 绘制 + touch 拖动共用) */
static bool     s_vol_slider  = false;
static bool     s_vol_drag    = false;   /* 正在拖动圆点 */
static int      s_vol_drag_y  = 0;       /* 拖动起点 y 基准 (防单点误触改音量) */
static uint32_t s_vol_slider_close_ms = 0;   /* 滑条弹出时刻 (自动收起用, 见下) */

static uint32_t s_last_scan_ms = 0;
static int     s_logged_count  = -1;

/* 循环模式: 0=单曲循环 1=列表循环 2=随机 (按钮点击 / 左键长按 切换) */
#define MP3_LOOP_SINGLE 0
#define MP3_LOOP_LIST   1
#define MP3_LOOP_RANDOM 2
#define MP3_LOOP_N      3
static int  s_loop_mode = MP3_LOOP_LIST;

/* V1.4.x: 随音乐震动开关 (NVS mp3/vib_music 持久化, 首次渲染懒加载) */
static bool s_vib_music = false;
static bool s_vib_music_loaded = false;
/* 循环模式图标由完美图标素材提供 (mp3ic_loop_*, 见 mp3_icons_auto.h) */
static const char *s_loop_toast[MP3_LOOP_N] = {
    "\xe5\x8d\x95\xe6\x9b\xb2\xe5\xbe\xaa\xe7\x8e\xaf",   /* 单曲循环 */
    "\xe5\x88\x97\xe8\xa1\xa8\xe5\xbe\xaa\xe7\x8e\xaf",   /* 列表循环 */
    "\xe9\x9a\x8f\xe6\x9c\xba\xe6\x92\xad\xe6\x94\xbe",   /* 随机播放 */
};
static uint32_t s_loop_tip_t0 = 0;   /* 废弃: 循环切换提示改用 os_dialog_toast 标准模板 */
/* 进度条几何 (render / touch 共用) */
#define MP3_BAR_X1          (UI_SCREEN_W - 15)
#define MP3_BAR_PAD_BOTTOM  10
#define MP3_BAR_H           8
#define MP3_BAR_Y           (UI_SCREEN_H - MP3_BAR_PAD_BOTTOM - MP3_BAR_H)
#define MP3_BAR_X0          10   /* 进度条恢复全宽 (底部不再被按钮占用) */
/* ==== V1.5.x: 光盘几何 (仅光盘, 已删除频谱视图) ==== */
#define MP3_CD_CX   296           /* 光盘中心 x */
#define MP3_CD_CY   130           /* 光盘中心 y */
#define MP3_CD_R    92            /* 光盘半径 */
/* ==== 光盘正下方一排 5 个无框图标按钮: 音量 / 上曲 / 播放暂停 / 下曲 / 循环 ==== */
#define MP3_BTN_SZ   32           /* 图标显示尺寸 (无框, 直接用素材位图) */
#define MP3_BTN_CY   (MP3_CD_CY + MP3_CD_R + 28)   /* 250 */
#define MP3_BTN_X0   212          /* 音量 */
#define MP3_BTN_X1   254          /* 上一首 */
#define MP3_BTN_X2   296          /* 播放/暂停 */
#define MP3_BTN_X3   338          /* 下一首 */
#define MP3_BTN_X4   380          /* 循环模式 */
#define MP3_BTN_GAP  8            /* 相邻图标中心间隔 */
/* 音量横向苹果药丸 (V2.x): 图标上方横向胶囊 + 中央圆点拖动调音量.
 * 读数左边小(音量0%)→右边大(音量100%), 圆点随音量左右移动. */
#define MP3_VOL_X0        205           /* 胶囊左端 x (留出左栏列表) */
#define MP3_VOL_X1        300           /* 胶囊右端 x */
#define MP3_VOL_CY        (MP3_BTN_CY - 58)   /* 188: 音量图标上方 */
#define MP3_VOL_H         22            /* 胶囊厚度 (药丸高度) */
#define MP3_VOL_DOT_R     10            /* 中央圆点半径 */
#define MP3_VOL_TOUCH     32            /* 触摸热区左右宽度 */
/* 播放列表: 底部止于进度条上方 (留 4px) */
#define MP3_LIST_BOTTOM  (MP3_BAR_Y - 4)

/* 屏幕 x -> 进度千分比 (0..1000) */
static int mp3_x_to_progress(int x)
{
    int x0 = MP3_BAR_X0, x1 = MP3_BAR_X1;
    if (x1 <= x0) return 0;
    int p = (x - x0) * 1000 / (x1 - x0);
    return (p < 0) ? 0 : (p > 1000 ? 1000 : p);
}
/* 命中: 进度条 (上下各扩 12px 热区, 便于手指点中) */
static bool mp3_in_bar_zone(int x, int y)
{
    return (x >= MP3_BAR_X0 - 4 && x <= MP3_BAR_X1 + 4 &&
            y >= MP3_BAR_Y - 12 && y <= MP3_BAR_Y + MP3_BAR_H + 12);
}
/* 切换循环模式: 单曲 -> 列表 -> 随机 -> 单曲 */
static void mp3_loop_mode_next(void) { s_loop_mode = (s_loop_mode + 1) % MP3_LOOP_N; }

/* 按循环模式计算下一首索引 */
static int mp3_next_index(void) {
    if (s_count <= 1) return s_current;
    if (s_loop_mode == MP3_LOOP_SINGLE) return s_current;   /* 单曲循环: 重播本曲 */
    if (s_loop_mode == MP3_LOOP_RANDOM) {
        int n = s_current;
        while (n == s_current) n = rand() % s_count;        /* 随机: 避免连播同一首 */
        return n;
    }
    return (s_current + 1) % s_count;                       /* 列表循环: 下一首 */
}

static void mp3_scan_files(void) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_scanned && s_count > 0) return;
    /* count==0 时 SD 可能重新挂载: 限频重扫 (1Hz), 避免每帧 opendir 刷爆 IO/日志 */
    if (s_count == 0 && (now - s_last_scan_ms) < 1000) return;
    s_last_scan_ms = now;
    if (!s_files) {
        s_files = heap_caps_malloc(MAX_MP3_FILES * MP3_FNAME_LEN, MALLOC_CAP_SPIRAM);
        if (!s_files) s_files = malloc(MAX_MP3_FILES * MP3_FNAME_LEN);
        if (!s_files) { ESP_LOGE(TAG, "MP3 列表内存不足"); return; }
    }
    s_count = 0;
    DIR *dir = opendir(MP3_DIR);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && s_count < MAX_MP3_FILES) {
            const char *n = ent->d_name;
            if (n[0] == '.' && (n[1] == '_' || strcmp(n, ".DS_Store") == 0)) continue;
            const char *dot = strrchr(n, '.');
            if (dot && (strcasecmp(dot, ".mp3") == 0)) {
                memcpy(s_files[s_count], n, MP3_FNAME_LEN - 1);
                s_files[s_count][MP3_FNAME_LEN - 1] = '\0';
                s_count++;
            }
        }
        closedir(dir);
    }
    /* 空结果不缓存: TF 卡重新挂载后会再次重扫 (受上面 1Hz 限频保护);
     * 修复: 挂载U盘卸载后回音乐页, 之前 count=0 导致永不重扫 → "没找到TF卡". */
    s_scanned = (s_count > 0);
    if (s_count != s_logged_count) {
        ESP_LOGI(TAG, "MP3 扫描: %d 个文件", s_count);
        s_logged_count = s_count;
    }
}

/* 旋转光盘 (从 menu_system.c draw_cd 移植) */
static void mp3_draw_cd(st7305_handle_t *lcd, int cx, int cy, int r, int angle_deg) {
    const int SCREEN_W = UI_SCREEN_W, SCREEN_H = UI_SCREEN_H;
    for (int y = -r; y <= r; y++) {
        int half_w = (int)sqrtf((float)(r * r - y * y));
        int yy = cy + y;
        if (yy < 0 || yy >= SCREEN_H) continue;
        int x0 = cx - half_w, x1 = cx + half_w;
        if (x0 < 0) x0 = 0;
        if (x1 >= SCREEN_W) x1 = SCREEN_W - 1;
        for (int x = x0; x <= x1; x++) st7305_draw_pixel(lcd, x, yy, ST7305_COLOR_BLACK);
    }
    int inner_r = 28;
    for (int y = -inner_r; y <= inner_r; y++) {
        int half_w = (int)sqrtf((float)(inner_r * inner_r - y * y));
        int yy = cy + y;
        if (yy < 0 || yy >= SCREEN_H) continue;
        int x0 = cx - half_w, x1 = cx + half_w;
        if (x0 < 0) x0 = 0;
        if (x1 >= SCREEN_W) x1 = SCREEN_W - 1;
        for (int x = x0; x <= x1; x++) st7305_draw_pixel(lcd, x, yy, ST7305_COLOR_WHITE);
    }
    int center_ring_r = 14;
    for (int a = 0; a < 360; a++) {
        float rad = (float)a * 3.14159f / 180.0f;
        int px = cx + (int)(center_ring_r * cosf(rad));
        int py = cy + (int)(center_ring_r * sinf(rad));
        if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H)
            st7305_draw_pixel(lcd, px, py, ST7305_COLOR_BLACK);
    }
    int inner_ring_r = 10;
    for (int a = 0; a < 360; a++) {
        float rad = (float)a * 3.14159f / 180.0f;
        int px = cx + (int)(inner_ring_r * cosf(rad));
        int py = cy + (int)(inner_ring_r * sinf(rad));
        if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H)
            st7305_draw_pixel(lcd, px, py, ST7305_COLOR_BLACK);
    }
    for (int a = 0; a < 360; a++) {
        int hash = (a * 73 + 37) % 181;
        if (hash < 50) {
            int dot_r = 15 + (hash % 13);
            float rad = (float)(a + angle_deg) * 3.14159f / 180.0f;
            int px = cx + (int)(dot_r * cosf(rad));
            int py = cy + (int)(dot_r * sinf(rad));
            if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H)
                st7305_draw_pixel(lcd, px, py, ST7305_COLOR_BLACK);
        }
    }
    for (int i = 0; i < 8; i++) {
        int deg = (angle_deg + i * 45) % 360;
        float rad = (float)deg * 3.14159f / 180.0f;
        float c = cosf(rad), s = sinf(rad);
        int x1 = cx + (int)((inner_r + 10) * c);
        int y1 = cy + (int)((inner_r + 10) * s);
        int x2 = cx + (int)((r - 4) * c);
        int y2 = cy + (int)((r - 4) * s);
        int steps = abs(x2 - x1) + abs(y2 - y1);
        if (steps < 1) steps = 1;
        for (int s2 = 0; s2 <= steps; s2++) {
            int px = x1 + (x2 - x1) * s2 / steps;
            int py = y1 + (y2 - y1) * s2 / steps;
            for (int w = -1; w <= 1; w++) {
                int wx = px + (int)(-s * w);
                int wy = py + (int)(c * w);
                if (wx >= 0 && wx < SCREEN_W && wy >= 0 && wy < SCREEN_H)
                    st7305_draw_pixel(lcd, wx, wy, ST7305_COLOR_WHITE);
            }
        }
    }
    for (int a = 0; a < 360; a++) {
        float rad = (float)a * 3.14159f / 180.0f;
        int px = cx + (int)(r * cosf(rad));
        int py = cy + (int)(r * sinf(rad));
        if (px >= 0 && px < SCREEN_W && py >= 0 && py < SCREEN_H)
            st7305_draw_pixel(lcd, px, py, ST7305_COLOR_WHITE);
    }
}

static int mp3_play_at(int idx) {
    if (idx < 0 || idx >= s_count) return -1;
    s_current = idx;
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", MP3_DIR, s_files[idx]);
    int ret = audio_player_play(path);
    if (ret == 0) {
        s_in_player = true;
    } else {
        ESP_LOGE(TAG, "播放失败: %s", path);
    }
    return ret;
}

/* 列表滚动偏移: 保证高亮行始终可见 (render 与 touch 共用, 避免两套计算漂移) */
static int mp3_list_scroll(int highlight_idx, int max_visible) {
    int scroll = 0;
    if (highlight_idx >= max_visible - 1) scroll = highlight_idx - max_visible + 2;
    if (scroll < 0) scroll = 0;
    return scroll;
}

/* 通用 1bpp 位图绘制 (无框纯图标, 位=1 黑). mirror=true 时水平翻转(用于"下一首"图标).
 * 素材 mp3ic_prev/mp3ic_next 方向相同, next 需翻转后才符合"向右=下一首"直觉. */
static void mp3_blit_1bpp_color(st7305_handle_t *lcd, int x0, int y0,
                                int w, int h, const uint8_t *src, bool mirror, uint8_t color); /* 前向声明 */

/* 实心圆 (音量滑条圆点用) */
static void mp3_fill_circle(st7305_handle_t *lcd, int cx, int cy, int r, st7305_color_t c) {
    for (int y = -r; y <= r; y++) {
        int yy = cy + y;
        if (yy < 0 || yy >= UI_SCREEN_H) continue;
        int half = (int)sqrtf((float)(r * r - y * y));
        int x0 = cx - half, x1 = cx + half;
        if (x0 < 0) x0 = 0;
        if (x1 >= UI_SCREEN_W) x1 = UI_SCREEN_W - 1;
        for (int x = x0; x <= x1; x++) st7305_draw_pixel(lcd, x, yy, c);
    }
}

/* 无框图标: 在 (cx,cy) 中心按素材原始尺寸 1:1 绘制 (1=黑), 不做缩放/不画背景框.
 * 各图标尺寸不同, 用各自的 W/H 宏 (见 mp3_icons_auto.h). mirror=true 水平翻转. */
static void mp3_draw_icon(st7305_handle_t *lcd, int cx, int cy,
                          int w, int h, const uint8_t *icon, bool mirror)
{
    mp3_blit_1bpp_color(lcd, cx - w / 2, cy - h / 2, w, h, icon, mirror, ST7305_COLOR_BLACK);
}
/* 循环模式对应图标 (单曲/列表/随机) 及其尺寸 */
static const uint8_t *mp3_loop_icon_bitmap(int mode, int *w, int *h)
{
    switch (mode) {
    case MP3_LOOP_SINGLE: *w = mp3ic_loop_single_W; *h = mp3ic_loop_single_H; return mp3ic_loop_single;
    case MP3_LOOP_RANDOM: *w = mp3ic_loop_random_W; *h = mp3ic_loop_random_H; return mp3ic_loop_random;
    default:              *w = mp3ic_loop_list_W;   *h = mp3ic_loop_list_H;   return mp3ic_loop_list;
    }
}
/* 播放/暂停图标 (原素材 48x48) */
static const uint8_t *mp3_playpause_bitmap(void)
{
    return (audio_player_get_state() == AUDIO_STATE_PLAYING) ? mp3ic_pause : mp3ic_play;
}
/* 光盘下方 5 个无框图标: 音量 / 上曲 / 播放暂停 / 下曲 / 循环.
 * 各图标按素材原始尺寸 1:1 绘制 (不缩放), 以各自按钮中心 (按钮行以光盘中心居中). */
static void mp3_draw_control_icons(st7305_handle_t *lcd)
{
    int w, h;
    mp3_draw_icon(lcd, MP3_BTN_X0, MP3_BTN_CY, mp3ic_vol_W, mp3ic_vol_H, mp3ic_vol, false);
    mp3_draw_icon(lcd, MP3_BTN_X1, MP3_BTN_CY, mp3ic_prev_W, mp3ic_prev_H, mp3ic_prev, false);
    mp3_draw_icon(lcd, MP3_BTN_X2, MP3_BTN_CY, mp3ic_play_W, mp3ic_play_H, mp3_playpause_bitmap(), false);
    mp3_draw_icon(lcd, MP3_BTN_X3, MP3_BTN_CY, mp3ic_next_W, mp3ic_next_H, mp3ic_next, true);   /* 下一首: 水平翻转 */
    { const uint8_t *ic = mp3_loop_icon_bitmap(s_loop_mode, &w, &h);
      mp3_draw_icon(lcd, MP3_BTN_X4, MP3_BTN_CY, w, h, ic, false); }
}

/* ==== 音量横向苹果药丸 ==== */
/* 当前音量 → 圆点 x 坐标 (0..100 → X0..X1) */
static int mp3_vol_slider_dot_x(void)
{
    int vol = audio_player_get_volume();
    int range = MP3_VOL_X1 - MP3_VOL_X0;
    return MP3_VOL_X0 + (range * vol / 100);
}
/* x 坐标 → 音量 (0..100) */
static int mp3_slider_x_to_vol(int x)
{
    int range = MP3_VOL_X1 - MP3_VOL_X0;
    int t = x - MP3_VOL_X0;
    int v = (range <= 0) ? 100 : (t * 100 / range);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}
/* 命中药丸 (圆点附近左右 TOUCH, 上下 ±H, 便于按住拖动) */
static bool mp3_in_vol_slider(int x, int y)
{
    int dx = x - mp3_vol_slider_dot_x();
    int dy = y - MP3_VOL_CY;
    if (dx < -MP3_VOL_TOUCH || dx > MP3_VOL_TOUCH) return false;
    if (dy < -MP3_VOL_H || dy > MP3_VOL_H) return false;
    return true;
}
/* 绘制音量横向苹果药丸: 白底圆角胶囊 + 中央圆点.
 * 垫白底遮住底下旋转光盘; 胶囊 = 圆角矩形 (两端半圆 + 中段矩形); 圆点随音量移动. */
static void mp3_draw_vol_slider(st7305_handle_t *lcd)
{
    /* 先垫一块白底 (胶囊区外扩), 盖住底下旋转光盘, 保证胶囊/圆点纯净 */
    fill_rect(lcd, MP3_VOL_X0 - MP3_VOL_DOT_R - 4, MP3_VOL_CY - MP3_VOL_H - 4,
              MP3_VOL_X1 + MP3_VOL_DOT_R + 4, MP3_VOL_CY + MP3_VOL_H + 4, ST7305_COLOR_WHITE);
    /* 胶囊描边 (黑) */
    int rr = MP3_VOL_H / 2;   /* 圆角半径 */
    draw_rect_outline(lcd, MP3_VOL_X0, MP3_VOL_CY - rr, MP3_VOL_X1, MP3_VOL_CY + rr, ST7305_COLOR_BLACK);
    /* 两端半圆 */
    for (int a = 0; a < 360; a++) {
        float rad = (float)a * 3.14159f / 180.0f;
        int px = MP3_VOL_X0 + (int)(rr * cosf(rad));
        int py = MP3_VOL_CY + (int)(rr * sinf(rad));
        if (px >= 0 && px < UI_SCREEN_W && py >= 0 && py < UI_SCREEN_H)
            st7305_draw_pixel(lcd, px, py, ST7305_COLOR_BLACK);
    }
    for (int a = 0; a < 360; a++) {
        float rad = (float)a * 3.14159f / 180.0f;
        int px = MP3_VOL_X1 + (int)(rr * cosf(rad));
        int py = MP3_VOL_CY + (int)(rr * sinf(rad));
        if (px >= 0 && px < UI_SCREEN_W && py >= 0 && py < UI_SCREEN_H)
            st7305_draw_pixel(lcd, px, py, ST7305_COLOR_BLACK);
    }
    /* 中央圆点 (白边 + 黑芯), 位置=当前音量 */
    int dx = mp3_vol_slider_dot_x();
    mp3_fill_circle(lcd, dx, MP3_VOL_CY, MP3_VOL_DOT_R + 2, ST7305_COLOR_WHITE);
    mp3_fill_circle(lcd, dx, MP3_VOL_CY, MP3_VOL_DOT_R, ST7305_COLOR_BLACK);
}

/* ============ V1.4.x: 随音乐震动 (保留能力, 由 NVS 控制; 图标按钮已从 UI 移除) ============ */
#define MP3_VIB_ICON_W 20
#define MP3_VIB_ICON_H 20
static const uint8_t s_icon_vib[MP3_VIB_ICON_H * 3] = {
    0x00, 0x00, 0x00,
    0x00, 0x60, 0x00,
    0x00, 0xF0, 0x00,
    0x01, 0x98, 0x00,
    0x02, 0x04, 0x00,
    0x06, 0x06, 0x00,
    0x08, 0x02, 0x00,
    0x18, 0x03, 0x00,
    0x20, 0x00, 0x80,
    0x60, 0x00, 0xC0,
    0x20, 0x00, 0x80,
    0x18, 0x03, 0x00,
    0x08, 0x02, 0x00,
    0x06, 0x06, 0x00,
    0x02, 0x04, 0x00,
    0x01, 0x98, 0x00,
    0x00, 0xF0, 0x00,
    0x00, 0x60, 0x00,
    0x00, 0x00, 0x00,
    0x00, 0x00, 0x00,
};
/* 1bpp 位图画到 (x0,y0), 支持指定颜色 (1bpp 屏反白显示用).
 * mirror=true 时水平镜像 (dx 读源列 w-1-dx), 用于"下一首"图标素材翻转. */
static void mp3_blit_1bpp_color(st7305_handle_t *lcd, int x0, int y0,
                                int w, int h, const uint8_t *src, bool mirror, uint8_t color) {
    int bpr = (w + 7) >> 3;
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            int sx = mirror ? (w - 1 - dx) : dx;
            if (src[dy * bpr + (sx >> 3)] & (0x80u >> (sx & 7)))
                st7305_draw_pixel(lcd, x0 + dx, y0 + dy, color);
        }
    }
}
/* 切换随音乐震动: 应用 + 存 NVS + toast (按钮已从 UI 移除, 保留接口供 NVS/复启用) */
static void mp3_vib_toggle(ui_ctx_t *ctx)
{
    s_vib_music = !s_vib_music;
    vibrator_music_mode(s_vib_music);
    nvs_handle_t h;
    if (nvs_open("mp3", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "vib_music", s_vib_music ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    os_dialog_toast(ctx, s_vib_music ? "\xe9\x9a\x8f\xe9\x9f\xb3\xe4\xb9\x90\xe9\x9c\x87\xe5\x8a\xa8\xef\xbc\x9a\xe5\xbc\x80"   /* 随音乐震动：开 */
                                     : "\xe9\x9a\x8f\xe9\x9f\xb3\xe4\xb9\x90\xe9\x9c\x87\xe5\x8a\xa8\xef\xbc\x9a\xe5\x85\xb3"); /* 随音乐震动：关 */
    ctx->needs_redraw = true;
}
/* 首次渲染懒加载 NVS 开关值 */
static void mp3_vib_load(void)
{
    if (s_vib_music_loaded) return;
    s_vib_music_loaded = true;
    nvs_handle_t h;
    if (nvs_open("mp3", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "vib_music", &v) == ESP_OK && v) {
            s_vib_music = true;
            vibrator_music_mode(true);
        }
        nvs_close(h);
    }
}

/* ============ 渲染 ============ */
static void p_mp3_render(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    const int SCREEN_W = UI_SCREEN_W;

    mp3_scan_files();
    mp3_vib_load();           /* V1.4.x: 首次渲染懒加载"随音乐震动"开关 */
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    audio_state_t astate = audio_player_get_state();
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    int progress = audio_player_get_progress();

    /* 循环切换提示已改用 os_dialog_toast 标准模板 (1s 自动关, 任意键/触摸关) */

    int highlight_idx = s_in_player ? s_current : s_sel;
    if (highlight_idx < 0) highlight_idx = 0;

    /* === 左菜单: 播放列表 === */
    int list_x = 0, list_w = 195, list_y = 28;
    int list_h = MP3_LIST_BOTTOM - list_y;   /* 底部让给循环模式按钮 */
    int line_h = 26;
    int max_visible = list_h / line_h;
    int scroll = mp3_list_scroll(highlight_idx, max_visible);

    for (int i = 0; i < s_count && i < max_visible; i++) {
        int idx = i + scroll;
        if (idx >= s_count) break;
        int yy = list_y + 2 + i * line_h;
        const char *name = s_files[idx];
        char disp[64];
        int nl = strlen(name);
        if (nl > 4 && name[nl - 4] == '.') nl -= 4;
        if (nl > 30) nl = 30;
        strncpy(disp, name, nl);
        disp[nl] = '\0';
        /* 按可用宽度裁剪, 防止长文件名溢出到右侧光盘 */
        {
            int avail_hl = (list_w - 8) - 24;   /* 预留【】标记空间 */
            int tw = text_width(disp);
            while (tw > avail_hl && nl > 1) { disp[--nl] = '\0'; tw = text_width(disp); }
        }

        int list_content_w = list_w - 8;
        if (idx == highlight_idx) {
            /* 当前歌: 【】选中标记 */
            int cur_tw = text_width(disp);
            int tx = list_x + (list_w - cur_tw) / 2;
            if (tx < list_x + 2) tx = list_x + 2;
            int glyph_h = 24, glyph_w = 10;
            int top = yy, bot = yy + glyph_h - 1;
            int left_x = tx - glyph_w - 4, right_x = tx + cur_tw + 4;
            for (int d = 0; d < 2; d++) {
                int bx = left_x + d;
                for (int yy2 = top; yy2 <= bot; yy2++)
                    if (bx >= 0) st7305_draw_pixel(lcd, bx, yy2, ST7305_COLOR_BLACK);
                for (int xx = bx; xx <= bx + 6 && xx < SCREEN_W; xx++)
                    st7305_draw_pixel(lcd, xx, top + d, ST7305_COLOR_BLACK);
                for (int xx = bx; xx <= bx + 6 && xx < SCREEN_W; xx++)
                    st7305_draw_pixel(lcd, xx, bot - d, ST7305_COLOR_BLACK);
            }
            for (int d = 0; d < 2; d++) {
                int bx = right_x + glyph_w - 1 - d;
                for (int yy2 = top; yy2 <= bot; yy2++)
                    if (bx < SCREEN_W) st7305_draw_pixel(lcd, bx, yy2, ST7305_COLOR_BLACK);
                for (int xx = bx - 6; xx <= bx; xx++)
                    if (xx >= 0) st7305_draw_pixel(lcd, xx, top + d, ST7305_COLOR_BLACK);
                for (int xx = bx - 6; xx <= bx; xx++)
                    if (xx >= 0) st7305_draw_pixel(lcd, xx, bot - d, ST7305_COLOR_BLACK);
            }
            draw_text(lcd, tx, yy + 2, disp, false);
        } else {
            int avail = list_content_w - 4;
            char truncated[16];
            int pos = 0, tw = 0;
            for (int si = 0; disp[si] && tw + 16 <= avail && pos < 15; ) {
                uint8_t c = (uint8_t)disp[si];
                if (c < 0x80) { tw += 16; truncated[pos++] = disp[si]; si++; }
                else if ((c & 0xF0) == 0xE0) {
                    if (tw + 24 <= avail) {
                        truncated[pos++] = disp[si++]; truncated[pos++] = disp[si++]; truncated[pos++] = disp[si++];
                        tw += 24;
                    } else break;
                } else si++;
            }
            truncated[pos] = '\0';
            int item_text_w = text_width(truncated);
            int item_text_x = list_x + (list_w - item_text_w) / 2;
            draw_text(lcd, item_text_x, yy + 2, truncated, false);
        }
    }

    if (s_count == 0) {
        /* 提示放左栏空白区 (居中), 避免被右侧光盘挡住 */
        int msg_cx = list_x + list_w / 2;           /* 左栏水平中心 195/2≈97 */
        draw_text(lcd, msg_cx - text_width("\xe6\x9c\xaa\xe6\x89\xbe\xe5\x88\xb0 MP3 \xe6\x96\x87\xe4\xbb\xb6") / 2, 100,
                  "\xe6\x9c\xaa\xe6\x89\xbe\xe5\x88\xb0 MP3 \xe6\x96\x87\xe4\xbb\xb6", false); /* 未找到 MP3 文件 */
        draw_text(lcd, msg_cx - text_width("\xe8\xaf\xb7\xe6\x94\xbe\xe5\x85\xa5 MP3\xe6\x96\x87\xe4\xbb\xb6") / 2, 126,
                  "\xe8\xaf\xb7\xe6\x94\xbe\xe5\x85\xa5 MP3\xe6\x96\x87\xe4\xbb\xb6", false);  /* 请放入 MP3文件 */
    }

    /* === 右侧: 光盘 + 底部控件 (几何统一取宏) === */
    int cd_cx = MP3_CD_CX, cd_cy = MP3_CD_CY, cd_r = MP3_CD_R;
    int angle = 0;
    if (astate == AUDIO_STATE_PLAYING) {
        angle = (int)((float)now_ms / 30.0f) % 360;
    } else if (astate == AUDIO_STATE_PAUSED) {
        angle = s_last_cd_angle;
    } else {
        angle = (int)((float)now_ms / 300.0f) % 360;
    }
    s_last_cd_angle = angle;
    mp3_draw_cd(lcd, cd_cx, cd_cy, cd_r, angle);

    /* === 底部: 5 个无框控制图标 + 进度条 === */
    int bar_y = MP3_BAR_Y, bar_h = MP3_BAR_H;
    int bar_x0 = MP3_BAR_X0, bar_x1 = MP3_BAR_X1;
    int bar_w = bar_x1 - bar_x0 + 1;
    int played_w = bar_w * progress / 1000;
    fill_rect(lcd, bar_x0, bar_y, bar_x0 + played_w - 1, bar_y + bar_h - 1, ST7305_COLOR_BLACK);
    fill_rect(lcd, bar_x0 + played_w, bar_y, bar_x1, bar_y + bar_h - 1, ST7305_COLOR_WHITE);
    fill_rect(lcd, bar_x0, bar_y, bar_x1, bar_y, ST7305_COLOR_BLACK);
    fill_rect(lcd, bar_x0, bar_y + bar_h - 1, bar_x1, bar_y + bar_h - 1, ST7305_COLOR_BLACK);
    fill_rect(lcd, bar_x0, bar_y, bar_x0, bar_y + bar_h - 1, ST7305_COLOR_BLACK);
    fill_rect(lcd, bar_x1, bar_y, bar_x1, bar_y + bar_h - 1, ST7305_COLOR_BLACK);
    /* 音量滑条 (若已弹出) 优先绘制在图标之前避免被覆盖 */
    if (s_vol_slider) mp3_draw_vol_slider(lcd);
    mp3_draw_control_icons(lcd);   /* 音量 / 上曲 / 播放暂停 / 下曲 / 循环 */
}

/* ============ 按键 ============ */
/* MP3 退出两键确认: 后台(退页音乐续播) / 退出(停音乐退页).
 * 复用 os_dialog 小确认模板 (no_footer+small): 弹窗内非按钮区=忽略(不关闭),
 * 返回键=-1=取消(弹窗自动关, 留在播放器); 手柄 LEFT/RIGHT 切换, CONFIRM=当前选中.
 * audio_player 解码任务是独立的, 离开 MP3 页不会自动停 → "后台"天然成立. */
static void mp3_exit_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result == 0) {            /* 后台: 退出页面, 音乐继续播 */
        os_dialog_clear_all(ctx);
        os_pop(ctx);
    } else if (result == 1) {     /* 退出: 停音乐 + 退页 */
        audio_player_stop();
        os_dialog_clear_all(ctx);
        os_pop(ctx);
    }
    /* result==-1 (返回键/弹窗外取消): 不操作, dlg_finish_top 自动关弹窗留在播放器 */
}
static void mp3_exit_confirm(ui_ctx_t *ctx) {
    audio_state_t st = audio_player_get_state();
    /* 仅正在播放才弹 后台/退出 两键确认; 暂停或空闲(列表/播完)直接退出, 不弹无意义弹窗.
     * 用户偏好: 没在播放音乐时返回就直接走, 不必确认. */
    if (st == AUDIO_STATE_PLAYING) {
        os_dlg_stack_t dlg;
        memset(&dlg, 0, sizeof(dlg));
        snprintf(dlg.title, sizeof(dlg.title), "\xe9\x80\x80\xe5\x87\xba\xe9\x9f\xb3\xe4\xb9\x90?"); /* 退出音乐? */
        snprintf(dlg.items[0], sizeof(dlg.items[0]), "\xe5\x90\x8e\xe5\x8f\xb0");  /* 后台 */
        snprintf(dlg.items[1], sizeof(dlg.items[1]), "\xe9\x80\x80\xe5\x87\xba");  /* 退出 */
        dlg.count = 2;
        dlg.sel = 0;              /* 默认选中"后台" (不打断播放) */
        dlg.cb = mp3_exit_cb;
        dlg.ud = NULL;
        dlg.no_footer = true;     /* 小确认模板: 无底部"返回"行 */
        dlg.small = true;         /* 小自适应确认弹窗 */
        os_dialog_push(ctx, &dlg);
    } else {
        os_pop(ctx);
    }
}

static void p_mp3_action(ui_ctx_t *ctx, os_action_t a)
{
    switch (a) {
    case OS_ACTION_UP:
        if (s_count == 0) break;
        s_sel = (s_sel - 1 + s_count) % s_count;
        if (!s_in_player) s_current = s_sel;
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_DOWN:
        if (s_count == 0) break;
        s_sel = (s_sel + 1) % s_count;
        if (!s_in_player) s_current = s_sel;
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_LEFT:
        if (s_in_player && s_count > 0) {
            audio_player_stop();
            mp3_play_at((s_current - 1 + s_count) % s_count);
            s_sel = s_current;   /* 同步选中位, 避免确认键跳回旧选择 */
            ctx->needs_redraw = true;
        }
        break;
    case OS_ACTION_RIGHT:
        if (s_in_player && s_count > 0) {
            audio_player_stop();
            mp3_play_at((s_current + 1) % s_count);
            s_sel = s_current;   /* 同步选中位, 避免确认键跳回旧选择 */
            ctx->needs_redraw = true;
        }
        break;
    case OS_ACTION_LONG_LEFT:
        /* 左键长按 500ms: 循环模式 单曲 -> 列表 -> 随机 -> ...
         * 注: input 层在"按下瞬间"就发射 OS_ACTION_LEFT(上一曲), 500ms 后才补发
         * LONG_LEFT, 故长按会顺带切到上一曲; 想无副作用请用进度条左侧的按钮点击. */
        mp3_loop_mode_next();
        os_dialog_toast(ctx, s_loop_toast[s_loop_mode]);   /* 标准提示模板 (1s 自动关) */
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_CONFIRM:
        if (s_count == 0) break;
        if (s_sel != s_current || !s_in_player) {
            mp3_play_at(s_sel);          /* 选新歌 */
        } else {
            audio_state_t st = audio_player_get_state();
            if (st == AUDIO_STATE_PLAYING) audio_player_pause();
            else if (st == AUDIO_STATE_PAUSED) audio_player_resume();
            else mp3_play_at(s_sel);
        }
        ctx->needs_redraw = true;
        break;
    case OS_ACTION_BACK:
        mp3_exit_confirm(ctx);   /* 三选: 取消(留页)/后台(续播)/关闭(停乐).
                                    HOME 由 os_core 直接 os_pop_to_main (音乐转后台), 此处不处理 */
        break;
    default:
        break;
    }
}

/* ============ 每帧轮询: 播放中持续重绘 + 音量滑条拖动 ============ */
/* 图标点击命中: 以按钮为中心 ±24px 见方 (图标 32 + 余量) */
static bool mp3_in_icon(int x, int y, int cx)
{
    int h = 24;
    return (x >= cx - h && x <= cx + h &&
            y >= MP3_BTN_CY - h && y <= MP3_BTN_CY + h);
}
/* 弹出/收起 音量滑条 */
static void mp3_vol_slider_toggle(ui_ctx_t *ctx)
{
    s_vol_slider = !s_vol_slider;
    s_vol_drag = false;
    s_vol_slider_close_ms = 0;
    ctx->needs_redraw = true;
}
/* 按下音量大图标 (touchez 调用) */
static void mp3_vol_icon_press(ui_ctx_t *ctx)
{
    mp3_vol_slider_toggle(ctx);
}
static void p_mp3_poll(ui_ctx_t *ctx)
{
    audio_state_t st = audio_player_get_state();

    /* 音量滑条拖动: 只有"真拖动"(位移 ≥4px)才改音量.
     * 修复: 点开滑条/单点触碰到滑条不应立刻把音量清成 0 (误触"一碰就哑"). */
    if (s_vol_slider) {
        tp_point_t pts[TP_MAX_POINTS];
        int n = 0;
        input_get_touch_multi(pts, &n);
        for (int i = 0; i < n; i++) {
            if (!pts[i].pressed) continue;
            int px = pts[i].x, py = pts[i].y;
            if (mp3_in_vol_slider(px, py)) {
                int ddx = px - s_vol_drag_y;
                if (!s_vol_drag) {
                    s_vol_drag = true;      /* 记录拖动起始基准, 本帧不改音量 */
                    s_vol_drag_y = px;
                } else if (ddx > 4 || ddx < -4) {  /* 真正拖动才实时调音量 */
                    audio_player_set_volume(mp3_slider_x_to_vol(px));
                    s_vol_drag_y = px;
                    ctx->needs_redraw = true;
                }
                break;
            }
        }
    }

    /* 自然播完 → 按循环模式自动切下一首.
     * 单曲=重播本曲 / 列表=下一首 / 随机=随机一首 */
    if (audio_player_track_ended()) {
        if (s_count > 0) {
            int nxt = mp3_next_index();
            audio_player_stop();
            mp3_play_at(nxt);
            s_sel = nxt;
        }
        ctx->needs_redraw = true;
    }

    if (st == AUDIO_STATE_PLAYING || s_vol_slider) {
        ctx->needs_redraw = true;   /* 播放/滑条打开时每帧重绘 */
    }
}

/* ============ 触摸: 5 个控制图标 / 滑条 / 进度条 / 列表 / 光盘 ============ */
static bool p_mp3_touch(ui_ctx_t *ctx, int x, int y)
{
    const int SCREEN_W = UI_SCREEN_W;
    const int list_x = 0, list_w = 195, list_y = 28, line_h = 26;

    /* 音量滑条区 (已打开): 点滑条圆点附近=拖动 (poll 持续调), 点图标区域外空白=收起 */
    if (s_vol_slider) {
        if (mp3_in_vol_slider(x, y)) {
            /* 只在按下时记录拖动起点, 不改音量; 实时调音量交给 poll 的位移门控 */
            if (!s_vol_drag) { s_vol_drag = true; s_vol_drag_y = x; }
            ctx->needs_redraw = true;
            return true;
        }
        /* 点击非滑条区域 → 收起滑条 (回到正常交互) */
        if (!mp3_in_icon(x, y, MP3_BTN_X0)) {
            s_vol_slider = false;
            s_vol_drag = false;
            ctx->needs_redraw = true;
        }
    }

    /* 播放控制图标 (光盘正下方一排): 优先级最高 */
    if (mp3_in_icon(x, y, MP3_BTN_X0)) {   /* 音量: 弹出/收起滑条 */
        mp3_vol_slider_toggle(ctx);
        return true;
    }
    if (mp3_in_icon(x, y, MP3_BTN_X1)) {   /* 上一首 */
        if (s_count > 0) {
            audio_player_stop();
            mp3_play_at((s_current - 1 + s_count) % s_count);
            s_sel = s_current;
            ctx->needs_redraw = true;
        }
        return true;
    }
    if (mp3_in_icon(x, y, MP3_BTN_X2)) {   /* 播放/暂停 */
        audio_state_t st = audio_player_get_state();
        if (st == AUDIO_STATE_PLAYING) audio_player_pause();
        else if (st == AUDIO_STATE_PAUSED) audio_player_resume();
        else if (s_count > 0) { if (!s_in_player) s_current = s_sel; mp3_play_at(s_current); }
        ctx->needs_redraw = true;
        return true;
    }
    if (mp3_in_icon(x, y, MP3_BTN_X3)) {   /* 下一首 */
        if (s_count > 0) {
            audio_player_stop();
            mp3_play_at((s_current + 1) % s_count);
            s_sel = s_current;
            ctx->needs_redraw = true;
        }
        return true;
    }
    if (mp3_in_icon(x, y, MP3_BTN_X4)) {   /* 循环模式 */
        mp3_loop_mode_next();
        os_dialog_toast(ctx, s_loop_toast[s_loop_mode]);   /* 标准提示模板 (1s 自动关) */
        ctx->needs_redraw = true;
        return true;
    }

    if (mp3_in_bar_zone(x, y)) {            /* 进度条: 点击跳转 (seek) */
        int p = mp3_x_to_progress(x);
        audio_state_t st = audio_player_get_state();
        if (st == AUDIO_STATE_PLAYING || st == AUDIO_STATE_PAUSED) {
            audio_player_seek_to(p);
        } else if (s_count > 0) {
            if (!s_in_player) s_current = s_sel;
            if (mp3_play_at(s_current) == 0) audio_player_seek_to(p);
        }
        ctx->needs_redraw = true;
        return true;
    }

    /* 左侧列表: 点击行 → 选中并播放 */
    if (s_count > 0 && x >= list_x && x < list_x + list_w && y >= list_y) {
        int rel = y - (list_y + 2);
        if (rel >= 0) {
            int row = rel / line_h;
            int highlight_idx = s_in_player ? s_current : s_sel;
            if (highlight_idx < 0) highlight_idx = 0;
            int max_visible = (MP3_LIST_BOTTOM - list_y) / line_h;
            int scroll = mp3_list_scroll(highlight_idx, max_visible);
            int idx = row + scroll;
            if (idx >= 0 && idx < s_count) {
                s_sel = idx;
                mp3_play_at(idx);
                ctx->needs_redraw = true;
                return true;
            }
        }
        return false;
    }

    /* 右侧大光盘: 播放/暂停切换 */
    int cxc = MP3_CD_CX, cyc = MP3_CD_CY, rr = MP3_CD_R;
    int dx = x - cxc, dy = y - cyc;
    if (dx * dx + dy * dy > rr * rr) return false;
    audio_state_t st = audio_player_get_state();
    if (st == AUDIO_STATE_PLAYING) {
        audio_player_pause();
    } else if (st == AUDIO_STATE_PAUSED) {
        audio_player_resume();
    } else if (s_count > 0) {
        if (!s_in_player) s_current = s_sel;
        mp3_play_at(s_current);
    }
    ctx->needs_redraw = true;
    return true;
}

/* ============ 模块契约 ============ */
static const os_module_t s_mod_mp3 = {
    .name      = "mp3",
    .page_id   = OS_PAGE_MP3_PLAYER,
    .render    = p_mp3_render,
    .action    = p_mp3_action,
    .poll      = p_mp3_poll,
    .touch     = p_mp3_touch,
    .fullscreen = false,
};

void os_page_mp3_register(void) { os_register(&s_mod_mp3); }
