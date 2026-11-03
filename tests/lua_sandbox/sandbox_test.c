/* sandbox_test.c — 第三方 Lua 隔离的主机测试
 *
 * 关键点: **编译的是生产源码**
 *    components/apps/lua_bridge/lua_sandbox.c   ← 被测对象(一字未改)
 *    components/libs/lua/lua/ 下全部 .c         ← 真实 Lua 引擎(与固件同版本 5.5)
 *    仅 esp_log.h / esp_timer.h 用桩替代(平台层, 不是逻辑层)
 *
 * 为什么可以测 L4: 沙箱只操作 Lua 层的表与闭包, 与硬件无关。
 *   我们在 openlibs 之后、apply 之前, 自己往 app 表里塞"探针函数",
 *   探针把收到的参数原样回传 —— 于是能看清包装器究竟把路径/时长改成了什么。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lua_sandbox.h"
#include "lua_path_guard.h"

/* ---------------- 桩时间 ---------------- */
static int64_t g_now = 1000000;          /* 任意起点 */
int64_t esp_timer_get_time(void) { return g_now; }
void stub_timer_set(int64_t us)  { g_now = us; }
void stub_timer_advance(int64_t us) { g_now += us; }

/* ---------------- 断言框架 ---------------- */
static int g_pass, g_fail;
#define CHECK(cond, fmt, ...) do {                                   \
    if (cond) { g_pass++; printf("  PASS  " fmt "\n", ##__VA_ARGS__); } \
    else      { g_fail++; printf("  FAIL  " fmt "\n", ##__VA_ARGS__); } \
} while (0)

/* ---------------- app.* 探针 ---------------- */
/* 每个探针都把"看到的参数"回传成字符串, 便于断言改写结果 */
static int p_arg1(lua_State *L) {                 /* → "arg1" */
    lua_pushvalue(L, 1); return 1;
}
static int p_two(lua_State *L) {                  /* → "a => b" */
    lua_pushfstring(L, "%s => %s", lua_tostring(L, 1), lua_tostring(L, 2));
    return 1;
}
static int p_ms(lua_State *L) {                   /* → nargs, arg1(ms) 或 0 */
    lua_pushinteger(L, lua_gettop(L));
    lua_pushinteger(L, lua_gettop(L) >= 1 && lua_isinteger(L, 1) ? lua_tointeger(L, 1) : -1);
    return 2;
}
static int p_http(lua_State *L) {                 /* → "n=<n> timeout=<arg4>" */
    const int n = lua_gettop(L);
    const long long t = (n >= 4 && lua_isinteger(L, 4)) ? (long long)lua_tointeger(L, 4) : -999;
    char buf[64];                                  /* lua_pushfstring 不支持 %lld */
    snprintf(buf, sizeof(buf), "n=%d timeout=%lld", n, t);
    lua_pushstring(L, buf);
    return 1;
}
static int p_marker(lua_State *L) { (void)L; lua_pushliteral(L, "RAN"); return 1; }

/* 在栈顶的 app 表里放一个子表并塞函数 */
static void set_fn(lua_State *L, const char *sub, const char *name, lua_CFunction f)
{
    lua_getglobal(L, "app");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, sub);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, sub);
    }
    lua_pushcfunction(L, f);
    lua_setfield(L, -2, name);
    lua_pop(L, 2);
}

static void build_mock_app(lua_State *L)
{
    lua_newtable(L);                          /* app */
    lua_setglobal(L, "app");

    set_fn(L, "fs",  "read",    p_arg1);
    set_fn(L, "fs",  "write",   p_arg1);
    set_fn(L, "fs",  "append",  p_arg1);
    set_fn(L, "fs",  "exists",  p_arg1);
    set_fn(L, "fs",  "size",    p_arg1);
    set_fn(L, "fs",  "listdir", p_arg1);
    set_fn(L, "fs",  "read_at", p_arg1);
    set_fn(L, "fs",  "mkdir",   p_arg1);
    set_fn(L, "fs",  "remove",  p_arg1);
    set_fn(L, "fs",  "rename",  p_two);
    set_fn(L, "img", "load",    p_arg1);
    set_fn(L, "hid", "start",   p_marker);
    set_fn(L, "hid", "key",     p_marker);
    set_fn(L, "wiki","open",    p_arg1);
    set_fn(L, "rec", "start",   p_marker);
    set_fn(L, "rec", "play",    p_marker);
    set_fn(L, "util","sleep",   p_ms);
    set_fn(L, "net", "http_get", p_http);
    set_fn(L, "fs2", "x",       p_marker);    /* 占位, 确认没被误伤 */
}

/* 跑一段 Lua, 返回值转成字符串(第一个返回值; 第二个用 '|' 接上) */
static bool run(lua_State *L, const char *code, char *out, size_t cap, char *err, size_t ecap)
{
    out[0] = 0; if (err) err[0] = 0;
    if (luaL_loadstring(L, code) != LUA_OK) {
        if (err) snprintf(err, ecap, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        if (err) snprintf(err, ecap, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    const int n = lua_gettop(L);
    if (n >= 1) {
        const char *s1 = luaL_tolstring(L, 1, NULL);
        snprintf(out, cap, "%s", s1 ? s1 : "?");
        lua_pop(L, 1);
        if (n >= 2) {
            const char *s2 = luaL_tolstring(L, 2, NULL);
            size_t l = strlen(out);
            snprintf(out + l, cap - l, "|%s", s2 ? s2 : "?");
            lua_pop(L, 1);
        }
    }
    lua_settop(L, 0);
    return true;
}

/* 造一个已 openlibs + mock app 的状态 */
static lua_State *new_state(void)
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    build_mock_app(L);
    return L;
}

int main(void)
{
    char v[512], e[512];

    /* ================= T1 信任级判定 ================= */
    printf("T1 信任级由安装路径决定, 不由文件头决定\n");
    CHECK(lua_sandbox_begin("/store/apps/英雄坛说.lua") == true, "官方路径可启动");
    CHECK(lua_sandbox_trust() == LUA_TRUST_OFFICIAL, "→ OFFICIAL");
    CHECK(strcmp(lua_sandbox_ns("core_db"), "core_db") == 0, "官方 app.db 仍是 core_db(行为不变)");
    CHECK(lua_sandbox_mem_limit() == 0, "官方不限内存");
    CHECK(lua_sandbox_remain_ms() == -1, "官方不限时间");

    CHECK(lua_sandbox_begin("/sdcard/apps/tool.lua") == true, "第三方路径可启动");
    CHECK(lua_sandbox_trust() == LUA_TRUST_THIRD_PARTY, "→ THIRD_PARTY");
    CHECK(strcmp(lua_sandbox_appid(), "tool") == 0, "appid=tool");
    CHECK(strcmp(lua_sandbox_data_dir(), "/sdcard/apps/.data/tool") == 0, "数据目录=%s", lua_sandbox_data_dir());
    CHECK(lua_sandbox_mem_limit() == 4096u * 1024u, "配额 4MB");
    CHECK(lua_sandbox_remain_ms() > 9000 && lua_sandbox_remain_ms() <= 10000, "剩余预算≈10s (%d ms)", lua_sandbox_remain_ms());
    printf("\n");

    /* ================= T2 命名空间规则 ================= */
    printf("T2 NVS 命名空间: appdb_<8hex>, 恒 14 字符\n");
    char ns1[32], ns2[32];
    snprintf(ns1, sizeof(ns1), "%s", lua_sandbox_ns("core_db"));
    CHECK(strncmp(ns1, "appdb_", 6) == 0, "前缀 appdb_ (%s)", ns1);
    CHECK(strlen(ns1) == 14, "长度 14 (%zu)", strlen(ns1));
    CHECK(strlen(ns1) < LUA_SANDBOX_NS_MAX, "小于 NVS 上限 %d", LUA_SANDBOX_NS_MAX);
    {
        bool hex = true;
        for (size_t i = 6; i < strlen(ns1); i++)
            if (!((ns1[i] >= '0' && ns1[i] <= '9') || (ns1[i] >= 'a' && ns1[i] <= 'f'))) hex = false;
        CHECK(hex, "主体是 8 位小写十六进制");
    }
    /* 官方 → 原样 (先切回官方状态, 否则这条断言测的是上一个 begin 的第三方态) */
    lua_sandbox_begin("/store/apps/x.lua");
    CHECK(strcmp(lua_sandbox_ns("luaapp"), "luaapp") == 0, "官方 app.nvs → luaapp");
    lua_sandbox_begin("/sdcard/apps/tool.lua");

    /* 不同应用 → 不同命名空间 */
    lua_sandbox_begin("/sdcard/apps/other.lua");
    snprintf(ns2, sizeof(ns2), "%s", lua_sandbox_ns("core_db"));
    CHECK(strcmp(ns1, ns2) != 0, "两个应用命名空间不同 (%s vs %s)", ns1, ns2);

    /* 超长 appid 也不超 15 字符 */
    lua_sandbox_begin("/sdcard/apps/abcdefghijklmnopqrstuvwxyz0123456789abc.lua");
    CHECK(strlen(lua_sandbox_ns("core_db")) == 14, "39 字符 appid → 命名空间仍 14 字符");
    printf("\n");

    /* ================= T3 摘库 ================= */
    printf("T3 第三方摘库(官方不动)\n");
    lua_sandbox_begin("/sdcard/apps/tool.lua");
    lua_State *L = new_state();
    /* apply 前: os.execute 存在 */
    run(L, "return type(os.execute)", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "function") == 0, "apply 前 os.execute 是函数");
    lua_sandbox_apply(L);
    run(L, "return type(os.execute)", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "nil") == 0, "apply 后 os.execute 已移除");
    run(L, "return type(os.exit) .. type(os.tmpname)", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "nilnil") == 0, "os.exit/os.tmpname 已移除");
    run(L, "return type(io.popen) .. type(io.tmpfile)", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "nilnil") == 0, "io.popen/io.tmpfile 已移除");
    run(L, "return type(loadfile) .. type(dofile) .. type(package.loadlib)", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "nilnilnil") == 0, "loadfile/dofile/loadlib 已移除");
    run(L, "return package.path .. '#' .. package.cpath", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "#") == 0, "package.path/cpath 清空");
    /* 保留可用的 */
    run(L, "return type(io.open) .. type(os.remove) .. type(os.rename) .. type(string.format)", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "functionfunctionfunctionfunction") == 0, "io.open/os.remove/os.rename/string 保留(但受限)");
    lua_close(L);
    printf("\n");

    /* ================= T4 模块加载边界 ================= */
    printf("T4 require 边界\n");
    L = new_state();
    lua_sandbox_apply(L);
    CHECK(run(L, "return require('string') ~= nil", v, sizeof(v), e, sizeof(e)),
          "已注册的 C 库仍可 require (%s)", v);
    bool ok = run(L, "return require('lfs')", v, sizeof(v), e, sizeof(e));
    CHECK(!ok, "未知模块 require 失败");
    CHECK(strstr(e, "不能加载模块") != NULL, "错误信息明确: %s", e);
    run(L, "local n=0; for _,f in pairs(package.searchers) do n=n+1 end; return n", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "1") == 0, "searchers 只剩 1 个(拒绝桩), 实际 %s", v);
    run(L, "return type(package.loaded.string)", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "table") == 0, "package.loaded 未被破坏");
    lua_close(L);
    printf("\n");

    /* ================= T5 能力白名单 ================= */
    printf("T5 能力白名单(L4)\n");
    L = new_state();
    lua_sandbox_apply(L);
    run(L, "return app.hid.start()", v, sizeof(v), e, sizeof(e));
    CHECK(strstr(e, "permission denied: app.hid.start") != NULL, "app.hid.start 被拒: %s", e);
    run(L, "return app.hid.key('a')", v, sizeof(v), e, sizeof(e));
    CHECK(strstr(e, "permission denied: app.hid.key") != NULL, "app.hid.key 被拒");
    run(L, "return app.wiki.open('/sdcard/wiki')", v, sizeof(v), e, sizeof(e));
    CHECK(strstr(e, "app.wiki.open") != NULL, "app.wiki.open 被拒 (目录参数=读文件旁路)");
    run(L, "return app.rec.start()", v, sizeof(v), e, sizeof(e));
    CHECK(strstr(e, "app.rec.start") != NULL, "app.rec.start(麦克风)被拒");
    run(L, "return app.rec.play()", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "RAN") == 0, "app.rec.play 仍然可用(不误伤播放)");
    /* 拒绝桩不能让 VM 崩: 被拒之后还能继续跑 */
    run(L, "return 'still alive'", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "still alive") == 0, "被拒之后 VM 仍可继续执行");
    lua_close(L);
    printf("\n");

    /* ================= T6 app.fs.* 路径包装 ================= */
    printf("T6 app.fs.* 也走路径守卫(绕过 io.open 的旁路)\n");
    L = new_state();
    lua_sandbox_apply(L);
    run(L, "return app.fs.write('save.dat')", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "/sdcard/apps/.data/tool/save.dat") == 0, "相对路径落到数据目录: %s", v);
    run(L, "return app.fs.read('/etc/passwd')", v, sizeof(v), e, sizeof(e));
    CHECK(strstr(v, "nil|permission denied") != NULL, "系统绝对路径被拒: %s", v);
    run(L, "return app.fs.read('/sdcard/apps/别的应用.lua')", v, sizeof(v), e, sizeof(e));
    CHECK(strstr(v, "permission denied") != NULL, "读别的应用目录被拒");
    run(L, "return app.fs.write('/sdcard/apps/.data/tool/../other/x')", v, sizeof(v), e, sizeof(e));
    CHECK(strstr(v, "permission denied") != NULL, "绝对路径带 .. 被拒");
    run(L, "return app.fs.remove('/store/apps/a.lua')", v, sizeof(v), e, sizeof(e));
    CHECK(strstr(v, "permission denied") != NULL, "删官方应用被拒");
    run(L, "return app.fs.rename('/etc/a','/etc/b')", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "false") == 0, "rename 双路径被拒返回 false");
    run(L, "return app.img.load('/sdcard/x.bmp')", v, sizeof(v), e, sizeof(e));
    CHECK(strstr(v, "permission denied") != NULL, "app.img.load 也受限");
    run(L, "return app.fs2.x()", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "RAN") == 0, "没被枚举到的子表未被误伤");
    lua_close(L);

    /* 官方状态: 包装一点都不能装 */
    printf("\n");
    printf("T7 OFFICIAL 完全不动\n");
    lua_sandbox_begin("/store/apps/tool.lua");
    L = new_state();
    lua_sandbox_apply(L);                        /* 官方应立即返回 */
    run(L, "return app.fs.read('/etc/hosts')", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "/etc/hosts") == 0, "官方 app.fs.read 路径原样透传: %s", v);
    run(L, "return app.hid.start()", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "RAN") == 0, "官方 app.hid.start 可调用");
    run(L, "return type(os.execute)..type(io.popen)", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "functionfunction") == 0, "官方标准库原样");
    lua_close(L);
    printf("\n");

    /* ================= T8 阻塞 API 夹紧 ================= */
    printf("T8 阻塞时长夹紧(防绕过 hook)\n");
    lua_sandbox_begin("/sdcard/apps/tool.lua");   /* 预算 10s */
    L = new_state();
    lua_sandbox_apply(L);
    run(L, "local n,ms = app.util.sleep(60000); return n..'/'..ms", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "1/60000") != 0, "sleep(60000) 被夹紧 → %s", v);
    CHECK(strncmp(v, "1/1", 3) == 0 || strncmp(v, "2/", 2) == 0, "夹到剩余预算内");
    run(L, "return app.net.http_get('h',80,'/',30000)", v, sizeof(v), e, sizeof(e));
    CHECK(strstr(v, "n=4") != NULL && strstr(v, "timeout=30000") == NULL, "http_get 超时被夹紧 → %s", v);
    run(L, "return app.net.http_get('h',80,'/')", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "n=4 timeout=5000") == 0, "省略超时 → 补默认 5000: %s", v);
    /* 官方不夹紧 */
    lua_sandbox_begin("/store/apps/tool.lua");
    L = new_state();
    lua_sandbox_apply(L);
    run(L, "return app.net.http_get('h',80,'/',30000)", v, sizeof(v), e, sizeof(e));
    CHECK(strcmp(v, "n=4 timeout=30000") == 0, "官方不被夹紧: %s", v);
    lua_close(L);
    printf("\n");

    /* ================= T9 超时钩子 ================= */
    printf("T9 死循环能被 hook 打断\n");
    stub_timer_set(1000000);
    lua_sandbox_begin("/sdcard/apps/tool.lua");   /* deadline = now + 10s */
    L = new_state();
    lua_sandbox_apply(L);
    stub_timer_advance(11000 * 1000);             /* 越过 deadline */
    ok = run(L, "local i=0 while true do i=i+1 end", v, sizeof(v), e, sizeof(e));
    CHECK(!ok && strstr(e, "timeout") != NULL, "死循环被终止: %s", e);
    /* 被打断后 VM 还能用(资源由 Lua 自己回收, 调用方 lua_close) */
    ok = run(L, "return 1+1", v, sizeof(v), e, sizeof(e));
    CHECK(ok && strcmp(v, "2") == 0, "超时后 VM 仍可执行(%s)", v);
    /* 未超时时不应误杀 */
    stub_timer_set(2000000);
    lua_sandbox_begin("/sdcard/apps/tool.lua");
    ok = run(L, "local n=0 for i=1,200000 do n=n+1 end return n", v, sizeof(v), e, sizeof(e));
    CHECK(ok, "正常计算不被误杀 (%s)", v);
    lua_close(L);
    printf("\n");

    printf("==== 合计 %d PASS / %d FAIL ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
