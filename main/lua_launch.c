/* lua_launch.c - Lua 5.5 (espressif/lua) 核心集成, 只做启动&冒烟, 无业务逻辑.
 * app_main 开机调用 lua_smoke_test(): 挂载 /lua(SPIFFS) -> 读 test.lua -> PSRAM 上建 lua_State -> 执行.
 * 仅演示性集成, 失败只打日志, 不影响主系统. */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

static const char *TAG = "LUA";

/* Lua 分配器: 全部从 PSRAM(SPIRAM) 申请, 大块脚本/字符串不占内部 RAM. */
static void *lua_psram_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud; (void)osize;
    if (nsize == 0) { heap_caps_free(ptr); return NULL; }
    return heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM);
}

/* 读取 SPIFFS 上的脚本到 PSRAM */
static char *load_script(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGD(TAG, "无法打开 %s", path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); ESP_LOGD(TAG, "空文件 %s", path); return NULL; }
    char *buf = heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); ESP_LOGD(TAG, "PSRAM 分配 %ldB 失败", sz); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    *out_len = rd;
    return buf;
}

/* ★★★ 2026-10-23 修 (linit: 应用中心看不到内置 Lua 应用):
 *   SPIFFS 挂载原来**只写在 lua_smoke_test() 里面**, 而那个函数在 main.c 已被注释掉
 *   (启动提速"改为按需")⇒ `/store` 从来不存在 ⇒ 打进 apps 分区的 Lua 应用在应用中心
 *   **既没有图标也没有名字**。设备上能看到的那几个 Lua 应用其实全部来自 TF 卡
 *   (`/sdcard/apps/...`), 这就是"新加的应用怎么都刷不出来"的根因。
 *   挂载约 40ms, 开机无条件调用一次即可(幂等)。 */
bool store_fs_mount(void)
{
    static bool s_mounted = false;
    if (s_mounted) return true;

    esp_vfs_spiffs_conf_t conf = {
        .base_path        = "/store",
        .partition_label  = "apps",   /* 分区表里叫 apps (旧名 store) */
        .max_files        = 24,
        .format_if_mount_failed = false,
    };
    esp_err_t e = esp_vfs_spiffs_register(&conf);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPIFFS(/store) 挂载失败: %s", esp_err_to_name(e));
        return false;
    }
    s_mounted = true;
    size_t total = 0, used = 0;
    /* ⚠ esp_spiffs_info 的 label 必须是**分区标签** "apps" (原来写 "spiffs" ⇒ 永远失败) */
    if (esp_spiffs_info("apps", &total, &used) == ESP_OK)
        ESP_LOGI(TAG, "SPIFFS(/store) 已挂载: total=%uK used=%uK",
                 (unsigned)(total / 1024), (unsigned)(used / 1024));
    else
        ESP_LOGI(TAG, "SPIFFS(/store) 已挂载 (info 不可用)");
    return true;
}

void lua_smoke_test(void)
{
    ESP_LOGI(TAG, "=== Lua smoke test start ===");

    if (!store_fs_mount()) return;

    size_t len = 0;
    char *script = load_script("/store/test.lua", &len);
    if (!script) return;
    ESP_LOGI(TAG, "已读取 test.lua (%u 字节)", (unsigned)len);

    lua_State *L = lua_newstate(lua_psram_alloc, NULL, (unsigned)esp_timer_get_time());
    if (!L) { ESP_LOGD(TAG, "lua_newstate 失败"); return; }
    luaL_openlibs(L);   /* 打开全部标准库 */

    /* 标准库 print 走 stdout -> ESP 串口/USB console, 可直接看到打印 */
    int rc = luaL_loadbuffer(L, script, len, "@test.lua");
    if (rc == LUA_OK) rc = lua_pcall(L, 0, 0, 0);
    if (rc != LUA_OK) {
        ESP_LOGD(TAG, "Lua 运行出错: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_close(L);
    ESP_LOGI(TAG, "=== Lua smoke test done ===");
}
