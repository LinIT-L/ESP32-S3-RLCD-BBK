/**
 * @file file_browser.c
 * @brief TF 卡文件浏览器 (类似手机文件管理器)
 *
 * 流程:
 *   1. 维护 g_fb_path (当前目录) + g_fb_entries (扫描结果)
 *   2. fb_build: 扫描 g_fb_path, 把目录/文件拼成显示文本写到 buf
 *   3. fb_on_confirm: 按 KEY 时:
 *      - 目录: 修改 g_fb_path 进入
 *      - .gam: 启动文曲星模拟器
 *      - 其它: 提示"不支持"
 *   4. fb_on_back: 长按 BOOT 返回上级 (根目录时返回主菜单)
 */
#include "file_browser.h"
#include "menu_system.h"
#include "sd_scan.h"
#include "st7305.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "gam4980_emu.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "FB";

/* 当前目录 (绝对路径) */
static char g_fb_path[128] = "/sdcard";
/* V1.0.44: g_fb_entries 移到 PSRAM, 释放约 4.6KB 内部 DRAM (64×72B).
 * 文件浏览器仅在浏览时访问, 非高频路径, PSRAM 无影响. */
EXT_RAM_BSS_ATTR static file_entry_t g_fb_entries[64];
static int  g_fb_count = 0;

void fb_init(void) {
    strncpy(g_fb_path, "/sdcard", sizeof(g_fb_path) - 1);
    g_fb_path[sizeof(g_fb_path) - 1] = '\0';
    g_fb_count = 0;
}

const char *fb_get_path(void) { return g_fb_path; }

/* 重新扫描当前目录 (供 build + on_confirm 用) */
static void fb_rescan(void) {
    g_fb_count = scan_path(g_fb_path, g_fb_entries,
                           sizeof(g_fb_entries) / sizeof(g_fb_entries[0]));
    if (g_fb_count < 0) g_fb_count = 0;
}

/* 格式化文件大小 (B / KB / MB) */
static void format_size(char *out, size_t out_size, uint32_t sz) {
    if (sz < 1024) {
        snprintf(out, out_size, "%luB", (unsigned long)sz);
    } else if (sz < 1024 * 1024) {
        snprintf(out, out_size, "%luK", (unsigned long)(sz / 1024));
    } else {
        /* xx.x M */
        unsigned long mb_int = sz / (1024 * 1024);
        unsigned long mb_dec = (sz % (1024 * 1024)) * 10 / (1024 * 1024);
        snprintf(out, out_size, "%lu.%luM", mb_int, mb_dec);
    }
}

/* 截断 UTF-8 字符串到 max_chars 个字符 (按 display column 算, 中文 1 列, ASCII 0.5 列)
 * 简化: 这里按字节截断, 避免破坏 UTF-8 序列.
 * 行布局: 名字最多 14 个 ASCII 字符 (12 中文) */
static void safe_filename(char *out, size_t out_size, const char *src) {
    size_t n = 0;
    size_t pos = 0;
    while (src[pos] != '\0' && n < out_size - 1) {
        uint8_t c = (uint8_t)src[pos];
        size_t step = 1;
        if (c < 0x80)      step = 1;     /* ASCII */
        else if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;  /* 中文 */
        else if ((c & 0xF8) == 0xF0) step = 4;
        else { pos++; continue; }  /* 非法字节跳过 */
        if (pos + step > 60) break;  /* 名字太长截断 */
        /* 复制 step 字节, 防越界 */
        for (size_t i = 0; i < step && n < out_size - 1; i++) {
            out[n++] = src[pos + i];
        }
        pos += step;
    }
    out[n] = '\0';
}

/* === build_items: 扫描 g_fb_path 把结果拼成显示文本写到 buf ===
 * 列表顺序:
 *   1. 目录 (字母序)
 *   2. 文件 (.gam 优先, 然后其它, 各按字母序)
 *   3. 倒数第 2 项: "[上一级]" (根目录不显示)
 *   4. 最后 1 项: "[返回主菜单]"
 */
int fb_build(menu_state_t *state, char buf[][64], int max) {
    (void)state;
    if (max <= 0) return 0;
    /* V1.0.67: 无 TF 卡时提示, 不再显示空白列表 */
    if (!sd_is_mounted()) {
        snprintf(buf[0], 64, "\xe6\x9c\xaa\xe6\xa3\x80\xe6\xb5\x8b\xe5\x88\xb0 TF \xe5\x8d\xa1"); /* 未检测到 TF 卡 */
        if (max > 1) snprintf(buf[1], 64, "\xe8\xbf\x94\xe5\x9b\x9e");                     /* 返回 */
        return (max > 1) ? 2 : 1;
    }
    fb_rescan();

    int n = 0;
    /* 目录 + 文件 (scan_path 已经排序) */
    for (int i = 0; i < g_fb_count && n < max - 2; i++) {
        const file_entry_t *e = &g_fb_entries[i];
        char name[48];
        safe_filename(name, sizeof(name), e->name);
        char sz[12];
        format_size(sz, sizeof(sz), e->size);

        switch (e->type) {
            case FB_TYPE_DIR:
                snprintf(buf[n++], 64, "/%-12s", name);
                break;
            case FB_TYPE_GAM:
                snprintf(buf[n++], 64, "G %-12s %3s", name, sz);
                break;
            case FB_TYPE_BIN:
                snprintf(buf[n++], 64, "R %-12s %3s", name, sz);
                break;
            default:
                snprintf(buf[n++], 64, ". %-12s %3s", name, sz);
                break;
        }
    }
    /* [上一级] (根目录不显示) */
    if (strcmp(g_fb_path, "/sdcard") != 0 && n < max - 1) {
        snprintf(buf[n++], 64, "..");
    }
    /* [返回主菜单] 永远显示 */
    if (n < max) {
        snprintf(buf[n++], 64, "返回");
    }
    return n;
}

/* 进入子目录 (修改 g_fb_path) */
static void fb_enter_dir(const char *dirname) {
    /* g_fb_path + "/" + dirname, 总长度限制 128 */
    size_t cur = strlen(g_fb_path);
    if (cur + 1 + strlen(dirname) >= sizeof(g_fb_path)) {
        ESP_LOGW(TAG, "路径过长, 不进入");
        return;
    }
    g_fb_path[cur] = '/';
    strncpy(g_fb_path + cur + 1, dirname, sizeof(g_fb_path) - cur - 2);
    g_fb_path[sizeof(g_fb_path) - 1] = '\0';
    ESP_LOGI(TAG, "进入目录: %s", g_fb_path);
}

/* 上一级目录 (修改 g_fb_path) */
static void fb_parent_dir(void) {
    if (strcmp(g_fb_path, "/sdcard") == 0) return;  /* 根目录无上级 */
    char *p = strrchr(g_fb_path, '/');
    if (!p) return;
    if (p == g_fb_path) {
        /* "/xxx" → "/" (根) */
        g_fb_path[1] = '\0';
    } else {
        *p = '\0';
    }
    /* 永远不低于 /sdcard */
    if (strncmp(g_fb_path, "/sdcard", 7) != 0) {
        strcpy(g_fb_path, "/sdcard");
    }
    ESP_LOGI(TAG, "上一级目录: %s", g_fb_path);
}

/* 启动 .gam 游戏 (跟 select_game_on_confirm 类似) */
static void launch_gam(menu_state_t *state, const char *fullpath) {
    ESP_LOGI(TAG, "启动游戏: %s", fullpath);

    /* 关闭确认弹窗等 */
    state->confirm_active = false;
    state->confirm_notice = false;

    /* 初始化 emu (如果还没初始化) */
    extern esp_err_t gam4980_emu_init(st7305_handle_t *lcd);
    static bool emu_inited = false;
    if (!emu_inited) {
        esp_err_t r = gam4980_emu_init(state->lcd);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "emu 初始化失败: %s", esp_err_to_name(r));
            state->confirm_active = true;
            state->confirm_notice = true;
            snprintf(state->confirm_title, sizeof(state->confirm_title), "启动失败");
            snprintf(state->confirm_msg, sizeof(state->confirm_msg), "emu init 错误");
            return;
        }
        emu_inited = true;
    }
    int rc = gam4980_emu_load(fullpath);
    if (rc != 0) {
        ESP_LOGE(TAG, "游戏加载失败: %d", rc);
        state->confirm_active = true;
        state->confirm_notice = true;
        snprintf(state->confirm_title, sizeof(state->confirm_title), "加载失败");
        snprintf(state->confirm_msg, sizeof(state->confirm_msg), "看串口: 缺8.BIN?");
        return;
    }
    ESP_LOGI(TAG, "游戏加载成功, 进入运行循环");
    gam4980_set_status_info(state->settings.battery, state->settings.pad_connected);
    gam4980_emu_run();
    ESP_LOGI(TAG, "游戏退出, 返回文件浏览器");
}

bool fb_on_confirm(menu_state_t *state, int idx) {
    /* 重新扫描以拿到最新列表 (用户可能在外部分配新文件, 但重新扫描更稳) */
    fb_rescan();

    int n_dir_or_file = g_fb_count;
    int n_total = n_dir_or_file;
    if (strcmp(g_fb_path, "/sdcard") != 0) n_total++;  /* [上一级] */
    /* 最后一项目 [返回] */

    /* [返回] (最后一项) */
    if (idx == n_total) {
        /* 返回主菜单 */
        state->current_page = MENU_PAGE_MAIN;
        state->selected_index = 0;
        state->scroll_offset = 0;
        state->needs_redraw = true;
        return true;
    }

    /* [上一级] (倒数第二项, 在非根目录时) */
    if (strcmp(g_fb_path, "/sdcard") != 0 && idx == n_total - 1) {
        fb_parent_dir();
        state->selected_index = 0;
        state->scroll_offset = 0;
        state->needs_redraw = true;
        return true;
    }

    /* 目录 / 文件 (idx 0..g_fb_count-1) */
    if (idx < 0 || idx >= g_fb_count) {
        return false;
    }
    const file_entry_t *e = &g_fb_entries[idx];
    if (e->type == FB_TYPE_DIR) {
        fb_enter_dir(e->name);
        state->selected_index = 0;
        state->scroll_offset = 0;
        state->needs_redraw = true;
        return true;
    }
    if (e->type == FB_TYPE_GAM) {
        /* 拼完整路径 */
        char full[256];
        snprintf(full, sizeof(full), "%s/%s", g_fb_path, e->name);
        launch_gam(state, full);
        state->needs_redraw = true;
        return true;
    }
    /* 其它文件: 提示不支持 */
    state->confirm_active = true;
    state->confirm_notice = true;  /* 通知, 任意键关闭 */
    snprintf(state->confirm_title, sizeof(state->confirm_title), "无法打开");
    /* 截短文件名以防截断警告 (msg 64 字节) */
    char shortname[40];
    safe_filename(shortname, sizeof(shortname), e->name);
    snprintf(state->confirm_msg, sizeof(state->confirm_msg),
             "%s ...", shortname);
    state->needs_redraw = true;
    return true;
}

bool fb_on_back(menu_state_t *state) {
    if (strcmp(g_fb_path, "/sdcard") != 0) {
        /* 非根: 返回上一级 */
        fb_parent_dir();
        state->selected_index = 0;
        state->scroll_offset = 0;
        state->needs_redraw = true;
        return true;  /* 已处理, 不进入默认回主菜单 */
    }
    /* 根: 返回主菜单 */
    state->current_page = MENU_PAGE_MAIN;
    state->selected_index = 0;
    state->scroll_offset = 0;
    state->needs_redraw = true;
    return true;
}

/* === 弹窗化文件浏览器 (list_dialog 弹窗样式) ===
 * 用户需求: 浏览文件使用 list_dialog 弹窗样式.
 * 流程:
 *   - fb_open_dialog: 重置 + 打开弹窗
 *   - 弹窗内 选中目录 -> 进入 (重建项)
 *           选中文件 -> 启动游戏 (退出游戏后回到弹窗)
 *           选中 "上一级" -> 返回上级
 *           选中 "返回"   -> 关闭弹窗 (通过 on_close 回到 SD 管理)
 *           按 BACK 键   -> 关闭弹窗 (通过 on_close 回到 SD 管理) */

/* 弹窗选中: 处理目录/文件/"上一级"/"返回" */
static void fb_dialog_on_select(menu_state_t *state, int idx);

/* 重新填充弹窗列表 (进入目录/启动游戏/.. 之后调用) */
static void fb_dialog_rebuild(menu_state_t *state) {
    int cnt = fb_build(state, state->list_dialog_items, 16);
    state->list_dialog_count = cnt;
    state->list_dialog_selected = 0;
    state->list_dialog_scroll = 0;
    /* 强制全量重绘, 防止 prev 状态机残留 */
    state->list_dialog_prev_active = false;
    state->list_dialog_prev_selected = -1;
    state->list_dialog_content_dirty = true;
    state->list_dialog_local_update = false;
    /* 标题跟随当前目录: 根时显示"文件浏览", 否则显示当前路径 (截断到 31 字节防溢出) */
    if (strcmp(g_fb_path, "/sdcard") == 0) {
        snprintf(state->list_dialog_title, sizeof(state->list_dialog_title),
                 "%s", "\xe6\x96\x87\xe4\xbb\xb6\xe6\xb5\x8f\xe8\xa7\x88");
    } else {
        /* 截断到 list_dialog_title 容量 - 1, 避免 snprintf 截断警告 */
        size_t maxlen = sizeof(state->list_dialog_title) - 1;
        size_t pathlen = strlen(g_fb_path);
        const char *src = g_fb_path;
        if (pathlen > maxlen) {
            /* 路径太长, 只显示最后 maxlen 个字节 (保留末尾路径) */
            src = g_fb_path + pathlen - maxlen;
        }
        /* 使用精确长度, 避免编译器无法追踪 src 已截断而误报 */
        size_t srclen = strnlen(src, maxlen);
        memcpy(state->list_dialog_title, src, srclen);
        state->list_dialog_title[srclen] = '\0';
    }
    state->needs_redraw = true;
}

/* 关闭弹窗: 跳到 list_dialog_return_page, 触发 on_close (回到 SD 管理弹窗) */
static void fb_dialog_close(menu_state_t *state) {
    state->list_dialog_active = false;
    state->list_dialog_prev_active = false;
    state->list_dialog_prev_selected = -1;
    state->current_page = state->list_dialog_return_page;
    if (state->list_dialog_return_page == MENU_PAGE_MAIN) {
        state->selected_index = state->main_selected_index;
    } else {
        state->selected_index = 0;
    }
    state->scroll_offset = 0;
    state->needs_redraw = true;
    if (state->list_dialog_on_close) {
        void (*cb)(menu_state_t *) = state->list_dialog_on_close;
        state->list_dialog_on_close = NULL;  /* 防递归 */
        cb(state);
    }
}

/* 打开文件浏览器弹窗 */
void fb_open_dialog(menu_state_t *state) {
    /* 重置到 /sdcard 根 (用户每次进入都从根开始) */
    fb_init();
    /* 打开弹窗: 标题用当前目录 (根时显示 "文件浏览") */
    state->list_dialog_return_page = state->current_page;
    /* V1.0.41: 先 menu_open_list_dialog (压栈保存父弹窗 items), 再 fb_build 写入子弹窗 items.
     * list_dialog_open 会自动压栈 SD 管理弹窗, 子弹窗关闭后通过 list_dialog_pop_parent
     * 恢复 SD 管理弹窗(含原选中位置), 实现"弹窗返回上级保持原位置". */
    const char *title = (strcmp(g_fb_path, "/sdcard") == 0)
                        ? "\xe6\x96\x87\xe4\xbb\xb6\xe6\xb5\x8f\xe8\xa7\x88"  /* 文件浏览 */
                        : g_fb_path;
    menu_open_list_dialog(state, title, 0, fb_dialog_on_select);
    int cnt = fb_build(state, state->list_dialog_items, 16);
    state->list_dialog_count = cnt;
    /* V1.0.41: 不再设 on_close, 关闭时由 list_dialog_pop_parent 恢复父弹窗位置 */
    /* 强制全量重绘 (初次打开) */
    state->list_dialog_content_dirty = true;
}

/* 弹窗选中处理 */
static void fb_dialog_on_select(menu_state_t *state, int idx) {
    /* 重新扫描以拿到最新列表 (用户可能在外部分配新文件) */
    fb_rescan();

    int has_parent = (strcmp(g_fb_path, "/sdcard") != 0);
    /* 计算 "返回" 项的 idx 和 "上一级" 项的 idx:
     *   - "返回" 永远在最后: n_total = g_fb_count + (has_parent ? 1 : 0)
     *   - "上一级" 在 has_parent 时位于 n_total - 1
     * 注: fb_build 已经按目录->文件排序, 索引与扫描结果一致 */
    int n_total = g_fb_count + (has_parent ? 1 : 0);

    /* "返回" 项: 关闭弹窗 (on_close 会回到 SD 管理) */
    if (idx == n_total) {
        fb_dialog_close(state);
        return;
    }

    /* "上一级" 项: 返回上级目录 (非根时才有) */
    if (has_parent && idx == g_fb_count) {
        fb_parent_dir();
        fb_dialog_rebuild(state);
        return;
    }

    /* 目录/文件 (idx 0..g_fb_count-1) */
    if (idx < 0 || idx >= g_fb_count) {
        return;
    }
    const file_entry_t *e = &g_fb_entries[idx];
    if (e->type == FB_TYPE_DIR) {
        fb_enter_dir(e->name);
        fb_dialog_rebuild(state);
        return;
    }
    if (e->type == FB_TYPE_GAM) {
        char full[256];
        snprintf(full, sizeof(full), "%s/%s", g_fb_path, e->name);
        launch_gam(state, full);
        /* 启动游戏后: 游戏退出回到这里, 重新扫描并重建弹窗项 */
        fb_dialog_rebuild(state);
        return;
    }
    /* 其它文件: 提示不支持 (用 confirm 弹窗即可) */
    state->confirm_active = true;
    state->confirm_notice = true;  /* 通知, 任意键关闭 */
    snprintf(state->confirm_title, sizeof(state->confirm_title), "\xe6\x97\xa0\xe6\xb3\x95\xe6\x89\x93\xe5\xbc\x80");
    char shortname[40];
    safe_filename(shortname, sizeof(shortname), e->name);
    snprintf(state->confirm_msg, sizeof(state->confirm_msg),
             "%s ...", shortname);
    state->needs_redraw = true;
}
