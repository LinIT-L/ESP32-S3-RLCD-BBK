/* test_md_savestate.cpp — MD 存档包装层主机测试
 *
 * 测的是**生产源码** components/engines/md_emu/md_savestate.cpp。
 * 内核读写 (save_genesis / load_genesis) 在这里用桩替代 —— 它们在生产里本来就是
 * extern "C" 声明, 真实实现在 genesis.cpp。
 *
 * 能证明什么:
 *   · 存档**字节流由内核决定、包装层一个字节没改** (格式兼容性)
 *   · "先写 .tmp → 512B 门槛 → os_file_replace 三段替换" 的完整分支
 *   · 旧档在任何失败路径下都不被破坏
 *   · 加载前的 .old 复位、.tmp 不提升
 *
 * 不能证明什么:
 *   · gwenesis_save_state() 本身写的字节是否正确 (内核行为, 需真机/上游)
 *   · 真机 FatFs 的 rename 语义 (主机 POSIX 会覆盖, FatFs 返回 FR_EXIST)
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

#include "os_file.h"

/* 生产文件里的 extern 声明 */
extern "C" void md_emu_savestate_load(const char *path);
extern "C" void md_emu_savestate_save(const char *path);

/* ---------------- 内核桩 ---------------- */

static long g_core_bytes = 4096;      /* 下一次 save_genesis 写多少字节 */
static bool g_core_write_file = true; /* false = 完全不创建文件 (模拟 fopen 失败) */
static int  g_save_calls = 0;
static int  g_load_calls = 0;
static char g_load_path[512] = {0};

extern "C" void save_genesis(std::string_view save_path)
{
    g_save_calls++;
    if (!g_core_write_file) return;
    std::string p(save_path);
    FILE *f = fopen(p.c_str(), "wb");
    if (!f) return;
    std::string data((size_t)g_core_bytes, 'G');
    if (!data.empty()) (void)fwrite(data.data(), 1, data.size(), f);
    (void)fclose(f);
}

extern "C" void load_genesis(std::string_view save_path)
{
    g_load_calls++;
    snprintf(g_load_path, sizeof g_load_path, "%.*s",
             (int)save_path.size(), save_path.data());
}

/* ---------------- 断言 ---------------- */

static int g_pass = 0, g_fail = 0;

#define CK(cond) do { \
        if (cond) g_pass++; \
        else { g_fail++; printf("  FAIL %s:%d  %s\n", __func__, __LINE__, #cond); } \
    } while (0)

enum { BASE_LEN = 26 };
static const char *BASE = "/tmp/lintos_emu_save_test";

static std::string P(const char *name) { return std::string(BASE) + "/" + name; }

static bool exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }
static long fsize(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 ? (long)st.st_size : -1; }

static void wfile(const std::string &p, const char *data, size_t n)
{
    FILE *f = fopen(p.c_str(), "wb");
    if (!f) return;
    if (n) (void)fwrite(data, 1, n, f);
    (void)fclose(f);
}

/* 全部字节是否都是 ch */
static bool all_byte(const std::string &p, char ch)
{
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return false;
    int c;
    bool ok = true;
    while ((c = fgetc(f)) != EOF)
        if ((char)c != ch) { ok = false; break; }
    (void)fclose(f);
    return ok;
}

static void clean()
{
    std::string cmd = std::string("rm -rf ") + BASE + " && mkdir -p " + BASE;
    if (system(cmd.c_str()) != 0) printf("  !! 清理失败\n");
}

int main()
{
    printf("MD 存档包装层测试 (生产源码 md_savestate.cpp)\n");
    (void)BASE_LEN;

    printf("\n== M1 正常保存: 字节流原样透传 (格式不被改写) ==\n");
    clean();
    {
        std::string f = P("a.sav");
        g_core_bytes = 4096; g_core_write_file = true; g_save_calls = 0;
        md_emu_savestate_save(f.c_str());
        CK(g_save_calls == 1);
        CK(exists(f));
        CK(fsize(f) == 4096L);
        CK(all_byte(f, 'G'));                    /* 内核写什么就是什么 */
        CK(!exists(f + ".tmp"));
        CK(!exists(f + ".old"));
    }

    printf("\n== M2 覆盖旧存档: 新档生效, .old 清理干净 ==\n");
    clean();
    {
        std::string f = P("b.sav");
        wfile(f, "OLDDATA", 7);
        g_core_bytes = 2048;
        md_emu_savestate_save(f.c_str());
        CK(fsize(f) == 2048L);
        CK(all_byte(f, 'G'));
        CK(!exists(f + ".old"));
        CK(!exists(f + ".tmp"));
    }

    printf("\n== M3 内核完全没写文件 → 门槛挡住, 旧档不变 ==\n");
    clean();
    {
        std::string f = P("c.sav");
        wfile(f, "PRECIOUS", 8);
        g_core_write_file = false;
        md_emu_savestate_save(f.c_str());
        CK(fsize(f) == 8L);
        CK(exists(f));
        CK(!exists(f + ".tmp"));
        CK(!exists(f + ".old"));
        g_core_write_file = true;
    }

    printf("\n== M4 写得太少 (<512B) → 门槛挡住, 旧档不变 ==\n");
    clean();
    {
        std::string f = P("d.sav");
        wfile(f, "PRECIOUS", 8);
        g_core_bytes = 100;
        md_emu_savestate_save(f.c_str());
        CK(fsize(f) == 8L);
        CK(!exists(f + ".tmp"));
        /* 首次落盘 (无旧档) 且写得太少 → 不产生正式档 */
        std::string g = P("d2.sav");
        md_emu_savestate_save(g.c_str());
        CK(!exists(g));
    }

    printf("\n== M5 512B 门槛边界 ==\n");
    clean();
    {
        std::string a = P("e511.sav"), b = P("e512.sav");
        g_core_bytes = 511;
        md_emu_savestate_save(a.c_str());
        CK(!exists(a));                          /* 511 < 512 ⇒ 拒绝 */

        g_core_bytes = 512;
        md_emu_savestate_save(b.c_str());
        CK(exists(b) && fsize(b) == 512L);       /* 512 ⇒ 通过 */
    }

    printf("\n== M6 残留 .tmp 被覆盖 (不接在后面写) ==\n");
    clean();
    {
        std::string f = P("f.sav");
        wfile(f + ".tmp", "GARBAGE-GARBAGE", 15);
        g_core_bytes = 1024;
        md_emu_savestate_save(f.c_str());
        CK(fsize(f) == 1024L);
        CK(all_byte(f, 'G'));                    /* 不是"GARBAGE"+新数据 */
        CK(!exists(f + ".tmp"));
    }

    printf("\n== M7 重复保存 10 次: 无残留、内容稳定 ==\n");
    clean();
    {
        std::string f = P("g.sav");
        bool ok = true;
        for (int i = 0; i < 10; i++) {
            g_core_bytes = 700 + i;
            md_emu_savestate_save(f.c_str());
            if (fsize(f) != 700 + i) { ok = false; break; }
            if (exists(f + ".tmp") || exists(f + ".old")) { ok = false; break; }
        }
        CK(ok);
        CK(all_byte(f, 'G'));
    }

    printf("\n== M8 加载前 .old 复位 (掉电恢复) ==\n");
    clean();
    {
        std::string f = P("h.sav");
        wfile(f + ".old", "RECOVERED", 9);       /* 正式档缺失, 只有 .old */
        g_load_calls = 0; g_load_path[0] = 0;
        md_emu_savestate_load(f.c_str());
        CK(g_load_calls == 1);
        CK(exists(f));                           /* 已复位 */
        CK(!exists(f + ".old"));
        CK(fsize(f) == 9L);
        CK(strcmp(g_load_path, f.c_str()) == 0); /* 交给内核的是正式路径 */
    }

    printf("\n== M9 加载时 .tmp 残留 → 不提升, 交给内核的是正式路径 ==\n");
    clean();
    {
        std::string f = P("i.sav");
        wfile(f + ".tmp", "HALF-WRITTEN", 12);
        g_load_calls = 0; g_load_path[0] = 0;
        md_emu_savestate_load(f.c_str());
        CK(g_load_calls == 1);
        CK(!exists(f));                          /* 半截数据绝不变成正式档 */
        CK(exists(f + ".tmp"));
        CK(strcmp(g_load_path, f.c_str()) == 0);
    }

    printf("\n== M10 入参守卫 ==\n");
    clean();
    {
        g_save_calls = 0; g_load_calls = 0;
        md_emu_savestate_save(nullptr);
        md_emu_savestate_save("");
        md_emu_savestate_load(nullptr);
        md_emu_savestate_load("");
        CK(g_save_calls == 0);
        CK(g_load_calls == 0);
    }

    printf("\n== M11 首次落盘 (无任何旧档) ==\n");
    clean();
    {
        std::string f = P("j.sav");
        g_core_bytes = 8192;
        md_emu_savestate_save(f.c_str());
        CK(fsize(f) == 8192L);
        CK(!exists(f + ".tmp") && !exists(f + ".old"));
    }

    printf("\n---------------------------------------------\n");
    printf("PASS=%d  FAIL=%d  合计=%d\n", g_pass, g_fail, g_pass + g_fail);
    printf("%s\n", g_fail ? "结果: FAILED" : "结果: ALL PASS");

    clean();
    return g_fail ? 1 : 0;
}
