/**
 * page_lua.c — Lua 单文件程序 通用应用页 (可放到内存卡运行的单文件程序).
 *
 * 页面结构: 单页双模式状态机.
 *   MODE_LIST — os_pane 双栏列表浏览 /sdcard/apps 下的 .lua 单文件; 确认选中 → 加载运行.
 *   MODE_RUN  — 为本次运行新建 lua_State (PSRAM 分配器), 执行该 .lua 单文件, 并把
 *               os 绘制/按键/时间/声音/震动回调桥接给 Lua (全局 table `app`).
 *
 * Lua 全局函数契约 (可选, 由脚本定义; 页面对应 os 回调里调用, 不存在则跳过):
 *   on_enter()  进入 RUN 态前调用
 *   render()    os 渲染回调里调用 (清屏 + 全套 app.* 自绘), 不要在里面 flush
 *   action(a)   os 按键回调里调用, 同时该 a 也投进 app.key() 的 FIFO
 *   poll()      os 每帧轮询里调用 (计时/刷新, 非阻塞)
 *   on_exit()   退出 RUN 态时调用 (清理/提示音收尾)
 *
 * 本页 fullscreen=true; os_render 会在模块 render 后统一 flush, 脚本绘制 API 不 flush.
 */
#include "os.h"
#include "netscan.h"   /* V1.2: 端口探测/ARP解析/MAC厂商 (L1 服务) */
#include "os_pane.h"
#include "lua_bridge.h"   /* 模块化后: 桥 API 在 components/apps/lua_bridge/ */
#include "lua_sandbox.h"  /* ★ 2026-10-31: 第三方应用信任级/路径/配额/时限 */
#include "lua_quota.h"    /* 配额账本算术(纯 C, 与主机测试同一份) */
#include "lua_exit_policy.h"  /* ★ 2026-11-23: 退出收尾判据(纯逻辑, 主机测试同一份) */
#include "statusbar_policy.h" /* ★ 2026-11-23: 状态栏三态声明 OS_SB_AUTO/HIDE/SHOW */
#include "gps.h"        /* 退出应用时兜底关 GPS (见 lua_stop) */
#include "gps_nmea.h"    /* gps_service_stop() */
#include "ui_common.h"
#include "os_notify.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"    /* EXT_RAM_BSS_ATTR: 大缓冲入 PSRAM, 省内部 RAM */
#include "esp_attr.h"
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>

#include "input.h"
#include "virtual_keys.h" /* ★ 2026-10-31: Lua 应用可用系统虚拟手柄 (app.vk) */
#include "esp_attr.h"      /* RTC_NOINIT_ATTR (崩溃定位探针) */
#include "esp_system.h"    /* esp_reset_reason() */
#include "esp_heap_caps.h" /* heap_caps_get_free_size (探针资源快照) */
#include "board_battery.h"
#include "os_hid.h"
#include "audio_player.h"   /* ★ 2026-11-22: 退出兜底回收麦克风录音 / 应用播放的音频 */
#include "espnow_link.h"    /* ★ 2026-11-22: 退出兜底拆掉应用自己开的 ESP-NOW 链路 */
#include "os_wifi.h"
#include "os_net.h"
#include "wifi_manager.h"
#include "esp_http_client.h"
#include "svc_nvs.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"
#include <dirent.h>
#include "esp_netif_ip_addr.h"
#include "font_zh.h"
#include "font_zh16.h"
#include "keyboard.h"
#define TAG "LUAAPP"

#define LUA_ROOT_PATH "/sdcard/apps"
/* 应用管家接管启动: 待运行的 .lua 全路径; 非空则 on_enter 直接进 RUN, 一次性消费.
 * 独立 Lua 启动器已移除 (应用管家统一接管), 此路径由 os_page_lua_set_boot 设置.
 * ★ 2026-11-22 (第2回合): 128 → 192, 与 am_app_core.h 的 AM_CORE_PATH_LEN 一致。
 *   原来应用管家送来的是 192 字节缓冲里的路径, 这里存进 128 字节就被 snprintf **静默截断** ——
 *   目录深一点 / 文件名带几个中文 (UTF-8 3 字节/字) 就超, 截断后 fopen 失败, 用户只看到
 *   "无法打开", 而同一路径在应用中心里明明能列出来。 */
#define LUA_BOOT_PATH_LEN 192
static char s_boot_path[LUA_BOOT_PATH_LEN] = "";
void os_page_lua_set_boot(const char *path) {
    if (path) snprintf(s_boot_path, sizeof(s_boot_path), "%s", path);
    else      s_boot_path[0] = '\0';
}

/* ==== 崩溃定位探针 (2026-09-28 建立; 2026-09-29 增强资源快照) ====
 * 背景: 首次打开 Lua 应用整机重启, 串口在复位瞬间丢字抓不到 panic → 用阶段号反推死在哪一步.
 * 开机由 apps_register_all() 调 lua_probe_boot_report() 打印 (立即 + 10s 后复读一次).
 *
 * 2026-09-29 增强: 只记"哪一步"还不够 —— 加记**当时的资源快照**, 一次就能区分三类根因
 *   (本项目历史上三类都踩过):
 *     ① 栈被吃光   → "栈余" 接近 0 词 (main 任务默认栈 12288B, 实测稳态只剩 ~10.1KB 可借,
 *                     而 luaL_openlibs + 解析 30~45KB 脚本是 C 栈大户)
 *     ② 堆被吃光   → 内部堆/PSRAM 空闲接近 0
 *     ③ 禁 cache 窗口访问 PSRAM (lua_State 与脚本缓冲都在 PSRAM)
 *                  → 栈与堆都还够, 但复位原因=panic/assert (见项目约定"禁 cache 期间禁止 PSRAM 栈")
 *   另外记下**是哪个任务**跑到这一步: 若不在 main 上, 就不该是 main 的栈问题, 指向 ③。 */
#define LUA_PROBE_MAGIC 0x4C554150u     /* "LUAP" */
static RTC_NOINIT_ATTR uint32_t s_probe_magic;
static RTC_NOINIT_ATTR uint32_t s_probe_stage;
static RTC_NOINIT_ATTR char     s_probe_info[40];
static RTC_NOINIT_ATTR uint32_t s_probe_stack;   /* 当前任务栈历史最低余量 (词, 1 词=4B) */
static RTC_NOINIT_ATTR uint32_t s_probe_int;     /* 内部堆空闲 */
static RTC_NOINIT_ATTR uint32_t s_probe_ps;      /* PSRAM 空闲 */
static RTC_NOINIT_ATTR char     s_probe_task[16];

void lua_probe_stage(int stage, const char *info) {
    s_probe_magic = LUA_PROBE_MAGIC;
    s_probe_stage = (uint32_t)stage;
    s_probe_info[0] = '\0';
    if (info) snprintf(s_probe_info, sizeof(s_probe_info), "%s", info);
    /* 资源快照: 栈用"历史最低余量"(uxTaskGetStackHighWaterMark), 它是单调下降的,
     * 所以崩后再读到的就是**崩前最紧张那一刻**的值 ✓ */
    s_probe_stack = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    s_probe_int   = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_probe_ps    = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const char *tn = pcTaskGetName(NULL);
    snprintf(s_probe_task, sizeof(s_probe_task), "%s", tn ? tn : "?");
}

/* 复位原因是否属于"真崩溃"。只有这几种才需要把探针当现场看;
 *   上电(1)/软重启(3)/深睡唤醒(2)/USB(11)/JTAG(12) 等属正常复位,
 *   探针里留着的是**上一次正常走到的阶段**(比如打开内置应用后停在阶段1),
 *   拿它判断崩溃就是误报 —— 之前每次 reboot 都刷一条"阶段=1", 白吓人。 */
static bool reset_is_crash(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
        return true;
    default:
        return false;
    }
}

static void lua_probe_print_once(void) {
    esp_reset_reason_t r = esp_reset_reason();
    const bool crash = reset_is_crash(r);
    if (s_probe_magic != LUA_PROBE_MAGIC) {
        if (crash) ESP_LOGW("LUAPROBE", "无探针记录 | 本次复位原因=%d", (int)r);
        else       ESP_LOGI("LUAPROBE", "无探针记录 | 本次复位原因=%d(非崩溃)", (int)r);
        return;
    }
    /* ★ 2026-11-08: 崩溃时必须 WARN —— 原来用 ESP_LOGD, 默认日志级别下根本打不出来。
     * ★ 2026-11-21: 非崩溃复位降 INFO, 并明确标注"无参考价值", 避免把
     *   "上次走到阶段1就正常退出"当成崩溃现场。 */
    if (crash)
        ESP_LOGW("LUAPROBE", "上次复位前: 阶段=%u '%s' 任务=%s 栈余=%u词(≈%uB) 内部堆=%u PSRAM=%u | 复位原因=%d",
                 (unsigned)s_probe_stage, s_probe_info, s_probe_task,
                 (unsigned)s_probe_stack, (unsigned)s_probe_stack * 4u,
                 (unsigned)s_probe_int, (unsigned)s_probe_ps, (int)r);
    else
        ESP_LOGI("LUAPROBE", "上次复位前: 阶段=%u '%s' | 复位原因=%d(非崩溃, 阶段号仅为上次停留位置, 无参考价值)",
                 (unsigned)s_probe_stage, s_probe_info, (int)r);
}

/* 开机延迟复读: 复位瞬间 USB CDC 重枚举, 早期启动日志必丢 (实测 0.26s→5.1s 空档),
 * 开机 10 秒后再打一遍, 此时串口链路已稳定 → 崩溃现场必被抓到. */
static void lua_probe_delayed_cb(void *arg) {
    (void)arg;
    lua_probe_print_once();
}

/* ============================ 启动/退出分阶段计时 ============================
 * 默认**关闭**；串口 `luaprof 1` 打开、`luaprof 0` 关闭，日志标签 LUAPROF。
 * 只在开关打开时读时钟/打日志，正常运行时零开销（一次 bool 判断）。
 * 用法：luaprof 1 → 打开应用 → 看每段 +xx us → 退出 → 看退出的分段。
 * ========================================================================== */
static bool    s_prof_on = false;
static int64_t s_prof_t0 = 0;
static uint32_t s_prof_frame_n = 0;   /* 首帧探针计数: 每次进应用归零 */
static uint32_t s_last_render_ms = 0; /* 上一帧 render 的时刻 → 判"是否在跑帧动画" */

void lua_prof_set(int on) { s_prof_on = (on != 0); s_prof_t0 = esp_timer_get_time(); }
int  lua_prof_get(void)    { return s_prof_on ? 1 : 0; }

static inline void prof_mark(const char *tag)
{
    if (!s_prof_on) return;
    int64_t now = esp_timer_get_time();
    ESP_LOGI("LUAPROF", "%-14s +%6lld us", tag, (long long)(now - s_prof_t0));
    s_prof_t0 = now;
}

void lua_probe_boot_report(void) {
    lua_probe_print_once();
    /* ⚠ 不在开机清零: 阶段号要一直保留到"下一次启动流程"覆盖它 ——
     * 否则复位后第一次上电就把崩溃现场抹掉, 再复位也读不到了.
     * 判读: 13 = 正常跑完; 1..12 = 崩在对应阶段; 0/'boot' = 还没启动过 Lua 应用. */
    const esp_timer_create_args_t targs = {
        .callback = lua_probe_delayed_cb, .name = "luaprobe" };
    esp_timer_handle_t th = NULL;
    if (esp_timer_create(&targs, &th) == ESP_OK)
        esp_timer_start_once(th, 10 * 1000 * 1000);
}

st7305_handle_t *s_lcd = NULL;
ui_ctx_t   *s_lua_ctx = NULL;   /* RUN 态可重绘上下文 */


typedef enum { LUA_MODE_LIST, LUA_MODE_RUN } lua_mode_t;

static lua_mode_t s_mode = LUA_MODE_LIST;
lua_State   *s_L = NULL;
char         s_err[96];      /* 最近一次 Lua 错误摘要 */
bool         s_err_pending;  /* 待在下一次 poll/action 弹 toast */
bool         s_exit_req;     /* 脚本调用 app.exit() 后置位, 页面择机退出 RUN 态 */
/* ★ 2026-10-03 全屏程序/游戏的"退出确认" (linit 规范: 按返回先问一句, 防误退丢进度)
 *   默认 true —— 所有 Lua 程序/游戏都弹; 脚本可 app.exit_confirm(false) 单独关掉。
 *   ⚠ 只在"脚本没消费 BACK"时生效: 脚本把 BACK 当返回键用 (关子窗/收菜单) 依旧优先。 */
bool         g_lua_exit_ask = true;
static bool  s_exit_dlg = false;   /* 确认框已弹出 → 防重复弹 (弹窗期间 BACK 归弹窗自己处理) */

/* ★ 2026-11-23 linit (第2轮): 本页状态栏声明 —— **三态**, 事件驱动。
 *
 *   旧写法是 `bool s_sb_want` + 在 render 里每帧调 os_statusbar_show(s_sb_want):
 *   一旦某帧它是 false(例如应用切换的瞬间), 就把 HIDE 声明写进 OS, 之后哪怕脚本
 *   从未表态也一直藏着 —— 页面对"没表态"和"表态不要"没有区分, 是状态栏丢失的结构性原因。
 *
 *   现在:
 *     OS_SB_AUTO  = 没表态 (脚本没调 app.statusbar) → OS 按"非全屏就显示"推
 *     OS_SB_HIDE  = 脚本明确 app.statusbar(false)
 *     OS_SB_SHOW  = 脚本明确 app.statusbar(true)
 *   声明**只在变化时**下发, render 不再每帧覆盖; 新应用启动时复位为 AUTO
 *   (os_statusbar_auto), 既清掉上一个应用的声明, 又不会把"没表态"误当成"不要"。
 */
static int   s_sb_req = OS_SB_AUTO;

void os_page_lua_set_statusbar(bool on)
{
    s_sb_req = on ? OS_SB_SHOW : OS_SB_HIDE;
    os_statusbar_show(on);                           /* 声明立即生效 (OS 负责重绘) */
    if (s_lua_ctx) s_lua_ctx->needs_redraw = true;
}

/* 新应用启动: 回到"没表态"。OS 侧一并撤销声明, 否则会沿用上一个应用的 HIDE。 */
static void lua_statusbar_reset(void)
{
    s_sb_req = OS_SB_AUTO;
    os_statusbar_auto();
}

/* 状态栏高度见 os.h 的 OS_STATUS_H (与 svc_statusbar.c 的 SB_STRIP_H 同值)。 */
static bool  s_exit_pend = false;  /* 已请求退出, 待下一帧 poll 统一弹确认框 (避免回调里重入) */
static bool  s_ran = false;        /* 应用是否真的跑起来过 (没跑起来 → 不弹确认, 直接退) */

/* ==== 2026-10-31 系统虚拟手柄 (Lua 侧) ====
 * 脚本用 app.vk(true) 声明"我要虚拟手柄"；本页负责 enable / 绘制 / 按键翻译。
 * 脚本仍然只从 action(a) 收键 —— 不新增第二套输入路径，也不改游戏引擎的 vk 设置。 */
extern bool lua_vk_want;              /* luat_lib_input.c: app.vk() */
static bool     s_vk_on;              /* 本页是否已启用虚拟手柄 */
static uint8_t  s_vk_prev = 0xFF;     /* 上一帧**消抖后**虚拟手柄掩码(边沿检测), 1=松开 */
static uint8_t  s_vk_dir;             /* 当前按住的方向位(单一位), 用于长按重复 */
static uint32_t s_vk_dir_t0, s_vk_dir_last;
/* ★ 2026-11-07: 虚拟按键消抖 + 长按上限。
 *   现象(BiOS模拟器实测): 点一下虚拟键, 菜单/数值连着动好几下。
 *   根因: 指纹大小的触点落在两个 hit 区交界处时, 触摸坐标抖动会让掩码在
 *     "按下/松开"之间反复跳变; 原来的边沿检测**没有任何时间去抖**, 每跳一次就发一个
 *     action。方向键重复也只有"400ms 后每 120ms 一次", 一旦漏掉一次释放就是无限重复。
 *   这里对齐 input.c 实体键那套(30ms 防抖 + 1.5s hold-cap), 阈值取 35ms。 */
#define VK_DEBOUNCE_MS   35
#define VK_REPEAT_DELAY  450
#define VK_REPEAT_RATE   120
#define VK_REPEAT_CAP_MS 1500
static uint8_t  s_vk_db = 0xFF;       /* 消抖后的稳定掩码 */
static uint8_t  s_vk_raw = 0xFF;      /* 上帧原始掩码(用于判定"已稳定多久") */
static uint32_t s_vk_raw_ms;          /* 原始掩码最后变化的时刻 */

/* 虚拟手柄掩码位 → OS 动作 (位定义见 virtual_keys.h: 低电平有效) */
static os_action_t vk_bit_to_action(uint32_t bit)
{
    switch (bit) {
    case 1u << 0: return OS_ACTION_CONFIRM;   /* A */
    case 1u << 1: return OS_ACTION_BACK;      /* B */
    case 1u << 4: return OS_ACTION_RIGHT;
    case 1u << 5: return OS_ACTION_LEFT;
    case 1u << 6: return OS_ACTION_UP;
    case 1u << 7: return OS_ACTION_DOWN;
    default:      return OS_ACTION_NONE;      /* Select/Start 暂不映射 */
    }
}

/* ---- 按键 FIFO: 承接 os action, 供 Lua app.key() 非阻塞读取 ---- */
int g_lua_keys[KEY_FIFO_N];
int g_lua_keys_head, g_lua_keys_tail;   /* tail==head 时空 */

void key_push(int a) {
    int nt = (g_lua_keys_tail + 1) % KEY_FIFO_N;
    if (nt == g_lua_keys_head) {            /* 满 → 丢弃最旧一条 */
        g_lua_keys_head = (g_lua_keys_head + 1) % KEY_FIFO_N;
        g_lua_keys[nt] = a;
        g_lua_keys_tail = nt;
        return;
    }
    g_lua_keys[g_lua_keys_tail] = a;
    g_lua_keys_tail = nt;
}

/* ---- 双栏列表 (PSRAM 状态, 省内部 RAM) ---- */
/* ★ LINTOS 2026-10-25: 列表 pane(约 59KB) 原来静态常驻 PSRAM, 改为进页面才分配/退出即还 */
static os_pane_t *s_pane__p;
static os_pane_t *s_pane__get(void) {
    if (!s_pane__p) {
        s_pane__p = heap_caps_calloc(1, sizeof(os_pane_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pane__p) ESP_LOGW("PANE", "列表 pane 分配失败 (%u B)", (unsigned)sizeof(os_pane_t));
    }
    if (!s_pane__p) s_pane__p = heap_caps_calloc(1, sizeof(os_pane_t), MALLOC_CAP_8BIT);   /* 兜底: 内部 RAM */
    return s_pane__p;
}
static inline void s_pane__free(void) { if (s_pane__p) { heap_caps_free(s_pane__p); s_pane__p = NULL; } }
#define s_pane (*s_pane__get())

static bool is_lua(const char *name) {
    size_t len = strlen(name);
    return (len > 4) && (strcasecmp(name + len - 4, ".lua") == 0);
}

static void lua_stop_ex(ui_ctx_t *ctx, bool leaving_page);
static void lua_stop(ui_ctx_t *ctx) { lua_stop_ex(ctx, false); }

/* ---- 报错策略: 不再"一报错就退出应用" ----
 * 旧行为: poll()/render()/action() 任何报错都 lua_stop() → 用户感受是"应用一打开就没了",
 * 一个 typo 就白屏回列表, 连错在哪都看不到。
 * 新行为: 首次报错 → 串口打印 + 弹一次 toast, **应用继续跑**;
 *         但同一处**连续**报错到 LUA_ERR_MAX 次就退回列表(防死循环刷屏 / 防卡死)。
 * 任何一次成功调用即清零。 */
/* ★ 2026-11-22: 90 → 6。
 *   90 次 × 500ms 限流 = **45 秒**才退回列表 —— 用户体感就是"按什么都没反应"。
 *   6 次 ≈ 3 秒, 仍保留限流(偶发错误不会被一次就杀掉), 但不再让人干等半分钟。
 *   ⚠ 超时类错误**不走这个计数**: 见 lua_handle_timeout(), 命中即停。 */
#define LUA_ERR_MAX 6
static int s_err_streak;
static const char *s_err_name;      /* 正在累积连续错误的是哪个回调 (NULL = 没有) */

/* ---- 错误计数限流 + 屏幕常驻错误条 (2026-10-03 重做) ----
 * 旧实现的致命处: 计数**每帧都 +1**。而 render() 是每帧调用的 → 一个坏参数
 * (如给 bmpr 传了小数) 就每帧报一次 → LUA_ERR_MAX=90 只需 **3 秒**就攒满,
 * 应用被"打开三秒自动退出", 用户只看到一闪而过的 toast → 根因无从得知。
 * 现在:
 *   ① 计数**限流**: 两次计数至少隔 LUA_ERR_GAP_MS(500ms) → 6 次≈3 秒, 仍留观察时间;
 *      (限流只为"别把一个偶发错误连记 90 次", 不是为了让人干等)
 *   ② **屏幕底部常驻错误条** (两行, 带 Lua 的行号与消息), 每次出错刷新, 持续 6 秒
 *      → 用户可以直接拍照/截图, 不用连串口;
 *   ③ toast 仍保留 (仅第一次), 保持原有观感。 */
#define LUA_ERR_GAP_MS     500
#define LUA_ERR_IDLE_MS    5000   /* 距上次出错超过这个间隔 ⇒ 视为新的一串, 计数重置 */
#define LUA_ERR_BANNER_MS  6000
static uint32_t s_err_last_ms;
static char     s_err_banner[136];
static uint32_t s_err_banner_until;

/* 按**字节**截断但同时避免切断 UTF-8 (错误消息多为 ASCII, 但应用的 msg 可能含中文) */
static void lua_err_clip(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n > cap - 1) n = utf8_align(src, cap - 1);   /* 共享实现, 不留半个汉字 */
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* ★★★ 2026-11-22: Lua 应用生命周期资源**统一兜底回收** ★★★
 * 以前只有 GPS / 虚拟手柄 / 提示音三样有兜底，麦克风录音、应用播放的音频、
 * ESP-NOW 链路、应用自己开的 USB HID 全靠脚本 on_exit 自觉停 ⇒ 脚本崩了、
 * 被超时强杀、或覆盖安装，这些就一直留在后台（耗电 + 污染下一个应用）。
 *
 * 规矩:
 *   ① 只回收 **Lua 应用自己开的**：HID 用 lua_hid_release_if_owned() 判所有权，
 *      因为设置页/日志页/收藏页也用同一套 os_hw 引用计数；
 *   ② 每一项都幂等（内部自己判状态），重复调用无害 ⇒ 多条退出路径可以都调；
 *   ③ **不碰** WiFi / 蓝牙：那是设置页的权威状态，系统持续托管，不随应用退出改变。 */
static void lua_release_app_res(void) {
    const bool had_rec  = audio_player_rec_active();      /* 麦克风录音 (app.rec.start) */
    const bool had_aud  = audio_player_any_playing();     /* 应用播放的 WAV/MP3 */
    const bool had_now  = espnow_link_is_running();       /* app.espnow.start */
    if (had_rec) audio_player_rec_stop();                 /* 只置停止标志, 录音任务自己收尾 */
    if (had_aud) audio_player_stop_all();
    if (had_now) espnow_link_stop();                      /* 只拆 ESP-NOW 链路, WiFi 不动 */
    const bool had_hid  = lua_hid_release_if_owned();     /* 仅当本应用开过 HID */
    /* ★★★ 2026-11-23: 震动也要兜底停 ★★★
     *   app.vibrate() 的实质是 `out_set(strength)` 把 duty 拉起来 + 起一个 50ms 软件定时器
     *   到点归零。脚本崩溃 / 被超时强杀 / on_exit 忘写 时, 这个"到点归零"没人保证:
     *   一旦那次 xTimerStart 因命令队列满而失败(阻塞参数是 0, 失败只返回 pdFAIL 不报错),
     *   duty 就永久停在 255 ⇒ 现场表现正是"点电子木鱼后震动一直不停, 随后重启"。
     *   vibrator_stop() 幂等(s_mode==VIB_NONE 直接返回), 多调无害。 */
    os_notify_vibrate_stop();
    if (had_rec || had_aud || had_now || had_hid)
        ESP_LOGI(TAG, "退出兜底回收: 录音=%d 音频=%d ESP-NOW=%d HID=%d",
                 (int)had_rec, (int)had_aud, (int)had_now, (int)had_hid);
}

/* ★★★ 2026-11-22: 超时 = 立即停止应用 ★★★
 * 旧行为: 超时错误和普通脚本错误走同一套计数 ⇒ hook 每 10 秒报一次错, 要攒满
 *   LUA_ERR_MAX 次才停 ⇒ `while true do end` 要 **90×10s = 15 分钟**才退出,
 *   期间主循环每帧被占满 10 秒, 触摸/按键/绘制全停(体感就是死机)。
 * 新行为: 只要本次 pcall 是被超时钩子打断的, 立刻 lua_stop() —— 一个超时周期就收尾,
 *   并把原因写进 toast 与屏幕错误条, 用户知道"是应用被停止了", 而不是莫名其妙回到列表。 */
static void lua_handle_timeout(ui_ctx_t *ctx, const char *name) {
    const uint32_t now = pl_now_ms();
    ESP_LOGW(TAG, "Lua %s() 运行超时 → 立即停止应用", name);
    snprintf(s_err_banner, sizeof(s_err_banner), "%s(): timeout", name);
    s_err_banner_until = now + LUA_ERR_BANNER_MS;
    snprintf(s_err, sizeof(s_err), "应用已停止：%s 运行超时", name);
    s_err_pending = true;
    s_err_streak = 0;
    s_err_name = NULL;
    lua_stop(ctx);      /* 关 VM + 兜底回收资源 + 回列表; 幂等, 重复调用安全 */
}

/* ★ 2026-11-22: 只有**出错的那个回调自己**成功返回, 才清掉连续错误计数。
 *   旧逻辑是"任何一次成功调用即清零" —— 而 render() 每帧都会失败、poll() 每帧都成功,
 *   于是计数被 poll 反复清零, 一个"render 永久报错"的应用**永远不会被停止**,
 *   6/90 次阈值形同虚设(只留下每帧重画一条错误条)。 */
static void lua_err_mark_ok(const char *name) {
    if (s_err_name && name && strcmp(s_err_name, name) == 0) {
        s_err_streak = 0;
        s_err_last_ms = 0;
        s_err_name = NULL;
    }
}

static void lua_report_error(ui_ctx_t *ctx, const char *name) {
    /* VM 可能已被超时路径关掉(lua_stop) ⇒ 栈不存在了, 再取值就是野指针 */
    if (!s_L) return;
    /* ★ 超时优先判定: 不计数, 直接停。判据由沙箱给(不是靠错误串里找 "timeout")。 */
    if (lua_sandbox_timeout_hit()) { lua_handle_timeout(ctx, name); return; }
    uint32_t now = pl_now_ms();
    /* ★ 计数归属: 同一个回调、且不是"隔了很久的孤立错误", 才接续上一串计数。
     *   避免"每 20 秒错一次"这种稀疏故障攒够 6 次后把正常应用误杀。 */
    const bool same_where = (s_err_name && strcmp(s_err_name, name) == 0);
    if (!same_where || (now - s_err_last_ms) > LUA_ERR_IDLE_MS) {
        s_err_streak = 0;
        s_err_last_ms = 0;                  /* 清限流界标 ⇒ 这一笔立刻计入 */
    }
    s_err_name = name;      /* ★ 记下是哪个回调在错, 见 lua_err_mark_ok() */
    const char *m = lua_tostring(s_L, -1);
    bool counted = (s_err_last_ms == 0) || (now - s_err_last_ms >= LUA_ERR_GAP_MS);
    if (counted) { s_err_streak++; s_err_last_ms = now; }
    snprintf(s_err_banner, sizeof(s_err_banner), "%s(): %s", name, m ? m : "?");
    s_err_banner_until = now + LUA_ERR_BANNER_MS;      /* 每次出错都续期 */
    if (s_err_streak <= 1) {
        /* ★ 2026-11-02: LOGD → LOGW。回调里的错误(render/action/on_enter)原来
         * 在默认 INFO 等级下也是整级被过滤, 串口看不到任何线索。 */
        ESP_LOGW(TAG, "Lua %s() 错误: %s", name, m ? m : "(null)");
        snprintf(s_err, sizeof(s_err), "%s: %s", name, m ? m : "?");
        s_err_pending = true;
    } else if ((s_err_streak % 30) == 0) {
        ESP_LOGW(TAG, "Lua %s() 连续出错 %d 次: %s", name, s_err_streak, m ? m : "(null)");
    }
    if (s_err_streak >= LUA_ERR_MAX) {
        ESP_LOGW(TAG, "Lua %s() 连续出错 %d 次 → 停止应用回列表", name, s_err_streak);
        snprintf(s_err, sizeof(s_err), "应用已停止：连续错误 %d 次", s_err_streak);
        s_err_pending = true;
        lua_stop(ctx);
    }
}

/* ★★★ 2026-11-22 (第2回合): 关闭 VM 的**唯一出口** ★★★
 * 以前本文件有 6 处 `lua_close(s_L)` 各写各的, 于是:
 *   ① 关了不置 NULL 的地方留下野指针 (上一轮修过一次, 但下次加分支还会漏);
 *   ② 桥里持有的**跨应用 C 状态**从来不回收 —— 已解码位图 (PSRAM, 单张地图 200KB)、
 *      app.dlg 的静态结果、app.key_repeat 的按住表, 都会带到下一个应用;
 *   ③ app.util 的定时器槽不清, 新 VM 里会拿旧 registry 引用去 pcall。
 * 现在统一走这里: 先回收桥状态 (此时 s_L 还有效, 无所谓先后), 再 lua_close, 再置空。
 * ⚠ 任何新增的"关 VM"路径都必须调本函数, 不要直接 lua_close(s_L)。 */
static void lua_vm_close(void) {
    if (s_L) {
        lua_bridge_reset_app_state();   /* 位图/定时器/弹窗结果/按键重复 (幂等) */
        lua_close(s_L);
        s_L = NULL;
    }
}

/* 屏幕底部两行错误条 (黑底白字, 48 字符/行 = 384px) */
static void lua_draw_err_banner(st7305_handle_t *lcd) {
    if (!lcd || !s_err_banner[0]) return;
    if ((int32_t)(s_err_banner_until - pl_now_ms()) <= 0) return;
    const int y0 = UI_SCREEN_H - 36;
    fill_rect(lcd, 0, y0, UI_SCREEN_W - 1, UI_SCREEN_H - 1, ST7305_COLOR_BLACK);
    char l1[49], l2[49];
    lua_err_clip(l1, sizeof(l1), s_err_banner);
    lua_err_clip(l2, sizeof(l2), s_err_banner + strlen(l1));
    draw_text(lcd, 4, y0 + 2,  l1, true);
    draw_text(lcd, 4, y0 + 19, l2, true);
}


/* ============ Lua 分配器: 全部走 PSRAM, 大块脚本/字符串不占内部 RAM ============
 * ★ 2026-10-31: 同时做**内存配额统计** —— osize 是旧块大小(Lua 保证传入),
 *   已用 = 已用 - osize + nsize, 释放时扣减。只统计 Lua 自己的分配, 不掺系统 PSRAM。
 *   超额返回 NULL ⇒ Lua 抛 "not enough memory", 由 pcall 收住, 不会打崩系统。 */
static size_t s_lua_mem_used;
static void *lua_psram_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    if (nsize == 0) {
        if (ptr) { heap_caps_free(ptr); s_lua_mem_used = lua_quota_after(s_lua_mem_used, ptr, osize, 0); }
        return NULL;
    }
    /* ★ 账本算术统一走 lua_quota_after(): 与主机测试编译同一份源码,
     *   扩容/缩容/释放三种情形的扣减都有断言覆盖(不再依赖"看着对")。 */
    const size_t used = lua_quota_after(s_lua_mem_used, ptr, osize, nsize);
    if (!lua_sandbox_mem_account(used)) {
        ESP_LOGW("LSANDBOX", "Lua 内存超配额: 需要 %uB, 上限 %uB (峰值 %uB)",
                 (unsigned)used, (unsigned)lua_sandbox_mem_limit(),
                 (unsigned)lua_sandbox_mem_peak());
        return NULL;
    }
    void *np = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM);
    if (!np) return NULL;              /* 真失败: 不改计数 */
    s_lua_mem_used = used;
    return np;
}
void app_big_text(st7305_handle_t *lcd, int x, int y, const char *s, int scale, bool inv) {   /* [modsplit] 共享给 lua_bridge 各模块 */
    for (int i = 0; s[i]; i++) app_big_ascii(lcd, x + i * 8 * scale, y, s[i], scale, inv);
}
void app_big_text_centered(st7305_handle_t *lcd, int cx, int y, const char *s, int scale, bool inv) {   /* [modsplit] 共享给 lua_bridge 各模块 */
    int n = (int)strlen(s);
    int x = cx - n * 4 * scale;   /* 每字宽 8*scale, 居中 */
    app_big_text(lcd, x, y, s, scale, inv);
}

/* ============ Lua: 绘制桥 ============ */

/* UTF-8 首字节 → 该字符字节数。非法首字节/续字节一律按 1 处理, 保证步进永不越界
 * (旧代码写死 str += 3: 字符串里混入 ASCII 时会从半个汉字处继续解码 → 乱码,
 *  末尾 ASCII 还会越过字符串终止符多读 1~2 字节)。 */
int utf8_len(const char *s) {   /* [modsplit] 共享给 lua_bridge 各模块 */
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80)           return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* ============ Lua: 软键盘 (kbd_lib 统一入口) ============
 * 项目里"很多键盘"的真相: 内核只有 kbd_lib 一份, 7 个页面共用, 但**外观有 4 种形态**
 * (std 全高 / edit 压低 / rows 只前几排 / dialog 缩放进弹窗)。历史上 draw 与 hit 是
 * 两组函数、要调用方自己配对, 一旦配错就命中全错且不报错。
 * 这里统一成"**一次 use(样式) 选定样式+几何+布局表, 之后 draw/hit 共用同一几何**":
 *
 *   app.kbd.use("std"[, y])                   5 排全高 145px, 布局 kbd_std
 *   app.kbd.use("edit"[, y])                  排高 19px(压低 1/3), 布局 kbd_edit
 *   app.kbd.use("rows", rows[, y])            只画前 rows 排, 布局 kbd_std
 *   app.kbd.use("dialog", x, y, w, row_h)     缩放进矩形, 布局 kbd_edit
 *   app.kbd.geom()  → {style=,x=,y=,w=,row_h=,rows=}
 *   app.kbd.height() → 当前样式占用像素高
 *   app.kbd.draw([press])                      用当前几何画
 *   app.kbd.hit(tx, ty)                        用同一几何命中 → 索引 / -1  (先 x 后 y)
 *   app.kbd.press(i) → 动作码(含 caps/shift/fn 副作用) | char(i) | label(i) | layer_on(i)
 *   app.kbd.get("caps"|"shift"|"fn") / set(...) / reset() / count()
 * 动作码常量与 KBD_ACT_* 同值: app.kbd.CHAR/ENTER/BKSP/SPACE/ESC/TAB/UP/DOWN/LEFT/RIGHT/CAPS/SHIFT/FN/CTRL/ALT
 * 样式常量: app.kbd.STYLE_STD/STYLE_EDIT/STYLE_ROWS/STYLE_DIALOG
 */
kbd_t     s_kbd;   /* [modsplit] 共享给 lua_bridge 各模块 */
kbd_geom_t s_kbd_geom;   /* [modsplit] 共享给 lua_bridge 各模块 */
bool      s_kbd_inited = false;   /* [modsplit] 共享给 lua_bridge 各模块 */

void kbd_lua_sync(void) {   /* [modsplit] 共享给 lua_bridge 各模块 */
    if (!s_kbd_inited) {
        s_kbd = kbd_std;
        kbd_geom_default(&s_kbd_geom, KBD_STYLE_STD, ST7305_HEIGHT - KBD_H);
        s_kbd_inited = true;
    }
}

/* ---- app.nvs.* : 掉电保留键值 ---- */
nvs_handle_t lua_nvs_h(void) {   /* [modsplit] 共享给 lua_bridge 各模块 */
    nvs_handle_t h;
    /* ★ 2026-10-31: 第三方应用按 appid 分命名空间, 不能与其它应用共用 luaapp */
    if (svc_nvs_open(lua_sandbox_ns("luaapp"), NVS_READWRITE, &h) != ESP_OK) return 0;
    return h;
}

/* ---- app.hid.* : 电脑键鼠 ---- */
/* ================= app.db — 应用键值库 ================= */
/* 存 金币/经验/玩家档案 等核心互通小KV; 键由调用方传 "ns:field"。
 *
 * ★ 2026-10-31 越权修复(第二次): 这三个函数原来**硬编码 "core_db"**,
 *   而写路径(app.db.set/seti)已经改走 lua_sandbox_ns()。结果:
 *     - 第三方写进 appdb_xxxx, 读却读 core_db ⇒ 读不到自己刚写的值;
 *     - app.db.del 直接**删系统 core_db 的键** ⇒ 这是真正的越权(P1)。
 *   现在读写删三条全部走同一个命名空间解算, 官方应用仍得到 "core_db"(行为不变)。 */
int os_core_db_get(const char *key, char *out, size_t cap){   /* [modsplit] 共享给 lua_bridge 各模块 */
    nvs_handle_t h;
    if(svc_nvs_open(lua_sandbox_ns("core_db"), NVS_READWRITE, &h)!=ESP_OK) return -1;
    size_t len=0;
    if(svc_nvs_get_str(h,key,NULL,&len)!=ESP_OK){ svc_nvs_close(h); return -1; }
    if(cap>0 && len>=cap){ svc_nvs_close(h); return -1; }
    esp_err_t e=svc_nvs_get_str(h,key,out,&len);
    svc_nvs_close(h);
    return e==ESP_OK?(int)len:-1;
}
int os_core_db_set(const char *key, const char *val){   /* [modsplit] 共享给 lua_bridge 各模块 */
    nvs_handle_t h;
    if(svc_nvs_open(lua_sandbox_ns("core_db"), NVS_READWRITE, &h)!=ESP_OK) return -1;
    esp_err_t e=svc_nvs_set_str(h,key,val);
    if(e==ESP_OK) e=svc_nvs_commit(h);
    svc_nvs_close(h);
    return e==ESP_OK?0:-1;
}
int os_core_db_del(const char *key){   /* [modsplit] 共享给 lua_bridge 各模块 */
    nvs_handle_t h;
    if(svc_nvs_open(lua_sandbox_ns("core_db"), NVS_READWRITE, &h)!=ESP_OK) return -1;
    esp_err_t e=svc_nvs_erase_key(h,key);
    if(e==ESP_OK) e=svc_nvs_commit(h);
    svc_nvs_close(h);
    return e==ESP_OK?0:-1;
}
uint8_t lua_hidc(char c) {   /* [modsplit] 共享给 lua_bridge 各模块 */
    if (c >= 'a' && c <= 'z') return (uint8_t)(0x04 + (c - 'a'));
    if (c >= 'A' && c <= 'Z') return (uint8_t)(0x04 + (c - 'A'));
    if (c >= '0' && c <= '9') return (uint8_t)(0x1E + (c - '0'));
    switch (c) { case ' ': return 0x2C; case '.': return 0x37; case '-': return 0x2D; case ':': return 0x33; case '/': return 0x38; default: return 0; }
}

/* 注册全局 app 表 */

static void lua_setup_globals(lua_State *L) {
    /* 模块化: 具体 app.* API 都在 components/apps/lua_bridge/luat_lib_<模块>.c;
     * 这里只做调度(见 lua_bridge.h)。加新 API 去对应模块文件, 不要再往这里堆。 */
    lua_bridge_setup(L);
}

/* ★ 2026-11-22 (第2回合): 运行环境初始化必须**在保护模式里**跑。
 *   luaL_openlibs + 桥注册都会分配内存 (标准库的十几张表、几百个 C 函数),
 *   PSRAM 或配额不足时 Lua 会 throw (longjmp)。原来是**裸调**这些函数 ⇒
 *   没有 pcall 接住 ⇒ 走到 lua_atpanic → abort / 整机重启, 而且现象就是
 *   "打开某个 Lua 应用就重启", 完全看不出是内存不足。
 *   这里包一层: 失败就关 VM、给可读原因, 应用回列表 (与脚本语法错误同一套处理)。 */
static int l_init_libs(lua_State *L) {
    luaL_openlibs(L);
    lua_setup_globals(L);
    lua_sandbox_apply(L);      /* 装死循环钩子; 权限已统一放开, 不再按信任级分级 */
    return 0;
}


/* 读取 SD 卡脚本到 PSRAM 并执行; 成功返回 true */
/* ═══════════════════════════════════════════════════════════════════════════
 * Lua 源码字节码缓存 (2026-11-23 linit「还是慢」)
 *   实测(矢量地图 175KB): 打开 646ms = fread 199ms + luaL_loadbuffer 389ms(纯编译)。
 *   GPS 早已异步到后台, 不再是它。真凶是**每次打开都在现场编译 175KB 源码**。
 *   做法: 首次加载成功后 lua_dump 出字节码, 写到脚本旁边 <name>.lbc;
 *         下次直接加载字节码 ⇒ 跳过词法/语法/代码生成, 文件也小一圈(注释被丢掉)。
 *   失效判据: 源文件 size + mtime (stat, 不读内容) + Lua 版本号, 任一不符就重编并覆盖。
 *   安全边界: 读缓存/解析字节码任何一步失败都**回退源码路径**, 功能不受影响;
 *             缓存写失败(SD 只读/空间不足)只是下次继续编译, 不报错。
 *   ⛔ 不用 CRC 校验源码: 那要先把 175KB 读一遍, 省下的编译时间又赔回去了。
 * ═══════════════════════════════════════════════════════════════════════════ */
#define LBC_MAGIC     0x3143424Cu        /* "LBC1" */
#define LBC_MAX_PAY   (8u * 1024u * 1024u)
#define LBC_CACHE_MAX 300

typedef struct {
    uint32_t magic;
    uint32_t lua_ver;
    uint32_t src_size;
    uint32_t src_mtime;
    uint32_t pay_len;
} lbc_hdr_t;

typedef struct { FILE *f; size_t n; int err; } lbc_wr_t;

/* ★ 2026-11-23: newlib 的 stdio 默认缓冲很小(百字节级), 读 175KB 要走**上千次** VFS 调用,
 *   实测 fread 199ms(≈880KB/s), 远低于 SD 卡的顺序读能力。给一个 32KB 读缓冲后交给
 *   FATFS 按 4KB 扇区批量取, 顺序读能到 2~3MB/s。缓冲放 PSRAM, 不占内部 RAM。
 *   单线程使用(load_run 串行), 源码与 .lbc 共用同一块, 不并发。 */
static char s_lua_iobuf[32 * 1024] EXT_RAM_BSS_ATTR;

static int lbc_writer(lua_State *L, const void *p, size_t sz, void *ud)
{
    (void)L;
    lbc_wr_t *w = (lbc_wr_t *)ud;
    if (!w->f || fwrite(p, 1, sz, w->f) != sz) { w->err = 1; return 1; }
    w->n += sz;
    return 0;
}

/* 缓存命中? 返回 1 并给出载荷长度。任何不一致都当"没缓存"。 */
static int lbc_probe(const char *cache, long src_sz, uint32_t src_mtime, uint32_t *pay_len)
{
    FILE *f = fopen(cache, "rb");
    if (!f) return 0;
    lbc_hdr_t h;
    int got = (fread(&h, sizeof(h), 1, f) == 1);
    fclose(f);
    if (!got) return 0;
    if (h.magic != LBC_MAGIC || h.lua_ver != (uint32_t)LUA_VERSION_NUM) return 0;
    if ((long)h.src_size != src_sz || h.src_mtime != src_mtime) return 0;
    if (h.pay_len == 0 || h.pay_len > LBC_MAX_PAY) return 0;
    *pay_len = h.pay_len;
    return 1;
}

/* 刚编译完的 chunk 落盘成缓存。失败就当没这回事（下次重编）。 */
static void lbc_store(lua_State *L, const char *cache, long src_sz, uint32_t src_mtime)
{
    FILE *f = fopen(cache, "wb");
    if (!f) return;                       /* 只读卡 / 无权限: 静默放弃 */
    setvbuf(f, s_lua_iobuf, _IOFBF, sizeof(s_lua_iobuf));   /* 顺序写, 同上 */
    lbc_hdr_t h = { LBC_MAGIC, (uint32_t)LUA_VERSION_NUM,
                    (uint32_t)src_sz, src_mtime, 0 };
    if (fwrite(&h, sizeof(h), 1, f) != 1) { fclose(f); remove(cache); return; }
    lbc_wr_t w = { f, 0, 0 };
    /* strip=0: 保留行号与局部变量名 —— 脚本报错时能定位到 app.lua:行 */
    if (lua_dump(L, lbc_writer, &w, 0) != 0 || w.err) { fclose(f); remove(cache); return; }
    h.pay_len = (uint32_t)w.n;
    if (fseek(f, 0, SEEK_SET) == 0 && fwrite(&h, sizeof(h), 1, f) == 1 && fflush(f) == 0) {
        fclose(f);
        ESP_LOGI(TAG, "字节码缓存已写入: %s (%u B / 源码 %ld B)", cache, (unsigned)w.n, src_sz);
        return;
    }
    fclose(f);
    remove(cache);                        /* 头没写全就不能留半成品 */
}

/* ★ 2026-11-23: 单文件应用包 (LAPP + 尾部 ZIP 素材) 的脚本长度。
 *   头里有一行 `--@pkg offset=N size=M`, 前 N 字节是 Lua 脚本, 之后是**二进制 ZIP**。
 *   整段交给 luaL_loadbuffer 会直接语法错误 (二进制里有 \0 / 非 UTF-8 字节),
 *   所以这里只把前 N 字节送去编译; 没有该行 = 老格式, 整文件都是脚本。
 *   只扫头部 2 KB, 不看内容主体。 */
/* 只从已读入的头部字节里解析 `--@pkg offset=N`, 不校验文件上界。
 * 返回 0 = 不是单文件包。读取阶段用它决定"只读多少字节",
 * 免得把 1.8MB 素材整段读进 PSRAM 再丢掉。 */
static size_t lapp_pkg_offset(const char *buf, size_t n)
{
    if (!buf || n < 8) return 0;
    for (size_t i = 0; i + 7 < n; i++) {
        if (buf[i] == '-' && buf[i+1] == '-' && buf[i+2] == '@' &&
            buf[i+3] == 'p' && buf[i+4] == 'k' && buf[i+5] == 'g') {
            const char *o = strstr(buf + i, "offset=");
            if (!o || (size_t)(o - buf) >= n) return 0;
            unsigned long v = strtoul(o + 7, NULL, 10);
            return (v > 0 && v < (1ul << 28)) ? (size_t)v : 0;
        }
    }
    return 0;
}

static size_t lapp_script_len(const char *buf, size_t total)
{
    if (!buf || total < 8) return total;
    size_t scan = total < 2048 ? total : 2048;
    for (size_t i = 0; i + 7 < scan; i++) {
        if (buf[i] == '-' && buf[i+1] == '-' && buf[i+2] == '@' &&
            buf[i+3] == 'p' && buf[i+4] == 'k' && buf[i+5] == 'g') {
            const char *o = strstr(buf + i, "offset=");
            if (!o || (size_t)(o - buf) >= scan) break;
            unsigned long v = strtoul(o + 7, NULL, 10);
            if (v > 0 && (size_t)v <= total) return (size_t)v;
            break;
        }
    }
    return total;
}

static bool lua_load_run(const char *path) {
    lua_probe_stage(5, "load_run 入口");
    if (s_prof_on) s_prof_t0 = esp_timer_get_time();
    s_prof_frame_n = 0;                 /* 首帧探针按"每次进应用"重新计 */
    s_last_render_ms = 0;               /* 上一应用的重绘时刻不能带到这一帧 */
    /* ★★★ 2026-11-22 修双重释放 ★★★
     *   原来只有 `if (s_L) lua_close(s_L);` —— 关完 **没把 s_L 置 NULL**。
     *   下面"应用路径非法"那条分支又写了 `lua_close(s_L)` ⇒ 对已释放的 VM 再关一次
     *   (double free / 堆破坏), 而这种崩溃现场离真正原因很远, 极难定位。
     *   现在统一走 lua_vm_close() (关 + 置空 + 回收桥里跨应用的状态)。 */
    if (s_L) { lua_release_app_res(); lua_vm_close(); }
    prof_mark("close_old");
    /* 启动前复位配额/超时基准并解析 appid (见 lua_sandbox.h)。
     * 返回 false 只在路径为空时 —— 权限已统一放开, 推不出 appid 也照常启动。 */
    if (!lua_sandbox_begin(path)) {
        snprintf(s_err, sizeof(s_err), "应用路径非法");
        s_err_pending = true; return false;   /* s_L 已是 NULL, 不能再 close */
    }
    s_lua_mem_used = 0;
    s_L = lua_newstate(lua_psram_alloc, NULL, (unsigned)esp_timer_get_time());
    if (!s_L) { ESP_LOGD(TAG, "lua_newstate 失败"); return false; }
    lua_probe_stage(6, "newstate 成功");
    prof_mark("newstate");
    /* openlibs + 桥注册 + 装钩子: 见 l_init_libs() 的注释 (必须 pcall 保护) */
    lua_pushcfunction(s_L, l_init_libs);
    if (lua_pcall(s_L, 0, 0, 0) != LUA_OK) {
        const char *m = lua_tostring(s_L, -1);
        ESP_LOGW(TAG, "Lua 运行环境初始化失败 [%s]: %s", path, m ? m : "(null)");
        snprintf(s_err, sizeof(s_err), "%s", m ? m : "Lua \xe5\x88\x9d\xe5\xa7\x8b\xe5\x8c\x96\xe5\xa4\xb1\xe8\xb4\xa5");
        s_err_pending = true;
        lua_vm_close();
        return false;
    }
    lua_probe_stage(7, "openlibs+bridge 成功");
    prof_mark("openlibs+bridge");

    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGD(TAG, "打开失败: %s", path);
        snprintf(s_err, sizeof(s_err), "\xe6\x97\xa0\xe6\xb3\x95\xe6\x89\x93\xe5\xbc\x80"); /* 无法打开 */
        s_err_pending = true; lua_vm_close(); return false; }
    setvbuf(f, s_lua_iobuf, _IOFBF, sizeof(s_lua_iobuf));   /* 大缓冲顺序读, 见上方说明 */
    lua_probe_stage(8, "fopen 成功");
    prof_mark("fopen");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); ESP_LOGD(TAG, "空文件: %s", path);
        snprintf(s_err, sizeof(s_err), "\xe7\xa9\xba\xe6\x96\x87\xe4\xbb\xb6"); /* 空文件 */
        s_err_pending = true; lua_vm_close(); return false; }
    /* ★ 2026-11-23: 单文件应用包 = 脚本 + 尾部 ZIP 素材。这里先读 2KB 头拿到脚本长度,
     *   之后**只把脚本段读进 PSRAM** —— 否则 1.8MB 素材会整段进内存再被丢掉,
     *   白白多一次 SD 读 + 一次 1.8MB 的 PSRAM 峰值。 */
    size_t script_len = (size_t)sz;
    {
        char hb[2048];
        size_t hn = fread(hb, 1, sizeof hb, f);
        size_t po = lapp_pkg_offset(hb, hn);
        if (po > 0 && po <= (size_t)sz) script_len = po;
        fseek(f, 0, SEEK_SET);
    }

    /* ★ 2026-11-23: 优先用字节码缓存 —— 源码 175KB 现编要 389ms, 字节码直接读 ≈ 十几 ms。
     *   只用 stat 判失效（源 size + mtime + Lua 版本），不读源码内容，否则省下的编译时间又赔回去。 */
    struct stat sst;
    uint32_t src_mt = (stat(path, &sst) == 0) ? (uint32_t)sst.st_mtime : 0u;
    char cache[LBC_CACHE_MAX];
    snprintf(cache, sizeof(cache), "%s.lbc", path);
    uint32_t pay = 0;
    bool from_cache = false;
    char *buf = NULL;
    size_t rd = 0;
    if (lbc_probe(cache, sz, src_mt, &pay)) {
        fclose(f); f = NULL;                 /* 先让出共享读缓冲, 再读缓存文件 */
        buf = heap_caps_malloc(pay, MALLOC_CAP_SPIRAM);
        FILE *cf = buf ? fopen(cache, "rb") : NULL;
        if (cf) {
            setvbuf(cf, s_lua_iobuf, _IOFBF, sizeof(s_lua_iobuf));
            fseek(cf, (long)sizeof(lbc_hdr_t), SEEK_SET);
            rd = fread(buf, 1, pay, cf);
            fclose(cf);
            from_cache = (rd == pay);
        }
        if (!from_cache) {
            if (buf) { heap_caps_free(buf); buf = NULL; }
            f = fopen(path, "rb");           /* 缓存读失败 ⇒ 重开源码走编译 */
            if (f) setvbuf(f, s_lua_iobuf, _IOFBF, sizeof(s_lua_iobuf));
        }
    }
    if (!from_cache) {
        if (!f) { ESP_LOGD(TAG, "重新打开失败: %s", path);
            snprintf(s_err, sizeof(s_err), "\xe6\x97\xa0\xe6\xb3\x95\xe6\x89\x93\xe5\xbc\x80");
            s_err_pending = true; lua_vm_close(); return false; }
        buf = heap_caps_malloc(script_len, MALLOC_CAP_SPIRAM);
        if (!buf) { fclose(f); ESP_LOGD(TAG, "PSRAM 分配 %uB 失败", (unsigned)script_len);
            snprintf(s_err, sizeof(s_err), "\xe5\x86\x85\xe5\xad\x98\xe4\xb8\x8d\xe8\xb6\xb3"); /* 内存不足 */
            s_err_pending = true; lua_vm_close(); return false; }
        rd = fread(buf, 1, script_len, f);
        fclose(f); f = NULL;
    }
    lua_probe_stage(9, from_cache ? "fread(lbc) 完成" : "fread 完成");
    prof_mark(from_cache ? "fread(lbc)" : "fread");

    /* 缓存里是**已编译字节码**, 长度就是 rd; 源码则要按 --@pkg 截断 */
    size_t slen = from_cache ? rd : lapp_script_len(buf, rd);
    int rc = luaL_loadbuffer(s_L, buf, slen, "@app.lua");
    /* 缓存坏了（版本/架构不符、半截文件）⇒ 就地回退源码重编，功能不能因此失败 */
    if (rc != LUA_OK && from_cache) {
        const char *m = lua_tostring(s_L, -1);
        ESP_LOGW(TAG, "字节码缓存无效 [%s]: %s ⇒ 回退源码重编", cache, m ? m : "(null)");
        lua_pop(s_L, 1);
        remove(cache);
        heap_caps_free(buf); buf = NULL;
        FILE *sf = fopen(path, "rb");
        if (sf) {
            buf = heap_caps_malloc(script_len, MALLOC_CAP_SPIRAM);
            if (buf) {
                rd = fread(buf, 1, script_len, sf);
                rc = luaL_loadbuffer(s_L, buf, lapp_script_len(buf, rd), "@app.lua");
                if (rc == LUA_OK) lbc_store(s_L, cache, sz, src_mt);
            }
            fclose(sf);
        }
        from_cache = false;
    } else if (rc == LUA_OK && !from_cache) {
        lbc_store(s_L, cache, sz, src_mt);   /* 首次编译成功 ⇒ 落盘，下次直接读 */
    }
    heap_caps_free(buf);
    lua_probe_stage(10, "loadbuffer 完成");
    prof_mark("loadbuffer");
    /* ★ 2026-11-22: 执行脚本体前重设计时 —— 否则 SD 读取/装载耗时也算进
     *   "单次回调超时"预算, 卡慢的卡上一读就可能把正常脚本误判成超时。 */
    lua_sandbox_touch();
    if (rc != LUA_OK || lua_pcall(s_L, 0, 0, 0) != LUA_OK) {
        const char *m = lua_tostring(s_L, -1);
        /* ★ 2026-11-02: 原来是 ESP_LOGD —— 默认日志等级是 INFO, DEBUG 被整级过滤,
         *   于是 Lua 语法错误/运行时错误**串口一条都看不到**, 只在屏幕上闪一下 toast,
         *   排查时等于没有信息(录音机那次就卡在这里)。
         *   改 WARN: 带脚本路径 + 完整错误串。chunkname 是 "@app.lua", 所以 m 里
         *   已经含行号(形如 "app.lua:720: ..."), 原样输出即可。 */
        ESP_LOGW(TAG, "Lua 执行失败 [%s]: %s", path, m ? m : "(null)");
        snprintf(s_err, sizeof(s_err), "%s", m ? m : "Lua error");
        s_err_pending = true;
        /* ★ 2026-11-22: 脚本体可能已经开了录音/播放/ESP-NOW 才报错 ⇒ 这里也要兜底,
         *   否则"启动即失败"的应用会把资源留在后台(与 lua_stop_ex 那条路径同理)。 */
        lua_release_app_res();
        lua_vm_close();
        return false;
    }
    lua_probe_stage(11, "pcall 完成 (脚本体跑完)");
    prof_mark("pcall(脚本体)");     /* 顶层代码执行耗时 (不含脚本 on_enter) */
    s_err_pending = false;
    s_err_streak = 0;               /* 新应用从干净计数开始 */
    s_err_name = NULL;
    return true;
}

/* ---- 调用 Lua 全局函数 (不存在则跳过) ----
 * arg = -1     无参调用
 * arg >= 0     先 push 一个整数参数再调用
 * Lua 要求"函数与参数在栈上连续、函数在下面", 所以必须**先 getglobal 再 push 参数**;
 * 旧代码只 getglobal 不 push 却传 nargs=1 → pcall 把函数下面的残留值当函数调用,
 * 表现为 action() 回调报错/收到垃圾值(所有依赖按键的 Lua 应用失效)。 */
static void lua_call_optional(ui_ctx_t *ctx, const char *name, int arg) {
    if (!s_L) return;
    /* 每次回调重新计 10 秒: 超时钩子对所有应用生效后, 不重置会把
     * 运行超过 10 秒的应用(游戏/阅读器)直接杀掉。 */
    lua_sandbox_touch();
    lua_getglobal(s_L, name);
    if (!lua_isfunction(s_L, -1)) { lua_pop(s_L, 1); return; }
    int narg = 0;
    if (arg >= 0) { lua_pushinteger(s_L, (lua_Integer)arg); narg = 1; }
    if (lua_pcall(s_L, narg, 0, 0) != LUA_OK) {
        lua_report_error(ctx, name);   /* 容忍: 首次只 toast, 连续出错才回列表 */
        return;
    }
    lua_err_mark_ok(name);      /* ★ 只有本次这个回调恢复才算"好了" */
}

/* 调 Lua `action(a)` 并取回返回值: true = 脚本已消费该动作。
 * 用途: BACK 键先交给脚本(如关闭子窗口/保存后返回), 脚本 `return true` 才不退出应用;
 * 未定义 action 或未返回 true → 返回 false, 交回框架默认行为(退出)。对老脚本向后兼容。 */
static bool lua_call_action_consumed(ui_ctx_t *ctx, int a) {
    if (!s_L) return false;
    lua_sandbox_touch();          /* 同上: 每次回调独立计时 */
    lua_getglobal(s_L, "action");
    if (!lua_isfunction(s_L, -1)) { lua_pop(s_L, 1); return false; }
    lua_pushinteger(s_L, (lua_Integer)a);
    if (lua_pcall(s_L, 1, 1, 0) != LUA_OK) {
        lua_report_error(ctx, "action");
        /* action() 挂了就不能把 BACK 吞掉: 明确退出应用(否则用户被卡在里面出不来) */
        if (s_mode == LUA_MODE_RUN) lua_stop_ex(ctx, true);
        return false;
    }
    lua_err_mark_ok("action");
    bool consumed = (lua_toboolean(s_L, -1) != 0);
    lua_pop(s_L, 1);
    return consumed;
}

/* ---- 退出 RUN 态: 调 on_exit + 释放 lua_State → 回 MODE_LIST ----
 * leaving_page=true 表示页面马上要出栈（退出应用/页面被弹），此时**不重建**右栏列表：
 *   os_pane_reset() 会把 built_folder 清掉，紧接着 os_pane_build_right() 必然重扫一遍
 *   目录 —— 而用户根本看不到它。这就是"退出应用慢"里最冤枉的一段 IO。 */
static void lua_stop_ex(ui_ctx_t *ctx, bool leaving_page) {
    if (s_prof_on) s_prof_t0 = esp_timer_get_time();
    if (s_L) {
        lua_getglobal(s_L, "on_exit");
        if (lua_isfunction(s_L, -1)) {
            lua_sandbox_touch();   /* on_exit 也可能长跑, 同样重置预算 */
            if (lua_pcall(s_L, 0, 0, 0) != LUA_OK)
                ESP_LOGW(TAG, "on_exit 出错: %s", lua_tostring(s_L, -1) ? lua_tostring(s_L, -1) : "(null)");
        }
        /* ★ 2026-11-22 (第2回合): 统一出口 —— 关 VM + 置空 + 回收桥里跨应用的状态
         *   (已解码位图 / app.util 定时器槽 / app.dlg 结果 / app.key_repeat 按住表)。
         *   回收必须在 lua_close 之后也好、之前也好都安全; 这里放后面, 保证 on_exit
         *   里脚本自己 free 的图先被它自己处理掉。 */
        lua_vm_close();
    }
    prof_mark("on_exit+close");
    /* ★ 2026-10-24 GPS 硬闸门: 任何 Lua 应用退出(码表/地图/…), 一律停解析任务
     *   并把模块送回 UBX-RXM-PMREQ 待机(46µA)。脚本自己调 app.gps.stop() 是"应该做的",
     *   但它可能忘记 / 崩了 / 没写 on_exit ⇒ 在这里兜底, 否则退出后 GPS 仍在
     *   ~10mA 连续跟踪, 一夜就没电。两个函数都是幂等的, 多调无害。 */
    if (gps_is_ready()) {
        gps_service_stop();
        gps_release();   /* ★ 2026-10-24: 待机 + 卸 UART + 还原引脚, 不留任何常驻 */
    }
    /* ★ 2026-11-22: 录音 / 播放 / ESP-NOW / 本应用开的 HID 统一兜底 (幂等, 见函数注释) */
    lua_release_app_res();
    s_mode = LUA_MODE_LIST;
    s_exit_req = false;
    s_exit_dlg = false;
    s_exit_pend = false;
    s_ran = false;
    /* ★ 2026-10-31: 退出应用 = 虚拟手柄一律关闭 + 边沿状态复位，
     *   否则下一次进别的应用(或游戏)会带着残留的"按下态"和全局 enabled 标志。 */
    if (s_vk_on) { s_vk_on = false; virtual_keys_set_enabled(false); }
    lua_vk_want = false;
    s_vk_prev = 0xFF;
    s_vk_dir  = 0;
    s_vk_db   = s_vk_raw = 0xFF;   /* 消抖状态一并复位, 否则下个应用第一拍算作"刚按下" */
    g_lua_exit_ask = true;          /* 退出确认开关复位 → 下一个应用从"默认弹"开始 */
    s_err_streak = 0;               /* 换了应用/退出应用 → 报错计数归零 */
    s_err_name = NULL;
    os_notify_tone_stop();
    if (ctx) {
        if (leaving_page) {
            /* 只清状态，不扫目录：页面紧接着就出栈了 */
            os_pane_reset(&s_pane);
            prof_mark("pane_reset(skip rescan)");
        } else {
            lua_probe_stage(14, "lua_stop: pane reset 前");
            os_pane_reset(&s_pane);
            s_pane.focus = 1;
            os_pane_build_right(&s_pane);
            ctx->needs_redraw = true;
            prof_mark("pane_reset+rescan");
        }
    }
}

/* ---- os_pane 右栏选中: 加载该 .lua 并进入 RUN 态 ---- */
static void lua_pane_select(ui_ctx_t *ctx, os_pane_t *p,
                            const char *path, const char *name) {
    (void)p; (void)name;
    if (!path || !path[0]) return;
    s_lcd = ctx->lcd;
    g_lua_keys_head = g_lua_keys_tail = 0;
    s_exit_req = false;
    ESP_LOGI(TAG, "启动: %s", path);
    lua_statusbar_reset();      /* 换应用: 撤掉上一个应用的声明, 由新脚本 on_enter 决定 */
    if (lua_load_run(path)) {
        s_mode = LUA_MODE_RUN;
        lua_call_optional(ctx, "on_enter", -1);   /* 可选; 出错已内部回列表 */
        if (s_mode == LUA_MODE_RUN) ctx->needs_redraw = true;
    } else {
        if (s_err_pending) { s_err_pending = false; os_dialog_toast(ctx, s_err); }
    }
}

/* ============ 页面回调 ============ */
static void lua_enter(ui_ctx_t *ctx) {
    lua_probe_stage(3, "lua_enter 进入");
    s_lcd = ctx->lcd;
    g_lua_keys_head = g_lua_keys_tail = 0;
    s_exit_req = false;
    s_exit_dlg = false;
    s_exit_pend = false;
    s_ran = false;
    g_lua_exit_ask = true;      /* 每个应用从默认值开始 (脚本可在 on_enter 里改) */
    /* ★ 2026-11-23 (第2轮): 回到"没表态"而不是"明确不要"。
     *   旧实现这里调 os_statusbar_show(false) —— 对没调用 app.statusbar 的老脚本,
     *   等于把状态栏永久关掉; 这正是"设备上录音机从来没有状态栏"的直接原因。
     *   现在 AUTO: 非全屏页自动显示; 要隐藏的脚本自己 app.statusbar(false)。 */
    lua_statusbar_reset();
    /* ⚠ pane 身份必须在任何 early-return 之前初始化:
     * 应用管家直接拉起应用时走下面的 boot_path 分支直接 return, 不会经过"列表态"初始化块,
     * 此时 s_pane.root 仍为 NULL; 之后退出应用 → lua_stop() → os_pane_reset() → opendir(NULL)
     * 在 VFS 里空指针崩溃 (实测 LoadProhibited / EXCVADDR=0, 按 HOME 必崩)。 */
    if (!s_pane.loaded) {
        s_pane.root = LUA_ROOT_PATH;
        s_pane.is_item = is_lua;
        s_pane.on_select = lua_pane_select;
        s_pane.list_y = 20;
    }
    /* 应用管家接管: 若设置了启动脚本, 直接进 RUN 态运行; 否则才显示列表 */
    if (s_boot_path[0]) {
        char p[LUA_BOOT_PATH_LEN];
        snprintf(p, sizeof(p), "%s", s_boot_path);
        s_boot_path[0] = '\0';                 /* 一次性消费 */
        lua_probe_stage(4, "boot_path 命中");
        if (lua_load_run(p)) {
            s_mode = LUA_MODE_RUN;
            lua_probe_stage(12, "RUN 态, 调 on_enter 前");
            if (s_prof_on) s_prof_t0 = esp_timer_get_time();   /* 单独量脚本 on_enter */
            lua_call_optional(ctx, "on_enter", -1);
            prof_mark("script on_enter");
            lua_probe_stage(13, "on_enter 返回");
            if (s_mode != LUA_MODE_RUN && s_err_pending) { s_err_pending = false; os_dialog_toast(ctx, s_err); }
        } else if (s_err_pending) {
            s_err_pending = false;
            os_dialog_toast(ctx, s_err);
        }
        ctx->needs_redraw = true;
        return;
    }
    s_mode = LUA_MODE_LIST;
    os_pane_reset(&s_pane);      /* pane 身份已在函数开头保证, 此处只重扫目录 */
    if (!s_pane.root) s_pane.root = LUA_ROOT_PATH;   /* 双保险: 绝不让 root 为空进 opendir */
    s_pane.focus = 1;   /* 直接进右栏 */
    os_pane_build_right(&s_pane);
    ctx->needs_redraw = true;
}

static void lua_render(ui_ctx_t *ctx) {
    if (!ctx->lcd) return;
    s_lcd = ctx->lcd;
    if (s_mode == LUA_MODE_RUN) {
        /* ★ 2026-11-23 (第2轮): 这里**不再**每帧下发声明。
         *   声明只在脚本调用 app.statusbar() 时变一次 (见 os_page_lua_set_statusbar),
         *   由 OS 合成显隐并负责刷新。render 里每帧重复下发, 一旦某帧取值不对
         *   (应用切换/LIST 态) 就会把 HIDE 永久写进 OS —— 那是"状态栏莫名消失"的根。
         *
         *   ctx->fullscreen 表达的是"本页默认全屏"(Lua 应用历来如此):
         *     脚本没声明 (AUTO) / 明确不要 (HIDE) → 全屏, 与历史行为一致;
         *     脚本声明要 (SHOW)                    → 让出顶部 24px 给系统状态栏。
         *   这样没调过 app.statusbar 的老应用观感不变 (不会突然被状态栏盖住顶部内容),
         *   而声明过的应用由 OS 保证显隐与刷新。 */
        ctx->fullscreen = (s_sb_req != OS_SB_SHOW);
        /* ★ 2026-11-23: 首帧探针 —— 用户说的"点开卡一会才进去", 卡的是**首帧 render**
         *   (地图包加载 + 瓦片同步), 不在 on_enter 里, 之前的 LUAPROF 看不到。
         *   只在 LUAPROF 打开时打印前 8 帧, 平时零开销。 */
        /* ★ 2026-11-23: 帧动画期间保持 240MHz。
         *   输入层的 boost 是"最后一次输入 + 400ms"(INPUT_BOOST_HOLD_MS), 而敲击动画
         *   有 510ms ⇒ 后 100ms 掉回 80MHz: 单帧从 ~14ms 变成 ~43ms(实测 >30ms 慢帧),
         *   观感就是"挥到一半突然一顿"。掐短动画不行(用户要慢抬升), 所以在这里续期:
         *   与上一帧间隔 <50ms 说明应用在跑帧循环(静态页不会这么频繁重绘), 续一次。
         *   只续不抢: 锁归输入层, 这里只把到期时间往后推; 动画一停, 50ms 内没有下一帧
         *   ⇒ 自然到期释放, 空闲降频/省电完全不受影响。
         *   ⛔ 不要改成"render 里无条件续期": 静止页面每帧也 render, 会永远钉在 240MHz。 */
        {
            uint32_t frame_now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if (s_last_render_ms && (uint32_t)(frame_now - s_last_render_ms) < 50u)
                input_pm_boost_now();
            s_last_render_ms = frame_now;
        }
        uint32_t t0 = s_prof_on ? (uint32_t)(esp_timer_get_time() / 1000) : 0;
        lua_call_optional(ctx, "render", -1);   /* 出错已内部回列表 */
        if (s_prof_on) {
            if (s_prof_frame_n < 8) ESP_LOGW(TAG, "LUAPROF render #%u +%ums", ++s_prof_frame_n,
                                (unsigned)((uint32_t)(esp_timer_get_time() / 1000) - t0));
        }
        lua_draw_err_banner(ctx->lcd);          /* ★ 出错时把原因画在屏上 (可截图), 不再只一闪 */
        /* ★ 2026-10-31: 虚拟手柄线条叠加在应用画面之上（os_render 之后统一 flush，
         *   所以必须画在这里，不能自己 flush）。线条式设计 => 下方 BIOS 菜单仍可读。 */
        if (s_vk_on) virtual_keys_draw(ctx->lcd);
    } else {
        /* 应用列表态是全屏列表 —— 用 fullscreen 表达"本帧不画状态栏"。
         * ★ 2026-11-23 (第2轮修): 这里原来每帧调 os_statusbar_show(false),
         *   那会把声明(黏在 Lua 页, 与 RUN 态是同一页)改成 HIDE。只要出现过一帧
         *   列表态(重启应用 / 切应用时都会), 之后回到 RUN 态脚本不会再声明一次
         *   ⇒ 状态栏永久消失。这是"状态栏时有时无"的直接原因。
         *   现在只影响本帧, 声明仍归脚本所有。 */
        ctx->fullscreen = true;
        st7305_clear(ctx->lcd, ST7305_COLOR_WHITE);
        os_pane_render(ctx, &s_pane);
    }
}

/* ---- 退出确认回调 (全屏程序/游戏 按返回时弹的那一问) ----
 * result: 0=确定退出 / 1=取消 / -1=取消(BACK/超时)。
 * ⚠ 不能在弹窗回调里直接 os_pop 页面: 回调返回后 dlg_finish_top 才关弹窗层,
 *   此时栈顶仍是弹窗, 直接弹会把弹窗弹掉、应用留着。所以这里只置 s_exit_req,
 *   由下一帧 lua_poll 统一走"停 Lua + 弹栈"那条既有的退出路径 (与 app.exit() 同源)。 */
static void on_exit_confirm(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    s_exit_dlg = false;
    if (result == 0) {
        s_exit_req = true;
        if (ctx) ctx->needs_redraw = true;
    }
}

/* ★ 请求退出应用的**唯一入口** —— 脚本 app.exit() 与 框架 BACK 未消费 都走这里。
 *   规范: 全屏程序/游戏默认先弹"退出程序?"(确认才退, 取消留在原处), 防误触丢进度。
 *   弹窗动作**延迟到下一帧 poll** 执行 (s_exit_pend): 本函数会被 Lua 的 action()
 *   回调间接调用, 在回调里 push 弹窗属于重入 (项目已踩过"弹窗被替换"的坑)。
 *   ctx 为 NULL (极少见) 或应用没真正跑起来 (on_enter 就退) → 不弹, 直接退。 */
void lua_request_exit(ui_ctx_t *ctx) {
    if (!ctx) ctx = s_lua_ctx;
    if (g_lua_exit_ask && ctx && s_ran && s_mode == LUA_MODE_RUN) {
        s_exit_pend = true;
        ctx->needs_redraw = true;
        return;
    }
    s_exit_req = true;
}

/* ★ 2026-11-23 linit 报障: 「退出远程控制测试应用后连续出现几个空白页, 每按一次
 *   返回才退一层」。
 *   根因(已实测): 远程控制台 `app <脚本>` 每执行一次都 os_push(OS_PAGE_LUA) 一层,
 *   不判断当前是否已经在 Lua 页 ⇒ 连开 N 次就在返回栈里堆 N 层 Lua。
 *   退出时这里原来写死只弹 3 层, N > 3 就剩下 (N-3) 层 Lua 页 —— 这些层是 LIST 态、
 *   又扫不到应用, 画面就是「空壳列表/白屏」, 每按一次 BACK 才再弹一层。
 *   修法: 按「弹到当前页不再是 Lua」来收 (一次退干净), 只留一个上限防"栈空时 os_pop
 *   早退导致空转"。上限取 16 远大于正常返回栈深度, 不影响其他应用。
 *   ⚠ 不放宽 os_pop 语义、只改 Lua 页自己的退出收尾 ⇒ 其他应用的导航一条没动。 */
#define LUA_EXIT_POP_MAX 16
static void lua_exit_pop_all(ui_ctx_t *ctx) {
    if (!ctx) return;
    for (int i = 0; lua_exit_pop_continue(ctx->current_page, (int)OS_PAGE_LUA,
                                          os_stack_depth(), i, LUA_EXIT_POP_MAX); i++)
        os_pop(ctx);
}

static void lua_action(ui_ctx_t *ctx, os_action_t a) {
    s_lcd = ctx->lcd;
    if (s_mode == LUA_MODE_LIST) {
        if (os_pane_action(ctx, &s_pane, a)) return;
        if (a == OS_ACTION_BACK) os_pop(ctx);
        return;
    }
    /* RUN 态: 投进 FIFO (供 app.key() 轮询式取键, 向后兼容) + 把动作码当参数转发给 action(a) */
    key_push((int)a);
    if (s_mode == LUA_MODE_RUN) s_lua_ctx = ctx;
    /* BACK 先交给脚本: 脚本 `return true` = 自己处理了(关闭详情窗/保存后返回…), 不退出应用;
     * 未消费才由框架退出。旧实现在调用 action() **之前**就 os_pop → 脚本永远收不到 BACK,
     * 导致"返回前保存"(wb.lua)、"BACK 关详情窗"(wifiprobe.lua) 等全部失效。 */
    if (a == OS_ACTION_BACK) {
        if (lua_call_action_consumed(ctx, (int)a)) {
            if (ctx) ctx->needs_redraw = true;
            return;                                   /* 脚本已处理 → 留在应用内 */
        }
        if (s_mode != LUA_MODE_RUN) return;            /* 脚本出错已自行回列表 */
        /* ★ 全屏程序/游戏: 返回不再直接退出 —— 走统一入口, 默认先弹"退出程序?" */
        lua_request_exit(ctx);
        return;
    }
    lua_call_optional(ctx, "action", (int)a);   /* 按键值作为参数传给 Lua 的 action(a) */
    if (ctx) ctx->needs_redraw = true;      /* 按键后立即重绘, 否则 RUN 态不刷新 */
    if (s_mode != LUA_MODE_RUN) {
        if (s_err_pending) { s_err_pending = false; os_dialog_toast(ctx, s_err); }
        return;
    }
    if (s_exit_req) {
        s_exit_req = false;
        lua_stop_ex(ctx, true);
        /* ★ 2026-10-02 修「按返回退出 -> 白屏 + 一条竖直分割线, 再按一次才退出」:
         *   app.exit() 的语义是"退出应用", 必须把 Lua 页**弹出栈**。
         *   原来只 lua_stop() → 页面留在 LIST 态去扫 /sdcard/apps(空的) →
         *   用户看到的就是双栏列表的空壳(白屏 + 一条竖直分割线), 要再按一次 BACK 才真退出。
         *   循环弹 3 次: 兼顾"退出时上面还压着 toast/弹窗"的情况。 */
        lua_exit_pop_all(ctx);
    }
}

static bool lua_touch(ui_ctx_t *ctx, int x, int y) {
    /* RUN 态不消费触摸: 脚本自己在 poll()/render() 里调用 app.touch() 取坐标绘制,
     * 页面层不介入, 否则会与脚本的绘制/状态机抢同一次触摸事件。 */
    if (s_mode == LUA_MODE_LIST) return os_pane_touch(ctx, &s_pane, x, y);
    return false;
}

static void lua_poll(ui_ctx_t *ctx) {
    if (s_mode == LUA_MODE_LIST) {
        os_pane_poll(ctx, &s_pane);
        /* ★ 2026-11-22: 应用是**在 render() 里被超时/连错强停**时, 本帧已经回到 LIST 态,
         *   若不在这里补一次提示, 用户只看到"突然回到列表", 不知道为什么(旧版同样静默)。 */
        if (s_err_pending) { s_err_pending = false; os_dialog_toast(ctx, s_err); }
        return;
    }
    /* ★ 退出确认: 由 lua_request_exit() 置位, 在这里(而非 action 回调里)才真正 push 弹窗。
     *   丢弃触发那一下残留的触摸 tap, 否则确认框一弹出就被当成"确定"自动退出
     *   (与 page_engine.c 的 eng_exit_confirm 同一套处理)。 */
    if (s_exit_pend) {
        s_exit_pend = false;
        s_exit_dlg = true;
        input_consume_tap(NULL, NULL);
        os_dialog_confirm_ex(ctx, "\xe9\x80\x80\xe5\x87\xba\xe7\xa8\x8b\xe5\xba\x8f?", /* 退出程序? */
                             0, -1, on_exit_confirm, NULL);
        return;
    }
    /* RUN 态: 先处理待办, 再转给 Lua poll() */
    if (s_exit_req) {
        s_exit_req = false;
        lua_stop_ex(ctx, true);
        lua_exit_pop_all(ctx);
        return;
    }
    if (s_err_pending) { s_err_pending = false; os_dialog_toast(ctx, s_err); }

    /* ★ 2026-10-31: 系统虚拟手柄处理（仅当脚本用 app.vk(true) 声明时才启用）
     *
     * 为什么在这里做：虚拟手柄是 C 侧的线条覆盖 + hit-test，Lua 拿不到；
     *   而脚本的按键入口只有 action(a)，所以在这里把"虚拟按键"翻译成同一个 action，
     *   脚本完全不用区分 实体键 / 蓝牙手柄 / 虚拟按键 三种来源。
     *
     * ⚠ 只取 virtual_keys_get_joypad()（**仅虚拟手柄**），不取合并后的
     *   input_get_held_gb_joypad() —— 后者含物理键(A/B)与蓝牙手柄，
     *   直接转 action 会和 input_get_action() 的边沿重复触发（一按两下）。
     *   这里先用合并查询驱动一次 hit-test（让组件内部刷新自己的掩码），再单独读它。 */
    if (s_mode == LUA_MODE_RUN && lua_vk_want) {
        if (!s_vk_on) {                       /* 进入时启用一次, 不逐帧切换 */
            s_vk_on = true;
            virtual_keys_set_enabled(true);
            s_vk_prev = 0xFF;
            s_vk_dir  = 0;
            s_vk_db = s_vk_raw = 0xFF;
            s_vk_raw_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        }
        (void)input_get_held_gb_joypad();      /* 驱动 virtual_keys 本帧 hit-test */
        uint8_t raw = virtual_keys_get_joypad();
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        /* ① 消抖: 掩码必须**稳定 35ms** 才认, 交界处抖动产生的一次按下/松开不算事件。
         *   (实体键在 input.c 里也有同样的 30ms 闸门; 虚拟键原来完全没有。) */
        if (raw != s_vk_raw) { s_vk_raw = raw; s_vk_raw_ms = now; }
        if ((uint32_t)(now - s_vk_raw_ms) >= VK_DEBOUNCE_MS) s_vk_db = raw;

        uint8_t pressed = (uint8_t)((uint8_t)~s_vk_prev & (uint8_t)~s_vk_db);  /* 本帧新按下 */
        s_vk_prev = s_vk_db;
        for (int b = 0; b < 8; b++) {
            if (pressed & (1u << b)) {
                os_action_t va = vk_bit_to_action(1u << b);
                if (va != OS_ACTION_NONE) lua_action(ctx, va);   /* 单次边沿: 不会重复 */
            }
        }
        /* ② 方向键长按重复: 450ms 后每 120ms 一次, 单次按住最长 1.5s 就停
         *   (与 input.c 的 NAV_HOLD_CAP_MS 一致) —— 释放丢失也不会一直滚。 */
        uint8_t dir = (uint8_t)((uint8_t)~s_vk_db & 0xF0);
        if (dir && (dir & (uint8_t)(dir - 1)) == 0) {
            if (dir != s_vk_dir) {
                s_vk_dir = dir; s_vk_dir_t0 = now; s_vk_dir_last = now;
            } else {
                uint32_t held = (uint32_t)(now - s_vk_dir_t0);
                if (held >= VK_REPEAT_DELAY && held <= VK_REPEAT_CAP_MS &&
                    (uint32_t)(now - s_vk_dir_last) >= VK_REPEAT_RATE) {
                    s_vk_dir_last = now;
                    os_action_t va = vk_bit_to_action(dir);
                    if (va != OS_ACTION_NONE) lua_action(ctx, va);
                }
            }
        } else {
            s_vk_dir = 0;
        }
    } else if (s_vk_on) {                     /* 脚本撤销 / 退出应用 → 立刻关闭并复位 */
        s_vk_on = false;
        virtual_keys_set_enabled(false);
        s_vk_prev = 0xFF;
        s_vk_dir  = 0;
        s_vk_db = s_vk_raw = 0xFF;
    }

    if (s_L) {
        lua_util_tick(s_L);            /* ★ 2026-10-24: 驱动 app.util 的 after/every 定时器 */
        /* ★ 2026-11-22: 定时器回调里死循环同样会被超时钩子打断, 但 tick 内部把错误
         *   静默 pop 了(见 luat_lib_util.c) ⇒ 这里补一次判定。不然"定时器跑飞"会
         *   每帧都卡满一个超时周期, 界面永远不响应。 */
        if (s_L && lua_sandbox_timeout_hit()) lua_handle_timeout(ctx, "util");
    }
    if (s_L) lua_call_optional(ctx, "poll", -1);
    if (s_mode == LUA_MODE_RUN) { s_lua_ctx = ctx; s_ran = true; }   /* 真跑起来了 → 退出才值得弹确认 */
    if (s_mode == LUA_MODE_RUN && ctx) ctx->needs_redraw = true;   /* 逐帧重绘: 支撑动画/触屏 */
    if (s_mode != LUA_MODE_RUN) s_lua_ctx = NULL;
    if (s_mode != LUA_MODE_RUN) {
        if (s_err_pending) { s_err_pending = false; os_dialog_toast(ctx, s_err); }
    }
}

static void lua_on_exit(ui_ctx_t *ctx) {
    /* 页面退栈 (BACK/HOME/回主菜单) 时兜底释放, 防止 lua_State 泄漏。
     * ⚠ 顺序不能反: lua_stop_ex(...,true) 内部会调 os_pane_reset(&s_pane), 而 s_pane
     *   是"用到才分配"的宏 (s_pane__get())。原来先 s_pane__free() 再 lua_stop_ex ⇒
     *   reset 时**又分配了一次**(~59KB PSRAM), 之后没人再 free —— 每次"应用运行中
     *   退出 Lua 页"都漏 59KB, 直到下次退出 Lua 页才被回收。
     *   现在先收 Lua (pane 仍有效), 再 free pane。 */
    if (s_mode == LUA_MODE_RUN) lua_stop_ex(ctx, true);
    s_pane__free();   /* LINTOS: 页面退出归还列表 pane */
    s_lcd = NULL;
}

/* ★ 2026-11-23 linit: 重载当前 Lua 页 (不压新层)。
 *   远程控制台 `app <脚本>` 原来每次都 os_push(OS_PAGE_LUA) —— 已在 Lua 页时
 *   就会再压一层, 连开几次堆成"一叠空白页"。改成: 已在 Lua 页 → 停掉当前脚本、
 *   重新走 lua_enter (它会消费刚设好的 boot 路径并直接进 RUN 态), 返回栈层数不变。 */
void os_page_lua_reload(ui_ctx_t *ctx) {
    if (!ctx) ctx = s_lua_ctx;
    if (!ctx) return;
    if (s_mode == LUA_MODE_RUN) lua_stop_ex(ctx, true);   /* 只清状态, 不弹页面 */
    lua_enter(ctx);
    ctx->needs_redraw = true;
}

static const os_module_t s_mod_lua = {
    .name       = "lua",
    .page_id    = OS_PAGE_LUA,
    .on_enter   = lua_enter,
    .on_exit    = lua_on_exit,
    .render     = lua_render,
    .action     = lua_action,
    .touch      = lua_touch,
    .poll       = lua_poll,
    /* ★ 2026-11-23 linit: 静态 fullscreen 改 false —— 显隐改由脚本声明
     *   (app.statusbar) 决定, 在 lua_render 里每帧调 os_statusbar_show()。
     *   静态写 true 的话 os_render 用 "m->fullscreen || ctx->fullscreen",
     *   脚本再想开状态栏也关不掉这个 true。 */
    .fullscreen = false,
    /* ★ 2026-11-23 统一状态栏的连带修正 (fullscreen 不再为 true 后必须显式补上):
     *   · keep_on_idle: 原来靠 fullscreen 挡住"空闲超时弹回桌面"。应用正在跑,
     *     不能被空闲弹走 —— 与改动前行为一致。
     *   · defer_exit: 退出时 on_exit 要关 Lua VM / 释放音频·GPS·HID / 回收 pane,
     *     沿用 fullscreen 页那条"先把父页刷上屏, 再跑清理"的路径, 否则用户会
     *     看到"按了返回还卡几十毫秒"。 */
    .keep_on_idle = true,
    .defer_exit   = true,
};

void os_page_lua_register(void) { os_register(&s_mod_lua); }
