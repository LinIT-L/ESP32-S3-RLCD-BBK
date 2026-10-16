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

void lua_smoke_test(void)
{
    ESP_LOGI(TAG, "=== Lua smoke test start ===");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/store",
        .partition_label = "apps",   /* 分区已由 store 改名为 apps */
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t e = esp_vfs_spiffs_register(&conf);
    if (e != ESP_OK) { ESP_LOGD(TAG, "SPIFFS 挂载失败: %s", esp_err_to_name(e)); return; }
    size_t total = 0, used = 0;
    if (esp_spiffs_info("spiffs", &total, &used) == ESP_OK)
        ESP_LOGI(TAG, "SPIFFS total=%uB used=%uB", (unsigned)total, (unsigned)used);

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
