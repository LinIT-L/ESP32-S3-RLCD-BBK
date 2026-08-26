/**
 * page_settings.c — 设置 页面模块 (P3 迁移: 从 menu_system.c 的 settings 弹窗体系迁移).
 *
 * 用 os_dialog 弹窗栈呈现: 主菜单 6 项 → 各子弹窗 (时间/字体/无线/系统信息/赞助).
 * 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include "ui_common.h"
#include "font_zh.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "board_battery.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

#define TAG "PSET"

/* 主菜单项 (UTF-8) */
static const char *const s_set_items[6] = {
    "\xe6\x97\xa5\xe6\x9c\x9f\xe6\x97\xb6\xe9\x97\xb4",  /* 日期时间 */
    "\xe5\xad\x97\xe4\xbd\x93\xe9\xa3\x8e\xe6\xa0\xbc",  /* 字体风格 */
    "\xe6\x97\xa0\xe7\xba\xbf\xe8\xbf\x9e\xe6\x8e\xa5",  /* 无线连接 */
    "\xe7\xb3\xbb\xe7\xbb\x9f\xe4\xbf\xa1\xe6\x81\xaf",  /* 系统信息 */
    "\xe8\xaf\xb7\xe4\xbd\x9c\xe8\x80\x85\xe5\x96\x9d\xe6\x9d\xaf\xe6\xb0\xb4", /* 请作者喝杯水 */
    "\xe8\xbf\x94\xe5\x9b\x9e",                          /* 返回 */
};

/* 时间编辑弹窗状态 */
typedef struct {
    int  field;          /* 0=年 1=月 2=日 3=时 4=分 5=秒 */
    char fmt[40];        /* 当前编辑行的显示格式 */
} time_dlg_t;
static time_dlg_t s_tdlg;

/* 系统信息弹窗内容 (静态生成) */
static char s_info_lines[8][32];
static int  s_info_n;

static void build_sysinfo(void) {
    s_info_n = 0;
    char b[32];
    esp_chip_info_t ci; esp_chip_info(&ci);
    snprintf(b, 32, "CPU: ESP32-S3 %dM", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ); strcpy(s_info_lines[s_info_n++], b);
    uint32_t fsz = 0;
    if (esp_flash_get_size(NULL, &fsz) == ESP_OK) {
        snprintf(b, 32, "Flash: %uMB", (unsigned)(fsz / 1048576)); strcpy(s_info_lines[s_info_n++], b);
    }
    snprintf(b, 32, "RAM: %uK", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024)); strcpy(s_info_lines[s_info_n++], b);
    board_battery_status_t bt = {0};
    if (board_battery_read(&bt) == ESP_OK) {
        snprintf(b, 32, "Battery: %u%%", bt.percent); strcpy(s_info_lines[s_info_n++], b);
    }
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    snprintf(b, 32, "IDF: %s", esp_get_idf_version()); strcpy(s_info_lines[s_info_n++], b);
    snprintf(b, 32, "Time: %02d:%02d", t->tm_hour, t->tm_min); strcpy(s_info_lines[s_info_n++], b);
}

/* ---- 时间弹窗: 自定义渲染 (显示当前值) + 自定义按键 (编辑字段) ---- */
static bool time_dlg_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char fields[6][16];
    snprintf(fields[0], sizeof(fields[0]), "%04d", t->tm_year + 1900);
    snprintf(fields[1], sizeof(fields[1]), "%02d", t->tm_mon + 1);
    snprintf(fields[2], sizeof(fields[2]), "%02d", t->tm_mday);
    snprintf(fields[3], sizeof(fields[3]), "%02d", t->tm_hour);
    snprintf(fields[4], sizeof(fields[4]), "%02d", t->tm_min);
    snprintf(fields[5], sizeof(fields[5]), "%02d", t->tm_sec);
    static const char *lbl[6] = { "\xe5\xb9\xb4","\xe6\x9c\x88","\xe6\x97\xa5","\xe6\x97\xb6","\xe5\x88\x86","\xe7\xa7\x92" }; /* 年月日时分秒 */
    int y0 = (UI_SCREEN_H - 140) / 2;
    draw_dialog_frame(lcd, 30, y0, UI_SCREEN_W - 60, 140, 3);
    draw_text_centered(lcd, y0 + 6, "\xe6\x97\xb6\xe9\x97\xb4", false); /* 时间 */
    for (int i = 0; i < 6; i++) {
        int y = y0 + 30 + i * 18;
        bool sel = (i == s_tdlg.field);
        if (sel) fill_rect(lcd, 42, y, UI_SCREEN_W - 42, y + 16, ST7305_COLOR_BLACK);
        draw_text(lcd, 52, y + 1, lbl[i], sel);
        draw_text(lcd, 100, y + 1, fields[i], sel);
    }
    return true;
}

static bool time_dlg_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)d; (void)ud;
    if (a == OS_ACTION_UP)   { if (s_tdlg.field > 0) s_tdlg.field--; ctx->needs_redraw = true; return true; }
    if (a == OS_ACTION_DOWN) { if (s_tdlg.field < 5) s_tdlg.field++; ctx->needs_redraw = true; return true; }
    if (a == OS_ACTION_LEFT || a == OS_ACTION_RIGHT) { ctx->needs_redraw = true; return true; }
    if (a == OS_ACTION_CONFIRM) {
        /* 保存时间到系统 */
        struct tm t = {0};
        time_t now = time(NULL);
        struct tm *ct = localtime(&now);
        t = *ct;
        /* 简化: 以当前系统时间为准, 暂不做逐字段写入 (需 setenv/TZ 处理) */
        os_dialog_pop(ctx);
        ctx->needs_redraw = true;
        return true;
    }
    return false;   /* BACK 等走默认 (弹栈) */
}

static bool info_dlg_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    build_sysinfo();
    int y0 = (UI_SCREEN_H - 160) / 2;
    draw_dialog_frame(lcd, 30, y0, UI_SCREEN_W - 60, 160, 3);
    draw_text_centered(lcd, y0 + 6, "\xe7\xb3\xbb\xe7\xbb\x9f\xe4\xbf\xa1\xe6\x81\xaf", false); /* 系统信息 */
    for (int i = 0; i < s_info_n; i++)
        draw_text(lcd, 48, y0 + 30 + i * 20, s_info_lines[i], false);
    return true;
}

/* 主菜单选中回调: 开对应子弹窗 */
static void on_settings_select(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result < 0 || result >= 6) return;
    switch (result) {
    case 0: {  /* 日期时间 */
        os_dlg_stack_t dlg;
        memset(&dlg, 0, sizeof(dlg));
        snprintf(dlg.title, sizeof(dlg.title), "\xe6\x97\xb6\xe9\x97\xb4");
        dlg.count = 0;
        dlg.cb = NULL;
        dlg.on_render = time_dlg_render;
        dlg.on_key = time_dlg_key;
        s_tdlg.field = 0;
        os_dialog_push(ctx, &dlg);
        break;
    }
    case 3: {  /* 系统信息 */
        os_dlg_stack_t dlg;
        memset(&dlg, 0, sizeof(dlg));
        snprintf(dlg.title, sizeof(dlg.title), "\xe7\xb3\xbb\xe7\xbb\x9f\xe4\xbf\xa1\xe6\x81\xaf");
        dlg.count = 0;
        dlg.cb = NULL;
        dlg.on_render = info_dlg_render;
        os_dialog_push(ctx, &dlg);
        break;
    }
    case 1:  /* 字体风格: 占位提示 */
    case 2:  /* 无线连接: 占位提示 */
        os_dialog_toast(ctx, "\xe5\xbc\x80\xe5\x8f\x91\xe4\xb8\xad"); /* 开发中 */
        break;
    case 4:  /* 请作者喝杯水: 占位 */
        os_dialog_toast(ctx, "\xe8\xb0\xa2\xe8\xb0\xa2\xe6\x94\xaf\xe6\x8c\x81"); /* 谢谢支持 */
        break;
    case 5:  /* 返回 */
        os_pop(ctx);
        break;
    default: break;
    }
}

static void p_settings_enter(ui_ctx_t *ctx) {
    (void)ctx;
    /* 进入: 打开设置主菜单弹窗 (覆盖当前页) */
    os_dialog_list(ctx, "\xe8\xae\xbe\xe7\xbd\xae", s_set_items, 6, 0, on_settings_select, NULL);
}

static const os_module_t s_mod_settings = {
    .name       = "settings",
    .page_id    = OS_PAGE_SETTINGS,
    .on_enter   = p_settings_enter,
    .fullscreen = false,
};

void os_page_settings_register(void) { os_register(&s_mod_settings); }
