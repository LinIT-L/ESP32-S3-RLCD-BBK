/*
 * test_install.c — 安装器（am_lpkg.c）的宿主机行为测试
 *
 * 与 tests/store_state 的区别：那边测**判定**（纯逻辑），这边测**动作**
 * —— 真写文件、真改名、真回滚、真从磁盘恢复。所以它能回答"是不是只做了代码走查"。
 *
 * 做法：把 am_lpkg.c 的安装根用 -DLPKG_ROOT 指到临时目录，编译**同一份生产源码**。
 * 每个用例由 run_all.sh 起一个独立进程（进程等于设备重启后的冷启动状态），
 * 用例之间由脚本 rm -rf 清根目录。
 *
 * 覆盖（对应本轮要求）：
 *   install_fresh   首次安装：文件到位、双版本记录、数据目录、注册条目
 *   install_nometa  手动安装（无商店版本）⇒ store_* 回落为包内版本，不该报 mismatch
 *   update          更新：内容换成新版、双版本都更新、data 保留、无残留 .old
 *   update_fail_reg 记录写失败 ⇒ 回滚（文件回到旧版、记录仍是旧版）
 *   update_fail_pkg 包坏了 ⇒ 拒绝，旧版本与记录完好
 *   powerloss       模拟"旧目录已让位、新目录未就位"的掉电现场 ⇒ 自愈 + 可继续更新
 *   recover         冷启动再读一遍：双版本能恢复（脚本在 update 后单独起进程）
 *   legacy          老格式记录（无 store_*）⇒ 仍判已安装、版本回落
 *   legacy_noreg    只有 installed/ 目录、没有 registry ⇒ 判已安装
 *   corrupt_reg     记录损坏 ⇒ 判已安装，版本取包内 manifest
 *   dots            app_id 含多个点 ⇒ 主干不截断
 *   stemof          am_lpkg_stem_of() 的边界（有无扩展名/隐藏文件/多段点）
 *
 * 用法: test_install <case> <root> [args...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

#include "am_lpkg.h"
#include "am_store_state.h"

/* ---------------- 断言 ---------------- */
static int g_ok = 0, g_fail = 0;

#define CHECK(cond, fmt, ...)                                              \
    do {                                                                   \
        if (cond) { g_ok++; }                                              \
        else { g_fail++; printf("  \xE2\x9C\x97 " fmt "\n", ##__VA_ARGS__); } \
    } while (0)

#define CHECK_STR(a, b, what)                                              \
    do {                                                                   \
        const char *xa = (a), *xb = (b);                                   \
        if (xa && xb && strcmp(xa, xb) == 0) { g_ok++; }                   \
        else { g_fail++; printf("  \xE2\x9C\x97 %s: 期望 \"%s\" 实得 \"%s\"\n", \
                                what, xb ? xb : "(null)", xa ? xa : "(null)"); } \
    } while (0)

/* ---------------- 小工具 ---------------- */
static void pj(char *out, size_t cap, const char *a, const char *b, const char *c)
{
    snprintf(out, cap, "%s%s%s%s%s", a, b ? "/" : "", b ? b : "", c ? "/" : "", c ? c : "");
}

static bool read_whole(const char *p, char *buf, size_t cap)
{
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return true;
}

static bool write_whole(const char *p, const char *txt)
{
    FILE *f = fopen(p, "wb");
    if (!f) return false;
    fwrite(txt, 1, strlen(txt), f);
    fclose(f);
    return true;
}

static bool exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static bool entry_is(const char *app_id, const char *want)
{
    char p[512], buf[128];
    pj(p, sizeof(p), LPKG_ROOT "/installed", app_id, "app/entry.lua");
    if (!read_whole(p, buf, sizeof(buf))) return false;
    return strstr(buf, want) != NULL;
}

static int count_registry(void)
{
    DIR *d = opendir(LPKG_ROOT "/registry");
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        if (l > 4 && strcmp(nm + l - 4, ".txt") == 0) n++;
    }
    closedir(d);
    return n;
}

static bool dir_exists(const char *a, const char *b)
{
    char p[512];
    pj(p, sizeof(p), a, b, NULL);
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ---------------- 用例 ---------------- */

/* 首次安装：文件、双版本、数据目录、注册 */
static void case_install_fresh(const char *lpk)
{
    char why[160] = {0};
    esp_err_t e = am_lpkg_install_src(lpk, "3", "202611011048", why, sizeof(why));
    CHECK(e == ESP_OK, "安装应成功, 实得 %s (%s)", esp_err_to_name(e), why);

    am_lpkg_info_t inf;
    CHECK(am_lpkg_info("com.test.app", &inf), "安装后应判为已安装");
    CHECK_STR(inf.ver,   "3",          "商店版本");
    CHECK_STR(inf.build, "202611011048", "商店构建");
    CHECK_STR(inf.mf_ver,   "1",          "包内版本");
    CHECK_STR(inf.mf_build, "202610010000", "包内构建");
    CHECK(inf.meta_mismatch, "商店版本≠包内版本 ⇒ 必须置 mismatch");
    CHECK(inf.registry_ok && inf.dir_ok, "registry 与目录都应可读");

    CHECK(entry_is("com.test.app", "V1"), "入口文件应为 v1 内容");
    CHECK(dir_exists(LPKG_ROOT "/data", "com.test.app"), "数据目录应已创建");
    CHECK(count_registry() == 1, "应只有一条注册记录, 实得 %d", count_registry());

    /* 装完不应该在 staging 留垃圾 */
    CHECK(!dir_exists(LPKG_ROOT "/staging", "com.test.app.new"), "staging/.new 应被清掉");
    CHECK(!dir_exists(LPKG_ROOT "/staging", "com.test.app.old"), "staging/.old 应被清掉");
}

/* 手动安装（无商店版本）⇒ store_* 回落成包内版本，不该报不一致 */
static void case_install_nometa(const char *lpk)
{
    char why[160] = {0};
    esp_err_t e = am_lpkg_install(lpk, why, sizeof(why));
    CHECK(e == ESP_OK, "安装应成功: %s", why);
    am_lpkg_info_t inf;
    CHECK(am_lpkg_info("com.test.app", &inf), "应已安装");
    CHECK_STR(inf.ver, "1", "没有商店版本时回落为包内版本");
    CHECK_STR(inf.build, "202610010000", "build 也应回落");
    CHECK(!inf.meta_mismatch, "没有商店版本 ⇒ 不该报 mismatch");
}

/* 更新：内容替换、双版本更新、data 保留、无残留 */
static void case_update(const char *lpkv1, const char *lpkv2)
{
    char why[160] = {0};
    CHECK(am_lpkg_install_src(lpkv1, "1", "202610010000", why, sizeof(why)) == ESP_OK, "先装 v1");
    /* 用户数据：更新绝不能碰 */
    char dm[512];
    pj(dm, sizeof(dm), LPKG_ROOT "/data", "com.test.app", "save.dat");
    CHECK(write_whole(dm, "SAVE"), "写用户数据标记");

    memset(why, 0, sizeof(why));
    esp_err_t e = am_lpkg_install_src(lpkv2, "2", "202611011048", why, sizeof(why));
    CHECK(e == ESP_OK, "更新应成功: %s", why);

    CHECK(entry_is("com.test.app", "V2"), "入口应换成 v2");
    am_lpkg_info_t inf;
    CHECK(am_lpkg_info("com.test.app", &inf), "更新后仍已安装");
    CHECK_STR(inf.ver, "2", "商店版本应更新");
    CHECK_STR(inf.build, "202611011048", "商店构建应更新");
    CHECK_STR(inf.mf_ver, "2", "包内版本应更新");
    CHECK(!inf.meta_mismatch, "两套版本一致 ⇒ 不该报 mismatch");
    CHECK(exists(dm), "用户数据必须保留");
    CHECK(count_registry() == 1, "更新不得产生第二条记录, 实得 %d", count_registry());
    CHECK(!dir_exists(LPKG_ROOT "/staging", "com.test.app.old"), "旧版本目录应被删掉");
}

/* 记录写失败 ⇒ 回滚到旧版本（要求 3：文件装成功、记录写失败） */
static void case_update_fail_reg(const char *lpkv1, const char *lpkv2)
{
    char why[160] = {0};
    CHECK(am_lpkg_install_src(lpkv1, "1", "202610010000", why, sizeof(why)) == ESP_OK, "先装 v1");

    char reg[256];
    snprintf(reg, sizeof(reg), "%s", LPKG_ROOT "/registry");
    chmod(reg, 0555);          /* 让 registry 目录不可写 → 记录写失败 */

    memset(why, 0, sizeof(why));
    esp_err_t e = am_lpkg_install_src(lpkv2, "2", "202611011048", why, sizeof(why));
    chmod(reg, 0755);          /* 复原，后面还要断言/清理 */

    CHECK(e != ESP_OK, "记录写不进去时必须报失败, 实得 %s", esp_err_to_name(e));

    /* 关键：文件必须回到旧版，记录仍是旧版 —— 不能停在"文件新、记录旧" */
    CHECK(entry_is("com.test.app", "V1"), "失败的更新不得留下新版文件");
    am_lpkg_info_t inf;
    CHECK(am_lpkg_info("com.test.app", &inf), "仍应判为已安装");
    CHECK_STR(inf.ver, "1", "记录应仍是旧版");
    CHECK(count_registry() == 1, "不得产生第二条记录, 实得 %d", count_registry());
    CHECK(!dir_exists(LPKG_ROOT "/staging", "com.test.app.old"), "回滚后不应残留 .old");
    CHECK(!dir_exists(LPKG_ROOT "/staging", "com.test.app.new"), "失败后不应残留 .new");
}

/* 包坏了 ⇒ 拒绝且不动旧版本（要求 2：记录在、文件装失败） */
static void case_update_fail_pkg(const char *lpkv1, const char *badlpk)
{
    char why[160] = {0};
    CHECK(am_lpkg_install_src(lpkv1, "1", "202610010000", why, sizeof(why)) == ESP_OK, "先装 v1");

    memset(why, 0, sizeof(why));
    esp_err_t e = am_lpkg_install(badlpk, why, sizeof(why));
    CHECK(e != ESP_OK, "坏包必须被拒绝, 实得 %s", esp_err_to_name(e));
    CHECK(entry_is("com.test.app", "V1"), "旧版本文件必须完好");
    am_lpkg_info_t inf;
    CHECK(am_lpkg_info("com.test.app", &inf) && strcmp(inf.ver, "1") == 0, "记录必须是旧版");
}

/* 掉电现场：installed/ 已让位成 staging/<id>.old，正式目录缺失 */
static void case_powerloss(const char *lpkv1, const char *lpkv2)
{
    char why[160] = {0};
    CHECK(am_lpkg_install_src(lpkv1, "1", "202610010000", why, sizeof(why)) == ESP_OK, "先装 v1");

    char fin[512], old[512], dm[512];
    pj(fin, sizeof(fin), LPKG_ROOT "/installed", "com.test.app", NULL);
    pj(old, sizeof(old), LPKG_ROOT "/staging",   "com.test.app.old", NULL);
    pj(dm,  sizeof(dm),  LPKG_ROOT "/data",      "com.test.app", "save.dat");
    CHECK(write_whole(dm, "SAVE"), "写用户数据");
    mkdir(LPKG_ROOT "/staging", 0755);
    CHECK(rename(fin, old) == 0, "制造掉电现场: installed → staging/.old");

    /* ① 此刻仍必须判为已安装（registry 还在）—— "应用不能凭空消失" */
    am_lpkg_info_t inf;
    CHECK(am_lpkg_info("com.test.app", &inf), "掉电现场仍应判为已安装(靠 registry)");

    /* ② 下一次安装应先把旧目录恢复，再正常更新 */
    memset(why, 0, sizeof(why));
    esp_err_t e = am_lpkg_install_src(lpkv2, "2", "202611011048", why, sizeof(why));
    CHECK(e == ESP_OK, "掉电后应能继续更新: %s", why);
    CHECK(entry_is("com.test.app", "V2"), "更新后应是 v2");
    CHECK(exists(dm), "用户数据必须还在");
    CHECK(!dir_exists(LPKG_ROOT "/staging", "com.test.app.old"), "自愈后不应残留 .old");
}

/* 冷启动恢复：新进程只读，不应看出任何异常 */
static void case_recover(const char *want_store, const char *want_mf)
{
    am_lpkg_info_t inf;
    CHECK(am_lpkg_info("com.test.app", &inf), "冷启动后应判为已安装");
    CHECK_STR(inf.ver,    want_store, "冷启动恢复: 商店版本");
    CHECK_STR(inf.mf_ver, want_mf,    "冷启动恢复: 包内版本");
    CHECK(inf.registry_ok, "冷启动后 registry 应可读");
}

/* 老格式记录：没有 store_* 两行（本轮之前装的） */
static void case_legacy(void)
{
    char dir[512], reg[512];
    pj(dir, sizeof(dir), LPKG_ROOT "/installed", "legacy.app", NULL);
    pj(reg, sizeof(reg), LPKG_ROOT "/registry", "legacy.app", NULL);
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s/app' '%s'", dir, LPKG_ROOT "/registry");
    CHECK(system(cmd) == 0, "造老版本目录");

    char p[600];
    snprintf(p, sizeof(p), "%s/manifest.json", dir);
    write_whole(p, "{\"schema_version\":1,\"app_id\":\"legacy.app\",\"label\":\"旧应用\","
                   "\"category\":\"software\",\"package_type\":\"app\",\"runtime\":\"lua\","
                   "\"entry\":\"app/entry.lua\",\"version\":\"5\",\"build\":\"202501010000\"}");
    snprintf(p, sizeof(p), "%s/app/entry.lua", dir);
    write_whole(p, "-- LEGACY\n");
    snprintf(p, sizeof(p), "%s.txt", reg);
    /* 老记录：只有这两行版本，没有 store_ver/store_build */
    write_whole(p, "app_id=legacy.app\nlabel=旧应用\nversion=5\nbuild=202501010000\n"
                   "package_type=app\nruntime=lua\nentry=app/entry.lua\n");

    am_lpkg_info_t inf;
    CHECK(am_lpkg_info("legacy.app", &inf), "老记录必须仍判为已安装(不能变成未安装)");
    CHECK_STR(inf.ver, "5", "老记录: 版本回落为包内版本");
    CHECK_STR(inf.build, "202501010000", "老记录: build 回落");
    CHECK(!inf.meta_mismatch, "老记录不能报 mismatch（没有两套版本可比）");
}

/* 只有目录、没有 registry（记录被删/写一半掉电） */
static void case_legacy_noreg(void)
{
    char dir[512];
    pj(dir, sizeof(dir), LPKG_ROOT "/installed", "noreg.app", NULL);
    char cmd[900];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s/app'", dir);
    CHECK(system(cmd) == 0, "造目录");
    char p[600];
    snprintf(p, sizeof(p), "%s/manifest.json", dir);
    write_whole(p, "{\"schema_version\":1,\"app_id\":\"noreg.app\",\"label\":\"无记录\","
                   "\"category\":\"software\",\"package_type\":\"app\",\"runtime\":\"lua\","
                   "\"entry\":\"app/entry.lua\",\"version\":\"7\",\"build\":\"202509090000\"}");
    snprintf(p, sizeof(p), "%s/app/entry.lua", dir);
    write_whole(p, "-- NOREG\n");

    am_lpkg_info_t inf;
    CHECK(am_lpkg_info("noreg.app", &inf), "目录在就必须判已安装");
    CHECK_STR(inf.ver, "7", "版本从包内 manifest 补读");
    CHECK(!inf.registry_ok && inf.dir_ok, "应标明 registry 不可用、目录可用");
}

/* 记录损坏（解析不出 app_id） */
static void case_corrupt_reg(void)
{
    char dir[512], reg[512];
    pj(dir, sizeof(dir), LPKG_ROOT "/installed", "broken.app", NULL);
    pj(reg, sizeof(reg), LPKG_ROOT "/registry", "broken.app", NULL);
    char cmd[900];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s/app' '%s'", dir, LPKG_ROOT "/registry");
    CHECK(system(cmd) == 0, "造目录");
    char p[600];
    snprintf(p, sizeof(p), "%s/manifest.json", dir);
    write_whole(p, "{\"schema_version\":1,\"app_id\":\"broken.app\",\"label\":\"坏的\","
                   "\"category\":\"software\",\"package_type\":\"app\",\"runtime\":\"lua\","
                   "\"entry\":\"app/entry.lua\",\"version\":\"9\",\"build\":\"202508080000\"}");
    snprintf(p, sizeof(p), "%s/app/entry.lua", dir);
    write_whole(p, "-- BROKEN\n");
    snprintf(p, sizeof(p), "%s.txt", reg);
    write_whole(p, "version=9\nbuild=202508080000\ngarbage line\n");   /* 没有 app_id */

    am_lpkg_info_t inf;
    CHECK(am_lpkg_info("broken.app", &inf), "记录损坏也不许判成未安装");
    CHECK_STR(inf.ver, "9", "版本从 manifest 补读");
    CHECK(!inf.registry_ok, "应标明 registry 不可用");
}

/* app_id 里多个点：主干不许被截断 */
static void case_dots(const char *lpk)
{
    char why[160] = {0};
    esp_err_t e = am_lpkg_install_src(lpk, "4", "202611011048", why, sizeof(why));
    CHECK(e == ESP_OK, "多点 app_id 应能安装: %s", why);
    am_lpkg_info_t inf;
    CHECK(am_lpkg_info("com.example.deep.nest", &inf), "多点 app_id 应按完整 id 判定已安装");
    CHECK(entry_is("com.example.deep.nest", "V1"), "文件应落在完整 id 目录下");
}

/* 卸载 = 该应用在设备上归零：程序目录/记录清掉，桌面图标归属(mainmenu+mmhide)也清掉。
 * 这里用桩记录两个 set_* 的调用，确认卸载确实把两个列表都置 false ——
 * 否则重装后会出现"删过的图标还是不出来 / 加过的图标又自己回来"。 */
static const char *g_mm_id = NULL;
static int g_mm_show = -1, g_mh_hide = -1;
void os_app_set_mainmenu(const char *id, bool show) { free((void *)g_mm_id); g_mm_id = id ? strdup(id) : NULL; g_mm_show = show ? 1 : 0; }
void os_app_set_hidden(const char *id, bool hide)   { g_mh_hide = hide ? 1 : 0; (void)id; }

static void case_uninstall_clears_desktop(const char *lpk)
{
    char why[160] = {0};
    CHECK(am_lpkg_install_src(lpk, "1", "202610010000", why, sizeof(why)) == ESP_OK, "先装: %s", why);
    g_mm_id = NULL; g_mm_show = -1; g_mh_hide = -1;
    esp_err_t e = am_lpkg_uninstall("com.test.app", true, why, sizeof(why));
    CHECK(e == ESP_OK, "卸载应成功: %s", why);
    CHECK(g_mm_id && strcmp(g_mm_id, "com.test.app") == 0, "卸载要按 app_id 清 mainmenu");
    CHECK(g_mm_show == 0, "卸载要把 mainmenu 置 false（不能保留\"加过\"的状态）");
    CHECK(g_mh_hide == 0, "卸载要把 mmhide 置 false（不能保留\"删过\"的状态）");
    CHECK(!am_lpkg_info("com.test.app", &(am_lpkg_info_t){0}), "卸载后不应再判为已安装");
}

static void case_stemof(void)
{
    char b[64];
    am_lpkg_stem_of("packages/com.lintos.recorder.lpk", b, sizeof(b));
    CHECK_STR(b, "com.lintos.recorder", "stem: 只去最后一个扩展名");
    am_lpkg_stem_of("/sdcard/apps/staging/com.example.a.b.lpk", b, sizeof(b));
    CHECK_STR(b, "com.example.a.b", "stem: 多点");
    am_lpkg_stem_of("pal.lpk", b, sizeof(b));
    CHECK_STR(b, "pal", "stem: 无目录");
    am_lpkg_stem_of("pkgs/font_wqy16.zip", b, sizeof(b));
    CHECK_STR(b, "font_wqy16", "stem: zip");
    am_lpkg_stem_of("noext", b, sizeof(b));
    CHECK_STR(b, "noext", "stem: 无扩展名");
    am_lpkg_stem_of("/a/.hidden", b, sizeof(b));
    CHECK_STR(b, ".hidden", "stem: 隐藏文件不当扩展名");
    am_lpkg_stem_of(NULL, b, sizeof(b));
    CHECK_STR(b, "", "stem: NULL → 空串");
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: %s <case> [args...]\n", argv[0]); return 2; }
    const char *c = argv[1];
    printf("[case] %s\n", c);

    if      (!strcmp(c, "install_fresh"))   case_install_fresh(argv[2]);
    else if (!strcmp(c, "install_nometa"))  case_install_nometa(argv[2]);
    else if (!strcmp(c, "update"))          case_update(argv[2], argv[3]);
    else if (!strcmp(c, "update_fail_reg")) case_update_fail_reg(argv[2], argv[3]);
    else if (!strcmp(c, "update_fail_pkg")) case_update_fail_pkg(argv[2], argv[3]);
    else if (!strcmp(c, "powerloss"))       case_powerloss(argv[2], argv[3]);
    else if (!strcmp(c, "recover"))         case_recover(argv[2], argv[3]);
    else if (!strcmp(c, "legacy"))          case_legacy();
    else if (!strcmp(c, "legacy_noreg"))    case_legacy_noreg();
    else if (!strcmp(c, "corrupt_reg"))     case_corrupt_reg();
    else if (!strcmp(c, "dots"))            case_dots(argv[2]);
    else if (!strcmp(c, "uninst_desktop"))  case_uninstall_clears_desktop(argv[2]);
    else if (!strcmp(c, "stemof"))          case_stemof();
    else { printf("unknown case %s\n", c); return 2; }

    printf("  ---- %d 通过, %d 失败 ----\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}

/* ---- 生产代码需要的桩 ---- */
void os_app_register(const os_app_t *app) { (void)app; }
int  ui_icon_register_extra(const uint8_t *bmp) { (void)bmp; return -1; }
