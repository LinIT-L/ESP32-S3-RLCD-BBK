/* luat_lib_store.c — LinTOS Lua 桥 · store 模块
 * 拆分自 components/apps/pages/page_lua.c (2026-10-02 模块化, 对标 LuatOS 的 luat_lib_*.c)。
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段;
 *       需要子表时自己 lua_newtable(L) → 填 → lua_setfield(L, -2, "子表名")。
 * 加新 API: 本文件写 static int l_xxx(lua_State *L), 再在文件末尾的 register 里加一行。
 */
#include "lua_bridge.h"
#include "lua_sandbox.h"   /* ★ 2026-10-31: NVS 命名空间按信任级隔离 */
#include "esp_timer.h"
#include "esp_cpu.h"              /* esp_cpu_get_sp() */
#include "esp_memory_utils.h"     /* esp_ptr_external_ram() */
#include "esp_heap_caps.h"       /* heap_caps_malloc: 强制内部 RAM */
#include "freertos/task.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ============================================================================
 * ★ 2026-10-26 linit「避免频繁写 NVS」: Lua 侧 NVS 写节流
 *
 * 问题: app.nvs.set_int/set_str 与 app.db.set/seti 都是**每次调用立刻 open+set+commit**,
 *   写 NVS 期间 flash cache 被关, 从 flash 执行的代码停顿十几~几十 ms。
 *   某个 Lua 应用在循环里写(比如每帧存一次分数), 就会反复掉帧, 而且白耗 flash 擦写。
 *
 * 做法: 同一 (命名空间, key) **1 秒窗口内只落盘一次**; 窗口内的后续写入只更新内存里的
 *   最新值(后写覆盖先写), 由一次性定时器在 1 秒后统一落盘。
 *   - 首次写该 key (本次开机) 立刻落盘, 不延迟
 *   - 读之前先把这个 key 的待落盘值刷下去 ⇒ **读到的永远是自己刚写的值**
 *   - 8 个槽位, 满了淘汰最久没写的那条(淘汰前先落盘, 不丢数据)
 *   - 定时器是 esp_timer, 应用退出/Lua 状态销毁后仍会把待写值落完
 * ⚠ 唯一风险窗口: 写完后 **1 秒内** 直接断电/进深睡, 最后一次写会丢。
 * ==========================================================================*/
#define LNS_WIN_US     (1000 * 1000)   /* 节流窗口: 1s */
#define LNS_SLOTS      8
#define LNS_KEY_MAX    16              /* NVS key 上限 15 字符 + NUL */
#define LNS_NS_LUAAPP  0
#define LNS_NS_COREDB  1

typedef struct {
    uint8_t  used;
    uint8_t  is_str;
    uint8_t  pending;   /* 有未落盘的新值 */
    uint8_t  ns;
    char     key[LNS_KEY_MAX];
    int32_t  ival;
    char    *sval;      /* is_str 时有效 (malloc) */
    int64_t  last_us;   /* 上次真正落盘的时刻 (0=本次开机还没写过) */
} lns_slot_t;

static lns_slot_t        s_lns[LNS_SLOTS];
static esp_timer_handle_t s_lns_timer;

/* ★ 2026-10-31 越权修复: 原来硬编码 "core_db"/"luaapp" ⇒ 第三方应用能写系统共享 KV。
 *   现在统一过 lua_sandbox_ns(): 官方原样返回(行为不变), 第三方映射成 appdb_<8hex>。 */
static const char *lns_ns_name(uint8_t ns) {
    return lua_sandbox_ns(ns ? "core_db" : "luaapp");
}

/* ★★★ 2026-11-08 修「点击后 assert 重启」★★★
 *  IDF `cache_utils.c:126` 有一条断言:
 *      assert(esp_task_stack_is_sane_cache_disabled());
 *  它只接受 **SP 在内部 DRAM** (esp_ptr_in_dram)。而本项目开了
 *  CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY / CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM,
 *  在 PSRAM 栈上调用 svc_nvs_commit() (关 cache 写 flash) 会直接 assert → 重启。
 *  串口实测: `assert failed: spi_flash_disable_interrupts_caches_and_other_c...`
 *
 *  两道防线:
 *   ① lns_write() 开头检查当前栈: 在 PSRAM 就**不写**, 返回 false (数据保留待下次),
 *      宁可少存一次也不许把系统打崩;
 *   ② 真正落盘交给一个**静态栈**任务 s_fl_task —— 静态 StackType_t 数组必然落在内部
 *      DRAM, 且没有 EXT_RAM 标注, 所以它的 flash 写永远安全。 */

/* ★★★ 2026-11-08 (第三轮, 真机拿到完整断言) ★★★
 *   串口原文:
 *     assert failed: xTaskCreateStaticPinnedToCore freertos_tasks_c_additions.h:298
 *                    (xPortCheckValidTCBMem(pxTaskBuffer))
 *   对应 IDF 源码 `freertos_tasks_c_additions.h:298`:
 *     configASSERT( portVALID_TCB_MEM( pxTaskBuffer ) );
 *   而 `xPortCheckValidTCBMem()` 的实现是:
 *     return esp_ptr_internal(ptr) && esp_ptr_byte_accessible(ptr);
 *   —— **TCB 必须在内部 DRAM**。
 *
 *   为什么静态变量不行: 本项目开了 CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y,
 *   .bss 段整体外置到 PSRAM, `static StaticTask_t` / `static StackType_t[]` 都跟着进了
 *   PSRAM。所以上一轮加 aligned(16) 完全无效 —— 问题不是对齐, 是**内存位置**。
 *   而且栈在 PSRAM 的话, 本任务"在内部栈上写 flash"的初衷也落空。
 *
 *   ⇒ 两块都改用 heap_caps_malloc(..., MALLOC_CAP_INTERNAL) 显式要内部 RAM,
 *     失败则降级(不建任务, 退回调用者栈 + PSRAM 安全门兜底)。
 *   注: 落盘只做 nvs open/set/commit, 8KB 内部栈足够(NVS 页写最吃栈)。 */
#define LNS_FL_STACK_WORDS   4096          /* words ⇒ 16KB 内部 RAM
                                            * ★ 2026-11-23 linit 报障: 打开详情(写"最近查询"
                                            *   → 触发落盘)后卡死复位。CRASH 记录 task=lns_fl /
                                            *   IllegalInstruction, 返回地址全落在无效区 ⇒ 栈被写坏。
                                            *   8KB 栈跑 nvs_set_str+commit(flash 擦写)不够, 加到 16KB。 */
static StaticTask_t *s_fl_tcb;
static StackType_t  *s_fl_stack;
static TaskHandle_t  s_fl_task;
static bool          s_fl_tried;            /* 只尝试一次: 失败反复建也会反复 assert */

static bool lns_write(lns_slot_t *s)
{
    /* 防线①: 栈在 PSRAM 时绝不写 flash (会触发 IDF 断言) */
    void *sp = (void *)esp_cpu_get_sp();
    if (esp_ptr_external_ram(sp)) {
        ESP_LOGW("LNS", "跳过落盘(栈在 PSRAM, 任务=%s) key=%s —— 数据保留待下次",
                 pcTaskGetName(NULL), s->key);
        return false;
    }
    nvs_handle_t h;
    if (svc_nvs_open(lns_ns_name(s->ns), NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = s->is_str ? svc_nvs_set_str(h, s->key, s->sval ? s->sval : "")
                            : svc_nvs_set_i32(h, s->key, s->ival);
    if (e == ESP_OK) e = svc_nvs_commit(h);
    svc_nvs_close(h);
    if (e == ESP_OK) { s->pending = 0; s->last_us = esp_timer_get_time(); }
    return e == ESP_OK;
}

static void lns_flush_task(void *arg)
{
    (void)arg;
    /* 自检: 本任务自己的 SP 在哪。若这里报 in=0, 说明栈被放到了 PSRAM,
     * 那么 lns_write() 的安全门会拦住(不写、不崩), 需要换内部 RAM 方案。 */
    {
        void *sp = (void *)esp_cpu_get_sp();
        ESP_LOGW("LNS", "lns_fl 已启动: SP=%p internal=%d", sp,
                 (int)!esp_ptr_external_ram(sp));
    }
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (int i = 0; i < LNS_SLOTS; i++)
            if (s_lns[i].used && s_lns[i].pending) lns_write(&s_lns[i]);
    }
}

static void lns_ensure_flush_task(void)
{
    if (s_fl_task || s_fl_tried) return;
    s_fl_tried = true;

    /* ⛔ 必须 MALLOC_CAP_INTERNAL: TCB 位置由 xPortCheckValidTCBMem 卡死;
     *    栈也必须内部, 否则落盘时 SP 在 PSRAM, 又会踩 spi_flash 的 assert。 */
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    s_fl_tcb   = heap_caps_malloc(sizeof(StaticTask_t), caps);
    s_fl_stack = heap_caps_malloc(sizeof(StackType_t) * LNS_FL_STACK_WORDS, caps);
    if (!s_fl_tcb || !s_fl_stack) {
        if (s_fl_tcb)   { heap_caps_free(s_fl_tcb);   s_fl_tcb = NULL; }
        if (s_fl_stack) { heap_caps_free(s_fl_stack); s_fl_stack = NULL; }
        ESP_LOGE("LNS", "lns_fl 内存分配失败(内部RAM不足) ⇒ 退回调用者栈落盘 "
                        "(有 PSRAM 安全门, 不会崩, 只是可能少存一次)");
        return;
    }
    ESP_LOGW("LNS", "建 lns_fl: tcb=%p in=%d | stack=%p in=%d",
             (void *)s_fl_tcb,  (int)esp_ptr_internal(s_fl_tcb),
             (void *)s_fl_stack, (int)esp_ptr_internal(s_fl_stack));
    s_fl_task = xTaskCreateStatic(lns_flush_task, "lns_fl", LNS_FL_STACK_WORDS, NULL, 4,
                                  s_fl_stack, s_fl_tcb);
    if (!s_fl_task) {
        heap_caps_free(s_fl_tcb);   s_fl_tcb = NULL;
        heap_caps_free(s_fl_stack); s_fl_stack = NULL;
        ESP_LOGE("LNS", "lns_fl 任务创建失败 ⇒ 退回调用者栈落盘(有安全门)");
    }
}

static void lns_timer_cb(void *arg)
{
    (void)arg;
    lns_ensure_flush_task();
    if (s_fl_task) xTaskNotifyGive(s_fl_task);
}

static void lns_arm_timer(void)
{
    if (!s_lns_timer) {
        const esp_timer_create_args_t a = { .callback = lns_timer_cb, .name = "lns_flush" };
        if (esp_timer_create(&a, &s_lns_timer) != ESP_OK) { s_lns_timer = NULL; return; }
    }
    esp_timer_stop(s_lns_timer);                              /* 可能没启动, 忽略返回值 */
    esp_timer_start_once(s_lns_timer, LNS_WIN_US);
}

static lns_slot_t *lns_slot_find(uint8_t ns, const char *key)
{
    for (int i = 0; i < LNS_SLOTS; i++)
        if (s_lns[i].used && s_lns[i].ns == ns && strncmp(s_lns[i].key, key, LNS_KEY_MAX - 1) == 0)
            return &s_lns[i];
    return NULL;
}

/* 取槽位: 已有 → 复用; 否则找空位; 再否则淘汰最久没写的 (先落盘) */
static lns_slot_t *lns_slot_take(uint8_t ns, const char *key, bool is_str)
{
    lns_slot_t *s = lns_slot_find(ns, key);
    if (s) { s->is_str = is_str ? 1 : 0; return s; }

    s = NULL;
    for (int i = 0; i < LNS_SLOTS; i++) if (!s_lns[i].used) { s = &s_lns[i]; break; }
    if (!s) {                                   /* 满了: 淘汰最久没写的 */
        s = &s_lns[0];
        for (int i = 1; i < LNS_SLOTS; i++) if (s_lns[i].last_us < s->last_us) s = &s_lns[i];
        if (s->pending) lns_write(s);            /* 淘汰前先落盘, 不丢数据 */
        free(s->sval);
    }
    memset(s, 0, sizeof(*s));
    s->used  = 1;
    s->ns    = ns;
    s->is_str = is_str ? 1 : 0;
    snprintf(s->key, sizeof(s->key), "%s", key);
    return s;
}

/* 核心入口: 同 key 1s 内只落盘一次 */
static bool lns_put(uint8_t ns, const char *key, bool is_str, int32_t iv, const char *sv)
{
    if (!key || !key[0]) return false;
    if (strlen(key) > LNS_KEY_MAX - 1) return false;   /* 超长 key NVS 本身也不接受 */

    lns_slot_t *s = lns_slot_take(ns, key, is_str);
    if (!s) return false;

    if (is_str) {
        char *nv = strdup(sv ? sv : "");
        if (!nv) return false;
        free(s->sval);
        s->sval = nv;
    } else {
        s->ival = iv;
    }

    lns_ensure_flush_task();
    int64_t now = esp_timer_get_time();
    if (s->last_us == 0 || now - s->last_us >= LNS_WIN_US) {
        /* 窗口外原本"立刻落盘", 但调用者栈可能是 PSRAM ⇒ 交给内部栈任务, 几十 us 后写 */
        s->pending = 1;
        if (s_fl_task) { xTaskNotifyGive(s_fl_task); return true; }
        return lns_write(s);                                  /* 兜底: 任务没建起来才直写 */
    }
    s->pending = 1;                                                              /* 窗口内: 只留最新值 */
    lns_arm_timer();
    return true;
}

/* 读之前把该 key 的待落盘值刷下去 ⇒ 读到的永远是自己刚写的值 */
static void lns_flush_key(uint8_t ns, const char *key)
{
    if (!key) return;
    lns_slot_t *s = lns_slot_find(ns, key);
    if (s && s->pending) lns_write(s);
}

static int l_nvs_get_int(lua_State *L) {
    const char *k = luaL_checkstring(L, 1);
    lns_flush_key(LNS_NS_LUAAPP, k);          /* 有未落盘的新值先写下去, 保证读到自己刚写的 */
    nvs_handle_t h = lua_nvs_h();
    if (!h) return 0;
    int32_t v = 0;
    esp_err_t e = svc_nvs_get_i32(h, k, &v);
    svc_nvs_close(h);
    if (e != ESP_OK) return 0;
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

static int l_nvs_set_int(lua_State *L) {
    const char *k = luaL_checkstring(L, 1);
    int32_t v = (int32_t)luaL_checkinteger(L, 2);
    lua_pushboolean(L, lns_put(LNS_NS_LUAAPP, k, false, v, NULL));   /* ★ 1s 内同 key 只落盘一次 */
    return 1;
}

static int l_nvs_get_str(lua_State *L) {
    const char *k = luaL_checkstring(L, 1);
    lns_flush_key(LNS_NS_LUAAPP, k);
    nvs_handle_t h = lua_nvs_h();
    if (!h) return 0;
    size_t len = 0;
    if (svc_nvs_get_str(h, k, NULL, &len) != ESP_OK) { svc_nvs_close(h); return 0; }
    char *s = malloc(len + 1);
    esp_err_t e = svc_nvs_get_str(h, k, s, &len);
    svc_nvs_close(h);
    if (e != ESP_OK) { free(s); return 0; }
    lua_pushstring(L, s);
    free(s);
    return 1;
}

static int l_nvs_set_str(lua_State *L) {
    const char *k = luaL_checkstring(L, 1);
    const char *v = luaL_checkstring(L, 2);
    lua_pushboolean(L, lns_put(LNS_NS_LUAAPP, k, true, 0, v));       /* ★ 同上 */
    return 1;
}


static int l_db_set(lua_State *L){
    const char *k=luaL_checkstring(L,1); const char *v=luaL_checkstring(L,2);
    lua_pushboolean(L, lns_put(LNS_NS_COREDB, k, true, 0, v));       /* ★ 同上 (core_db) */
    return 1;
}

static int l_db_get(lua_State *L){
    const char *k=luaL_checkstring(L,1); char b[128];
    lns_flush_key(LNS_NS_COREDB, k);
    if(os_core_db_get(k,b,sizeof(b))<0){ lua_pushnil(L); return 1; }
    lua_pushstring(L,b); return 1;
}

static int l_db_del(lua_State *L){
    const char *k=luaL_checkstring(L,1); lua_pushboolean(L, os_core_db_del(k)==0); return 1;
}

static int l_db_seti(lua_State *L){
    const char *k=luaL_checkstring(L,1); char b[32]; snprintf(b,sizeof(b),"%lld",(long long)luaL_checkinteger(L,2));
    lua_pushboolean(L, lns_put(LNS_NS_COREDB, k, true, 0, b));       /* ★ 同上 (core_db) */
    return 1;
}

static int l_db_geti(lua_State *L){
    const char *k=luaL_checkstring(L,1); long long def=(long long)luaL_optinteger(L,2,0); char b[32];
    lns_flush_key(LNS_NS_COREDB, k);
    if(os_core_db_get(k,b,sizeof(b))<0){ lua_pushinteger(L,(lua_Integer)def); return 1;}
    lua_pushinteger(L,(lua_Integer)strtoll(b,NULL,10)); return 1;
}

void lua_mod_store_register(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_nvs_get_int);         lua_setfield(L, -2, "get_int");
    lua_pushcfunction(L, l_nvs_set_int);         lua_setfield(L, -2, "set_int");
    lua_pushcfunction(L, l_nvs_get_str);         lua_setfield(L, -2, "get_str");
    lua_pushcfunction(L, l_nvs_set_str);         lua_setfield(L, -2, "set_str");
    lua_setfield(L, -2, "nvs");
    lua_newtable(L);
    lua_pushcfunction(L, l_db_set);              lua_setfield(L, -2, "set");
    lua_pushcfunction(L, l_db_get);              lua_setfield(L, -2, "get");
    lua_pushcfunction(L, l_db_del);              lua_setfield(L, -2, "del");
    lua_pushcfunction(L, l_db_seti);             lua_setfield(L, -2, "seti");
    lua_pushcfunction(L, l_db_geti);             lua_setfield(L, -2, "geti");
    lua_setfield(L, -2, "db");
}
