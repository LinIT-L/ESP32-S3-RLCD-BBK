/*
 * test_state.c — 商店条目状态机的宿主机测试
 *
 * 测的是**生产源码** components/apps/pages/am_store_state.c（不是副本），
 * 与 tests/lpk 的做法一致：同一份代码在主机上跑断言，避免"测试副本漂移"。
 *
 * 覆盖三块（对应 linit 2026-11-01 的要求）：
 *   ① 版本比较：build 优先 / 老包迁移 / 老索引退回 ver / 未知版本 / 同版本不同构建
 *   ② 状态表：未安装→下载、版本一致→卸载、有新版→更新、四种任务态、"安装失败回到原态"
 *   ③ 异步结果防覆盖：旧代次的结果不许写回；同一应用中途改状态后最终一致
 *
 * 编译: cc -std=c11 -Wall -Wextra -I components/apps/pages \
 *          tests/store_state/test_state.c components/apps/pages/am_store_state.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "am_store_state.h"

static int g_fail = 0;
static int g_ok   = 0;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) { g_ok++; }                                                   \
        else {                                                                  \
            g_fail++;                                                           \
            printf("  \xE2\x9C\x97 %s:%d  " fmt "\n", __FILE__, __LINE__,       \
                   ##__VA_ARGS__);                                              \
        }                                                                       \
    } while (0)

#define CHECK_STR(a, b, what)                                                  \
    do {                                                                       \
        const char *xa = (a), *xb = (b);                                        \
        if (xa && xb && strcmp(xa, xb) == 0) { g_ok++; }                        \
        else { g_fail++; printf("  \xE2\x9C\x97 %s: 期望\"%s\" 实得\"%s\"\n",  \
                                what, xb ? xb : "(null)", xa ? xa : "(null)");  \
        }                                                                       \
    } while (0)

static void sec(const char *t) { printf("\n== %s ==\n", t); }

/* ---------------- ① 版本比较 ---------------- */
static void t_version(void)
{
    sec("版本比较 am_st_ver_newer");

    /* build 都存在：定长数字串按字典序比（== 时间先后） */
    CHECK(am_st_ver_newer("202610300638", 1, "202611011048", 1) == 1,
          "本地旧 build、云端新 build ⇒ 有更新");
    CHECK(am_st_ver_newer("202611011048", 1, "202611011048", 1) == 0,
          "build 相同 ⇒ 一致");
    CHECK(am_st_ver_newer("202611011048", 1, "202610300638", 1) == -1,
          "本地比云端新 ⇒ 不提示更新（返回 -1）");

    /* 同一版本号、不同构建 → 必须能识别（这是"只比 ver 字符串"漏掉的情况） */
    CHECK(am_st_ver_newer("202610310919", 2, "202611011048", 2) == 1,
          "ver 相同但 build 更新 ⇒ 有更新");
    CHECK(am_st_ver_newer("202610310919", 2, "202610300638", 2) == -1,
          "ver 相同 build 更旧 ⇒ 没有更新");

    /* ② 老包（LAPP 头里没有 build）：提示一次迁移更新 */
    CHECK(am_st_ver_newer("", 4, "202611011048", 1) == 1,
          "本地老包无 build、云端有 ⇒ 提示一次更新（迁移）");
    CHECK(am_st_ver_newer(NULL, 0, "202611011048", 0) == 1,
          "本地完全没有版本信息、云端有 build ⇒ 提示一次更新");

    /* ③ 老索引（没有 build）：退回数字比较，未知版本(0)按 1 算 */
    CHECK(am_st_ver_newer("", 1, "", 3) == 1, "老索引：3 > 1 ⇒ 有更新");
    CHECK(am_st_ver_newer("", 3, "", 3) == 0, "老索引：相等 ⇒ 一致");
    CHECK(am_st_ver_newer("", 3, "", 2) == -1, "老索引：云端更旧 ⇒ 不提示");
    CHECK(am_st_ver_newer("", 0, "", 1) == 0, "本地版本未知(0)按 1 算 ⇒ 与 1 一致");
    CHECK(am_st_ver_newer("", 0, "", 0) == 0, "两边都未知 ⇒ 不提示更新");
}

/* ---------------- ② 状态表 ---------------- */
static void t_decide(void)
{
    sec("状态判定 am_st_decide");

    /* 未安装：无论版本如何都是"下载" */
    CHECK(am_st_decide(false, "202601010000", 1, "202611011048", 9) == AM_ST_NONE,
          "未安装 ⇒ NONE");
    CHECK_STR(am_st_btn(AM_ST_NONE), "下载", "未安装按钮（2026-11-03 定稿：只有 下载/安装/下载中/更新/卸载）");

    /* 已安装 + 版本一致 ⇒ 打开（2026-11-02 由「卸载」改为「打开」） */
    am_st_t st = am_st_decide(true, "202611011048", 2, "202611011048", 2);
    CHECK(st == AM_ST_INSTALLED, "已安装且一致 ⇒ INSTALLED");
    CHECK_STR(am_st_btn(st), "卸载", "一致按钮（已装且最新→卸载，2026-11-03 定稿）");

    /* 已安装 + 商店更新 ⇒ 更新 */
    st = am_st_decide(true, "202610300638", 1, "202611011048", 1);
    CHECK(st == AM_ST_UPDATE, "已安装且商店更新 ⇒ UPDATE");
    CHECK_STR(am_st_btn(st), "更新", "有新版按钮（linit 定：两字「更新」）");

    /* 商店侧没有版本信息（字体这类 zip 资源包）⇒ 保持"已安装=卸载"，不凭空造更新 */
    CHECK(am_st_decide(true, "", 0, "", 0) == AM_ST_INSTALLED,
          "商店无版本信息 ⇒ 已安装即最终，不提示更新");
    /* 本地版本比云端新（回滚过）⇒ 不提示更新 */
    CHECK(am_st_decide(true, "202612010000", 5, "202611011048", 3) == AM_ST_INSTALLED,
          "本地更新 ⇒ 不提示更新");
}

static void t_task(void)
{
    sec("任务叠加 am_st_with_task");
    CHECK(am_st_with_task(AM_ST_NONE, AM_TASK_DL) == AM_ST_DL, "下载任务");
    CHECK_STR(am_st_btn(AM_ST_DL), "下载中", "下载中文案");

    CHECK(am_st_with_task(AM_ST_NONE, AM_TASK_INSTALL) == AM_ST_INSTALLING,
          "首次安装的落盘阶段 ⇒ 安装中");
    CHECK_STR(am_st_btn(AM_ST_INSTALLING), "安装中", "安装中文案");

    CHECK(am_st_with_task(AM_ST_UPDATE, AM_TASK_INSTALL) == AM_ST_UPDATING,
          "更新的落盘阶段 ⇒ 更新中（不是安装中）");
    CHECK(am_st_with_task(AM_ST_UPDATE, AM_TASK_UPDATE) == AM_ST_UPDATING, "更新下载阶段");
    CHECK_STR(am_st_btn(AM_ST_UPDATING), "更新中", "更新中文案");

    CHECK(am_st_with_task(AM_ST_INSTALLED, AM_TASK_UNINSTALL) == AM_ST_UNINSTALLING,
          "卸载任务");
    CHECK_STR(am_st_btn(AM_ST_UNINSTALLING), "卸载中", "卸载中文案");

    /* 任务结束后必须回到**重新判定**的基础状态，不是靠"减一" */
    CHECK(am_st_with_task(AM_ST_INSTALLED, AM_TASK_NONE) == AM_ST_INSTALLED,
          "任务结束 ⇒ 卸载");
    CHECK(am_st_with_task(AM_ST_NONE, AM_TASK_NONE) == AM_ST_NONE,
          "安装失败（状态回到未安装）⇒ 下载");

    /* busy：进行中的四种状态都不允许再起任务 */
    CHECK(am_st_btn_busy(AM_ST_DL) && am_st_btn_busy(AM_ST_INSTALLING) &&
          am_st_btn_busy(AM_ST_UPDATING) && am_st_btn_busy(AM_ST_UNINSTALLING),
          "四种进行中态都算 busy");
    CHECK(!am_st_btn_busy(AM_ST_NONE) && !am_st_btn_busy(AM_ST_INSTALLED) &&
          !am_st_btn_busy(AM_ST_UPDATE),
          "稳定态不算 busy");

    /* 文案表完整性：每种状态都有非空文案，且 2~3 个汉字（按钮宽度按字数自适应） */
    for (int s = 0; s < AM_ST__COUNT; s++) {
        const char *btn = am_st_btn((am_st_t)s);
        int nch = 0;
        for (const unsigned char *q = (const unsigned char *)btn; *q; ) {
            q += (*q >= 0xf0) ? 4 : (*q >= 0xe0) ? 3 : (*q >= 0xc0) ? 2 : 1;
            nch++;
        }
        CHECK(btn && btn[0] && nch >= 2 && nch <= 3, "状态 %d 的按钮文案字数=%d", s, nch);
    }
}

/* ---------------- registry 解析 ---------------- */
static void t_registry(void)
{
    sec("registry 解析 am_st_reg_parse");

    const char *real =
        "app_id=com.lintos.recorder\n"
        "label=\xE5\xBD\x95\xE9\x9F\xB3\xE6\x9C\xBA\n"
        "version=2\n"
        "build=202610310919\n"
        "package_type=app\n"
        "runtime=lua\n"
        "entry=app/entry.lua\n"
        "source=/sdcard/apps/staging/com.lintos.recorder.lpk\n";
    am_st_reg_t r;
    CHECK(am_st_reg_parse(real, strlen(real), &r), "正常记录可解析");
    CHECK_STR(r.app_id, "com.lintos.recorder", "app_id");
    CHECK_STR(r.ver, "2", "version");
    CHECK_STR(r.build, "202610310919", "build");
    CHECK_STR(r.runtime, "lua", "runtime");
    CHECK_STR(r.entry, "app/entry.lua", "entry");

    /* 值里含 '=' 不该把键劈错 */
    const char *eq = "app_id=com.a.b\nlabel=x=y\n";
    CHECK(am_st_reg_parse(eq, strlen(eq), &r), "值里含 '=' 也能解析");
    CHECK_STR(r.app_id, "com.a.b", "app_id 含点");
    CHECK_STR(r.label, "x=y", "值里等号保留");

    /* 重复键：第一次为准（后面的补丁行不许覆盖身份/版本） */
    const char *dup = "app_id=com.a\nversion=1\nversion=9\n";
    CHECK(am_st_reg_parse(dup, strlen(dup), &r), "重复键可解析");
    CHECK_STR(r.ver, "1", "重复键取第一次");

    /* CRLF 容忍（Windows 编辑器/手改过） */
    const char *crlf = "app_id=com.b\r\nversion=3\r\nbuild=202611011048\r\n";
    CHECK(am_st_reg_parse(crlf, strlen(crlf), &r), "CRLF 记录可解析");
    CHECK_STR(r.app_id, "com.b", "CRLF app_id");
    CHECK_STR(r.build, "202611011048", "CRLF build");

    /* 损坏：没有 app_id 行 ⇒ 解析失败（调用方按"记录损坏"处理并打日志） */
    const char *bad = "version=2\nbuild=202610310919\n";
    CHECK(!am_st_reg_parse(bad, strlen(bad), &r), "缺 app_id ⇒ 解析失败");

    /* 前导空格的行不算字段（避免把正文里的 x=y 当记录） */
    const char *sp = " app_id=com.c\napp_id=com.d\n";
    CHECK(am_st_reg_parse(sp, strlen(sp), &r), "带前导空格的行被忽略后仍能解析");
    CHECK_STR(r.app_id, "com.d", "忽略前导空格行");

    /* 空串/超长不崩 */
    CHECK(!am_st_reg_parse("", 0, &r), "空记录 ⇒ 失败");
}

/* ---------------- 索引口径 = 更新判定的基准 ---------------- */
static void t_index_stamp(void)
{
    sec("索引版本口径（服务器只抬索引、没重打包的场景）");

    /* 现场真身：线上 index.json 说 录音机 ver=3 build=202611011048，
     * 而服务器上那份包的 manifest 还是 version=2 build=202610310919（SHA-256 已核对同一份）。 */
    const char *reg =
        "app_id=com.lintos.recorder\n"
        "version=2\n"
        "build=202610310919\n"
        "store_ver=3\n"
        "store_build=202611011048\n";
    am_st_reg_t r;
    CHECK(am_st_reg_parse(reg, strlen(reg), &r), "带 store_* 的记录可解析");
    CHECK_STR(r.ver, "2", "包内版本");
    CHECK_STR(r.build, "202610310919", "包内构建");
    CHECK_STR(r.store_ver, "3", "商店版本");
    CHECK_STR(r.store_build, "202611011048", "商店构建");

    /* ★ 规则：更新判定必须用 store_* —— 与商店索引同一口径 */
    am_st_t with_store = am_st_decide(true, r.store_build, (unsigned)atoi(r.store_ver),
                                     "202611011048", 3);
    CHECK(with_store == AM_ST_INSTALLED, "按商店口径比 ⇒ 已是最新（显示打开）");

    /* 反例：如果拿包内 manifest 版本去比，会**永远**显示"更新"，点一次下一次 */
    am_st_t with_manifest = am_st_decide(true, r.build, (unsigned)atoi(r.ver),
                                        "202611011048", 3);
    CHECK(with_manifest == AM_ST_UPDATE,
          "按包内版本比 ⇒ 永远提示更新（这就是必须用 store_* 的原因）");

    /* 老记录（还没有 store_* 两行，本轮之前装的）⇒ 退回包内版本，行为与以前一致 */
    const char *old = "app_id=com.lintos.recorder\nversion=2\nbuild=202610310919\n";
    CHECK(am_st_reg_parse(old, strlen(old), &r), "老记录可解析");
    CHECK(!r.store_ver[0] && !r.store_build[0], "老记录没有 store_* 字段");
}

/* ---------------- ③ 唯一更新判据：服务器文件修改时间 ---------------- */
static void t_mtime(void)
{
    sec("更新判据 am_st_decide_meta（服务器文件 mtime / 迁移核对）");

    bool bf = false;
    am_st_meta_t m;

    /* 未安装 ⇒ 下载 */
    memset(&m, 0, sizeof(m));
    CHECK(am_st_decide_meta(&m, &bf) == AM_ST_NONE, "未安装 ⇒ NONE");
    CHECK(!bf, "未安装不触发补记录");

    /* ① 记录 mtime == 索引 mtime ⇒ 打开，不补记录 */
    memset(&m, 0, sizeof(m));
    m.installed = true; m.has_idx = true;
    m.has_rec = true; m.rec_mtime = 1762000000; m.rec_crc = 0xabc; m.rec_size = 100;
    m.idx_mtime = 1762000000; m.idx_crc = 0xabc; m.idx_size = 100;
    bf = true;
    CHECK(am_st_decide_meta(&m, &bf) == AM_ST_INSTALLED, "时间相同 ⇒ 打开");
    CHECK(!bf, "时间相同不补记录");

    /* ② 索引 mtime 更新 + 内容不同 ⇒ 更新 */
    m.idx_mtime = 1762100000; m.idx_crc = 0xdef; m.idx_size = 120;
    bf = false;
    CHECK(am_st_decide_meta(&m, &bf) == AM_ST_UPDATE, "服务器文件更新且内容不同 ⇒ 更新");
    CHECK(!bf, "更新态不补记录");

    /* ③ 索引 mtime 更新但内容完全一致（重建索引/重打包没改内容）⇒ 打开 + 补记录
     *    —— 这就是"更新完还是更新"死循环的根治点 */
    m.idx_mtime = 1762100000; m.idx_crc = 0xabc; m.idx_size = 100;
    bf = false;
    CHECK(am_st_decide_meta(&m, &bf) == AM_ST_INSTALLED, "时间变了但内容相同 ⇒ 打开");
    CHECK(bf, "内容相同且 mtime 前移 ⇒ 要求把记录推进到新 mtime");

    /* ④ 老应用（无任何记录）+ 本地 crc == 索引 crc ⇒ 打开 + 补记录（迁移）
     *    现场真身：BIOS模拟器 / 航班雷达 —— size 与 crc32 与服务器完全一致 */
    memset(&m, 0, sizeof(m));
    m.installed = true; m.has_idx = true;
    m.idx_mtime = 1762000000; m.idx_crc = 0x824595c1; m.idx_size = 221453;
    m.loc_crc_ok = true; m.loc_crc = 0x824595c1; m.loc_size = 221453;
    bf = false;
    CHECK(am_st_decide_meta(&m, &bf) == AM_ST_INSTALLED,
          "老应用内容与服务器一致 ⇒ 打开（不集体显示更新）");
    CHECK(bf, "老应用核对通过 ⇒ 补写安装记录");

    /* ⑤ 老应用 + 本地 crc 与索引不同 ⇒ 更新（本地确实是旧版） */
    m.loc_crc = 0x11111111;
    bf = true;
    CHECK(am_st_decide_meta(&m, &bf) == AM_ST_UPDATE, "老应用内容不同 ⇒ 更新");
    CHECK(!bf, "核对不通过不补记录（绝不伪造）");

    /* ⑥ 老应用 + 本地文件读不出来（无卡/权限）⇒ 打开，不下结论 */
    memset(&m, 0, sizeof(m));
    m.installed = true; m.has_idx = true;
    m.idx_mtime = 1762000000; m.idx_crc = 0x824595c1; m.idx_size = 221453;
    m.loc_crc_ok = false;
    bf = true;
    CHECK(am_st_decide_meta(&m, &bf) == AM_ST_INSTALLED, "核对不了 ⇒ 维持打开");
    CHECK(!bf, "核对不了不伪造记录");

    /* ⑦ 老索引（没有 mtime）+ 记录里有 crc ⇒ 比内容 */
    memset(&m, 0, sizeof(m));
    m.installed = true; m.has_idx = true;
    m.has_rec = true; m.rec_crc = 0xabc; m.rec_size = 100;   /* rec_mtime = 0 */
    m.idx_crc = 0xabc; m.idx_size = 100;                     /* idx_mtime = 0 */
    bf = false;
    CHECK(am_st_decide_meta(&m, &bf) == AM_ST_INSTALLED, "老索引：内容相同 ⇒ 打开");
    m.idx_crc = 0x999;
    CHECK(am_st_decide_meta(&m, NULL) == AM_ST_UPDATE, "老索引：内容不同 ⇒ 更新");

    /* ⑧ 索引里没有这条应用 ⇒ 不造更新 */
    memset(&m, 0, sizeof(m));
    m.installed = true; m.has_rec = true; m.rec_mtime = 1762000000;
    CHECK(am_st_decide_meta(&m, NULL) == AM_ST_INSTALLED, "索引无此条 ⇒ 打开");

    /* ⑨ crc 为 0（两边都不知道）不能当成"内容相同" */
    memset(&m, 0, sizeof(m));
    m.installed = true; m.has_idx = true;
    m.has_rec = true; m.rec_mtime = 1762000000;
    m.idx_mtime = 1762100000;
    CHECK(am_st_decide_meta(&m, NULL) == AM_ST_UPDATE,
          "crc 未知时不按内容判同，宁可提示一次更新");
}

/* ---------------- ③ 代次守卫（异步结果防覆盖） ---------------- */
static void t_gen_guard(void)
{
    sec("异步结果防覆盖 am_st_cell_*");

    am_st_cell_t c;
    memset(&c, 0, sizeof(c));

    /* 进商店：发起查询（拿到 gen=1） */
    uint32_t g1 = am_st_cell_invalidate(&c);           /* 1 */
    CHECK(am_st_cell_put(&c, g1, AM_ST_NONE), "第一次结果写入成功");
    CHECK(c.val == AM_ST_NONE, "写入值正确");

    /* 用户装好了应用 ⇒ 现场变了，作废（gen=2），并写入新结果 */
    uint32_t g2 = am_st_cell_invalidate(&c);           /* 2 */
    CHECK(g2 == g1 + 1, "作废后代次 +1");
    CHECK(!c.valid, "作废后不再有效");
    CHECK(am_st_cell_put(&c, g2, AM_ST_INSTALLED), "新结果写入成功");
    CHECK(c.val == AM_ST_INSTALLED, "已安装落盘");

    /* ★ 关键：第一次查询的**慢结果**现在才回来（带着旧代次 g1） */
    CHECK(!am_st_cell_put(&c, g1, AM_ST_NONE), "旧代次的结果被丢弃");
    CHECK(c.val == AM_ST_INSTALLED, "状态没有被旧结果覆盖成未安装");

    /* 同代次内：后到者胜（同一轮查询的重复回报） */
    CHECK(am_st_cell_put(&c, g2, AM_ST_UPDATE), "同代次允许写入");
    CHECK(c.val == AM_ST_UPDATE, "同代次后到者覆盖");

    /* 代次回绕：0xFFFFFFFF → 0 之后，新代次仍被认成"更新" */
    am_st_cell_t w;
    memset(&w, 0, sizeof(w));
    w.gen = 0xFFFFFFFEu; w.valid = true; w.val = 1;
    CHECK(am_st_cell_put(&w, 0xFFFFFFFFu, 2), "回绕前写入");
    CHECK(am_st_cell_put(&w, 0u, 3), "回绕后（0）仍算更新，不被当成旧代次");
    CHECK(w.val == 3, "回绕后值正确");
}

/* ---------------- 端到端时序：进入商店 → 安装 → 旧查询晚到 ---------------- */
static void t_scenario(void)
{
    sec("时序场景：进入商店 → 安装 → 旧结果晚到");

    /* ① 进店：本地 registry 还没读，先按"索引有、本地无"发起一次异步判定 */
    am_st_cell_t row;
    memset(&row, 0, sizeof(row));
    uint32_t gen_query = am_st_cell_invalidate(&row);

    /* ② 后台慢查询用的是"当时的现场"：未安装 */
    am_st_t slow_result = am_st_decide(false, "", 0, "202611011048", 3);

    /* ③ 中途用户点了下载并装好（现场变了：gen 前进），UI 立刻按新现场判一次 */
    uint32_t gen_now = am_st_cell_invalidate(&row);
    am_st_t fast_result = am_st_decide(true, "202611011048", 3, "202611011048", 3);
    CHECK(fast_result == AM_ST_INSTALLED, "装好后立刻判为已安装");
    CHECK(am_st_cell_put(&row, gen_now, fast_result), "新结果生效");

    /* ④ 慢查询这时才回来 */
    CHECK(!am_st_cell_put(&row, gen_query, slow_result), "慢查询被拒");
    CHECK(row.val == AM_ST_INSTALLED, "最终显示仍是已安装（不会闪回下载）");
    CHECK_STR(am_st_btn((am_st_t)row.val), "卸载", "按钮稳定在卸载（已装且最新）");

    /* ⑤ 再进一次商店（重新查询）：现场仍是已安装，结果一致 ⇒ 不闪变 */
    uint32_t gen_re = am_st_cell_invalidate(&row);
    CHECK(am_st_cell_put(&row, gen_re, am_st_decide(true, "202611011048", 3,
                                                    "202611011048", 3)),
          "重进商店写入");
    CHECK(row.val == AM_ST_INSTALLED, "重进商店后状态一致");

    /* ⑥ 服务器推新版：索引变了 ⇒ 同一现场但商店版本更新 ⇒ 变成"更新" */
    CHECK(am_st_decide(true, "202611011048", 3, "202612010000", 4) == AM_ST_UPDATE,
          "商店推新版 ⇒ 更新");
}

/* ========== 2026-11-03: 显示名 + 本地独有（服务器已删除） ==========
 * ⚠ 原来的 am_st_alias / am_st_same_app 已随统一应用核心一起删除：
 *   身份只由 am_app_core 的 key 给出（见 tests/app_core），这里不再有第二套判定。 */
static void t_identity_and_orphan(void)
{
    sec("显示名择优 am_st_display_name（规则 13）");

    CHECK_STR(am_st_display_name("矢量地图", "VectorMap", "map.lua"), "矢量地图",
              "中文名优先");
    CHECK_STR(am_st_display_name("", "VectorMap", "map.lua"), "VectorMap",
              "无中文名 ⇒ 英文名");
    CHECK_STR(am_st_display_name("", "", "map.lua"), "map.lua", "都无 ⇒ 文件名");
    CHECK_STR(am_st_display_name(NULL, NULL, NULL), "", "全空安全");

    sec("本地独有 am_st_decide_meta（服务器已删除）");

    am_st_meta_t m;
    /* ① 装着 + 索引已就绪 + 索引里没有 ⇒ 卸载（本地独有） */
    memset(&m, 0, sizeof(m));
    m.installed = true; m.orphan_ok = true; m.idx_ready = true; m.has_idx = false;
    CHECK(am_st_decide_meta(&m, NULL) == AM_ST_ORPHAN, "服务器已删 ⇒ ORPHAN");
    CHECK_STR(am_st_btn(AM_ST_ORPHAN), "卸载", "ORPHAN 按钮=卸载");

    /* ② 离线/索引没到 ⇒ has_idx=false 只代表"不知道"，⛔ 不能给卸载 */
    memset(&m, 0, sizeof(m));
    m.installed = true; m.orphan_ok = true; m.idx_ready = false; m.has_idx = false;
    CHECK(am_st_decide_meta(&m, NULL) == AM_ST_INSTALLED, "离线 ⇒ 保持已安装，不误判删除");

    /* ③ 云端行（orphan_ok 未置）解析失败 ⇒ 也保持已安装 */
    memset(&m, 0, sizeof(m));
    m.installed = true; m.orphan_ok = false; m.idx_ready = true; m.has_idx = false;
    CHECK(am_st_decide_meta(&m, NULL) == AM_ST_INSTALLED, "云端行不参与本地独有判定");

    /* ④ 服务器有这条 ⇒ 照常按 mtime 判更新/最新 */
    memset(&m, 0, sizeof(m));
    m.installed = true; m.orphan_ok = true; m.idx_ready = true; m.has_idx = true;
    m.has_rec = true; m.rec_mtime = 1000;
    m.idx_mtime = 2000; m.idx_crc = 0xAA; m.idx_size = 10; m.rec_crc = 0xBB; m.rec_size = 10;
    CHECK(am_st_decide_meta(&m, NULL) == AM_ST_UPDATE, "有新版 ⇒ 更新");
    m.idx_mtime = 1000;
    CHECK(am_st_decide_meta(&m, NULL) == AM_ST_INSTALLED, "同版 ⇒ 已安装");

    /* ⑤ 未装 + 服务器无此条 ⇒ 下载（不是卸载） */
    memset(&m, 0, sizeof(m));
    m.installed = false; m.orphan_ok = true; m.idx_ready = true;
    CHECK(am_st_decide_meta(&m, NULL) == AM_ST_NONE, "未装 ⇒ NONE");
    CHECK_STR(am_st_btn(AM_ST_NONE), "下载", "未装按钮=下载");

    /* ⑥ 双位置安装：dst / dst_sd 任一存在都由调用方折成 installed=true，
     *    这里只需证明 installed 的两种来源得到同一状态（不出现两条不同结论）。 */
    memset(&m, 0, sizeof(m));
    m.installed = true; m.orphan_ok = true; m.idx_ready = true; m.has_idx = true;
    m.has_rec = true; m.rec_mtime = 1000; m.idx_mtime = 1000;
    am_st_t flash_st = am_st_decide_meta(&m, NULL);
    am_st_t sd_st    = am_st_decide_meta(&m, NULL);   /* 同一 meta：Flash/TF 无差别 */
    CHECK(flash_st == sd_st && flash_st == AM_ST_INSTALLED, "双位置 ⇒ 同一状态，只一条");
}

int main(void)
{
    printf("商店状态机测试（生产源码 am_store_state.c）\n");
    t_version();
    t_decide();
    t_task();
    t_registry();
    t_index_stamp();
    t_mtime();
    t_gen_guard();
    t_scenario();
    t_identity_and_orphan();

    printf("\n---- %d 通过, %d 失败 ----\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
