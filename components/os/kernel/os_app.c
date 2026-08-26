/**
 * os_app.c — 应用注册表.
 *
 * 统一登记所有"可启动应用" (主菜单/应用管理共用同一张表).
 * 支持运行时注册 (os_app_register), 为 WASM 应用商店下载安装后动态加入主菜单预留.
 * 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include "esp_log.h"
#include <string.h>

#define TAG "OSAPP"

#define OS_APP_MAX 24
static const os_app_t *s_apps[OS_APP_MAX];
static int s_app_count = 0;

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
    ESP_LOGI(TAG, "app +%s (%s)", app->id, app->label);
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
