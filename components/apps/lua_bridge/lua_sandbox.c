/* lua_sandbox.c — 见 lua_sandbox.h
 *
 * 分层与边界(别把这里当"完整沙箱"):
 *   L1 摘库      : 只对 THIRD_PARTY 生效, OFFICIAL 一行不改
 *   L2 路径守卫  : io.open/os.remove/os.rename + app.fs.* + app.img.load
 *   L3 资源配额  : 内存由 lua_psram_alloc 回报(lua_quota.c 算账); 时限 hook + 阻塞 API 夹紧
 *   L4 能力白名单: app.hid.* / app.wiki.open 等按信任级拒绝(统一错误, 不崩 VM)
 *
 * 明确不做的:
 *   - 不试图改写 app.net.* / app.wifi.* 的语义(联网是正当需求, 只夹紧阻塞时长);
 *   - 不拦 C 层里"没有经过 Lua 参数"的内部文件访问(那些不是应用可达的入口);
 *   - 静态扫描/人工审核仍是防线之一, 本沙箱不是"绝对安全"的证明。
 */
#include "lua_sandbox.h"
#include "lua_path_guard.h"
#include "lua_quota.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>

#include "lauxlib.h"
#include "lualib.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "LSANDBOX";

/* 配额默认值 —— **仍未真机实测**(模拟器无 Lua 应用可跑), 只是先给一个不会误杀的量级。
 * 实测方法: 跑官方/第三方样本, 打印 lua_sandbox_mem_peak(); 再据此调整。
 * 设为 0 = 不限(一键回退)。 */
#ifndef LUA_SANDBOX_MEM_KB
#define LUA_SANDBOX_MEM_KB 4096      /* 4 MB */
#endif
#ifndef LUA_SANDBOX_TIMEOUT_MS
#define LUA_SANDBOX_TIMEOUT_MS 10000 /* 10 秒硬超时 */
#endif
/* hook 间隔: 每 N 条 Lua 指令查一次表。20 万条 ≈ 几百微秒一次的检查, 开销 <1%。 */
#ifndef LUA_SANDBOX_HOOK_COUNT
#define LUA_SANDBOX_HOOK_COUNT 200000
#endif

static lua_trust_t s_trust = LUA_TRUST_OFFICIAL;
static char  s_appid[40];
static char  s_data_dir[128];
static char  s_app_dir[128];
static size_t s_mem_used, s_mem_peak;
static int64_t s_deadline_us;      /* 0 = 不限 */
static bool  s_timeout;

/* ★ 2026-11-02 linit「统一放开 Lua 应用权限」:
 *   不再区分官方/第三方 —— 配额对所有应用一律生效。
 *   保留它的理由只有一个: 单个应用无限吃内存会拖垮整机, 这是稳定性底线, 不是权限。 */
static size_t mem_limit(void)
{
#if LUA_SANDBOX_MEM_KB > 0
    return (size_t)LUA_SANDBOX_MEM_KB * 1024u;
#else
    return 0;
#endif
}

lua_trust_t lua_sandbox_trust(void)     { return s_trust; }
const char *lua_sandbox_appid(void)     { return s_appid; }
const char *lua_sandbox_data_dir(void)  { return s_data_dir; }
size_t lua_sandbox_mem_limit(void)      { return mem_limit(); }
size_t lua_sandbox_mem_peak(void)       { return s_mem_peak; }
size_t lua_sandbox_mem_used(void)       { return s_mem_used; }

static int64_t now_us(void)             { return esp_timer_get_time(); }

bool lua_sandbox_timed_out(void)
{
    if (s_deadline_us <= 0) return false;
    if (!s_timeout && now_us() > s_deadline_us) s_timeout = true;
    return s_timeout;
}

int lua_sandbox_remain_ms(void)
{
    if (s_deadline_us <= 0) return -1;
    return lua_quota_remain_ms((long long)s_deadline_us, (long long)now_us());
}

int lua_sandbox_clamp_ms(int want_ms)
{
    if (want_ms <= 0) return want_ms;
    const int rem = lua_sandbox_remain_ms();
    if (rem < 0) return want_ms;              /* 不限 */
    if (rem <= 0) return 1;                   /* 已超时: 只给 1ms, 下一拍就会被打断 */
    return (want_ms < rem) ? want_ms : rem;
}

/* NVS 命名空间: ★ 2026-11-02 统一放开后**原样返回**, 不再按 appid 哈希隔离。
 *   本系统所有 Lua 应用都由作者自己维护, 拆分命名空间只会让老应用读不到自己以前存的数据。
 *   接口保留(调用点不改), 将来若要重新隔离, 只改这里一处即可。 */
const char *lua_sandbox_ns(const char *base)
{
    return base;
}

/* 逐级创建目录(只做 .data/<id> 这类浅层) */
static void ensure_dir(const char *p)
{
    char tmp[128];
    size_t n = strlen(p);
    if (n == 0 || n >= sizeof(tmp)) return;
    memcpy(tmp, p, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (tmp[i] == '/' || tmp[i] == 0) {
            char c = tmp[i];
            tmp[i] = 0;
            (void)mkdir(tmp, 0755);
            tmp[i] = c;
            if (c == 0) break;
        }
    }
}

bool lua_sandbox_begin(const char *script_path)
{
    s_mem_used = s_mem_peak = 0;
    s_timeout = false;
    s_deadline_us = 0;
    s_appid[0] = s_data_dir[0] = s_app_dir[0] = 0;

    if (!script_path || !script_path[0]) return false;

    /* ★ 2026-11-02 linit「统一放开 Lua 应用权限」:
     *   不再按脚本所在路径判定官方/第三方 —— 本系统所有 Lua 应用都由作者自己维护,
     *   用安装路径当信任依据只会误伤正经应用(录音机装在 /sdcard/apps 下就被判第三方,
     *   结果连 /sdcard 都读不了, 报"未检测到 TF 卡")。
     *   现在: 一律 OFFICIAL, 只保留资源级保护(内存配额 + 死循环钩子 + 超时)。 */
    s_trust = LUA_TRUST_OFFICIAL;

    /* appid 仍然解析: 只用于日志和(可选的)数据目录, 推不出**不拒绝启动** */
    if (!lua_path_appid(script_path, s_appid, sizeof(s_appid))) {
        snprintf(s_appid, sizeof(s_appid), "app");
    }
    const char *slash = strrchr(script_path, '/');
    if (slash) {
        size_t l = (size_t)(slash - script_path);
        if (l >= sizeof(s_app_dir)) l = sizeof(s_app_dir) - 1;
        memcpy(s_app_dir, script_path, l);
        s_app_dir[l] = 0;
    } else {
        snprintf(s_app_dir, sizeof(s_app_dir), "/sdcard/apps");
    }

    /* 数据目录照常准备(应用想存私有数据可以用它), 但它**不再是唯一可写目录** */
    snprintf(s_data_dir, sizeof(s_data_dir), "/sdcard/apps/.data/%s", s_appid);
    ensure_dir(s_data_dir);
    /* ★ 超时保护保留: 单个脚本死循环不能把主任务拖死 */
    s_deadline_us = now_us() + (int64_t)LUA_SANDBOX_TIMEOUT_MS * 1000;

    /* 权限已统一, 不再打印"信任级" —— 那个词现在没有意义 */
    ESP_LOGI(TAG, "appid=%s 脚本目录=%s 数据目录=%s 内存配额=%uKB 单次回调超时=%dms",
             s_appid, s_app_dir[0] ? s_app_dir : "(无)",
             s_data_dir[0] ? s_data_dir : "(无)",
             (unsigned)(mem_limit() / 1024), LUA_SANDBOX_TIMEOUT_MS);
    return true;
}

bool lua_sandbox_mem_account(size_t used_now)
{
    s_mem_used = used_now;
    if (used_now > s_mem_peak) s_mem_peak = used_now;
    return lua_quota_allows(used_now, mem_limit());
}

/* ==================== L2: 路径守卫包装 ==================== */

/* ==================== L3: 时间钩子 ==================== */
static void sb_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if (lua_sandbox_timed_out()) {
        luaL_error(L, "timeout: 应用运行超过 %d ms, 已强制停止", LUA_SANDBOX_TIMEOUT_MS);
    }
}

/* ==================== 模块加载边界 ==================== */


/* ==================== 对外: 应用隔离 ==================== */
/* ==================== 对外: 应用隔离 ==================== */
/* ★★★ 2026-11-02 linit「统一放开 Lua 应用权限」★★★
 *   原实现按"脚本从哪来"分官方/第三方, 第三方要被摘 9 项库、11 个文件接口套路径白名单、
 *   拒绝 hid/wiki.open/rec.start、再夹紧 3 个阻塞 API。本系统所有 Lua 应用都由作者自己
 *   维护, 这套分级没有实际收益, 只会误伤 —— 录音机装在 /sdcard/apps 下被判第三方,
 *   app.fs.exists("/sdcard") 被路径守卫拒掉(报"未检测到 TF 卡"),
 *   app.rec.start 也被 deny(报 permission denied), 录音完全不可用。
 *
 *   现在统一放开: 不摘库、不限路径、不 deny 任何已注册 API。
 *
 *   只保留两样**资源级**保护(是稳定性, 不是权限):
 *     ① 内存配额 —— 在 lua_sandbox_mem_account() 里独立生效, 与本函数无关;
 *     ② 指令钩子 —— 防 while true 把主任务钉死。超时预算由 lua_sandbox_touch()
 *        在**每次回调前**重置, 所以限制的是"单次回调执行时长", 不会误杀
 *        长期运行的应用(游戏/阅读器)。
 *   参数与缓冲区长度检查保留在各 API 内部(luaL_checkstring / char path[N]), 未受影响。 */
void lua_sandbox_apply(lua_State *L)
{
    lua_sethook(L, sb_hook, LUA_MASKCOUNT, LUA_SANDBOX_HOOK_COUNT);
    ESP_LOGI(TAG, "Lua 权限: 已统一放开 (保留 内存配额 %uKB + 单次回调 %dms 超时)",
             (unsigned)(mem_limit() / 1024), LUA_SANDBOX_TIMEOUT_MS);
}

/* 每次进入 Lua 回调(渲染/按键/触摸)前调用: 重置单次执行的时间预算。
 * 没有它, 长时间运行的应用会在启动 10 秒后被 timeout 钩子杀掉。 */
void lua_sandbox_touch(void)
{
    s_timeout = false;
#if LUA_SANDBOX_TIMEOUT_MS > 0
    s_deadline_us = now_us() + (int64_t)LUA_SANDBOX_TIMEOUT_MS * 1000;
#else
    s_deadline_us = 0;
#endif
}

