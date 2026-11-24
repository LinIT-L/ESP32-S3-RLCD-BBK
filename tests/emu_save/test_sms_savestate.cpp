/* test_sms_savestate.cpp — SMS 存档包装层主机测试
 *
 * 测的是**生产源码** components/engines/sms_emu/sms_cbridge.cpp 的
 * sms_emu_savestate_save / sms_emu_savestate_load。
 * 内核读写 (save_sms / load_sms) 用桩替代; sms.hpp 由 tests/emu_save/stub 覆盖。
 *
 * 用例与 MD 那份**逐条对等** —— 两个引擎的存档策略必须一致 (同为 512B 门槛 +
 * os_file_replace 三段替换), 若有一边被改动而另一边没跟上, 这里会不对称地红。
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>
#include <span>
#include <sys/stat.h>

#include "os_file.h"

extern "C" void sms_emu_savestate_load(const char *path);
extern "C" void sms_emu_savestate_save(const char *path);

/* ---------------- 内核桩 ---------------- */

static long g_core_bytes = 4096;
static bool g_core_write_file = true;
static int  g_save_calls = 0;
static int  g_load_calls = 0;
static char g_load_path[512] = {0};

void save_sms(std::string_view save_path)
{
    g_save_calls++;
    if (!g_core_write_file) return;
    std::string p(save_path);
    FILE *f = fopen(p.c_str(), "wb");
    if (!f) return;
    std::string data((size_t)g_core_bytes, 'S');
    if (!data.empty()) (void)fwrite(data.data(), 1, data.size(), f);
    (void)fclose(f);
}

void load_sms(std::string_view save_path)
{
    g_load_calls++;
    snprintf(g_load_path, sizeof g_load_path, "%.*s",
             (int)save_path.size(), save_path.data());
}

/* sms_cbridge.cpp 里的 sms_video_rgb565 会调它; 本测试不使用, 只需能链接 */
std::span<uint8_t> get_sms_video_buffer() { return {}; }

/* ---------------- 断言 ---------------- */

static int g_pass = 0, g_fail = 0;

#define CK(cond) do { \
        if (cond) g_pass++; \
        else { g_fail++; printf("  FAIL %s:%d  %s\n", __func__, __LINE__, #cond); } \
    } while (0)

static const char *BASE = "/tmp/lintos_emu_save_sms";

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
    printf("SMS 存档包装层测试 (生产源码 sms_cbridge.cpp)\n");

    printf("\n== S1 正常保存: 字节流原样透传 ==\n");
    clean();
    {
        std::string f = P("a.sav");
        g_core_bytes = 4096; g_core_write_file = true; g_save_calls = 0;
        sms_emu_savestate_save(f.c_str());
        CK(g_save_calls == 1);
        CK(fsize(f) == 4096L);
        CK(all_byte(f, 'S'));
        CK(!exists(f + ".tmp") && !exists(f + ".old"));
    }

    printf("\n== S2 覆盖旧存档 ==\n");
    clean();
    {
        std::string f = P("b.sav");
        wfile(f, "OLDDATA", 7);
        g_core_bytes = 2048;
        sms_emu_savestate_save(f.c_str());
        CK(fsize(f) == 2048L);
        CK(all_byte(f, 'S'));
        CK(!exists(f + ".old") && !exists(f + ".tmp"));
    }

    printf("\n== S3 内核没写文件 → 旧档不变 ==\n");
    clean();
    {
        std::string f = P("c.sav");
        wfile(f, "PRECIOUS", 8);
        g_core_write_file = false;
        sms_emu_savestate_save(f.c_str());
        CK(fsize(f) == 8L);
        CK(!exists(f + ".tmp") && !exists(f + ".old"));
        g_core_write_file = true;
    }

    printf("\n== S4 写得太少 (<512B) → 旧档不变 / 不产生正式档 ==\n");
    clean();
    {
        std::string f = P("d.sav"), g = P("d2.sav");
        wfile(f, "PRECIOUS", 8);
        g_core_bytes = 100;
        sms_emu_savestate_save(f.c_str());
        CK(fsize(f) == 8L);
        sms_emu_savestate_save(g.c_str());
        CK(!exists(g));
    }

    printf("\n== S5 512B 门槛边界 ==\n");
    clean();
    {
        std::string a = P("e511.sav"), b = P("e512.sav");
        g_core_bytes = 511;
        sms_emu_savestate_save(a.c_str());
        CK(!exists(a));
        g_core_bytes = 512;
        sms_emu_savestate_save(b.c_str());
        CK(exists(b) && fsize(b) == 512L);
    }

    printf("\n== S6 残留 .tmp 被覆盖 ==\n");
    clean();
    {
        std::string f = P("f.sav");
        wfile(f + ".tmp", "GARBAGE-GARBAGE", 15);
        g_core_bytes = 1024;
        sms_emu_savestate_save(f.c_str());
        CK(fsize(f) == 1024L);
        CK(all_byte(f, 'S'));
    }

    printf("\n== S7 重复保存 10 次 ==\n");
    clean();
    {
        std::string f = P("g.sav");
        bool ok = true;
        for (int i = 0; i < 10; i++) {
            g_core_bytes = 700 + i;
            sms_emu_savestate_save(f.c_str());
            if (fsize(f) != 700 + i) { ok = false; break; }
            if (exists(f + ".tmp") || exists(f + ".old")) { ok = false; break; }
        }
        CK(ok);
    }

    printf("\n== S8 加载前 .old 复位 ==\n");
    clean();
    {
        std::string f = P("h.sav");
        wfile(f + ".old", "RECOVERED", 9);
        g_load_calls = 0; g_load_path[0] = 0;
        sms_emu_savestate_load(f.c_str());
        CK(g_load_calls == 1);
        CK(exists(f) && !exists(f + ".old"));
        CK(fsize(f) == 9L);
        CK(strcmp(g_load_path, f.c_str()) == 0);
    }

    printf("\n== S9 加载时 .tmp 残留 → 不提升 ==\n");
    clean();
    {
        std::string f = P("i.sav");
        wfile(f + ".tmp", "HALF-WRITTEN", 12);
        g_load_calls = 0; g_load_path[0] = 0;
        sms_emu_savestate_load(f.c_str());
        CK(g_load_calls == 1);
        CK(!exists(f));
        CK(exists(f + ".tmp"));
        CK(strcmp(g_load_path, f.c_str()) == 0);
    }

    printf("\n== S10 入参守卫 ==\n");
    clean();
    {
        g_save_calls = 0; g_load_calls = 0;
        sms_emu_savestate_save(nullptr);
        sms_emu_savestate_save("");
        sms_emu_savestate_load(nullptr);
        sms_emu_savestate_load("");
        CK(g_save_calls == 0 && g_load_calls == 0);
    }

    printf("\n== S11 首次落盘 ==\n");
    clean();
    {
        std::string f = P("j.sav");
        g_core_bytes = 8192;
        sms_emu_savestate_save(f.c_str());
        CK(fsize(f) == 8192L);
        CK(!exists(f + ".tmp") && !exists(f + ".old"));
    }

    printf("\n---------------------------------------------\n");
    printf("PASS=%d  FAIL=%d  合计=%d\n", g_pass, g_fail, g_pass + g_fail);
    printf("%s\n", g_fail ? "结果: FAILED" : "结果: ALL PASS");

    clean();
    return g_fail ? 1 : 0;
}
