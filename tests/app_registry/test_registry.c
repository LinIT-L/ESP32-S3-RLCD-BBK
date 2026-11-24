/**
 * tests/app_registry/test_registry.c — 应用注册表同步（os_app.c）宿主机行为测试
 *
 * 被测对象是**生产源码** components/system/os/kernel/os_app.c，
 * 用 stub/ 下的最小内核接口替身在电脑上编译（不抄副本、不改生产代码）。
 *
 * 覆盖本轮「应用动态同步」整改的关键行为：
 *   1. 从未同步过 ⇒ prune 不动
 *   2. 首次 sync ⇒ 全部登记、顺序一致
 *   3. 重复 sync ⇒ 不产生重复条目
 *   4. prune ⇒ 本轮没出现的动态条目被回收
 *   5. prune 的 keep 回调 ⇒ 调用方判定要保留的条目不删（拔卡场景）
 *   6. 内置 C（page != OS_PAGE_LUA）⇒ 永不回收
 *   7. 同一轮里重复 id ⇒ 只登记一次
 *   8. 容量上限 ⇒ 满表时用"上一轮残留"腾位，不静默丢新应用
 *   9. 满表时更新已有条目 ⇒ 仍然生效（不能被容量挡掉）
 *
 * 用法: sh tests/app_registry/run_all.sh
 */
#include <stdio.h>
#include <string.h>

#include "os.h"

static int g_ok = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_ok++; } \
    else { g_fail++; printf("  x %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

static void sec(const char *s) { printf("\n[%s]\n", s); }

/* ───────── 测试对象池（注册表只存指针 ⇒ 对象必须活到测试结束） ───────── */
#define POOL_MAX 160
static os_app_t g_pool[POOL_MAX];
static int      g_pool_n = 0;

static const os_app_t *mk(const char *id, os_page_t page)
{
    if (g_pool_n >= POOL_MAX) return NULL;
    os_app_t *a = &g_pool[g_pool_n++];
    memset(a, 0, sizeof(*a));
    a->id    = id;
    a->label = id;
    a->page  = page;
    return a;
}

static int count_id(const char *id)
{
    int n = 0;
    for (int i = 0; i < os_app_count(); i++) {
        const os_app_t *a = os_app_get(i);
        if (a && a->id && strcmp(a->id, id) == 0) n++;
    }
    return n;
}

static bool has_id(const char *id) { return count_id(id) > 0; }

/* ───────── 用例 ───────── */

static void t_before_sync(void)
{
    sec("未同步过：prune 不动");
    CHECK(os_app_count() == 0, "初始注册表为空");
    CHECK(os_app_prune(NULL, NULL) == 0, "没 sync 过 ⇒ 不回收（没有\"本轮\"概念）");
    CHECK(os_app_sync_epoch() == 0, "同步序号尚未推进");
}

static const os_app_t *V_A, *V_B, *V_C;

static void t_first_sync(void)
{
    sec("首次 sync：全部登记");
    V_A = mk("lua:a.lua", OS_PAGE_LUA);
    V_B = mk("lua:b.lua", OS_PAGE_LUA);
    V_C = mk("lua:c.lua", OS_PAGE_LUA);

    const os_app_t *views[3] = { V_A, V_B, V_C };
    const uint32_t g0 = os_app_vis_gen();
    os_app_sync(views, 3);

    CHECK(os_app_count() == 3, "3 条全部登记");
    CHECK(os_app_get(0) == V_A && os_app_get(1) == V_B && os_app_get(2) == V_C,
          "顺序与传入一致（列表顺序稳定）");
    CHECK(os_app_vis_gen() != g0, "登记后可见性代次推进（桌面会重建）");
    CHECK(os_app_sync_epoch() == 1, "同步序号 +1");
}

static void t_idempotent(void)
{
    sec("重复 sync：不重复登记");
    const os_app_t *views[3] = { V_A, V_B, V_C };
    os_app_sync(views, 3);
    os_app_sync(views, 3);

    CHECK(os_app_count() == 3, "重复同步仍是 3 条");
    CHECK(count_id("lua:a.lua") == 1, "同 id 只有一条");
    CHECK(os_app_sync_epoch() == 3, "每次 sync 序号都推进");
}

static void t_dup_in_one_batch(void)
{
    sec("同一轮里重复 id：只登记一次");
    const os_app_t *views[4] = { V_A, V_A, V_B, V_C };
    os_app_sync(views, 4);
    CHECK(os_app_count() == 3, "批内重复不产生第二条");
}

static void t_prune(void)
{
    sec("prune：回收本轮没出现的条目");
    const os_app_t *views[2] = { V_A, V_B };
    os_app_sync(views, 2);
    const int dropped = os_app_prune(NULL, NULL);

    CHECK(dropped == 1, "回收 1 条");
    CHECK(os_app_count() == 2, "剩 2 条");
    CHECK(!has_id("lua:c.lua"), "c 已从注册表消失");
    CHECK(has_id("lua:a.lua") && has_id("lua:b.lua"), "a/b 不受影响");
}

/* keep 回调：模拟"卡暂时没挂载" ⇒ 这一条保留 */
static bool keep_all(const char *id, void *ud)
{
    (void)id;
    return ud != NULL;
}

static void t_prune_keep(void)
{
    sec("prune keep 回调：调用方判定保留的不删");
    const os_app_t *views[3] = { V_A, V_B, V_C };
    os_app_sync(views, 3);
    CHECK(os_app_count() == 3, "c 重新登记");

    const os_app_t *views2[2] = { V_A, V_B };
    os_app_sync(views2, 2);
    int dummy = 1;
    const int dropped = os_app_prune(keep_all, &dummy);

    CHECK(dropped == 0, "keep 返回 true ⇒ 不回收");
    CHECK(has_id("lua:c.lua"), "c 仍在（拔卡/来源未扫到时的保守行为）");
}

static void t_builtin(void)
{
    sec("内置 C：永不回收");
    const os_app_t *b = mk("settings", OS_PAGE_SETTINGS);
    os_app_register(b);
    CHECK(has_id("settings"), "内置应用已登记");

    const os_app_t *views[2] = { V_A, V_B };
    os_app_sync(views, 2);
    os_app_prune(NULL, NULL);

    CHECK(has_id("settings"), "prune 不碰 page != OS_PAGE_LUA 的条目");
    CHECK(!has_id("lua:c.lua"), "动态条目照常回收");
}

static void t_capacity(void)
{
    sec("容量上限：满表时用残留条目腾位");
    /* 先探出上限（不写死 96，避免常量和实现分叉） */
    /* ⚠ static: 这些字符串会被注册表条目长期引用（id 指针），
     *   放栈上会在函数返回后失效 —— ASan 的 stack-use-after-scope 就是抓这个。 */
    static char     ids[POOL_MAX][32];
    const os_app_t *objs[POOL_MAX];
    int n = 0;
    while (n < 96) {                     /* 上限就是 96，循环自然在"加不进去"时停 */
        snprintf(ids[n], sizeof(ids[n]), "lua:fill%02d.lua", n);
        const os_app_t *o = mk(ids[n], OS_PAGE_LUA);
        if (!o) break;
        const int before = os_app_count();
        os_app_register(o);
        if (os_app_count() == before) break;   /* 满了 */
        objs[n] = o;
        n++;
    }
    const int cap = os_app_count();
    printf("  注册表容量上限 = %d（本次填充 %d 条）\n", cap, n);
    CHECK(cap >= 64, "容量至少 64（34+ 内置 + TF Lua）");
    CHECK(n > 0 && n < 96, "确实填到了上限");

    /* 造一个"上一轮残留"：本轮登记 a/b + 前 n-1 条 + 一个新应用，
     * 只剩 fill(n-1) 这一条是"上一轮有、本轮没有"。 */
    static const os_app_t *views[POOL_MAX];
    int vn = 0;
    views[vn++] = V_A;
    views[vn++] = V_B;
    for (int i = 0; i < n - 1; i++) views[vn++] = objs[i];
    const os_app_t *fresh = mk("lua:newcomer.lua", OS_PAGE_LUA);
    views[vn++] = fresh;

    os_app_sync(views, vn);

    CHECK(os_app_count() == cap, "满表时不会撑破上限");
    CHECK(has_id("lua:newcomer.lua"), "新应用仍然进得来（不被容量静默丢弃）");
    CHECK(!has_id(ids[n - 1]), "最后一条残留被回收腾位");
    CHECK(has_id(ids[0]) && has_id(ids[n - 2]), "其余条目不受影响");
}

static void t_update_when_full(void)
{
    sec("满表时更新已有条目");
    /* 找一个当前还在的条目，换一个 label 指针重新登记 */
    const char *id = "lua:fill00.lua";
    if (!has_id(id)) id = "lua:newcomer.lua";
    const int before = os_app_count();
    const os_app_t *u = mk(id, OS_PAGE_LUA);
    os_app_register(u);

    CHECK(os_app_count() == before, "更新不增加条目");
    bool found = false;
    for (int i = 0; i < os_app_count(); i++) {
        const os_app_t *a = os_app_get(i);
        if (a == u) { found = true; break; }
    }
    CHECK(found, "同 id 覆盖生效（满表也不能挡住更新）");
}

int main(void)
{
    printf("应用注册表同步 · 宿主机行为测试（生产源码 os_app.c）\n");
    t_before_sync();
    t_first_sync();
    t_idempotent();
    t_dup_in_one_batch();
    t_prune();
    t_prune_keep();
    t_builtin();
    t_capacity();
    t_update_when_full();

    printf("\n---- %d 通过, %d 失败 ----\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
