/**
 * tests/app_core/test_core.c — 统一应用核心（纯逻辑）宿主机测试
 *
 * 被测对象是**生产源码** components/apps/pages/am_core_logic.c
 * （身份归一 / 安装状态 / 列表投影），不抄副本、不 mock。
 *
 * 用法: sh tests/app_core/run_all.sh
 */
#include <stdio.h>
#include <string.h>

#include "am_core_logic.h"

static int g_ok = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_ok++; } \
    else { g_fail++; printf("  ✗ %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

#define CHECK_STR(got, want, msg) do { \
    const char *_g = (got); const char *_w = (want); \
    if (_g && strcmp(_g, _w) == 0) { g_ok++; } \
    else { g_fail++; printf("  ✗ %s  期望 \"%s\" 实得 \"%s\"  (%s:%d)\n", \
                            msg, _w, _g ? _g : "(null)", __FILE__, __LINE__); } \
} while (0)

static void sec(const char *s) { printf("\n[%s]\n", s); }

/* ───────── 1. 身份归一 ───────── */
static void t_norm(void)
{
    sec("身份归一 am_core_norm（规则 ②）");
    char b[128];

    am_core_norm("map.lua", b, sizeof(b));
    CHECK_STR(b, "map", "去扩展名");

    am_core_norm("MAP.LUA", b, sizeof(b));
    CHECK_STR(b, "map", "英文不分大小写");

    am_core_norm("  map.lua  ", b, sizeof(b));
    CHECK_STR(b, "map", "去首尾空白 + 扩展名");

    am_core_norm("矢量地图.lua", b, sizeof(b));
    CHECK_STR(b, "矢量地图", "中文原样保留");

    am_core_norm("矢量地图", b, sizeof(b));
    CHECK_STR(b, "矢量地图", "无扩展名时不动");

    am_core_norm("a.b.lua", b, sizeof(b));
    CHECK_STR(b, "a.b", "只去最后一层扩展名");

    am_core_norm(".hidden", b, sizeof(b));
    CHECK_STR(b, ".hidden", "纯扩展名不当扩展名切");

    am_core_norm(NULL, b, sizeof(b));
    CHECK_STR(b, "", "NULL 安全");
}

/* ───────── 2. 身份键（类型隔离） ───────── */
static void t_key(void)
{
    sec("身份键 am_core_make_key（类型隔离）");
    char k1[96], k2[96], k3[96];

    am_core_make_key(AM_KIND_LUA, "Map.lua", k1, sizeof(k1));
    CHECK_STR(k1, "lua:map", "Lua 键带前缀 + 规范化");

    am_core_make_key(AM_KIND_LPK, "com.lintos.map", k2, sizeof(k2));
    CHECK_STR(k2, "lpk:com.lintos.map", "LPK 键用 app_id（点号不受影响）");

    am_core_make_key(AM_KIND_BUILTIN, "pal", k3, sizeof(k3));
    CHECK_STR(k3, "builtin:pal", "内置键带类型前缀");

    /* 14. 同名不同应用：Lua 与 LPK 同名绝不能合并 */
    char kl[96], kp[96];
    am_core_make_key(AM_KIND_LUA, "pal", kl, sizeof(kl));
    am_core_make_key(AM_KIND_LPK, "pal", kp, sizeof(kp));
    CHECK(!am_core_same(kl, kp), "同名 Lua / LPK ⇒ 不是同一个应用");
    CHECK(am_core_same(kl, kl), "自身相等");

    /* 15. 不同文件名但 app_id 相同（LPK 换名重打包）⇒ 同一个 */
    char a[96], b[96];
    am_core_make_key(AM_KIND_LPK, "com.lintos.recorder", a, sizeof(a));
    am_core_make_key(AM_KIND_LPK, "COM.LINTOS.RECORDER", b, sizeof(b));
    CHECK(am_core_same(a, b), "app_id 相同（大小写不同）⇒ 同一个应用");

    CHECK(!am_core_same(NULL, "lua:x"), "NULL 不相等");
    CHECK(!am_core_same("lua:", ""), "空键不相等");
}

/* ───────── 3. 资源包过滤 ───────── */
static void t_res(void)
{
    sec("资源包不进应用列表（规则 ①）");
    CHECK(am_core_type_is_res("asset"), "type=asset ⇒ 资源");
    CHECK(am_core_type_is_res("lpk"), "type=lpk ⇒ 资源（不是应用！）");
    CHECK(!am_core_type_is_res("lua"), "type=lua ⇒ 应用");
    CHECK(!am_core_type_is_res(""), "老索引无 type ⇒ 应用");
    CHECK(!am_core_type_is_res(NULL), "NULL 安全");
}

/* ───────── 4. 安装状态 ───────── */
static void t_state(void)
{
    sec("安装状态 am_core_decide（按钮表）");

    /* 1/9. 服务器有、本地没有 → 下载（NONE）
     * ⚠ 2026-11-03: 按钮文案已定为五态「下载/安装/下载中/更新/卸载」，
     *   未安装 = 「下载」（旧断言写「获取」，重构时漏改）。 */
    CHECK(am_core_decide(false, true, true, "", "", "202601010000", "") == AM_ST_NONE,
          "未安装 + 目录有 ⇒ 下载");
    CHECK_STR(am_st_btn(AM_ST_NONE), "下载", "未安装按钮=下载");

    /* 5. 本地有安装包（LPK）未安装 → 也是 NONE，按钮在商店层显示「安装」 */
    CHECK(am_core_decide(false, false, true, "202601010000", "", "", "") == AM_ST_NONE,
          "本地有包未安装 + 目录没有 ⇒ NONE");

    /* 3. 已安装 + 目录有 + 同版本 → 卸载 */
    CHECK(am_core_decide(true, true, true, "202601010000", "", "202601010000", "") == AM_ST_INSTALLED,
          "版本一致 ⇒ INSTALLED");
    CHECK_STR(am_st_btn(AM_ST_INSTALLED), "卸载", "已装最新 ⇒ 卸载（linit 2026-11-03 定稿）");

    /* 10. 服务器有新版 → 更新 */
    CHECK(am_core_decide(true, true, true, "202601010000", "", "202603010000", "") == AM_ST_UPDATE,
          "服务器 build 更新 ⇒ UPDATE");
    CHECK_STR(am_st_btn(AM_ST_UPDATE), "更新", "有新版按钮=更新");

    /* 本地比服务器新 → 不降级 */
    CHECK(am_core_decide(true, true, true, "202603010000", "", "202601010000", "") == AM_ST_INSTALLED,
          "本地更新 ⇒ 不提示更新（不降级）");

    /* build 缺失时退回 ver 比较 */
    CHECK(am_core_decide(true, true, true, "", "1", "", "2") == AM_ST_UPDATE,
          "build 缺失 ⇒ 比 ver");
    CHECK(am_core_decide(true, true, true, "", "5", "", "2") == AM_ST_INSTALLED,
          "build 缺失 + 本地 ver 更大 ⇒ 不更新");
    CHECK(am_core_decide(true, true, true, "", "", "", "") == AM_ST_INSTALLED,
          "两版版本信息都缺 ⇒ 保持已安装（不造更新）");

    /* 11. 服务器已删除（索引就绪 + 目录没有）→ 卸载，不给获取 */
    CHECK(am_core_decide(true, false, true, "", "", "", "") == AM_ST_ORPHAN,
          "已装 + 服务器已删 ⇒ ORPHAN");
    CHECK_STR(am_st_btn(AM_ST_ORPHAN), "卸载", "ORPHAN 按钮=卸载（⛔ 不是获取）");

    /* 12/13. 离线 / 索引未就绪 → 不能判成已删除 */
    CHECK(am_core_decide(true, false, false, "", "", "", "") == AM_ST_INSTALLED,
          "索引未加载 ⇒ 保持已安装（不误判下架）");

    /* 19. 卸载后（loc 清零）→ 回到未安装，不会残留"已安装" */
    CHECK(am_core_decide(false, true, true, "202601010000", "", "202601010000", "") == AM_ST_NONE,
          "卸载后 ⇒ 获取（状态与安装事实同源）");
}

/* ───────── 5. 列表投影 ───────── */
static void t_project(void)
{
    sec("列表投影（应用中心 / 商店）");

    /* 1/2/3/4/6/7. 已安装的应用：两边都在 */
    CHECK(am_core_center_ok(true, AM_KIND_LUA, AM_LOC_FLASH, true, false), "Flash Lua ⇒ 进应用中心");
    CHECK(am_core_center_ok(true, AM_KIND_LUA, AM_LOC_TF,    true, false), "TF Lua ⇒ 进应用中心");
    CHECK(am_core_center_ok(true, AM_KIND_LPK, AM_LOC_TF,    true, false), "已装 LPK ⇒ 进应用中心");
    CHECK(am_core_store_ok(true, AM_KIND_LUA), "Lua ⇒ 进商店");
    CHECK(am_core_store_ok(true, AM_KIND_LPK), "LPK ⇒ 进商店");

    /* 5. 未安装的 LPK 安装包：只进商店 */
    CHECK(!am_core_center_ok(true, AM_KIND_LPK, AM_LOC_NONE, true, false),
          "未安装 LPK ⇒ 不进应用中心");
    CHECK(am_core_store_ok(true, AM_KIND_LPK), "未安装 LPK ⇒ 仍在商店");

    /* 内置 C：只进应用中心 */
    CHECK(am_core_center_ok(true, AM_KIND_BUILTIN, AM_LOC_NONE, true, false),
          "内置 C ⇒ 进应用中心（loc 无关）");
    CHECK(!am_core_store_ok(true, AM_KIND_BUILTIN), "内置 C ⇒ 不进商店");

    /* 17. 损坏包：两边都不显示 */
    CHECK(!am_core_center_ok(false, AM_KIND_LUA, AM_LOC_TF, true, false), "损坏包 ⇒ 不进应用中心");
    CHECK(!am_core_store_ok(false, AM_KIND_LUA), "损坏包 ⇒ 不进商店");

    /* 12. TF 拔卡：TF-only 条目不显示，但安装事实（loc）没被改 */
    CHECK(!am_core_center_ok(true, AM_KIND_LUA, AM_LOC_TF, false, false),
          "TF 拔出 ⇒ TF-only 不进应用中心（不产生幽灵）");
    CHECK(am_core_center_ok(true, AM_KIND_LUA, AM_LOC_FLASH | AM_LOC_TF, false, false),
          "TF 拔出 ⇒ 内置还有一份，照常显示");

    /* 8. 双副本合并成一条：投影只问"有没有位置"，不按位置数重复 */
    CHECK(am_core_center_ok(true, AM_KIND_LUA, AM_LOC_FLASH | AM_LOC_TF, true, false),
          "Flash+TF 双副本 ⇒ 仍是同一个条目");

    /* 内置同 id 优先占应用中心（仙剑这类"内置页 + LPK 包"不出两条） */
    CHECK(!am_core_center_ok(true, AM_KIND_LPK, AM_LOC_TF, true, true),
          "同 id 已有内置 ⇒ LPK 不进应用中心");
    CHECK(am_core_store_ok(true, AM_KIND_LPK), "但它在商店里仍然存在（可更新/卸载）");
}

/* ───────── 6. 场景串起来 ───────── */
static void t_scenario(void)
{
    sec("端到端场景（同一份数据喂两个投影）");

    /* 场景 A：旧固件装到 TF 卡 → 刷机 → 不格卡
     *   文件还在（loc=TF）、索引里有这条且版本更新 ⇒ 应用中心有它、商店显示更新。 */
    {
        const unsigned loc = AM_LOC_TF;
        const bool in_cat = true, idx_ready = true;
        CHECK(am_core_center_ok(true, AM_KIND_LUA, loc, true, false), "A: 应用中心能看到");
        CHECK(am_core_store_ok(true, AM_KIND_LUA), "A: 商店也能看到（同一条记录）");
        CHECK(am_core_decide(true, in_cat, idx_ready, "202601010000", "", "202604010000", "") == AM_ST_UPDATE,
              "A: 服务器有新版 ⇒ 更新");
    }

    /* 场景 B：同上，但服务器后来把这应用删了 ⇒ 仍是「卸载」，不是「获取」。 */
    {
        CHECK(am_core_decide(true, false, true, "202601010000", "", "", "") == AM_ST_ORPHAN,
              "B: 服务器已删 ⇒ 卸载");
        CHECK_STR(am_st_btn(AM_ST_ORPHAN), "卸载", "B: ⛔ 绝不显示获取");
    }

    /* 场景 C：用户自己写了个 Lua 拷进 TF，从没上架 ⇒ 商店里也要能管理。 */
    {
        CHECK(am_core_store_ok(true, AM_KIND_LUA), "C: 本地独立应用进商店");
        CHECK(am_core_center_ok(true, AM_KIND_LUA, AM_LOC_TF, true, false), "C: 应用中心也能跑");
        CHECK(am_core_decide(true, false, true, "", "", "", "") == AM_ST_ORPHAN,
              "C: 无服务器记录 ⇒ 卸载（不是获取）");
    }

    /* 场景 D：拔卡 → 重新插回
     *   拔卡只让 storage_ok=false（不显示），loc 不变 ⇒ 插回后立刻恢复，安装事实没被改。 */
    {
        CHECK(!am_core_center_ok(true, AM_KIND_LUA, AM_LOC_TF, false, false), "D: 拔卡 ⇒ 不显示");
        CHECK(am_core_center_ok(true, AM_KIND_LUA, AM_LOC_TF, true, false),  "D: 插回 ⇒ 恢复显示");
        CHECK(am_core_decide(true, true, true, "202601010000", "", "202601010000", "") == AM_ST_INSTALLED,
              "D: 安装事实没变 ⇒ 仍是已安装");
    }

    /* 场景 E：重启后重扫 —— 同一个文件必须得到同一个 key（否则桌面图标/去重全乱） */
    {
        char k1[96], k2[96];
        am_core_make_key(AM_KIND_LUA, "/sdcard/apps/app/map.lua", k1, sizeof(k1));
        am_core_make_key(AM_KIND_LUA, "map.lua", k2, sizeof(k2));
        /* 传完整路径时要按文件名主干比 —— 由调用方取 basename；
         * 这里验证"同一个文件名两次归一结果一致"（幂等）。 */
        char a[96], b[96];
        am_core_norm("map.lua", a, sizeof(a));
        am_core_norm(a, b, sizeof(b));
        CHECK_STR(a, b, "E: 归一幂等（重启重扫不会换个身份）");
    }
}

int main(void)
{
    printf("统一应用核心 · 纯逻辑测试（生产源码 am_core_logic.c）\n");
    t_norm();
    t_key();
    t_res();
    t_state();
    t_project();
    t_scenario();

    printf("\n---- %d 通过, %d 失败 ----\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
