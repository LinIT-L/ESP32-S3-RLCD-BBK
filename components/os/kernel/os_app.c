/**
 * os_app.c — 应用注册表.
 *
 * 统一登记所有"可启动应用" (主菜单/应用管理共用同一张表).
 * 支持运行时注册 (os_app_register), 为 WASM 应用商店下载安装后动态加入主菜单预留.
 * 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include "touch_panel.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

#define TAG "OSAPP"

#define OS_APP_MAX 64
#define APPN_NS "os_apps"
static const os_app_t *s_apps[OS_APP_MAX];
static int s_app_count = 0;
static bool s_hidden_visible = false;

/* 隐藏应用开关 (赞助页 5 连点切换): 打开后主菜单显示隐藏的引擎/应用管理 */
static void hidden_load(void) {
    int32_t v = 0;
    nvs_handle_t h;
    if (nvs_open(APPN_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, "show_hidden", &v) != ESP_OK) v = 0;
        nvs_close(h);
    }
    s_hidden_visible = (v != 0);
}
bool os_app_hidden_visible(void) {
    static bool s_loaded = false;
    if (!s_loaded) { hidden_load(); s_loaded = true; }
    return s_hidden_visible;
}
void os_app_toggle_hidden_visible(void) {
    s_hidden_visible = !s_hidden_visible;
    nvs_handle_t h;
    if (nvs_open(APPN_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "show_hidden", s_hidden_visible ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "隐藏应用: %s", s_hidden_visible ? "显示" : "隐藏");
}
bool os_app_visible(const os_app_t *a) {
    if (!a) return false;
    /* 无触摸屏 → 触摸专用应用 (白板/触屏测试等) 一律隐藏 */
    if (a->touch_only && !touch_panel_is_present()) return false;
    /* 用户显式隐藏 → 不显示 (无论默认) */
    if (os_app_in_hidden(a->id)) return false;
    /* 用户显式加入主菜单 → 显示 (应用管家长按设置, 不受彩蛋开关影响) */
    if (os_app_in_mainmenu(a->id)) return true;
    /* 默认: 非隐藏 直接显示.
     * 彩蛋(赞助页5连点)开关只揭示 reveal 类 (引擎游戏+应用管家),
     * 其余隐藏软件(壁纸/番茄钟/终端等)不随彩蛋显示, 需在应用管家长按调出. */
    return !a->hidden || (os_app_hidden_visible() && a->reveal);
}
int os_app_visible_count(void) {
    int n = 0;
    for (int i = 0; i < s_app_count; i++)
        if (os_app_visible(s_apps[i])) n++;
    return n;
}
const os_app_t *os_app_visible_at(int idx) {
    int n = 0;
    for (int i = 0; i < s_app_count; i++) {
        if (!os_app_visible(s_apps[i])) continue;
        if (n == idx) return s_apps[i];
        n++;
    }
    return NULL;
}

/* ==== 主菜单自定义管理 (应用管家): 用户显式加入/移出主菜单的 id 列表 ====
 * NVS "os_apps/mainmenu" = 逗号分隔的 id 列表 (用户显式"添加到主菜单");
 * NVS "os_apps/mmhide"   = 逗号分隔的 id 列表 (用户显式"移出主菜单").
 * 可见性 = (默认非隐藏 或 在mainmenu) 且 不在 mmhide. */
#define MAINMENU_KEY "mainmenu"
#define MMHIDE_KEY   "mmhide"
#define MAINMENU_MAX 256

/* 读逗号分隔 id 列表, 判断 id 是否在其中 */
static bool id_in_list(const char *key, const char *id) {
    if (!id) return false;
    char buf[MAINMENU_MAX];
    buf[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(APPN_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(buf);
        if (nvs_get_str(h, key, buf, &len) != ESP_OK) buf[0] = '\0';
        nvs_close(h);
    }
    char *p = buf;
    while (*p) {
        char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == strlen(id) && strncmp(p, id, n) == 0) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}
/* 把 id 加入 (show=true) 或移出 (show=false) 某个逗号分隔列表 */
static void id_set_in_list(const char *key, const char *id, bool show) {
    if (!id) return;
    char buf[MAINMENU_MAX];
    buf[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(APPN_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(buf);
        if (nvs_get_str(h, key, buf, &len) != ESP_OK) buf[0] = '\0';
        nvs_close(h);
    }
    bool already = id_in_list(key, id);
    if (show == already) return;
    char out[MAINMENU_MAX];
    out[0] = '\0';
    char *p = buf;
    while (*p) {
        char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (!(n == strlen(id) && strncmp(p, id, n) == 0)) {
            if (out[0]) strncat(out, ",", sizeof(out) - strlen(out) - 1);
            strncat(out, p, sizeof(out) - strlen(out) - 1);
        }
        if (!end) break;
        p = end + 1;
    }
    if (show) {
        if (out[0]) strncat(out, ",", sizeof(out) - strlen(out) - 1);
        strncat(out, id, sizeof(out) - strlen(out) - 1);
    }
    if (nvs_open(APPN_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (out[0]) nvs_set_str(h, key, out);
        else        nvs_erase_key(h, key);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool os_app_in_mainmenu(const char *id) { return id_in_list(MAINMENU_KEY, id); }
void os_app_set_mainmenu(const char *id, bool show) {
    id_set_in_list(MAINMENU_KEY, id, show);
    if (show) id_set_in_list(MMHIDE_KEY, id, false);   /* 加入时清除隐藏标记 */
    ESP_LOGI(TAG, "主菜单 %s: %s", show ? "加入" : "移出", id);
}
bool os_app_in_hidden(const char *id) { return id_in_list(MMHIDE_KEY, id); }
void os_app_set_hidden(const char *id, bool hide) {
    id_set_in_list(MMHIDE_KEY, id, hide);
    if (hide) id_set_in_list(MAINMENU_KEY, id, false);   /* 隐藏时清除加入标记 */
    ESP_LOGI(TAG, "主菜单 %s: %s", hide ? "隐藏" : "恢复", id);
}

void os_app_register(const os_app_t *app)
{
    if (!app || !app->id || s_app_count >= OS_APP_MAX) {
        ESP_LOGW(TAG, "register: bad app or full");
        return;
    }
    /* 同 id 覆盖 (商店更新安装) */
    for (int i = 0; i < s_app_count; i++) {
        if (s_apps[i] && strcmp(s_apps[i]->id, app->id) == 0) {
            s_apps[i] = app;
            ESP_LOGI(TAG, "app %s updated", app->id);
            return;
        }
    }
    s_apps[s_app_count++] = app;
    ESP_LOGI(TAG, "app +%s (%s)%s", app->id, app->label, app->hidden ? " [隐藏]" : "");
}

int os_app_count(void)
{
    return s_app_count;
}

const os_app_t *os_app_get(int idx)
{
    if (idx < 0 || idx >= s_app_count) return NULL;
    return s_apps[idx];
}
