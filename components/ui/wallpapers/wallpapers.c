/* 内置壁纸程序 (V4): 星空（含底部驾驶舱）.
 * 从 menu_system 屏保完整移植到本库, 供 os/桌面 与 屏保 共用.
 * 依赖: st7305(clear/pixel) + ui_common(ascii 小字). */
#include "wallpapers.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_pm.h"
#include "esp_timer.h"   /* 开机 60s 浅睡宽限期 */
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_common.h"
#include "esp_log.h"

#include "svc_nvs.h"
#include "board_rlcd.h"   /* 屏保独占 LCD: 冻结游戏帧刷新, 壁纸恒在最上层 */
#include "input.h"        /* 统一"最近输入"时钟: 独立监视据此判定空闲 */
#include "wifi_manager.h"  /* ★ 进屏保关 WiFi 射频 / 解锁恢复 */
#include "driver/usb_serial_jtag.h"   /* ★ 2026-11-08: 判断"是否插着电脑" ⇒ 决定桌面档能不能浅睡 */

/* [二分定位·测试版] 屏保/睡眠期间彻底关日志: 由 main.c 提供.
 * suspend=true 时 log_to_file_vprintf 直接 return 0(连 vsnprintf 都不执行),
 * 壁纸态/浅睡窗口内所有 ESP_LOG 输出完全短路, 不碰 flash 格式化. */

#define WP_W ST7305_WIDTH     /* 400 */
#define WP_H ST7305_HEIGHT    /* 300 */
#define WP_ROW ((WP_W + 7) / 8)          /* 50 */
#define WP_FB_BYTES (WP_ROW * WP_H)



/* ==== 壁纸模块注册表 ==== */
#define WP_MAX_MODULES 8
static wp_module_t s_modules[WP_MAX_MODULES];
static int s_module_count = 0;

/* 当前选中的屏保壁纸 id (NVS "os_wp"/"prog", 默认星空)。
 * 在 supervisor_start(内部 RAM 栈上下文) 读一次 —— 屏保任务栈在 PSRAM, 不宜自己读 NVS。 */
static int s_ss_prog = WP_PROG_STARS;

void wp_register_module(const wp_module_t *m) {
    if (s_module_count >= WP_MAX_MODULES) return;
    s_modules[s_module_count++] = *m;
}
const wp_module_t *wp_get_module(int id) {
    for (int i = 0; i < s_module_count; i++)
        if (s_modules[i].id == id) return &s_modules[i];
    return NULL;
}
const wp_module_t *wp_get_module_by_index(int idx) {
    if (idx < 0 || idx >= s_module_count) return NULL;
    return &s_modules[idx];
}
int wp_get_module_count(void) { return s_module_count; }

const char *wp_prog_name(int id) {
    const wp_module_t *m = wp_get_module(id);
    return m ? m->name : "";
}

int wp_program_render(st7305_handle_t *lcd, int prog, uint32_t now_ms) {
    const wp_module_t *m = wp_get_module(prog);
    if (m && m->render) return m->render(lcd, now_ms);
    return 0;
}
/* ==== 屏保状态机 (从 os_mgr 迁移归位: 屏保职责归壁纸模块) ==== */
#define WP_TAG "WPSS"

/* ★★★ 2026-11-08 linit「桌面档按 USB 状态决定浅睡」★★★
 * 历史包袱: 桌面档原来**永久** light_sleep_enable=false, 理由写在下面那段注释里 ——
 * ESP32-S3 进 Automatic Light Sleep 会停 USB PHY, 主机侧 /dev/cu.usbmodem* 反复
 * 消失重枚举, 烧录与 remcon 都连不上。为了调试方便, 干脆把电池供电时的睡眠也一起禁了。
 * 现在改成**按连接状态自动判断**:
 *   · 插着电脑(收到主机 SOF) → 不浅睡, 串口/烧录照常
 *   · 拔了线(电池/充电宝)   → 允许浅睡, 空闲时 20~40mA 掉到 1~3mA
 * USB 判定用 usb_serial_jtag_is_connected(): 它看的是**有没有收到主机 SOF 包**,
 * 所以插充电宝/充电器**不会**被当成"连着电脑"(IDF 文档明确写了这一点)。
 * 代价: 一旦睡着 USB PHY 就停了, 再插回电脑要按一下 RST 才重新枚举
 *       (烧录时 esptool 本来就会拉 RST, 所以烧录不受影响)。
 * 逃生阀: 串口 `lsleep 0` 永久禁浅睡 / `lsleep 1` 强开 / `lsleep auto` 回到按 USB 判断。 */
#define WP_LS_AUTO   (-1)
#define WP_LS_OFF     0
#define WP_LS_ON      1
static int  s_ls_mode   = WP_LS_AUTO;   /* 串口 lsleep 可改 */
static int  s_ls_eff    = -1;           /* 上次实际生效值, -1=还没配过 */
static int  s_usb_conn  = -1;           /* 上次 USB 连接状态, -1=未知 */
/* USB "已断开"的连续确认计数: 只有连续 ~8 轮(≈500ms)都读不到 SOF 才允许浅睡。
 * 一旦误判进浅睡, USB PHY 就停了, 再插回来也收不到 SOF ⇒ 串口永久丢失 + 必须按 RST。
 * 所以宁可保守: 单次抖动不算数。 */
static int  s_usb_gone_n = 0;
#define WP_USB_GONE_CONFIRM  8

/* 串口 `lsleep` 逃生阀: -1=按 USB 自动 / 0=永久禁 / 1=强开。
 * 改了立刻把 s_ls_eff 置脏, 下一次循环就重新 esp_pm_configure。 */
void wallpapers_set_light_sleep(int mode)
{
    s_ls_mode = (mode < 0) ? WP_LS_AUTO : (mode ? WP_LS_ON : WP_LS_OFF);
    s_ls_eff  = -1;                      /* 强制重配 */
    ESP_LOGI(WP_TAG, "浅睡模式: %s",
             s_ls_mode == WP_LS_AUTO ? "按USB自动" : (s_ls_mode ? "强制开" : "强制关"));
}

int wallpapers_light_sleep_mode(void) { return s_ls_mode; }
int wallpapers_light_sleep_effective(void) { return s_ls_eff; }
int wallpapers_usb_connected(void) { return s_usb_conn; }
static volatile bool s_ss_active = false;   /* volatile: 屏保监督任务(另一核) 与主循环并发读写 */
/* 屏保所用的 LCD 句柄, 由 wp_screensaver_supervisor_start() 记录。
 * 提到这里(原在监督任务前)是因为 os_screensaver_force_enter() 也要用它复位方向。 */
static st7305_handle_t *s_ss_lcd = NULL;
static volatile bool s_ss_manual = false;  /* 锁屏键点按强制进入, 不自动复位 */
static volatile uint32_t s_ss_enter_ms = 0;  /* 进入屏保的时刻 (唤醒判定 + 休眠计时起点) */
static volatile bool s_ss_shutdown = false;  /* 屏保播满"休眠时间", 请求自动关机 */
/* "关机时间"缓存(ms): 屏保播多久 → 自动关机。
 * ★ 2026-11-24: 初值必须写成出厂默认 3 分钟, **不能是 0** ——
 *   万一哪条路径在 wp_ss_prime() 之前就问到它(理论上只有开机极早期), 0 会让
 *   "屏保播满 0 毫秒"立刻成立 ⇒ 一进壁纸就关机。默认值就是安全值。 */
static uint32_t s_ss_run_ms = 3u * 60000u;
/* 唤醒保护窗口: 记录最近一次"真实唤醒"(屏保由激活→退出)的时刻.
 * 根因修复 (V1.6.x): 屏保态唤醒由"按键事件到达"触发, 而软关机键(rel_short)的
 * POWER_LOCK 是"松手才投递" —— 同一物理点按的松手事件(或与屏保监视任务的介入)
 * 会在唤醒后紧接着再投递一次 POWER_LOCK, 走到 force_enter 立刻复锁, 表现为
 * "按下解锁, 一放手又进壁纸". 凡唤醒后 SS_WAKE_GUARD_MS 内的进入请求一律忽略,
 * 让"松手=退出壁纸"成为稳定语义 —— 单次点按只退出, 不会自我复锁. */
#define SS_WAKE_GUARD_MS 600
static uint32_t s_ss_wake_ms = 0;    /* 最近一次真实唤醒时刻; 0=从未唤醒 */

/* 进入壁纸暂停一切: 记录是否因屏保而暂停了音乐, 唤醒时恢复 */

bool os_screensaver_active(void) { return s_ss_active; }
void os_screensaver_reset(void)
{
    bool was = s_ss_active;
    if (was) {
        /* 真实唤醒: 打上唤醒保护窗口时间戳, 阻止同一物理点按的后续 POWER_LOCK 立刻复锁 */
        s_ss_wake_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        ESP_LOGI(WP_TAG, "屏保退出 (输入唤醒)");
    }
    s_ss_active = false;
    s_ss_manual = false;
    s_ss_shutdown = false;   /* 唤醒即取消自动关机请求 */
    /* 刷新统一空闲时钟(从现在重新计"何时再进壁纸").
     * 否则进屏保后输入被屏蔽, s_last_any_ms 停在进屏保前的旧值 → 唤醒瞬间
     * 屏保监视任务又判定"空闲超时" → 立刻再次进入壁纸(反复醒/锁). */
    input_mark_activity();
    /* 屏保退出: 解除 LCD 独占 + 解除刷屏门禁, 后台程序/引擎可恢复 */
    board_rlcd_screensaver_set(false);
    st7305_set_flush_blocked(false);
    input_set_touch_blocked(false);   /* 唤醒: 恢复触屏 -- 休眠期间禁触屏, 仅物理键/手柄唤醒 */
    input_set_touch_alive(true);      /* ★ 2026-11-22: 恢复"按住即刷新空闲时钟" (屏保期关掉, 见下) */

    /* 唤醒: 还原由本屏保静音的音频 (MP3 保持停止, 用户需手动重播) */
    /* 屏保进入时未关闭任何设备(WiFi/蓝牙/音频/日志), 唤醒也无需恢复. */
}
void os_screensaver_force_enter(void)
{
    /* 唤醒保护窗口: 刚被输入唤醒后 SS_WAKE_GUARD_MS 内拒绝再次进入屏保.
     * 覆盖所有进入路径(锁屏键 POWER_LOCK / 空闲监视任务 / 壁纸页手动锁屏),
     * 彻底消除"松手即被自身复锁"的抖动 —— 单次点按=仅退出壁纸. */
    uint32_t nowg = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (s_ss_wake_ms != 0 && (int32_t)(nowg - s_ss_wake_ms) < (int32_t)SS_WAKE_GUARD_MS) {
        ESP_LOGI(WP_TAG, "唤醒保护窗口内(%ums), 忽略进入屏保请求",
                 (unsigned)(nowg - s_ss_wake_ms));
        return;
    }

    /* ★★★ 2026-10-30 修复「阅读器调方向后进休眠, 方向错/残影/图标缺半边」★★★
     *
     * 进入屏保前必须把显示栈整体拉回**系统默认横屏 400×300**, 三层都要动:
     *
     *   ① 驱动层 — st7305_reset_rotation(): 清掉 dev->pt_pfb / pt_rot / r_flip1,
     *      并把控制器 MADCTL 与地址窗口重设回横屏。
     *      不做的后果: st7305_draw_pixel / st7305_clear 在 pt_pfb 非空时只写 300 宽的
     *      竖屏缓冲 (x>299 直接被 pt_px 丢弃), 而屏保的 flush_override 发的是 dev->fb
     *      ⇒ 休眠界面根本没送出去, 屏幕停在阅读器最后一帧的残留 (残影/错位/缺半边)。
     *
     *   ② UI 层 — ui_set_portrait(false): 让 ui_screen_w/h() 回到 400×300,
     *      否则屏保按 300×400 算的居中/行高全错 (图标异常、显示不完整)。
     *
     *   ③ 触摸层 — input_set_screen_rotation(0): 休眠期虽屏蔽触屏, 但唤醒后的
     *      第一次点击仍会用旧映射, 不清会出现"点这里响那里"。
     *
     * ⚠ 这是**单向复位**: 阅读器方向不在这里保存, 也不需要保存 ——
     *   阅读器每帧渲染都会 st7305_set_rotation(lcd, s_rot, s_pfb) 重新绑定,
     *   唤醒后它重绘一次就自动回到用户选的方向 (见 book_render)。
     */
    st7305_reset_rotation(s_ss_lcd);
    ui_set_rotation_mode(0);      /* 与 reset_rotation 同语义: 0 → 横屏 400×300 */
    input_set_screen_rotation(0);

    s_ss_active = true;
    s_ss_manual = true;
    s_ss_enter_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_ss_shutdown = false;   /* 新一轮屏保重新计"休眠时间" */
    /* 用户要求: 进入动态壁纸=暂停+播放, **不关闭任何设备**(WiFi/蓝牙/音频/日志).
     * 只做前台暂停: 独占 LCD + 关刷屏门禁 冻结程序渲染, 壁纸由监督任务渲染. */
    board_rlcd_screensaver_set(true);
    st7305_set_flush_blocked(true);
    /* 触摸屏蔽随"触摸唤醒"设置: 开=不禁触屏, 允许触摸长按1秒唤醒(走 os_core);
     * 关=完全禁触屏, 触摸不响应也不刷新时钟, 仅物理键/手柄可唤醒. */
    input_set_touch_blocked(!wp_screensaver_touch_wake_enabled());
    /* ★ 2026-11-22: 屏保期关掉"按住即刷新空闲时钟"(touch_alive)。
     *   触摸唤醒开时 blocked=false, 若不关这一道, 手指一碰就会刷新输入时钟,
     *   监督任务的唤醒条件(input_last_any_ms > enter_ms)立刻成立 ⇒ "长按1秒唤醒"
     *   退化成"轻触即醒"。关掉后唤醒仍由 os_core 的 1 秒长按手势判定。 */
    input_set_touch_alive(false);
    ESP_LOGI(WP_TAG, "锁屏键点按: 进入壁纸屏保 (触摸唤醒%s)", 
             wp_screensaver_touch_wake_enabled() ? "开" : "关");
}
/* ═══════════════════════════════════════════════════════════════════════════
 *  省电模式 (2026-11-24 linit) —— 本文件是**唯一判定点**
 *
 *  为什么放在这里: "无操作 → 壁纸 → 关机" 本来就由本文件的独立监督任务驱动,
 *    无线电闲置判定必须和它共用同一把"最近输入"时钟, 否则又会出现
 *    "两个计时器各说各话" (历史上休眠/关机两档就是这么漂移的)。
 *
 *  四件事, 全部集中在 wp_eco_tick() 与下面两个 getter 里:
 *    ① 无操作 1 分钟 → 进壁纸 (WP_ECO_SS_DELAY_MS, 固定档, 不暴露设置项)
 *    ② 进壁纸后「关机时间」到点 → 自动关机 (复用 s_ss_shutdown + power_mgr)
 *    ③ Wi-Fi 闲置 3 分钟 → 关射频 (复用 s_ss_wifi_off + wifi_restore_task)
 *    ④ 蓝牙闲置 3 分钟 → 深拆栈还内存 (bt_manager_release_async)
 *  关掉开关 ⇒ ①②③④ 全部停用; 手动锁屏键/手动关机/USB 保护一律不受影响。
 *  NVS: "os_settings"/"eco" (1=开, **默认开**)。
 * ⛔ 不要在别处再加一套计时器: 一个量只能有一个来源(2026-11-02 假设置 bug 的教训)。
 * ═══════════════════════════════════════════════════════════════════════════ */
#define WP_ECO_SS_DELAY_MS   60000u    /* 无操作 1 分钟 → 壁纸/屏保 */
#define WP_ECO_IDLE_MS      180000u    /* 无线电连续闲置 3 分钟 → 关 Wi-Fi / 蓝牙 */
#define WP_ECO_NS           "os_settings"

static void wp_ss_prime(void);   /* 前向声明: 开机在内部栈预载 NVS 缓存 */
static bool    s_ss_cache_ok = false; /* 全部 NVS 缓存是否已预载(避免 PSRAM 栈监督任务读 NVS 触发禁缓存断言→panic重启循环) */
static int     s_touch_wake  = -1;    /* 触摸唤醒开关缓存(-1=未载, 0/1); 由 wp_ss_prime 预载 */
static int     s_eco_on      = -1;    /* 省电模式缓存(-1=未载, 0=关, 1=开); 由 wp_ss_prime 预载 */
/* ---- 屏保时长两个量: 壁纸时间(os_wp/min) / 关机时间(os_wp/wp_shut) ----
 * ★ 2026-11-24 linit: 「壁纸时间」恢复为用户可调项。省电模式的**作用是一键快速设置**
 *   (打开时把这两项强制改成 1 分 / 3 分并落盘 + 关闲置无线电), 但设置项本身**不隐藏**：
 *   用户手动改任一档 ⇒ 设置页自动退出省电模式, 以手动值为准。这样"强制"与"手动"
 *   不会互相打脸, 也不会出现"界面写了 X 实际按 Y 跑"的假设置。
 *   配置档只由 wp_ss_prime() 从 NVS 预载; 设置页写 NVS 后调 wp_screensaver_reload()。 */
static uint32_t s_ss_delay_cfg_ms = WP_ECO_SS_DELAY_MS;  /* 壁纸时间: UINT32_MAX=不自动进入 */
/* 诊断覆盖 (remcon `wpss` / 壁纸页): 只影响本次运行, 不进 NVS, 由 reload 清除 */
static uint32_t s_ss_delay_diag_ms = 0;
static volatile bool s_ss_delay_diag = false;

static uint32_t wp_ss_delay_ms(void)
{
    if (s_ss_delay_diag) return s_ss_delay_diag_ms;   /* 现场诊断: 用给定量 */
    return s_ss_delay_cfg_ms;
}
void wp_screensaver_set_delay_ms(uint32_t ms) { s_ss_delay_diag_ms = ms; s_ss_delay_diag = true; }
uint32_t wp_screensaver_delay_ms(void) { return wp_ss_delay_ms(); }   /* 供阻塞引擎循环在内部做空闲息屏 */
/* 设置页「关机时间」写入点: 屏保播满该时长 → 自动关机(任意键唤醒)。ms=0 表示不关机。 */
void wp_screensaver_set_shutdown_ms(uint32_t ms) { s_ss_run_ms = (ms > 0) ? ms : UINT32_MAX; }
/* 老接口, 保留给壁纸页/remcon (同一个量 = 关机时间) */
void wp_screensaver_set_run_ms(uint32_t ms) { s_ss_run_ms = ms; }
/* "关机时间"(ms); UINT32_MAX = 不自动关机(档位 0)。 */
static uint32_t wp_ss_run_ms(void)
{
    if (!s_ss_cache_ok) wp_ss_prime();
    return s_ss_run_ms;
}
/* 开机预载全部 NVS 缓存: 须在"内部栈"上下文(os_init→监督任务创建前)调用一次.
 * 监督任务栈位于 PSRAM(供星空渲染用大栈), 若在其上读 NVS 会触发 spi_flash 禁缓存
 * 断言 → panic → 重启循环(正是本 bug). 预载后各 getter 仅返回缓存值, 不再碰 NVS. */
/* ★ 2026-11-24(终版): 关机时间档位 = 0(不关机) / 1 / 3 / 5 / 10 / 20 / 30 /
 *   1 / 2 / 3 / 6 / 12 小时。**用户选什么就是什么**, 不再被省电模式压成 1/3/5。
 *   归一仍需要: 老固件档位表是 0/5/10/30/60/1440…, 用户 NVS 里可能存着 15、1440 这类
 *   不在表里的值 —— 按**不超过它的最大有效档**收敛(1440→720, 15→10), 0 保留(0=不关机)。
 *   ⛔ 设置页显示与 wp_ss_prime 实际生效必须同源 —— 这个函数是唯一实现,
 *      表必须与 page_settings.c 的 s_shutgrp_mins 完全一致。 */
static const int k_shut_mins[] = { 1, 3, 5, 10, 20, 30, 60, 120, 180, 360, 720 };
#define K_SHUT_N (int)(sizeof(k_shut_mins) / sizeof(k_shut_mins[0]))

int wp_shut_mins_norm(int mins)
{
    if (mins <= 0) return 0;     /* 0=不关机: 用户明确选过, 保留 */
    int best = k_shut_mins[0];
    for (int i = 0; i < K_SHUT_N; i++) {
        if (k_shut_mins[i] <= mins) best = k_shut_mins[i];
        else break;
    }
    return best;
}
static void wp_ss_prime(void)
{
    /* 历史: 2026-11-02 曾把这里写成 3 分而设置页默认仍是 10 分 ⇒ "界面显示 10、实际 3 分就睡"
     * 的假设置 bug(linit「休眠和关机不按我设置来」)。教训: **默认值只能有一份口径**,
     * 本函数与设置页 getter 必须同步改。
     *
     * ★★ 2026-11-24 linit「省电模式」: 省电模式的作用是**一键快速设置**——
     *   · 壁纸时间 / 关机时间都是**用户可调的正式档位**(NVS os_wp/min 与 os_wp/wp_shut),
     *     本函数只负责把 NVS 值载进缓存, 不做任何"开关门控"。
     *   · 打开省电模式时由 wp_eco_set_enabled() 把这两项**写成 1 分 / 3 分**并落盘(一次性),
     *     之后不再强制; 用户手动改任一档 ⇒ 直接以手动值为准, 省电模式开关本身不动。
     *   · 关机时间档位见 k_shut_mins[] (0/1/3/5/10/20/30/1h/2h/3h/6h/12h),
     *     老值按 wp_shut_mins_norm() 就地折算, 不回写 NVS。
     *   ⚠ 默认值必须与设置页的 sleep_min_get()/shutgrp_min_get() 完全一致, 否则又是假设置 bug。 */
    s_ss_run_ms   = (uint32_t)3 * 60000UL;   /* 关机时间: 屏保播满 3 分钟 → 自动关机 */
    s_ss_delay_cfg_ms = WP_ECO_SS_DELAY_MS;  /* 壁纸时间: 默认 1 分钟进屏保 */
    s_touch_wake  = 0;                        /* 默认触摸不唤醒 */
    s_eco_on      = 1;                        /* 省电模式: 默认开 */
    nvs_handle_t h;
    if (svc_nvs_open("os_wp", NVS_READONLY, &h) == ESP_OK) {
        int32_t t = 0;
        /* ★ 2026-10-24 linit: 「休眠时间」取消, 统一成 **关机时间**(os_wp/wp_shut)。
         *   旧档位 wp_run 只作迁移兜底读一次, 以后不再写。 */
        if (svc_nvs_get_i32(h, "wp_shut", &t) != ESP_OK &&
            svc_nvs_get_i32(h, "wp_run",  &t) != ESP_OK) t = 3;
        int mins = wp_shut_mins_norm((int)t);
        s_ss_run_ms = (mins <= 0) ? UINT32_MAX : ((uint32_t)mins * 60000UL);
        /* ★ 2026-11-24: 壁纸时间(无操作多久进屏保)恢复由 NVS 决定; 0/负 = 不自动进入。 */
        if (svc_nvs_get_i32(h, "min", &t) == ESP_OK) {
            int wm = (int)t;
            s_ss_delay_cfg_ms = (wm <= 0) ? UINT32_MAX : ((uint32_t)wm * 60000UL);
        }
        if (svc_nvs_get_i32(h, "wp_touch",&t) == ESP_OK) s_touch_wake  = (int)t;
        svc_nvs_close(h);
    }
    {   /* 省电模式开关: 与设置页共用 "os_settings"/"eco" (u8, 1=开) */
        nvs_handle_t he;
        if (svc_nvs_open(WP_ECO_NS, NVS_READONLY, &he) == ESP_OK) {
            uint8_t ev = 1;
            if (svc_nvs_get_u8(he, "eco", &ev) == ESP_OK) s_eco_on = (ev != 0) ? 1 : 0;
            svc_nvs_close(he);
        }
    }
    s_ss_cache_ok = true;
    /* 开机打印生效参数 (两个时长都是用户可调档位; 省电模式只影响无线电闲置关闭) */
    ESP_LOGI(WP_TAG, "省电参数: 省电模式=%s, 壁纸时间=%d分(-1=不自动), 关机时间=%d分(-1=不关机), 触摸唤醒=%d",
             (s_eco_on ? "开" : "关"),
             (s_ss_delay_cfg_ms == UINT32_MAX) ? -1 : (int)(s_ss_delay_cfg_ms / 60000),
             (s_ss_run_ms == UINT32_MAX) ? -1 : (int)(s_ss_run_ms / 60000),
             s_touch_wake);
}
/* 屏保播满"休眠时间" → 请求自动关机(任意键唤醒)。由 main 主循环消费。 */
bool wp_screensaver_shutdown_requested(void) { return s_ss_shutdown; }
/* V1.5.x: 触摸唤醒屏保开关 — 默认关 (触摸不唤醒壁纸, 仅物理键/手柄唤醒).
 * NVS "os_wp"/"wp_touch"; 缓存 s_touch_wake 已在上方 NVS 缓存区声明并由 wp_ss_prime 预载. */
bool wp_screensaver_touch_wake_enabled(void)
{
    if (s_touch_wake < 0) {
        int v = 0;
        nvs_handle_t h;
        if (svc_nvs_open("os_wp", NVS_READONLY, &h) == ESP_OK) {
            int32_t t = 0;
            if (svc_nvs_get_i32(h, "wp_touch", &t) == ESP_OK) v = (int)t;
            svc_nvs_close(h);
        }
        s_touch_wake = v;
    }
    return s_touch_wake != 0;
}
void wp_screensaver_set_touch_wake(bool en)
{
    s_touch_wake = en ? 1 : 0;
    nvs_handle_t h;
    if (svc_nvs_open("os_wp", NVS_READWRITE, &h) == ESP_OK) {
        svc_nvs_set_i32(h, "wp_touch", s_touch_wake);
        svc_nvs_commit(h);
        svc_nvs_close(h);
    }
}
/* ============ 独立屏保监视任务 ============
 * 目标: 空闲超时强制进壁纸、壁纸覆盖一切。
 * 传统做法把休眠判定放主循环/页面内, 会被游戏(阻塞引擎循环)堵死, 导致
 * "游戏内无法休眠、退出游戏才休眠". 本任务独立于所有页面/游戏运行, 无论
 * 前台/后台在干什么, 只要统一输入时钟停摆超过设定延时就强制进入壁纸并独占渲染。
 * 本任务只负责"进壁纸 + 渲染", 不产生任何关机动作。 */
#define SS_SUPER_STACK  4096   /* words(→16KB, PSRAM)。
                                * 历史: 8KB(xTaskCreate=2048 words)曾栈溢出→进屏保就重启；
                                * 实测(屏保未激活时)仅用 7.6KB。取 16KB = 历史爆掉值的 2 倍。
                                * ★ 2026-10-26 linit「不用就不给」: 32KB→16KB, 省 16KB PSRAM。 */
EXT_RAM_BSS_ATTR static StackType_t s_ss_stack[SS_SUPER_STACK];  /* 静态 PSRAM 栈(同 board_shim 视频任务模式) */
static StaticTask_t s_ss_tcb;   /* ⛔ 必须内部 RAM: xPortCheckValidTCBMem 校验 */
static TaskHandle_t s_ss_task_h = NULL;  /* 深睡前 vTaskSuspend */
static int s_ss_pm_mode = -1;      /* 屏保分档: -1=未定 0=静态省电 1=动态流畅 (退出时复位) */
/* ★ 2026-10-26 linit「进屏保直接关 WiFi」: 射频 20~30mA。
 *   即使 MIN_MODEM 也要按 DTIM 醒来收包; disable() 才真正 stop+deinit 并释放内部 RAM。
 *   短暂息屏也一样关（linit 要求）; 解锁后后台重新初始化，不阻塞界面。 */
static volatile bool s_ss_wifi_off = false;
/* ★ 2026-10-28: WiFi 恢复的**重试节流**时间戳。
 *   背景: 恢复代码位于 ss_supervisor_task 的"非屏保"分支, 那个分支**每 250ms** 循环一次。
 *   若不加节流, 一旦 xTaskCreate 失败或 wifi_manager_enable() 返回 false, 标志会保持 true,
 *   于是每 250ms 就新建一个 4096B 栈的任务 —— 内部 RAM 被迅速耗光, 恢复反而永远失败。
 *   5 秒足够让一次 WiFi 初始化跑完, 又不会让用户等太久。 */
static uint32_t s_wifi_restore_last_ms = 0;
/* ★ 2026-10-28 【恢复中标志】防止两个恢复任务并发, 并让任务成为最终状态的唯一裁决者。
 *   生命周期:
 *     置 true : ss_supervisor_task 的非屏保分支, 在 **xTaskCreate 之前**占位
 *               (必须先占位: 见该处注释 —— 同优先级 tick 轮转会让"创建后再置位"永久卡死)。
 *     置 false: ① 任务结束(成功/失败/又被屏保覆盖, 三条出口全覆盖)
 *               ② xTaskCreate 失败时立即回滚。
 *   语义: true = "已经有一个 enable 在跑(或即将跑), 别再创建第二个"。
 *   为什么需要: wifi_manager_enable() 要 1~2 秒, 这期间用户可能又进出屏保,
 *   若允许再创建, 会出现两个任务同时操作 esp_wifi。
 *   volatile: 由恢复任务(可能另一核)写, 监督任务读写。
 *   ⚠ volatile 只保证"不被编译器优化掉/每次都真的读写", **不提供原子性**:
 *     它与 s_ss_wifi_off 是两个独立变量, 二者之间不存在原子配对, 可被观察到中间态。
 *     代价已评估: 最坏结果是多创建一个幂等任务(enable 首行 return true)或晚 5 秒恢复,
 *     不会出现"永久卡死"或"误关用户 WiFi"。 */
static volatile bool s_wifi_restoring = false;
static void wifi_restore_task(void *arg)
{
    (void)arg;
    /* ★★★ 2026-10-28 【竞态修复】★★★
     *   原写法: 创建成功即由**调用方**清 s_ss_wifi_off, 任务只管 enable 并打日志。
     *   问题时序: 退出屏保 → 建任务 → 调用方清标志 → 任务 enable 耗时 1~2s
     *            → 用户此刻又进屏保(屏保分支看到 s_ss_wifi_off 已 false, 会 disable)
     *            → 但任务随后 enable 成功 ⇒ **屏保期间 WiFi 又开着**, 白耗 20~30mA。
     *   现在: 任务结束前重新核对屏保状态, 自己决定 s_ss_wifi_off 的最终值:
     *     · 仍在屏保 ⇒ 重新关掉 WiFi + 保留标志(等下次退出屏保再恢复)
     *     · 已退出   ⇒ 保持开启 + 清标志
     *   ⚠ 绝不能在"不在屏保"时关 WiFi —— 那正是用户要用的网络。
     *   ⚠ 失败路径也必须清 s_wifi_restoring, 否则外层永远不再重试。 */
    if (!wifi_manager_enable()) {
        s_ss_wifi_off  = true;      /* 保留重试条件(5 秒节流由外层控制) */
        s_wifi_restoring = false;
        ESP_LOGE(WP_TAG, "解锁: WiFi 恢复失败 ⇒ 保留标志, 5 秒后重试");
        vTaskDelete(NULL);
        return;
    }

    if (os_screensaver_active()) {
        /* enable 这 1~2 秒里用户又进屏保了 ⇒ 现在开着的 WiFi 是错的, 收回 */
        s_ss_wifi_off    = true;    /* 恢复条件仍在: 下次退出屏保要再开一次 */
        wifi_manager_disable();
        s_wifi_restoring = false;
        ESP_LOGW(WP_TAG, "解锁期间又进屏保 ⇒ WiFi 重新关闭, 待下次退出再恢复");
    } else {
        s_ss_wifi_off    = false;   /* 真正恢复完成 */
        s_wifi_restoring = false;
        ESP_LOGI(WP_TAG, "解锁: WiFi 已恢复");
    }
    vTaskDelete(NULL);
}

#include "usb_hid.h" /* usb_hid_host_mounted(): USB 插着只抑制**自动关机**, 不阻止息屏 (2026-11-22 收窄) */
#include "audio_player.h" /* ★ 2026-11-22: 播放/录音中不自动息屏/关机 (见 wp_media_busy) */
#include "power_stat.h" /* power_stat_tick(): USB 插入边沿 → 电源统计清零 (电池信息页) */
#include "net_client.h" /* ★ 2026-11-24: netc_busy() —— 下载/上传/商店/OTA/Lua 网络任务不算"无操作" */
#include "bt_manager.h" /* ★ 2026-11-24: 省电模式蓝牙闲置判定 + 内部栈异步深拆 */

/* USB(主机已枚举) 期间不自动休眠: 插在电脑上时正在传文件/调试/充电, 不希望它息屏甚至自动关机。 */
static bool s_usb_suppress_prev = false;

/* ---- 页面级"别息屏"占位 (2026-10-20) ---- */
static volatile bool s_idle_hold = false;
static volatile uint32_t s_idle_hold_off_ms = 0;

/* ★★★ 2026-11-22 linit「MP3 这种在播放也不休眠」★★★
 * 背景: 进屏保**不关音频**(见 os_screensaver_force_enter: 只暂停画面, 不关设备),
 *   所以一首歌播着播着, 空闲 1 分钟就被星空盖住、再过 5 分钟整机自动关机 —— 歌就断了。
 * 做法: 播放/录音期间视同"一直有人在用" —— 把空闲起点顶到当前时刻, 于是
 *   · 播放中不判空闲超时 ⇒ 不进屏保 ⇒ 也就不会连锁到自动关机;
 *   · 停止/暂停的那一刻停刷, s_media_hold_ms 停在"最后还在播"的时刻
 *     ⇒ 从停播起**重新完整计时** 1 分钟, 不会一停就立刻息屏。
 * 为什么用 audio_player 状态而不是"页面在不在": MP3 是**后台播放**设计
 *   (模块没注册 on_exit, 退出页面继续放), 按页面判会在"边听边逛别的页面"时误睡。
 * 为什么只认 PLAYING: 暂停 = 用户此刻不用它, 该正常息屏; 提示音/按键音走 wav 通道, 不计入。 */
static volatile uint32_t s_media_hold_ms = 0;

static bool wp_media_busy(void)
{
    if (audio_player_get_state() == AUDIO_STATE_PLAYING) return true;
    if (audio_player_rec_active()) return true;      /* 录音中睡了就是丢录音 */
    return false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  "系统正在被使用" 的**唯一聚合点** (2026-11-24 省电模式)
 *
 *  任务书要求: "优先复用现有状态接口, 不为每个应用单独创建复杂的省电判断"。
 *  所以这里只把**已有**的状态接口聚成一处, 不新增任何 per-app 逻辑:
 *    · 音乐播放 / 录音         → audio_player_get_state() / audio_player_rec_active()
 *    · 按住游戏键在玩          → input_game_key_held()   (松开才重新计时)
 *    · 下载/上传/商店/OTA/Lua  → netc_busy()             (单请求槽, 有请求就是忙)
 *    · 页面显式占位            → wp_idle_hold(true)      (频谱仪/收藏夹/仙剑/Lua 应用可自调)
 *  用途只有两个: ① 暂缓进屏保 ② 暂缓自动关机 / 暂缓关无线电。三处口径完全一致。
 *
 *  ⚠ input_is_game_mode() 为什么**不在**这一层: 引擎只要停在游戏页就一直是 true,
 *    连"暂停着没人在玩"也算。历史上 linit 明确要求"游戏内也要能息屏休眠"
 *    ("游戏内无法休眠、退出游戏才休眠"是当年要修的 bug), 所以息屏仍按**真实输入**判:
 *    正在玩 ⇒ 输入时钟在刷新, 本来就不会超时; 放下手柄 ⇒ 正常息屏。
 *    它只进下一层 wp_shutdown_busy() —— 息屏可以, 强杀关卡进度不行。
 *    真正在跑、连屏幕都不能盖住的会话(如仙剑、频谱仪)自己调 wp_idle_hold(true)。
 * ═══════════════════════════════════════════════════════════════════════════ */
static bool wp_activity_busy(void)
{
    if (wp_media_busy())        return true;   /* 音乐播放 / 录音 */
    if (input_game_key_held())  return true;   /* 正按着游戏键 */
    if (netc_busy())            return true;   /* 下载 / 上传 / 商店 / OTA / Lua 网络 */
    if (s_idle_hold)            return true;   /* 页面占位 */
    return false;
}

/* 「允许息屏、但不许自动关机」的活动 —— 目前只有**游戏/模拟器运行中**。
 * 任务书的说法是"对于允许进入屏保但不允许关机的活动，暂停自动关机计时"。
 * 为什么单独一层而不并进 wp_activity_busy():
 *   · 并进去 ⇒ 游戏页里永不进屏保, 屏幕整晚亮着烧电 —— 这正是 linit 当年报的
 *     "游戏内无法休眠、退出游戏才休眠"那个 bug, 不能回退;
 *   · 只用于关机 ⇒ 1 分钟照常息屏(星空 + 80MHz + 浅睡 + 无线电已关), 但不把
 *     关卡进度/存档一起关掉。两者各得其所。
 * ⚠ 引擎退出/崩溃没清标志的最坏后果只是"不自动关机"(还可手动关机), 不会丢数据。 */
static bool wp_shutdown_busy(void)
{
    if (wp_activity_busy())   return true;
    if (input_is_game_mode()) return true;     /* 引擎/模拟器运行中: 不强杀 */
    return false;
}

/* ---- 省电模式开关 ---- */
bool wp_eco_enabled(void)
{
    /* ⛔ 这里**不许**读 NVS: 本函数会被 PSRAM 栈上的屏保监督任务调用(wp_eco_tick /
     *   wp_ss_delay_ms), 读 NVS 会触发 spi_flash 禁缓存断言 → panic 重启循环。
     *   缓存由 wp_ss_prime() 在内部 RAM 栈上下文预载(supervisor_start / reload)。
     *   万一还没载入(-1), 按出厂默认"开"回答 —— 语义与默认值一致, 且绝不碰 flash。 */
    if (s_eco_on < 0) return true;
    return s_eco_on != 0;
}
void wp_eco_set_enabled(bool on)
{
    s_eco_on = on ? 1 : 0;
    nvs_handle_t h;
    if (svc_nvs_open(WP_ECO_NS, NVS_READWRITE, &h) == ESP_OK) {
        svc_nvs_set_u8(h, "eco", (uint8_t)(on ? 1 : 0));
        svc_nvs_commit(h);
        svc_nvs_close(h);
    }
    /* 关掉省电模式 ⇒ 立即作废"到点关机"的待办, 不能让它残留到下一次进屏保 */
    if (!on) s_ss_shutdown = false;
    if (on) {
        /* ★ 2026-11-24 linit: 省电模式的**作用是一键快速设置** —— 打开时把两个时长
         *   强制写成省电档 (壁纸 1 分 / 关机 3 分)并落盘。它们仍是设置页里**可见可改**的
         *   普通设置项, 用户手动改任一档后由设置页退出本模式(见 wheel_confirm_save)。
         *   ⛔ 不在这里做"关模式就不进屏保"的运行时门控 —— 那会让手动设置变成假设置。 */
        /* ⛔ 这里只能用 svc_nvs(wallpapers 已登记的历史依赖), 不要引 svc_cfg ——
         *   UI 层新增对 services 的依赖会被 tools/check_deps.py 判错。 */
        nvs_handle_t hw;
        if (svc_nvs_open("os_wp", NVS_READWRITE, &hw) == ESP_OK) {
            svc_nvs_set_i32(hw, "min", 1);
            svc_nvs_set_i32(hw, "wp_shut", 3);
            svc_nvs_commit(hw);
            svc_nvs_close(hw);
        }
    }
    wp_screensaver_reload();   /* 让缓存与 NVS 同源(含上面强制写入的两档) */
    ESP_LOGI(WP_TAG, "省电模式: %s", on ? "开 (已快速设置: 1分钟进壁纸/3分钟关机/闲置关无线电)" : "关");
}
void wp_screensaver_reload(void)
{
    /* ⚠ 不清 s_ss_cache_ok: 清了就等于"缓存无效", 而监督任务(PSRAM 栈)一旦在这个窗口里
     *   调 wp_ss_run_ms() 就会自己去 wp_ss_prime() 读 NVS → 禁缓存断言。
     *   wp_ss_prime() 本来就会把所有字段整组重载, 不清标志同样能拿到新值。 */
    s_ss_delay_diag = false;      /* 配置以 NVS 为准, 清掉上次诊断覆盖 */
    wp_ss_prime();
}

/* ---- 省电模式的无线电闲置判定 (由监督任务每秒调一次) ----
 * s_eco_*_idle_ms = 本轮"连续闲置"的起点 (0=正在计时或没开) */
static uint32_t s_eco_tick_ms   = 0;   /* 上一次执行时刻 (节流到 1 秒) */
static uint32_t s_eco_wifi_idle = 0;
static uint32_t s_eco_bt_idle   = 0;
static uint32_t s_eco_boot_ms   = 0;   /* 首选空闲起点(开机以来没人碰过时兜底) */
static uint32_t s_eco_wifi_closed_ms = 0;  /* 省电模式关 Wi-Fi 的时刻 (恢复判据, 见下) */

/* 关过 Wi-Fi 之后**只有"用户又动了"才恢复**。
 * 为什么要这道闸: 原来(屏保关 WiFi 的年代)恢复动作只出现在"非屏保分支", 而关只发生在
 * 屏保里 ⇒ 天然成对。现在关闭也可能发生在桌面态(开着但没连、没任务的空转射频),
 * 若恢复仍无条件执行, 就变成"关 3 分钟 → 1 秒后自己开回来 → 再关", 既没省电又刷日志。
 * 判据用**输入时间戳**而不是"现在闲不闲": 一次按键/触摸就足够把 Wi-Fi 要回来。 */
static bool wp_wifi_wants_restore(void)
{
    uint32_t last_in = input_last_any_ms();
    return last_in != 0 && (int32_t)(last_in - s_eco_wifi_closed_ms) > 0;
}

static void wp_eco_tick(uint32_t now)
{
    /* 节流: 屏保分支 62ms 一轮, 这里只按秒判定 */
    if (s_eco_tick_ms != 0 && (uint32_t)(now - s_eco_tick_ms) < 1000u) return;
    s_eco_tick_ms = now;
    if (s_eco_boot_ms == 0) s_eco_boot_ms = now;

    if (!wp_eco_enabled()) {          /* 省电模式关: 不执行任何专属关闭 */
        s_eco_wifi_idle = 0;
        s_eco_bt_idle   = 0;
        return;
    }

    /* "整机真的没人用" 的判据: 最近一次输入已过 3 分钟, 且没有任何在跑的业务。
     * · 一般情况 1 分钟时早该进屏保 ⇒ 这条等价于"屏保里再放 2 分钟仍没人动"。
     * · 开机后**一次都没碰过**(last_in==0)时, 用开机时刻兜底 —— 否则这种机器
     *   (开机放桌上没人用)会永远不算闲置, 射频白开着。 */
    const uint32_t last_in = input_last_any_ms();
    const uint32_t ref     = last_in ? last_in : s_eco_boot_ms;
    const bool sys_idle = ((uint32_t)(now - ref) >= WP_ECO_IDLE_MS) &&
                          !wp_activity_busy();

    /* ---------- Wi-Fi: 开了、没连接、没扫描、没任务、闲置 3 分钟 → 关 ---------- */
    if (!wifi_manager_is_enabled() || wifi_manager_is_busy() || s_ss_wifi_off) {
        s_eco_wifi_idle = 0;      /* 有业务/已关: 计时清零, 业务结束后重新计 */
    } else if (s_eco_wifi_idle == 0) {
        s_eco_wifi_idle = now;
    } else if (sys_idle && (uint32_t)(now - s_eco_wifi_idle) >= WP_ECO_IDLE_MS) {
        s_eco_wifi_idle = 0;
        /* 复用**原有**的关闭/恢复链路: s_ss_wifi_off 一置位, 用户下次操作(或退出屏保)时
         * 由下面的 wifi_restore_task 自动开回来 —— 不新建第二套恢复逻辑。 */
        s_ss_wifi_off = true;
        s_eco_wifi_closed_ms = now;
        wifi_manager_disable();
        ESP_LOGI(WP_TAG, "省电模式: Wi-Fi 闲置 ≥%u 秒且无网络任务 ⇒ 关射频 (下次操作自动恢复)",
                 (unsigned)(WP_ECO_IDLE_MS / 1000));
    }

    /* ---------- 蓝牙: 开了、没连接、没扫描、没回连窗口、闲置 3 分钟 → 深拆栈 ---------- */
    if (!bt_manager_is_enabled() || bt_manager_is_busy()) {
        s_eco_bt_idle = 0;
    } else if (s_eco_bt_idle == 0) {
        s_eco_bt_idle = now;
    } else if (sys_idle && (uint32_t)(now - s_eco_bt_idle) >= WP_ECO_IDLE_MS) {
        s_eco_bt_idle = 0;
        /* ⛔ 深拆栈会经过 controller/Bluedroid deinit, 本任务栈在 PSRAM ⇒ 不能直接调,
         *    走 bt_manager_release_async() 的内部 RAM 任务。
         * ⚠ 只关射频栈, **不碰** 用户的「自动回连」开关与手柄映射, 也不动页面/弹窗。 */
        if (bt_manager_release_async()) {
            ESP_LOGI(WP_TAG, "省电模式: 蓝牙闲置 ≥%u 秒且无连接/扫描/回连 ⇒ 深拆栈还内存",
                     (unsigned)(WP_ECO_IDLE_MS / 1000));
        }
    }
}

static void ss_supervisor_task(void *arg) {
    (void)arg;
    /* [诊断] 开机复位原因: 6=esp_restart软复位 / 7=CPU看门狗(panic) / 其它=上电/深睡 等.
     *   用于区分浅睡唤醒后的"重启回桌面"是软复位还是异常panic. */
    ESP_LOGD(WP_TAG, "开机: reset_reason=%d", (int)esp_reset_reason());
    while (1) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        power_stat_tick();   /* USB 插入边沿 → 电源统计清零 (电池信息页; 本任务已在轮询 USB) */
        /* ★ 2026-11-24 省电模式: 无线电闲置关闭的唯一判定点。
         *   放在**两个分支之外** —— 屏保态也要判(闲置 3 分钟关 Wi-Fi/蓝牙主要就发生在屏保里);
         *   内部自带 1 秒节流, 屏保分支 62ms 一轮也不会变成高频轮询。 */
        wp_eco_tick(now);
        if (os_screensaver_active()) {
            /* ★★★ 2026-10-02 修复「按一次键画面定住、要按第二次才返回」:
             *   原来本分支是"**先画星空+刷屏, 再判输入唤醒**"。按键唤醒时:
             *     主循环 os_handle_action → os_screensaver_reset() → os_render 用"刚退出那一帧"
             *     的边沿把页面重绘回来; 但本任务这一轮已经把 active 判过(true)了, 于是
             *     **又画了一帧星空盖在页面上** → 屏幕停在壁纸不动 → 必须再按一次才回到原界面。
             *   修法: **唤醒判定提到渲染之前**, 有新输入就直接跳过本帧渲染(交给主循环重绘页面)。
             *   顺序: 唤醒判定 → 自动关机判定 → 渲染。 */
            if (input_last_any_ms() > s_ss_enter_ms) {
                os_screensaver_reset();
                vTaskDelay(pdMS_TO_TICKS(52));
                continue;                       /* 本帧不画壁纸, 让主循环把页面画回来 */
            }
            /* 屏保播满"休眠时间"(wp_run) → 请求自动关机(任意键唤醒)。
             * 本任务栈在 PSRAM, 不能自己深睡(禁缓存瞬间会撞断言), 故只置标志并自挂起;
             * 真正入睡由 main 主循环(内部 RAM 栈)执行 wp_power_off(WP_OFF_AUTO)。 */
            /* USB 插着 → **只**不自动关机 (屏保照常播, 息屏照常生效)
             * ★★★ 2026-11-22 linit 定「插 USB 只抑制关机, 仍允许息屏」★★★
             *   原来桌面分支的 `!usb` 让"插着线"整晚不进壁纸 —— 屏保参数看着完全不生效。
             *   现在抑制范围收窄到关机这一件事。
             * ★ 两路都要问: 走 console 调试时 TinyUSB 没挂, 只有 USJ 为真;
             *   只问 usb_hid_host_mounted() 就会出现"插着电脑仍被 5 分钟关机"。
             * ★ 播放/录音中同样不关机 —— 这是用户按锁屏键进来的场景(屏保不关音频),
             *   若照旧按 5 分钟关机, 歌会被硬切。播放期间把 s_ss_enter_ms 顶到当前,
             *   停播后再给足整档时长, 不会"停播即关机"。 */
            /* ★ 2026-11-24: 判据换成统一聚合点 wp_shutdown_busy() (= 音乐/录音/下载/占位
             *   + 游戏运行中), 并额外要求**省电模式已开** (关掉就不自动关机, 见 wp_ss_run_ms)。
             *   仍在跑时把 s_ss_enter_ms 顶到现在 ⇒ 结束后重新给足整档时长,
             *   不会"一停就关机"。 */
            if (wp_shutdown_busy()) s_ss_enter_ms = now;
            if (!usb_hid_host_mounted() && !usb_serial_jtag_is_connected() &&
                !wp_shutdown_busy() && now - s_ss_enter_ms >= wp_ss_run_ms()) {
                s_ss_shutdown = true;
                vTaskSuspend(xTaskGetCurrentTaskHandle());
                continue;
            }
            /* 屏保中: 独占渲染星空 (主循环可能正被游戏阻塞, 不能等它) */
            int drew = 0;
            /* ★ 2026-10-31: 先取模块的静态间隔 —— 它决定"这一帧要不要升档"和"下一轮睡多久"。
             *   idle_ms>0 = 静态壁纸(人生指南)；0 = 动态壁纸(星空) 或未注册。 */
            const wp_module_t *ss_mod  = wp_get_module(s_ss_prog);
            const uint32_t      idle_ms = ss_mod ? ss_mod->idle_ms : 0;
            if (s_ss_lcd) {
                uint32_t t = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                /* ★ 只有"本帧有新内容"才送屏: 静态壁纸(人生指南)每分钟才刷一次,
                 *   动态壁纸(星空)每帧都返回 1。避免静态壁纸白刷 15000B/帧 耗电。 */
                drew = wp_program_render(s_ss_lcd, s_ss_prog, t);
                if (drew)
                    st7305_flush_override(s_ss_lcd);   /* 屏保期间唯一允许写屏的通道 */
            }
            /* ★ 2026-10-24 linit: **动态壁纸恢复常速**, 不再按"进屏保多久"降帧。
             *   之前搞过 60s 后 250ms / 5min 后 1000ms 的三档 —— 实测就是"一秒一秒的",
             *   用户明确要求恢复。现在只按**壁纸类型**分:
             *     动画壁纸(星空, 每帧都返回 1) → 52ms ≈19fps, 常速;
             *     静态壁纸(人生指南, 大多数帧返回 0) → 1000ms 轮询, 它本来就不动, 不亏画面。
             * 省电仍靠: 屏保期 CPU 降到 80MHz + 静态壁纸不刷屏 + 面板 LPM(仅空闲时)。
             * 必须有 vTaskDelay 让出 CPU **并喂狗**, 否则本任务(优先级 IDLE+2)在此分支
             * 100% 空转, 饿死同一核上的主循环/输入, 表现为"任意键都点不醒壁纸"+ task_wdt。 */
            {
                /* ★★★ 2026-10-26 linit「动态壁纸要流畅」: 按壁纸类型分档。
                 *   动态(星空, 每帧都有新内容) ⇒ max=240/min=80 + **不开浅睡**, 62ms/帧(16fps);
                 *     浅睡会让帧间隔变得不可预测(抖), 动画看着就卡。
                 *   静态(人生指南, 大多数帧无变化) ⇒ 80MHz + 开浅睡, 1000ms 轮询(省电).
                 *   两种模式只在切换时调一次 esp_pm_configure, 不逐帧调。 */
                /* s_ss_pm_mode: 文件作用域(-1=未定 0=静态 1=动态), 退出屏保时复位 */
                /* ★ 2026-10-31 修复「静态壁纸每分钟整机调频两次」:
                 *   原式 want = drew ? 1 : 0 —— 人生指南每 60 秒换条的那一帧 drew=1,
                 *   立刻 esp_pm_configure(240MHz)；62ms 后下一帧 drew=0 又切回 80MHz。
                 *   画 20 行 16px 汉字在 80MHz 下不过几 ms，为这一帧升档完全不划算，
                 *   而且 DFS 重配置 / 浅睡门控 SPI 都有代价。
                 *   现在按**壁纸类型**定档：静态(idle_ms>0) 永远 80MHz + 浅睡；
                 *   动态(星空) 保持原样 —— 有内容 240MHz，偶尔空帧才回落。 */
                /* ★★★ 2026-11-02 linit「无论怎么样全部搞省电」★★★
                 *   原来按壁纸类型分档：动态壁纸(星空) 走 **240MHz + 关浅睡 + 62ms/帧(16fps)**
                 *   + 每帧刷 15000B。屏保一挂一整夜 ⇒ 60~80mA 常流、模组发烫，
                 *   用户体感就是"明明息屏睡了，机器却一直烫、电掉 2/3"。
                 *   反射屏静止画面根本不需要 16fps。现在**统一进省电档**：
                 *   一律 80MHz + 开浅睡。
                 *   ⚠ 帧率后来又被 linit 打回 16fps（4fps 观感太卡），见下方 dly 的注释；
                 *     省电主要靠"关 WiFi + 80MHz + 浅睡"这三项，不靠压帧率。 */
                int want = 0;
                if (want != s_ss_pm_mode) {
                    s_ss_pm_mode = want;
                    esp_pm_config_t pm = (esp_pm_config_t){ .max_freq_mhz = 80, .min_freq_mhz = 80,
                                                            .light_sleep_enable = true };
                    esp_pm_configure(&pm);
                    ESP_LOGI(WP_TAG, "屏保省电档: 80MHz + 浅睡 (动态 62ms/帧 = 16fps)");
                }
                /* ★★★ 2026-11-24 省电模式: 「进屏保就关 WiFi」**取消**, 合并进 wp_eco_tick()。
                 *   原来这里是无条件关射频(屏保一进就关), 任务书要求与"闲置 3 分钟关"统一,
                 *   不允许两套判据并存 ⇒ 现在只有一个判定点:
                 *     无连接 + 无扫描 + 无网络任务 + 整机闲置 3 分钟 ⇒ 关 (见 wp_eco_tick)。
                 *   为什么不保留"一进屏保就关": 它会把"连着网、正有后台任务"的射频也切掉,
                 *   与"正在执行任务就保持开启"直接冲突; 而省电模式默认 3 分钟后就会自动关机,
                 *   屏保里少省的那点射频电, 远不如"别打断业务"重要。 */
                /* 静态壁纸：固定按 idle_ms 轮询（不因"这帧画了"而缩短成 62ms）。
                 * 动态壁纸：有内容 62ms，无内容回落到 1000ms（原行为）。 */
                /* ★ 2026-11-02 linit「动态壁纸不要跑那么高的帧率」:
                 *   原来 62ms(16fps) 是"要流畅"的老设定；屏保没人看时纯烧电。
                 *   现在动态壁纸 250ms(4fps) 起步，进屏保满 60 秒后降到 1000ms(1fps)，
                 *   画面仍在动，功耗再降一档。静态壁纸照旧按它自己的 idle_ms。 */
                /* ★★ 2026-11-02 修正(linit「动态壁纸太卡」):
                 *   上一版把动态壁纸压到 250ms(4fps)、进屏保 60 秒后再降到 1000ms(1fps),
                 *   省电是省了, 但星空几乎是在"跳帧", 观感就是卡。
                 *   反射屏静止画面不费力, 真正的耗电大头是射频和 CPU 满频 —— 这两项已单独处理
                 *   (屏保关 WiFi + 80MHz 浅睡)。这里把帧率恢复到 16fps(62ms), 并去掉 1fps 降级。
                 *   静态壁纸仍按自己的 idle_ms 轮询, 不受影响。 */
                uint32_t dly;
                if (idle_ms > 0)  dly = idle_ms;   /* 静态(人生指南): 按模块间隔 */
                else if (!drew)   dly = 1000u;     /* 本帧没画成(被门禁挡): 别空转 */
                else              dly = 62u;       /* 动态(星空): 16fps */
                vTaskDelay(pdMS_TO_TICKS(dly));
            }
        } else {

            {   /* 桌面态: 按状态切换, 不再用"只执行一次"的 static 开关 */
                /* ★★★ 2026-11-07 修「进过一次屏保就永久 80MHz」★★★
                 *   原来是 `static bool s_pm_fast`: 第一次退出屏保后置 true 便**永不回到
                 *   这个分支**。而屏保入场设的是 80/80MHz ⇒ 之后整个生命周期都被钉在
                 *   80MHz, 界面发滞、游戏掉帧, 却没人看得出原因。
                 *   现在按**目标档位**比较: 桌面档 = 1, 屏保档 = 0 (见上面 want)。
                 *   两个方向都能正确切换, 且只在跨档时调一次 esp_pm_configure。 */
                /* ★ 2026-11-08: 每次循环都看一眼 USB 状态(62ms 一次, 开销可忽略),
                 *   状态变了就重配 PM —— 拔线后自动允许浅睡, 插线后自动退出浅睡。 */
                int usb_now = usb_serial_jtag_is_connected() ? 1 : 0;
                /* ★★★ 2026-11-08 实测教训: 不能只靠 IDF 官方锁 ★★★
                 *   CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y 号称"USB 连着就持
                 *   ESP_PM_NO_LIGHT_SLEEP 锁, 阻止自动浅睡"。真机实测**没拦住**:
                 *   插着电脑跨过开机 60s 后仍然进了浅睡, 串口直接断(设备还活着, 按
                 *   RST 才回来)。原因没继续深挖(怀疑 esp_pm_lock 在 tick hook 里没生效),
                 *   结论是「不能把串口可用性押在一个没验证过的机制上」。
                 *   所以这里用**已经实测准确**的 usb_serial_jtag_is_connected() 自己判:
                 *     插着电脑(s_usb_conn==1) ⇒ light_sleep=false, 串口恒定
                 *     拔了线                  ⇒ light_sleep=true, 空闲进浅睡
                 *   官方选项保留当第二道保险, 但不再依赖它。
                 *   另加开机 60s 宽限: 万一把配置改坏, 还有一分钟能连串口救回来。 */
                int ls_now;
                if (s_ls_mode == WP_LS_OFF)      ls_now = 0;
                else if (s_ls_mode == WP_LS_ON)  ls_now = 1;
                else {
                    /* auto: 插着就绝不睡; 要连续 WP_USB_GONE_CONFIRM 轮都判定断开才睡 */
                    if (usb_now) s_usb_gone_n = 0;
                    else if (s_usb_gone_n < WP_USB_GONE_CONFIRM) s_usb_gone_n++;
                    ls_now = (s_usb_gone_n >= WP_USB_GONE_CONFIRM) ? 1 : 0;
                }
                if (esp_timer_get_time() < 60LL * 1000 * 1000) ls_now = 0;
                if (s_ss_pm_mode != 1 || ls_now != s_ls_eff || usb_now != s_usb_conn) {
                    s_ss_pm_mode = 1;
                    s_ls_eff     = ls_now;
                    s_usb_conn   = usb_now;
                    /* ★★ 2026-10-26 linit「动态调频省电」: min 从 240 改为 80 ——
                     *   原来 min=max=240 把 DFS **锁死**了, CPU 永远满频。
                     *   现在 max=240 (有任务/动画自动跑满, 性能不变),
                     *   min=80 (纯空闲时由 IDF 的 DFS 自动降频)。
                     *   APB 仍是 80MHz ⇒ SPI 屏/PSRAM 时序不受影响。 */
                    /* ★★★ 2026-11-07 linit「桌面必须持久保持串口状态」★★★
                     *   桌面档**不进自动浅睡**(light_sleep_enable=false)。原因:
                     *   ESP32-S3 进 Automatic Light Sleep 会关 USB PHY / USB Serial-JTAG,
                     *   主机侧串口反复消失+重枚举(实测: 旧版 DISABLED 端口稳定, 新版
                     *   ENABLED 后 /dev/cu.usbmodem* 时有时无, esptool 报 No serial data
                     *   received, 烧录与 remcon 都连不上)。这条是硬约束, 不能为省电牺牲。
                     *   桌面仍保留 DFS 降频(max=240 / min=80), 有负载自动跑满, 性能不变;
                     *   只是空闲停在 WFI+80MHz, 不真正睡眠。
                     *   ⚠ 睡眠/屏保档(want=0, 见上方)仍然 light_sleep_enable=true,
                     *     即「睡眠时才退出串口状态」, 省电与 PM tagmem 保留缓冲照常生效。
                     *   ⚠ CONFIG_PM_SLP_DISABLE_GPIO / PM_RESTORE_CACHE_TAGMEM 等
                     *     sdkconfig 项一律未动, 屏保浅睡时仍由 IDF 使用保留缓冲。 */
                    esp_pm_config_t pm = { .max_freq_mhz = 240, .min_freq_mhz = 80,
                                           .light_sleep_enable = (ls_now != 0) };
                    esp_err_t pe = esp_pm_configure(&pm);
                    if (pe == ESP_OK)
                        ESP_LOGI(WP_TAG, "桌面档: 240/80MHz + %s (usb=%d ls_mode=%d)",
                                 (ls_now ? "允许浅睡(省电)" : "保串口不浅睡"), usb_now, s_ls_mode);
                    else
                        ESP_LOGE(WP_TAG, "桌面档配置失败(%s)", esp_err_to_name(pe));
                }
                /* ★★★ 2026-10-28 教训: WiFi 恢复**不许**放进"只执行一次"的开关里 ★★★
                 *   (当年是 s_pm_fast, 现在换成分档判断, 道理一样) ——
                 *   建任务一旦失败就再也没有第二次机会, 用户表现为"解锁后一直没网"。
                 *   放在外面则每轮都检查 s_ss_wifi_off, 失败 5 秒后自然重试。
                 *   s_ss_wifi_off 只在"省电模式判定为闲置、由本模块关掉"时才为 true,
                 *   用户手动关 WiFi 不会误触发。
                 * ★ 2026-11-24: 现在还要过 wp_wifi_wants_restore() —— 只有**关掉之后
                 *   用户又操作过**才恢复。否则"桌面态关射频"会在 1 秒后被自己开回来,
                 *   变成关/开空转(既没省电, 又把日志刷满)。 */
                if (s_ss_wifi_off && !s_wifi_restoring && wp_wifi_wants_restore()) {
                    /* ★ 2026-10-28 【节流】本分支每 250ms 跑一次, 必须限制重试频率:
                     *   创建失败 / enable 失败时标志会保持 true, 不节流就是每 250ms
                     *   新建一个 4096B 栈任务 ⇒ 内部 RAM 耗尽 ⇒ 恢复永远失败。
                     *   首次尝试(s_wifi_restore_last_ms==0)立即执行, 之后至少间隔 5 秒。 */
                    bool due = (s_wifi_restore_last_ms == 0) ||
                               ((uint32_t)(now - s_wifi_restore_last_ms) >= 5000u);
                    if (due) {
                        s_wifi_restore_last_ms = now;
                        /* ★★★ 2026-10-28 【必须"先占位、后创建"】★★★
                         *   ss_supervisor_task 优先级 = tskIDLE_PRIORITY+2 = 2,
                         *   wifi_rest 也是 2 ⇒ 同优先级按 tick 轮转, **不保证**当前任务
                         *   连续执行。原写法是"创建成功后再置 s_wifi_restoring = true",
                         *   中间那条窗口里一旦发生 tick 抢占, 且新任务很快跑完
                         *   (WiFi 已 enabled 时 wifi_manager_enable() 第一行就 return true),
                         *   它会把 s_wifi_restoring 清成 false 并自删; 随后本任务又把
                         *   它置回 **true** ⇒ 永久卡住。
                         *   后果: 之后 s_ss_wifi_off 再变 true 时, 外层 `!s_wifi_restoring`
                         *   恒为假 ⇒ **再也不创建恢复任务**, 屏保退出后永远没网,
                         *   只能重启才恢复。
                         *   先置位则窗口消失: 任务启动前标志已是 true, 它结束时清掉的就是
                         *   自己这一次的占位; 创建失败则立即回滚。 */
                        s_wifi_restoring = true;
                        if (xTaskCreate(wifi_restore_task, "wifi_rest", 4096, NULL, 2, NULL) != pdPASS) {
                            s_wifi_restoring = false;   /* 回滚, 5 秒后可重试 */
                            ESP_LOGE(WP_TAG, "wifi_rest 任务创建失败 ⇒ WiFi 未恢复, 5 秒后重试");
                        }
                        /* 注意: **不在这里清 s_ss_wifi_off** ——
                         *   最终状态由任务自己核对屏保后裁决(见 wifi_restore_task)。
                         *   调用方抢着清会让"任务 enable 期间又进屏保"这条时序
                         *   出现"屏保里 WiFi 开着"的漏洞。 */
                    }
                }
            }
            /* ★ 2026-10-31: 收尾"已登记待释放"的壁纸数据。
             *   走到这里说明：不在屏保（上面的 if 分支没进）⇒ render 没在跑，
             *   是唯一能确认"屏保任务不再访问那 380KB"的时机。
             *   真正的 free 在 wp_lifeguide_reap() 里，本任务栈虽在 PSRAM，
             *   但 heap_caps_free 只操作堆结构（内部 RAM），不碰 flash，安全。
             *   没有待释放项时它第一句就返回，每 250ms 一次的开销可忽略。 */
            wp_lifeguide_reap();

            /* 空闲检测 (低频省电)
             * ★ 2026-11-08 审计修 (B1): 原来只问 usb_hid_host_mounted() → TinyUSB 的
             *   tud_mounted()。但本机 console 走的是 **USB Serial JTAG**, S3 上两者共用
             *   同一个 USB PHY、互斥 —— TinyUSB 很可能根本没挂起来, 于是这个判定恒假。
             * ★★★ 2026-11-22 linit 定「插 USB 只抑制关机, 仍允许息屏」★★★
             *   原来这里带 `!usb` ⇒ 插着线就永不进壁纸, 用户以为"息屏/关机设置没生效"。
             *   息屏不关串口、不影响传文件, 没有理由拦; 抑制只保留给"自动关机"(见上面屏保分支)。
             *   ⛔ 不要再把 `!usb` 加回来。 */
            bool usb = usb_hid_host_mounted() || usb_serial_jtag_is_connected();
            if (usb != s_usb_suppress_prev) {
                s_usb_suppress_prev = usb;
                ESP_LOGD(WP_TAG, "USB 主机%s → %s", usb ? "已连接" : "已断开",
                         usb ? "暂停自动关机(仍会息屏)" : "恢复自动关机");
            }
            uint32_t last_in = input_last_any_ms();
            /* ★ 2026-11-22: 播放/录音中视同有人在用 —— 空闲起点顶到现在,
             *   停播后 s_media_hold_ms 停在"最后还在播"的时刻 ⇒ 从停播起重新计时。 */
            if (wp_media_busy()) s_media_hold_ms = now;
            if (s_media_hold_ms && (int32_t)(s_media_hold_ms - last_in) > 0)
                last_in = s_media_hold_ms;
            /* ★ 2026-10-20: 页面可以"占住"不休眠 (wp_idle_hold(1)), 例如收藏夹的
             *   设置管理界面 —— 用户在那儿翻配置, 不该被息屏打断。
             *   解除 hold 的那一刻重新开始算空闲, 否则一松手就立刻息屏。 */
            if (s_idle_hold_off_ms && (int32_t)(s_idle_hold_off_ms - last_in) > 0)
                last_in = s_idle_hold_off_ms;
            /* ★ 2026-11-24 省电模式: 判据统一走 wp_activity_busy()
             *   (音乐/录音/下载/页面占位/按住游戏键), 与"关机"用同一个聚合点 ——
             *   以前这里手工列 wp_media_busy + s_idle_hold + input_game_key_held 三项,
             *   新增活动源(下载)时容易漏一处, 造成"息屏了但关机不判"的口径分裂。
             *   wp_ss_delay_ms() 返回「壁纸时间」档位(os_wp/min): UINT32_MAX = 档位 0
             *   (不自动进入壁纸); 省电模式只负责把该档位改写成 1 分, 不做运行时门控。 */
            if (!wp_activity_busy() && last_in != 0 &&
                now - last_in >= wp_ss_delay_ms()) {
                ESP_LOGD(WP_TAG, "独立监视: 空闲超时, 强制进入壁纸");
                os_screensaver_force_enter();
            }
            /* ★ 2026-11-08 审计优化 (A2): 桌面态原来是 250ms 轮询 —— 这在浅睡下就是
             *   "最多睡 250ms 就被叫醒", 把浅睡收益砍掉一大半。
             *   本分支只做三件秒级精度足够的事:
             *     ① wp_lifeguide_reap()  (有东西才 free, 无待释放时首句返回)
             *     ② USB 插入边沿 (只影响电池统计清零时机)
             *     ③ 空闲超时进屏保 (默认 10 分钟)
             *   ⇒ 放宽到 1000ms。代价: 进屏保最多晚 0.75s(用户无感), USB 检测同理。
             *   屏保分支(上面)仍是自己的节奏, 不受影响。 */
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void wp_idle_hold(bool on)
{
    if (s_idle_hold == on) return;
    s_idle_hold = on;
    if (!on) s_idle_hold_off_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    ESP_LOGD(WP_TAG, "空闲占位 %s", on ? "开(不息屏)" : "关(恢复计时)");
}
bool wp_idle_held(void) { return s_idle_hold; }

/* 关机前钩子: 挂起屏保监督任务 + 解除屏保独占。
 * 该任务栈在 PSRAM 且持续刷屏, 深睡瞬间禁用 flash/PSRAM 缓存会撞断言 → 关机变重启。
 * 关机后是复位重启, 本任务随重启重建, 无需恢复。 */
void wp_screensaver_halt(void) {
    if (s_ss_task_h) vTaskSuspend(s_ss_task_h);
    os_screensaver_reset();   /* 停独占/刷门禁/触摸屏蔽 */
}

/* 创建独立屏保监视任务 (由 os_init 传入 LCD 调用一次) */
void wp_stars_register_module(void);       /* 星空, 见 wp_module_stars.c */
void wp_lifeguide_register_module(void);   /* 人生指南, 见 wp_module_lifeguide.c */

/* ★ 2026-10-31: 切换壁纸立即生效（见 wallpapers.h 的说明）。
 *   屏保监督任务只在开机时读过一次 NVS，之后 s_ss_prog 就固定了；
 *   壁纸设置页改完 NVS 后再调这里，下一帧屏保就会用新模块 —— 不用重启。
 *   （放在本文件靠后: 要写 s_ss_pm_mode，它在上面才定义。）
 *   传越界 id 收敛到星空，避免"选了不存在的模块 → 屏保一片空白"。 */
void wp_set_program(int prog) {
    s_ss_prog = wp_get_module(prog) ? prog : WP_PROG_STARS;
    /* 让下一次进入屏保重新判定档位（静态 idle_ms / 动态 62ms + 对应频率），
     * 否则会沿用上一个壁纸的档位，直到"本帧有无新内容"翻转才纠正。 */
    s_ss_pm_mode = -1;
    ESP_LOGI(WP_TAG, "屏保壁纸切换为: %s (id=%d)", wp_prog_name(s_ss_prog), s_ss_prog);
}

void wp_screensaver_supervisor_start(st7305_handle_t *lcd) {
    s_ss_lcd = lcd;
    wp_ss_prime();   /* 内部栈上下文预载 NVS 缓存(必须在创建 PSRAM 栈监督任务前) */
    wp_stars_register_module();      /* 显式注册(不依赖 constructor, 防 gc-sections 丢弃) */
    wp_lifeguide_register_module();  /* 人生指南文字壁纸 */
    {
        /* 读 NVS "os_wp"/"prog" (用已有的 svc_nvs, 避免 ui→services 新增反向依赖) */
        int p = WP_PROG_STARS;
        nvs_handle_t h;
        if (svc_nvs_open("os_wp", NVS_READONLY, &h) == ESP_OK) {
            int32_t v = WP_PROG_STARS;
            if (svc_nvs_get_i32(h, "prog", &v) == ESP_OK) p = (int)v;
            svc_nvs_close(h);
        }
        s_ss_prog = wp_get_module(p) ? p : WP_PROG_STARS;   /* 越界/模块不存在 → 回星空 */
    }
    /* ★ 2026-10-31: 上次选的是人生指南 ⇒ 在这里先把它读进 PSRAM。
     *   本函数运行在**内部 RAM 栈**（os_init → app_main），可以安全碰 flash；
     *   而真正渲染的屏保任务栈在 PSRAM，自己去 fopen 会触发 spi_flash 禁缓存断言。
     *   预加载失败就回星空，不留"选了却没有画面"的状态。 */
    if (s_ss_prog == WP_PROG_LIFEGUIDE) {
        if (!wp_lifeguide_preload()) {
            ESP_LOGW(WP_TAG, "人生指南数据预加载失败 ⇒ 本次开机回星空");
            s_ss_prog = WP_PROG_STARS;
        }
    }
    static bool started = false;
    if (started) return;
    started = true;
    /* 静态 PSRAM 栈 (同 board_shim 视频任务): 32KB 保证星空渲染/刷屏/日志不溢出 */
    xTaskCreateStatic(ss_supervisor_task, "sse", SS_SUPER_STACK, NULL, tskIDLE_PRIORITY + 2,
                      s_ss_stack, &s_ss_tcb);
    s_ss_task_h = (TaskHandle_t)&s_ss_tcb;
    ESP_LOGI(WP_TAG, "屏保独立监视任务已启动 (PSRAM 栈 %u words)", (unsigned)SS_SUPER_STACK);
}

