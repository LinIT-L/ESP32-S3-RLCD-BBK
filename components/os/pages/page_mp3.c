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
#include "vibrator.h"       /* V1.4.x: 随音乐震动开关 */
#include "input.h"          /* input_get_touch_multi: 循环按钮"按下即触发" */
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "nvs_flash.h"
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
/* 循环模式按钮: 三枚 20x20 1bpp 图标 (MSB-first, 位=1 黑) 替代文字.
 * 图标后续可随时更换 (用户保留替换权): 替换下方位图数组即可.
 * 切换提示 (toast) 仍用文字 */
static const uint8_t s_icon_loop_single[60] = {
    0x00, 0x00, 0x00,
    0x01, 0xF8, 0x00,
    0x07, 0xFE, 0x00,
    0x0E, 0x07, 0x00,
    0x1C, 0x03, 0x80,
    0x38, 0x01, 0xC0,
    0x30, 0x00, 0xC0,
    0x60, 0xF0, 0x60,
    0x60, 0xF0, 0x60,
    0x60, 0xF0, 0x60,
    0x60, 0x30, 0x60,
    0x60, 0x30, 0x60,
    0x60, 0x30, 0x60,
    0x30, 0x30, 0xC0,
    0x38, 0x31, 0xC0,
    0x1C, 0x1B, 0x80,
    0x0E, 0x07, 0x00,
    0x07, 0xFE, 0x00,
    0x01, 0xF8, 0x00,
    0x00, 0x00, 0x00
};
static const uint8_t s_icon_loop_list[60] = {
    0x00, 0x00, 0x00,
    0x00, 0x90, 0x00,
    0x03, 0x0C, 0x00,
    0x0F, 0x0F, 0x00,
    0x06, 0x06, 0x00,
    0x3A, 0x05, 0xC0,
    0x32, 0x04, 0xC0,
    0x60, 0x00, 0x60,
    0x60, 0x00, 0x60,
    0x60, 0x00, 0x60,
    0x60, 0x00, 0x60,
    0x60, 0x00, 0x60,
    0x60, 0x00, 0x60,
    0x30, 0x00, 0xC0,
    0x38, 0x01, 0xC0,
    0x1C, 0x03, 0x80,
    0x0E, 0x07, 0x00,
    0x07, 0xFE, 0x00,
    0x01, 0xF8, 0x00,
    0x00, 0x00, 0x00
};
static const uint8_t s_icon_loop_random[60] = {
    0x00, 0x00, 0x00,
    0x00, 0x00, 0x00,
    0x00, 0x00, 0x00,
    0x18, 0x00, 0xC0,
    0x1C, 0x1F, 0xC0,
    0x0E, 0x0F, 0x80,
    0x07, 0x07, 0x80,
    0x03, 0x8F, 0x80,
    0x01, 0xDD, 0x80,
    0x00, 0xF8, 0x80,
    0x00, 0x70, 0x00,
    0x08, 0xF8, 0x00,
    0x1D, 0xDC, 0x00,
    0x1F, 0x8E, 0x00,
    0x1F, 0x07, 0x00,
    0x1F, 0x83, 0x80,
    0x1F, 0xC1, 0xC0,
    0x1F, 0x80, 0xC0,
    0x00, 0x00, 0x00,
    0x00, 0x00, 0x00
};
#define MP3_LOOP_ICON_W 20
#define MP3_LOOP_ICON_H 20
static const uint8_t *const s_loop_icon[MP3_LOOP_N] = {
    s_icon_loop_single,   /* 单曲循环 */
    s_icon_loop_list,     /* 列表循环 */
    s_icon_loop_random,   /* 随机播放 */
};
static const char *s_loop_toast[MP3_LOOP_N] = {
    "\xe5\x8d\x95\xe6\x9b\xb2\xe5\xbe\xaa\xe7\x8e\xaf",   /* 单曲循环 */
    "\xe5\x88\x97\xe8\xa1\xa8\xe5\xbe\xaa\xe7\x8e\xaf",   /* 列表循环 */
    "\xe9\x9a\x8f\xe6\x9c\xba\xe6\x92\xad\xe6\x94\xbe",   /* 随机播放 */
};
/* 进度条几何 (render / touch 共用) */
#define MP3_BAR_X1          (UI_SCREEN_W - 15)
#define MP3_BAR_PAD_BOTTOM  10
#define MP3_BAR_H           8
#define MP3_BAR_Y           (UI_SCREEN_H - MP3_BAR_PAD_BOTTOM - MP3_BAR_H)
/* 循环模式按钮: 进度条左侧, 24x24 方框 (内 draw_zh_sb 16x16 字) */
#define MP3_LOOP_BTN_W      24
#define MP3_LOOP_BTN_H      24
#define MP3_LOOP_BTN_X      10
#define MP3_LOOP_BTN_Y      (MP3_BAR_Y + MP3_BAR_H / 2 - MP3_LOOP_BTN_H / 2)
#define MP3_LOOP_GAP        6
/* V1.4.x: 随音乐震动按钮: 紧邻循环按钮右侧 (几何与触摸热区共用) */
#define MP3_VIB_BTN_W       24
#define MP3_VIB_BTN_H       24
#define MP3_VIB_BTN_X       (MP3_LOOP_BTN_X + MP3_LOOP_BTN_W + MP3_LOOP_GAP)
#define MP3_VIB_BTN_Y       MP3_LOOP_BTN_Y
/* 进度条左端让位给两个按钮 */
#define MP3_BAR_X0          (MP3_VIB_BTN_X + MP3_VIB_BTN_W + MP3_LOOP_GAP)
/* 播放列表: 底部止于循环按钮上方 (留 4px). 原先按"屏幕底 -5"算会排到第 10 行,
 * 末行文字延伸到 y≈290, 与按钮 (y 274..297) 重叠被白底盖掉一角. */
#define MP3_LIST_BOTTOM     (MP3_LOOP_BTN_Y - 4)

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
/* 命中: 循环模式按钮 (扩展热区: 四周外扩 6px, 便于手指点中).
 * 注意: poll"按下即触发"与 touch 的 tap 消费必须共用同一热区,
 * 否则 tap 落在扩展区/原区不一致会把"点按钮"穿透成"点歌". */
static bool mp3_in_loop_btn(int x, int y)
{
    return (x >= MP3_LOOP_BTN_X - 6 && x < MP3_LOOP_BTN_X + MP3_LOOP_BTN_W + 6 &&
            y >= MP3_LOOP_BTN_Y - 6 && y < MP3_LOOP_BTN_Y + MP3_LOOP_BTN_H + 6);
}
/* 切换循环模式: 单曲 -> 列表 -> 随机 -> 单曲 */
static void mp3_loop_mode_next(void) { s_loop_mode = (s_loop_mode + 1) % MP3_LOOP_N; }

/* 循环模式按钮"按下即触发" (绕开 input 手势判定).
 * 背景: 按钮位于屏幕底部"上滑返回"手势带附近 (y>=285, 15px=5%屏高, 按钮 y274-298 已绝大部分移出),
 *       input 层**松手**时才判定手势:
 *   位移<40px → tap(CONFIRM, 正常); 上滑>=60px → BACK(弹退出框);
 *   其他位移>=40px → 事件被吞 (MENU_ACTION_NONE, 无任何反应)
 *   → 表现为"要多点几次才切换" (点击时手指轻微滑动即被吞, 或误弹退出框).
 * 修复: 本函数直读原始触点 (input_get_touch_multi, 已含 rotate, 不受手势判定/
 * 底部屏蔽带影响, 与手势判定同帧共用同一触摸缓存), **按下沿立即切换**,
 * 松手后由 touch 层消费 tap 时复位 (见 p_mp3_touch), 不再重复切换. */
static bool     s_loop_btn_down = false;   /* 触点正按在按钮上 (poll 按下沿已处理) */
static uint32_t s_loop_btn_t_ms = 0;       /* 上次按下沿切换时刻 (延迟复位兜底用) */
static void mp3_poll_loop_btn(ui_ctx_t *ctx)
{
    tp_point_t pts[TP_MAX_POINTS];
    int n = 0;
    input_get_touch_multi(pts, &n);
    bool in = false;
    for (int i = 0; i < n; i++) {
        if (pts[i].pressed && mp3_in_loop_btn(pts[i].x, pts[i].y)) { in = true; break; }
    }
    if (in && !s_loop_btn_down) {
        s_loop_btn_down = true;
        s_loop_btn_t_ms = (uint32_t)(esp_timer_get_time() / 1000);
        mp3_loop_mode_next();
        os_dialog_toast(ctx, s_loop_toast[s_loop_mode]);
        ctx->needs_redraw = true;
    } else if (!in) {
        /* 触点已离开: 不立即复位 — 松手后的 tap 还在途, 若这里复位,
         * touch 层会把该 tap 当"再次点击"而重复切换.
         * 正常路径由 touch 消费 tap 时复位; 此处仅兜底"点击被手势吞掉
         * (无 tap 投递)"的场景, 超时后复位允许下次按下再触发. */
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_loop_btn_down && (int32_t)(now - s_loop_btn_t_ms) > 500)
            s_loop_btn_down = false;
    }
}

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

/* 循环模式按钮: 24x24 方框 + 20x20 图标 (单曲/列表/随机), 位于进度条左侧.
 * 图标来自 s_loop_icon 位图 (1bpp, 后续可换图). */
static void mp3_draw_loop_btn(st7305_handle_t *lcd)
{
    int x0 = MP3_LOOP_BTN_X, y0 = MP3_LOOP_BTN_Y;
    int x1 = x0 + MP3_LOOP_BTN_W - 1, y1 = y0 + MP3_LOOP_BTN_H - 1;
    fill_rect(lcd, x0, y0, x1, y1, ST7305_COLOR_WHITE);
    draw_rect_outline(lcd, x0, y0, x1, y1, ST7305_COLOR_BLACK);
    draw_1bpp_stretched(lcd, x0 + MP3_LOOP_BTN_W / 2, y0 + MP3_LOOP_BTN_H / 2,
                        MP3_LOOP_ICON_W, MP3_LOOP_ICON_H,
                        MP3_LOOP_ICON_W, MP3_LOOP_ICON_H, s_loop_icon[s_loop_mode]);
}

/* ============ V1.4.x: 随音乐震动按钮 ============
 * 20x20 1bpp 波纹图标 (位=1 前景色). 开=黑底白波纹, 关=白底黑波纹. */
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
/* 1bpp 位图画到 (x0,y0), 支持指定颜色 (1bpp 屏反白显示用) */
static void mp3_blit_1bpp_color(st7305_handle_t *lcd, int x0, int y0,
                                int w, int h, const uint8_t *src, uint8_t color) {
    int bpr = (w + 7) >> 3;
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            if (src[dy * bpr + (dx >> 3)] & (0x80u >> (dx & 7)))
                st7305_draw_pixel(lcd, x0 + dx, y0 + dy, color);
        }
    }
}
static void mp3_draw_vib_btn(st7305_handle_t *lcd)
{
    int x0 = MP3_VIB_BTN_X, y0 = MP3_VIB_BTN_Y;
    int x1 = x0 + MP3_VIB_BTN_W - 1, y1 = y0 + MP3_VIB_BTN_H - 1;
    if (s_vib_music) {
        fill_rect(lcd, x0, y0, x1, y1, ST7305_COLOR_BLACK);   /* 开: 黑底 */
        mp3_blit_1bpp_color(lcd, x0 + 2, y0 + 2, MP3_VIB_ICON_W, MP3_VIB_ICON_H,
                            s_icon_vib, ST7305_COLOR_WHITE);  /* 白波纹 */
    } else {
        fill_rect(lcd, x0, y0, x1, y1, ST7305_COLOR_WHITE);
        draw_rect_outline(lcd, x0, y0, x1, y1, ST7305_COLOR_BLACK);
        mp3_blit_1bpp_color(lcd, x0 + 2, y0 + 2, MP3_VIB_ICON_W, MP3_VIB_ICON_H,
                            s_icon_vib, ST7305_COLOR_BLACK);  /* 黑波纹 */
    }
}
/* 命中: 随音乐震动按钮 (热区与循环按钮一致: 四周外扩 6px) */
static bool mp3_in_vib_btn(int x, int y)
{
    return (x >= MP3_VIB_BTN_X - 6 && x < MP3_VIB_BTN_X + MP3_VIB_BTN_W + 6 &&
            y >= MP3_VIB_BTN_Y - 6 && y < MP3_VIB_BTN_Y + MP3_VIB_BTN_H + 6);
}
/* 切换随音乐震动: 应用 + 存 NVS + toast */
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
/* 首次渲染懒加载 NVS 开关值 (模块无 on_enter 钩子, 用标志位保证只读一次) */
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
/* 随音乐震动按钮"按下即触发" (与循环按钮同策略, 绕开 input 手势判定) */
static bool     s_vib_btn_down = false;
static uint32_t s_vib_btn_t_ms = 0;
static void mp3_poll_vib_btn(ui_ctx_t *ctx)
{
    tp_point_t pts[TP_MAX_POINTS];
    int n = 0;
    input_get_touch_multi(pts, &n);
    bool in = false;
    for (int i = 0; i < n; i++) {
        if (pts[i].pressed && mp3_in_vib_btn(pts[i].x, pts[i].y)) { in = true; break; }
    }
    if (in && !s_vib_btn_down) {
        s_vib_btn_down = true;
        s_vib_btn_t_ms = (uint32_t)(esp_timer_get_time() / 1000);
        mp3_vib_toggle(ctx);
    } else if (!in) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_vib_btn_down && (int32_t)(now - s_vib_btn_t_ms) > 500)
            s_vib_btn_down = false;   /* 兜底: 点击被手势吞掉时超时复位 */
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

    /* === 右侧: 光盘 + 进度条 === */
    int right_x = list_x + list_w + 25;
    int right_w = SCREEN_W - right_x - 8;
    int cd_cx = right_x + right_w / 2 - 10;
    int cd_cy = 130;
    int cd_r = 92;
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

    /* === 底部: 循环模式按钮 + 进度条 (几何统一取 MP3_BAR_* 宏) === */
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
    mp3_draw_loop_btn(lcd);   /* 循环模式按钮: 进度条左侧, 点击切换 单曲/列表/随机 */
    mp3_draw_vib_btn(lcd);    /* V1.4.x: 随音乐震动按钮: 循环按钮右侧, 黑底=开 */
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
    /* 仅音乐在播/暂停才弹两键确认; 空闲态(列表/播完)直接退页, 不弹无意义弹窗.
     * 用户偏好: 没放歌时返回就直接走, 不必每次都确认. */
    if (st == AUDIO_STATE_PLAYING || st == AUDIO_STATE_PAUSED) {
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
        os_dialog_toast(ctx, s_loop_toast[s_loop_mode]);
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

/* ============ 每帧轮询: 播放中持续重绘 (CD 旋转 + 进度) ============ */
static void p_mp3_poll(ui_ctx_t *ctx)
{
    audio_state_t st = audio_player_get_state();

    /* 循环模式按钮: 按下即触发 (绕开 input 手势判定, 修"多点几次才切换") */
    mp3_poll_loop_btn(ctx);
    /* V1.4.x: 随音乐震动按钮: 同策略按下即触发 */
    mp3_poll_vib_btn(ctx);

    /* 自然播完 → 按循环模式自动切下一首.
     * audio_player_track_ended() 为一次性消费标志, 仅"自然播完"置位,
     * 用户主动 stop()/切歌不会置位, 故不会与手动切歌打架. */
    if (audio_player_track_ended()) {
        if (s_count > 0) {
            int nxt = mp3_next_index();   /* 单曲=重播本曲 / 列表=下一首 / 随机=随机一首 */
            audio_player_stop();
            mp3_play_at(nxt);
            s_sel = nxt;                  /* 同步列表选中位, 避免确认键跳回旧选择 */
        }
        ctx->needs_redraw = true;
    }

    if (st == AUDIO_STATE_PLAYING) {
        ctx->needs_redraw = true;   /* 每帧重绘, 驱动 CD 旋转与进度条 */
    }
}

/* ============ 触摸: 左侧列表点击选歌 + 右侧大光盘 播放/暂停 ============ */
static bool p_mp3_touch(ui_ctx_t *ctx, int x, int y)
{
    const int SCREEN_W = UI_SCREEN_W;
    const int list_x = 0, list_w = 195, list_y = 28, line_h = 26;

    /* 底部播放控件优先: 循环按钮/进度条的坐标落在左侧列表的列范围内 (x<195 且 y>=28),
     * 但语义属于播放控制. 必须先于列表 hit-test 拦截, 否则点按钮会被当成"点歌"播放. */
    if (mp3_in_loop_btn(x, y)) {
        /* poll 已在"按下沿"切换过 (s_loop_btn_down=true), 这里只消费松手后的 tap,
         * 不再重复切换; 复位标志允许下次按下再触发.
         * 兜底: poll 若漏掉按下沿 (极少数丢帧), 此处补一次切换. */
        if (s_loop_btn_down) {
            s_loop_btn_down = false;
        } else {
            mp3_loop_mode_next();
            os_dialog_toast(ctx, s_loop_toast[s_loop_mode]);
            ctx->needs_redraw = true;
        }
        return true;
    }
    /* V1.4.x: 随音乐震动按钮 (与循环按钮同策略: poll 按下沿已切换, 这里消费 tap 复位) */
    if (mp3_in_vib_btn(x, y)) {
        if (s_vib_btn_down) {
            s_vib_btn_down = false;
        } else {
            mp3_vib_toggle(ctx);   /* 兜底: poll 漏掉按下沿时补切换 */
        }
        return true;
    }
    if (mp3_in_bar_zone(x, y)) {            /* 进度条: 点击跳转 (seek) */
        int p = mp3_x_to_progress(x);
        audio_state_t st = audio_player_get_state();
        if (st == AUDIO_STATE_PLAYING || st == AUDIO_STATE_PAUSED) {
            audio_player_seek_to(p);        /* 播放/暂停中: 直接跳到点击位置 */
        } else if (s_count > 0) {
            /* 尚未起播: 先播当前选中曲, 再跳到点击位置 */
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
            int max_visible = (MP3_LIST_BOTTOM - list_y) / line_h;   /* 与 render 同一算法 */
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
    int right_x = list_x + list_w + 25;
    int right_w = SCREEN_W - right_x - 8;
    int cx = right_x + right_w / 2 - 10;
    int cy = 130, r = 92;
    int dx = x - cx, dy = y - cy;
    if (dx * dx + dy * dy > r * r) return false;   /* 不在光盘上 */
    /* 播放/暂停切换 */
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
