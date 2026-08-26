/**
 * page_mp3.c — MP3 播放器 页面模块 (P3 迁移).
 *
 * 从 menu_system.c 的 render_mp3_player / mp3_* 迁移:
 *   - 左: 播放列表 (中文 24px 每行, 当前歌【】标记)
 *   - 右: 旋转光盘 (播放时旋转)
 *   - 底部: 全宽进度条
 *   - 按键: 上下=选曲, 确认=播放/暂停/切歌, 左右=上一/下一曲, 返回=停止退出
 *   - 私有 state 全部 static 留本文件.
 *
 * 注: 文件列表用 opendir 直读 /sdcard/mp3 (POSIX), 不依赖 menu 组件.
 */
#include "os.h"
#include "ui_common.h"
#include "audio_player.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <strings.h>

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

static void mp3_scan_files(void) {
    if (s_scanned && s_count > 0) return;
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
                strncpy(s_files[s_count], n, MP3_FNAME_LEN - 1);
                s_files[s_count][MP3_FNAME_LEN - 1] = '\0';
                s_count++;
            }
        }
        closedir(dir);
    }
    s_scanned = true;
    ESP_LOGI(TAG, "MP3 扫描: %d 个文件", s_count);
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

/* ============ 渲染 ============ */
static void p_mp3_render(ui_ctx_t *ctx)
{
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return;
    const int SCREEN_W = UI_SCREEN_W, SCREEN_H = UI_SCREEN_H;

    mp3_scan_files();
    st7305_clear(lcd, ST7305_COLOR_WHITE);

    audio_state_t astate = audio_player_get_state();
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    int progress = audio_player_get_progress();

    int highlight_idx = s_in_player ? s_current : s_sel;
    if (highlight_idx < 0) highlight_idx = 0;

    /* === 左菜单: 播放列表 === */
    int list_x = 0, list_w = 195, list_y = 28;
    int list_h = SCREEN_H - list_y - 5;
    int line_h = 26;
    int max_visible = list_h / line_h;
    int scroll = 0;
    if (highlight_idx >= max_visible - 1) scroll = highlight_idx - max_visible + 2;
    if (scroll < 0) scroll = 0;

    for (int i = 0; i < s_count && i < max_visible - 1; i++) {
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
        draw_text_centered(lcd, 100, "\xe6\x9c\xaa\xe6\x89\xbe\xe5\x88\xb0 MP3 \xe6\x96\x87\xe4\xbb\xb6", false); /* 未找到 MP3 文件 */
        draw_text_centered(lcd, 130, "\xe8\xaf\xb7\xe6\x94\xbe\xe5\x85\xa5 /sdcard/mp3", false);              /* 请放入 /sdcard/mp3 */
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

    int bar_pad_bottom = 10, bar_h = 8;
    int bar_y = SCREEN_H - bar_pad_bottom - bar_h;
    int bar_x0 = 15, bar_x1 = SCREEN_W - 15;
    int bar_w = bar_x1 - bar_x0 + 1;
    int played_w = bar_w * progress / 1000;
    fill_rect(lcd, bar_x0, bar_y, bar_x0 + played_w - 1, bar_y + bar_h - 1, ST7305_COLOR_BLACK);
    fill_rect(lcd, bar_x0 + played_w, bar_y, bar_x1, bar_y + bar_h - 1, ST7305_COLOR_WHITE);
    fill_rect(lcd, bar_x0, bar_y, bar_x1, bar_y, ST7305_COLOR_BLACK);
    fill_rect(lcd, bar_x0, bar_y + bar_h - 1, bar_x1, bar_y + bar_h - 1, ST7305_COLOR_BLACK);
    fill_rect(lcd, bar_x0, bar_y, bar_x0, bar_y + bar_h - 1, ST7305_COLOR_BLACK);
    fill_rect(lcd, bar_x1, bar_y, bar_x1, bar_y + bar_h - 1, ST7305_COLOR_BLACK);
}

/* ============ 按键 ============ */
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
            ctx->needs_redraw = true;
        }
        break;
    case OS_ACTION_RIGHT:
        if (s_in_player && s_count > 0) {
            audio_player_stop();
            mp3_play_at((s_current + 1) % s_count);
            ctx->needs_redraw = true;
        }
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
    case OS_ACTION_HOME:
        audio_player_stop();
        os_pop(ctx);
        break;
    default:
        break;
    }
}

/* ============ 每帧轮询: 播放中持续重绘 (CD 旋转 + 进度) ============ */
static void p_mp3_poll(ui_ctx_t *ctx)
{
    audio_state_t st = audio_player_get_state();
    if (st == AUDIO_STATE_PLAYING) {
        ctx->needs_redraw = true;   /* 每帧重绘, 驱动 CD 旋转与进度条 */
    }
}

/* ============ 模块契约 ============ */
static const os_module_t s_mod_mp3 = {
    .name      = "mp3",
    .page_id   = OS_PAGE_MP3_PLAYER,
    .render    = p_mp3_render,
    .action    = p_mp3_action,
    .poll      = p_mp3_poll,
    .fullscreen = false,
};

void os_page_mp3_register(void) { os_register(&s_mod_mp3); }
