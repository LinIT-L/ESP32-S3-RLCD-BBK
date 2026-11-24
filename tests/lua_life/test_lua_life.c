/* test_lua_life.c — Lua 桥生命周期契约测试
 *
 * 测的是**生产源码** components/apps/lua_bridge/lua_bridge.c:
 *   · k_mods[]      —— 18 个模块注册函数的调度 (lua_bridge_setup)
 *   · k_app_resets[] —— 6 个"应用退出回收"钩子的调度 (lua_bridge_reset_app_state)
 *
 * 每个模块函数在这里被替换成记录调用次序的桩。生产文件不改一行。
 *
 * 为什么值得测: 第2/4回合新增的 k_app_resets 是**唯一**保证"应用退出后 C 侧
 * 跨应用状态被回收"的地方 (位图 PSRAM / 定时器槽 / 弹窗结果 / 按键重复表 /
 * 128KB fetch 缓冲)。若有人删掉其中一行或改错顺序, 表现是"下一个应用莫名
 * 继承了上一个应用的状态", 极难复现 —— 本测试把它变成编译期/运行期门禁。
 *
 * ⚠ 能力边界: 桩只验证"调度到了、次数对、顺序对"。各 reset 函数**内部**是否
 *   真的把资源还回去, 由各模块自身的实现决定, 本测试不覆盖 (真机项)。
 */
#include "lua_bridge.h"

#include <stdio.h>
#include <string.h>

/* ---------------- 记录桩 ---------------- */

#define MAX_EVENTS 128
static char g_ev[MAX_EVENTS][32];
static int  g_ev_n = 0;

static void rec(const char *name)
{
    if (g_ev_n < MAX_EVENTS) {
        snprintf(g_ev[g_ev_n], sizeof g_ev[0], "%s", name);
        g_ev_n++;
    }
}

static void clear_ev(void) { g_ev_n = 0; }

/* ---------------- 18 个模块注册桩 ---------------- */

#define DEF_MOD(name) void lua_mod_##name##_register(lua_State *L) { (void)L; rec("mod:" #name); }
DEF_MOD(consts)
DEF_MOD(gfx)
DEF_MOD(input)
DEF_MOD(sys)
DEF_MOD(fs)
DEF_MOD(store)
DEF_MOD(net)
DEF_MOD(json)
DEF_MOD(wifi)
DEF_MOD(hid)
DEF_MOD(kbd)
DEF_MOD(img)
DEF_MOD(pkg)
DEF_MOD(gps)
DEF_MOD(espnow)
DEF_MOD(wiki)
DEF_MOD(util)
DEF_MOD(rec)
#undef DEF_MOD

/* ---------------- 6 个应用退出回收桩 ---------------- */

void lua_img_reset_all(void)   { rec("reset:img"); }
void lua_pkg_reset_all(void)   { rec("reset:pkg"); }
void lua_util_reset(void)      { rec("reset:util"); }
void lua_sys_reset_app(void)   { rec("reset:sys"); }
void lua_input_reset_app(void) { rec("reset:input"); }
void lua_net_reset_app(void)   { rec("reset:net"); }

/* ---------------- lua API 桩 ---------------- */

void  lua_newtable(lua_State *L) { (void)L; rec("lua:newtable"); }
void  lua_setfield(lua_State *L, int idx, const char *k) { (void)L; (void)idx; (void)k; }
void  lua_setglobal(lua_State *L, const char *name) { (void)L; (void)name; rec("lua:setglobal"); }

/* ---------------- 断言框架 ---------------- */

static int g_pass = 0, g_fail = 0;

#define CK(cond) do { \
        if (cond) g_pass++; \
        else { g_fail++; printf("  FAIL %s:%d  %s\n", __func__, __LINE__, #cond); } \
    } while (0)

static void dump_events(void)
{
    for (int i = 0; i < g_ev_n; i++) printf("    [%02d] %s\n", i, g_ev[i]);
}

/* 断言事件序列恰好等于期望 (顺序 + 内容) */
static void ck_events(const char *what, const char *const *want, int want_n)
{
    int ok = (g_ev_n == want_n);
    for (int i = 0; ok && i < want_n; i++)
        if (strcmp(g_ev[i], want[i]) != 0) ok = 0;
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s: 期望 %d 个事件, 实际 %d 个\n", what, want_n, g_ev_n);
        printf("  --- 期望 ---\n");
        for (int i = 0; i < want_n; i++) printf("    [%02d] %s\n", i, want[i]);
        printf("  --- 实际 ---\n");
        dump_events();
    }
}

int main(void)
{
    printf("Lua 桥生命周期契约测试 (生产源码 lua_bridge.c)\n");

    printf("\n== L1 k_app_resets 调度: 6 个钩子, 顺序固定 ==\n");
    {
        static const char *const want[] = {
            "reset:img", "reset:pkg", "reset:util", "reset:sys", "reset:input", "reset:net",
        };
        clear_ev();
        lua_bridge_reset_app_state();
        ck_events("reset_app_state 顺序", want, 6);
    }

    printf("\n== L2 幂等: 连续调用 3 次, 每次都完整执行 6 个 ==\n");
    {
        static const char *const want[] = {
            "reset:img", "reset:pkg", "reset:util", "reset:sys", "reset:input", "reset:net",
            "reset:img", "reset:pkg", "reset:util", "reset:sys", "reset:input", "reset:net",
            "reset:img", "reset:pkg", "reset:util", "reset:sys", "reset:input", "reset:net",
        };
        clear_ev();
        lua_bridge_reset_app_state();
        lua_bridge_reset_app_state();
        lua_bridge_reset_app_state();
        ck_events("3 次调用", want, 18);
    }

    printf("\n== L3 k_mods 调度: 18 个模块注册, 顺序固定 ==\n");
    {
        static const char *const want[] = {
            "lua:newtable",
            "mod:consts", "mod:gfx", "mod:input", "mod:sys", "mod:fs",
            "mod:store", "mod:net", "mod:json", "mod:wifi", "mod:hid",
            "mod:kbd", "mod:img", "mod:pkg", "mod:gps", "mod:espnow", "mod:wiki",
            "mod:util", "mod:rec",
            "lua:setglobal",
        };
        clear_ev();
        lua_bridge_setup(NULL);
        ck_events("setup 序列", want, 20);
    }

    printf("\n== L4 两张表互不串扰 ==\n");
    {
        /* 只调 setup, 不应触发任何 reset; 只调 reset, 不应触发 register */
        clear_ev();
        lua_bridge_setup(NULL);
        int has_reset = 0;
        for (int i = 0; i < g_ev_n; i++)
            if (strncmp(g_ev[i], "reset:", 6) == 0) has_reset = 1;
        CK(!has_reset);
        CK(g_ev_n == 20);

        clear_ev();
        lua_bridge_reset_app_state();
        int has_mod = 0;
        for (int i = 0; i < g_ev_n; i++)
            if (strncmp(g_ev[i], "mod:", 4) == 0) has_mod = 1;
        CK(!has_mod);
        CK(g_ev_n == 6);
    }

    printf("\n== L5 reset 表里没有重复项 (同一钩子不重复调用) ==\n");
    {
        clear_ev();
        lua_bridge_reset_app_state();
        int dup = 0;
        for (int i = 0; i < g_ev_n; i++)
            for (int j = i + 1; j < g_ev_n; j++)
                if (strcmp(g_ev[i], g_ev[j]) == 0) dup = 1;
        CK(!dup);
    }

    printf("\n---------------------------------------------\n");
    printf("PASS=%d  FAIL=%d  合计=%d\n", g_pass, g_fail, g_pass + g_fail);
    printf("%s\n", g_fail ? "结果: FAILED" : "结果: ALL PASS");
    return g_fail ? 1 : 0;
}
