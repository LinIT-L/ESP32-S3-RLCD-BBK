/* lua_guard_test.c — 路径守卫主机测试（直接编译生产源码 lua_path_guard.c）
 * 编译:
 *   CC=/Library/Developer/CommandLineTools/usr/bin/clang
 *   SDK=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
 *   $CC -isysroot $SDK -O2 -std=c11 -Wall -Wextra -I components/apps/lua_bridge \
 *       tests/lua_guard/lua_guard_test.c components/apps/lua_bridge/lua_path_guard.c -o /tmp/lg
 */
#include "lua_path_guard.h"

#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;

static void expect(const char *in, bool want_write, bool want_ok,
                   const char *want_out, const char *why)
{
    char out[256];
    const char *DATA = "/sdcard/apps/.data/myapp";
    const char *APP  = "/sdcard/apps/app";
    bool ok = lua_path_guard(DATA, APP, in, want_write, out, sizeof(out));
    bool good = (ok == want_ok);
    if (good && want_ok && want_out) good = (strcmp(out, want_out) == 0);
    if (good) { g_pass++; printf("  PASS  %-28s %s\n", in, why); }
    else {
        g_fail++;
        printf("  FAIL  %-28s %s  (ok=%d out='%s')\n", in, why, ok, ok ? out : "");
    }
}

static void expect_id(const char *path, bool want_ok, const char *want)
{
    char id[64];
    bool ok = lua_path_appid(path, id, sizeof(id));
    bool good = (ok == want_ok) && (!want_ok || strcmp(id, want) == 0);
    if (good) { g_pass++; printf("  PASS  appid %-30s -> '%s'\n", path, ok ? id : "(拒绝)"); }
    else { g_fail++; printf("  FAIL  appid %-30s -> ok=%d '%s'\n", path, ok, ok ? id : ""); }
}

int main(void)
{
    printf("=== Lua 路径守卫主机测试（生产源码）===\n\n");

    printf("T1 相对路径落在数据目录\n");
    expect("a.txt", true, true, "/sdcard/apps/.data/myapp/a.txt", "普通相对路径");
    expect("./sub/b.bin", true, true, "/sdcard/apps/.data/myapp/sub/b.bin", "./ 被规整");
    expect("a//b", false, true, "/sdcard/apps/.data/myapp/a/b", "重复斜杠规整");
    printf("\n");

    printf("T2 穿越必须被挡\n");
    expect("..", true, false, NULL, "裸 ..");
    expect("../x", true, false, NULL, "../x");
    expect("a/../../x", true, false, NULL, "多退一级");
    expect("a/../../../etc/passwd", false, false, NULL, "连续越界(读也不行)");
    expect("/sdcard/apps/.data/myapp/../../app/x", false, false, NULL, "绝对+穿越");
    printf("\n");

    printf("T3 合法回退仍可用\n");
    expect("a/../b", true, true, "/sdcard/apps/.data/myapp/b", "a/../b 归一回数据目录");
    printf("\n");

    printf("T4 绝对路径授权边界\n");
    expect("/sdcard/apps/.data/myapp/x", true, true, "/sdcard/apps/.data/myapp/x", "数据目录内可写");
    expect("/sdcard/apps/app/x.lua", false, true, "/sdcard/apps/app/x.lua", "自身目录可读");
    expect("/sdcard/apps/app/x.lua", true, false, NULL, "自身目录不可写");
    expect("/sdcard/apps/.data/other/x", true, false, NULL, "别的应用数据目录");
    expect("/sdcard/fonts/a.lbf", false, false, NULL, "系统目录");
    expect("/store/apps/app/x.lua", true, false, NULL, "商店目录不可写");
    printf("\n");

    printf("T5 前缀攻击（/data/ab 不能冒充 /data/a）\n");
    expect("/sdcard/apps/.data/myappx/x", true, false, NULL, "同前缀不同目录");
    printf("\n");

    printf("T6 边界与空值\n");
    expect("", true, false, NULL, "空串");
    expect("/", false, false, NULL, "只有根");
    {
        char big[512];
        memset(big, 'a', sizeof(big) - 1);
        big[sizeof(big) - 1] = 0;
        char out[64];
        bool ok = lua_path_guard("/sdcard/apps/.data/myapp", "/sdcard/apps/app", big, true, out, sizeof(out));
        if (!ok) { g_pass++; printf("  PASS  超长路径被拒(不截断)\n"); }
        else { g_fail++; printf("  FAIL  超长路径被接受\n"); }
    }
    printf("\n");

    printf("T7 appid 推导\n");
    expect_id("/sdcard/apps/app/calculator.lua", true, "calculator");
    expect_id("my-app_2.lua", true, "my-app_2");
    {   /* 纯中文名: 不能拒绝(商店里大量中文包), 应得到稳定的哈希 id */
        char id[32];
        bool ok = lua_path_appid("/sdcard/apps/app/中文应用.lua", id, sizeof(id));
        char id2[32];
        bool ok2 = lua_path_appid("/sdcard/apps/app/中文应用.lua", id2, sizeof(id2));
        bool ishex = (strlen(id) == 8);
        for (size_t i = 0; ishex && i < 8; i++)
            if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) ishex = false;
        if (ok && ok2 && ishex && strcmp(id, id2) == 0) {
            g_pass++; printf("  PASS  appid 中文名 -> '%s' (稳定 8 位十六进制)\n", id);
        } else {
            g_fail++; printf("  FAIL  appid 中文名 -> ok=%d '%s'\n", ok, ok ? id : "");
        }
    }
    expect_id("/sdcard/apps/app/.lua", false, NULL);
    printf("\n");

    printf("T8 编码/分隔符/等价路径专项\n");
    expect("%2e%2e/x", true, true, "/sdcard/apps/.data/myapp/%2e%2e/x", "URL 编码不解析=普通文件名(无法穿越)");
    expect("..\\..\\x", true, false, NULL, "反斜杠路径: 视为越界段被拒");
    expect("....//x", true, true, "/sdcard/apps/.data/myapp/..../x", ".... 不是 ..(只是普通目录名)");
    expect("a/././b", true, true, "/sdcard/apps/.data/myapp/a/b", "多个 ./ 规整");
    expect("//sdcard//apps//.data//myapp//z", true, true, "/sdcard/apps/.data/myapp/z", "重复分隔符规整");
    expect("a.txt ", true, true, "/sdcard/apps/.data/myapp/a.txt ", "尾部空格原样保留(不当作 ..)");
    expect("/sdcard/apps/.data/myapp", true, true, "/sdcard/apps/.data/myapp", "数据目录本身");
    expect("/sdcard/apps/.data/myapp/", true, true, "/sdcard/apps/.data/myapp", "数据目录带尾斜杠");
    expect("/sdcard/apps/.data", true, false, NULL, "数据目录的父目录不可写");
    expect("/sdcard/apps/.data/", true, false, NULL, ".data 根不可写");
    {
        /* 超长但合法的相对路径(超过内部缓冲) —— 必须拒绝而不是截断成别的路径 */
        char big[400];
        memset(big, 'b', sizeof(big) - 1);
        big[sizeof(big) - 1] = 0;
        char out[256];
        bool ok = lua_path_guard("/sdcard/apps/.data/myapp", NULL, big, true, out, sizeof(out));
        if (!ok) { g_pass++; printf("  PASS  超长(400B)路径被拒, 不截断\n"); }
        else { g_fail++; printf("  FAIL  超长路径被接受\n"); }
    }
    printf("\n");

    printf("T9 沙箱实际传 app_dir=NULL(第三方连自身目录也不放行)\n");
    expect("/sdcard/apps/app/x.lua", false, true, "/sdcard/apps/app/x.lua", "传 NULL 时该用例退化为拒绝");
    printf("\n");

    printf("================ %d PASS / %d FAIL ================\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
