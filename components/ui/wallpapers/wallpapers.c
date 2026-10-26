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
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_common.h"
#include "esp_log.h"

#include "svc_nvs.h"
#include "board_rlcd.h"   /* 屏保独占 LCD: 冻结游戏帧刷新, 壁纸恒在最上层 */
#include "input.h"        /* 统一"最近输入"时钟: 独立监视据此判定空闲 */
#include "wifi_manager.h"  /* ★ 进屏保关 WiFi 射频 / 解锁恢复 */

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
static volatile bool s_ss_active = false;   /* volatile: 屏保监督任务(另一核) 与主循环并发读写 */
static volatile bool s_ss_manual = false;  /* 锁屏键点按强制进入, 不自动复位 */
static volatile uint32_t s_ss_enter_ms = 0;  /* 进入屏保的时刻 (唤醒判定 + 休眠计时起点) */
static volatile bool s_ss_shutdown = false;  /* 屏保播满"休眠时间", 请求自动关机 */
static uint32_t s_ss_run_ms = 0;             /* "休眠时间"缓存(ms): 屏保播多久 → 自动关机 */
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
    ESP_LOGI(WP_TAG, "锁屏键点按: 进入壁纸屏保 (触摸唤醒%s)", 
             wp_screensaver_touch_wake_enabled() ? "开" : "关");
}
/* 息屏(星空屏保)延时: 读壁纸 NVS "os_wp"/"min" (分钟).
 *   min<=0 = 自动不息眠 (返回 UINT32_MAX, 空闲差永远到不了);
 *   未在 NVS 设置时默认 10 分钟. */
static void wp_ss_prime(void);   /* 前向声明: 开机在内部栈预载 NVS 缓存 */
static bool    s_ss_cache_ok = false; /* 全部 NVS 缓存是否已预载(避免 PSRAM 栈监督任务读 NVS 触发禁缓存断言→panic重启循环) */
static int     s_touch_wake  = -1;    /* 触摸唤醒开关缓存(-1=未载, 0/1); 由 wp_ss_prime 预载 */
static uint32_t s_ss_delay_ms = 0;   /* 缓存; 0=尚未从 NVS 加载 */
static uint32_t wp_ss_delay_ms(void)
{
    if (!s_ss_cache_ok) wp_ss_prime();
    return s_ss_delay_ms;
}
void wp_screensaver_set_delay_ms(uint32_t ms) { s_ss_delay_ms = ms; }
uint32_t wp_screensaver_delay_ms(void) { return wp_ss_delay_ms(); }   /* 供阻塞引擎循环在内部做空闲息屏 */
/* 设置页「关机时间」写入点: 屏保播满该时长 → 自动关机(任意键唤醒)。ms=0 表示不关机。 */
void wp_screensaver_set_shutdown_ms(uint32_t ms) { s_ss_run_ms = (ms > 0) ? ms : UINT32_MAX; }
/* 老接口, 保留给壁纸页/remcon (同一个量 = 关机时间) */
void wp_screensaver_set_run_ms(uint32_t ms) { s_ss_run_ms = ms; }
/* "休眠时间"(ms); UINT32_MAX = 永不休眠(档位 0)。 */
static uint32_t wp_ss_run_ms(void)
{
    if (!s_ss_cache_ok) wp_ss_prime();
    return s_ss_run_ms;
}
/* 开机预载全部 NVS 缓存: 须在"内部栈"上下文(os_init→监督任务创建前)调用一次.
 * 监督任务栈位于 PSRAM(供星空渲染用大栈), 若在其上读 NVS 会触发 spi_flash 禁缓存
 * 断言 → panic → 重启循环(正是本 bug). 预载后各 getter 仅返回缓存值, 不再碰 NVS. */
static void wp_ss_prime(void)
{
    s_ss_delay_ms = (uint32_t)10 * 60000UL;  /* 壁纸(息屏)延时: 默认 10 分钟 */
    s_ss_run_ms   = (uint32_t)10 * 60000UL;  /* 关机时间: 默认 10 分钟 → 屏保播满即自动关机 */
    s_touch_wake  = 0;                        /* 默认触摸不唤醒 */
    nvs_handle_t h;
    if (svc_nvs_open("os_wp", NVS_READONLY, &h) == ESP_OK) {
        int32_t t = 0;
        if (svc_nvs_get_i32(h, "min",    &t) == ESP_OK) s_ss_delay_ms = (t <= 0) ? UINT32_MAX : ((uint32_t)t * 60000UL);
        /* ★ 2026-10-24 linit: 「休眠时间」取消, 统一成 **关机时间**(os_wp/wp_shut)。
         *   旧档位 wp_run 只作迁移兜底读一次, 以后不再写。 */
        if (svc_nvs_get_i32(h, "wp_shut", &t) != ESP_OK &&
            svc_nvs_get_i32(h, "wp_run",  &t) != ESP_OK) t = 10;
        s_ss_run_ms = (t <= 0) ? UINT32_MAX : ((uint32_t)t * 60000UL);
        if (svc_nvs_get_i32(h, "wp_touch",&t) == ESP_OK) s_touch_wake  = (int)t;
        svc_nvs_close(h);
    }
    s_ss_cache_ok = true;
    /* 开机打印生效的屏保参数 (NVS "os_wp" 有值则优先于代码默认; -1 = 永不) */
    ESP_LOGI(WP_TAG, "屏保参数: 息屏延时=%d分, 关机时间=%d分, 触摸唤醒=%d",
             (s_ss_delay_ms == UINT32_MAX) ? -1 : (int)(s_ss_delay_ms / 60000),
             (s_ss_run_ms   == UINT32_MAX) ? -1 : (int)(s_ss_run_ms / 60000),
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
static StaticTask_t s_ss_tcb;
static TaskHandle_t s_ss_task_h = NULL;  /* 深睡前 vTaskSuspend */
static int s_ss_pm_mode = -1;      /* 屏保分档: -1=未定 0=静态省电 1=动态流畅 (退出时复位) */
/* ★ 2026-10-26 linit「进屏保直接关 WiFi」: 射频 20~30mA。
 *   即使 MIN_MODEM 也要按 DTIM 醒来收包; disable() 才真正 stop+deinit 并释放内部 RAM。
 *   短暂息屏也一样关（linit 要求）; 解锁后后台重新初始化，不阻塞界面。 */
static volatile bool s_ss_wifi_off = false;
static void wifi_restore_task(void *arg)
{
    (void)arg;
    wifi_manager_enable();
    ESP_LOGI(WP_TAG, "解锁: WiFi 已恢复");
    vTaskDelete(NULL);
}
static st7305_handle_t *s_ss_lcd = NULL;

#include "sleep.h"   /* 电源模块: wp_power_init(绑定 LCD + 注册深睡唤醒源) */
#include "usb_hid.h" /* usb_hid_host_mounted(): USB 插着就不自动休眠 (2026-10-02) */
#include "power_stat.h" /* power_stat_tick(): USB 插入边沿 → 电源统计清零 (电池信息页) */

/* USB(主机已枚举) 期间不自动休眠: 插在电脑上时正在传文件/调试/充电, 不希望它息屏甚至自动关机。 */
static bool s_usb_suppress_prev = false;

/* ---- 页面级"别息屏"占位 (2026-10-20) ---- */
static volatile bool s_idle_hold = false;
static volatile uint32_t s_idle_hold_off_ms = 0;

static void ss_supervisor_task(void *arg) {
    (void)arg;
    /* [诊断] 开机复位原因: 6=esp_restart软复位 / 7=CPU看门狗(panic) / 其它=上电/深睡 等.
     *   用于区分浅睡唤醒后的"重启回桌面"是软复位还是异常panic. */
    ESP_LOGD(WP_TAG, "开机: reset_reason=%d", (int)esp_reset_reason());
    while (1) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        power_stat_tick();   /* USB 插入边沿 → 电源统计清零 (电池信息页; 本任务已在轮询 USB) */
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
            /* USB 插着 → 不自动关机 (屏保可以继续播, 但永不入睡) */
            if (!usb_hid_host_mounted() && now - s_ss_enter_ms >= wp_ss_run_ms()) {
                s_ss_shutdown = true;
                vTaskSuspend(xTaskGetCurrentTaskHandle());
                continue;
            }
            /* 屏保中: 独占渲染星空 (主循环可能正被游戏阻塞, 不能等它) */
            int drew = 0;
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
                int want = drew ? 1 : 0;
                if (want != s_ss_pm_mode) {
                    s_ss_pm_mode = want;
                    esp_pm_config_t pm = want
                        ? (esp_pm_config_t){ .max_freq_mhz = 240, .min_freq_mhz = 80,  .light_sleep_enable = false }
                        : (esp_pm_config_t){ .max_freq_mhz = 80,  .min_freq_mhz = 80,  .light_sleep_enable = true  };
                    esp_pm_configure(&pm);
                    ESP_LOGI(WP_TAG, "屏保分档: %s (%dms/帧, 浅睡=%d)",
                             want ? "动态流畅 240MHz" : "静态省电 80MHz",
                             want ? 62 : 1000, want ? 0 : 1);
                }
                /* ★ 2026-10-26 linit「进入屏保直接关 WiFi」: 射频占 20~30mA, 是屏保期最大耗电项。
                 *   disable() = disconnect + stop + deinit(释放内部 RAM); 解锁后后台恢复(约 2s)。 */
                if (!s_ss_wifi_off && wifi_manager_is_enabled()) {
                    s_ss_wifi_off = true;
                    wifi_manager_disable();
                    ESP_LOGI(WP_TAG, "屏保: WiFi 已关闭 (省 20~30mA), 解锁后自动恢复");
                }
                uint32_t dly = drew ? 62u : 1000u;   /* ★ 2026-10-26 linit: 动态 16fps (62.5ms) */
                vTaskDelay(pdMS_TO_TICKS(dly));
            }
        } else {

            {   /* 退出屏保: 频率恢复 240MHz */
                static bool s_pm_fast = false;
                if (!s_pm_fast) {
                    s_pm_fast = true;
                    s_ss_pm_mode = -1;   /* 退出屏保: 分档状态复位, 下次进入重新判定 */
                    if (s_ss_wifi_off) {   /* 屏保期间关过 WiFi ⇒ 后台恢复 (不阻塞解锁) */
                        s_ss_wifi_off = false;
                        xTaskCreate(wifi_restore_task, "wifi_rest", 4096, NULL, 2, NULL);
                    }
                    /* ★★ 2026-10-26 linit「动态调频省电」: min 从 240 改为 80 ——
                     *   原来 min=max=240 把 DFS **锁死**了, CPU 永远满频。
                     *   现在 max=240 (有任务/动画自动跑满, 性能不变),
                     *   min=80 (纯空闲时由 IDF 的 DFS 自动降频;
                     *   CONFIG_PM_DFS_INIT_AUTO=y 已开, FreeRTOS 有任务就会拉回 240)。
                     *   APB 仍是 80MHz ⇒ SPI 屏/PSRAM 时序不受影响。 */
                    esp_pm_config_t pm = { .max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = false };
                    esp_pm_configure(&pm);
                }
            }
            /* 空闲检测 (低频省电) —— USB 插着就不进屏保(也就不会连锁到自动关机) */
            bool usb = usb_hid_host_mounted();
            if (usb != s_usb_suppress_prev) {
                s_usb_suppress_prev = usb;
                ESP_LOGD(WP_TAG, "USB 主机%s → %s", usb ? "已连接" : "已断开",
                         usb ? "暂停自动休眠/自动关机" : "恢复自动休眠");
            }
            uint32_t last_in = input_last_any_ms();
            /* ★ 2026-10-20: 页面可以"占住"不休眠 (wp_idle_hold(1)), 例如收藏夹的
             *   设置管理界面 —— 用户在那儿翻配置, 不该被息屏打断。
             *   解除 hold 的那一刻重新开始算空闲, 否则一松手就立刻息屏。 */
            if (s_idle_hold_off_ms && (int32_t)(s_idle_hold_off_ms - last_in) > 0)
                last_in = s_idle_hold_off_ms;
            /* ★ 2026-10-09: "正在玩(按住游戏键)不自动进屏保" —— 按住不再刷新 last_in,
             *   所以这里必须显式避开, 否则按住键玩 10 分钟会被判空闲而进屏保。
             *   (松开的一瞬间 input_mark_game_held() 会重置时钟, 于是"放下手柄才开始计时") */
            if (!usb && !s_idle_hold && last_in != 0 && !input_game_key_held() &&
                now - last_in >= wp_ss_delay_ms()) {
                ESP_LOGD(WP_TAG, "独立监视: 空闲超时, 强制进入壁纸");
                os_screensaver_force_enter();
            }
            vTaskDelay(pdMS_TO_TICKS(250));
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
    static bool started = false;
    if (started) return;
    started = true;
    /* 静态 PSRAM 栈 (同 board_shim 视频任务): 32KB 保证星空渲染/刷屏/日志不溢出 */
    xTaskCreateStatic(ss_supervisor_task, "sse", SS_SUPER_STACK, NULL, tskIDLE_PRIORITY + 2,
                      s_ss_stack, &s_ss_tcb);
    s_ss_task_h = (TaskHandle_t)&s_ss_tcb;
    wp_power_init(s_ss_lcd);   /* 电源模块: 绑定 LCD(关机清屏用) + 注册深睡唤醒源 */
    ESP_LOGI(WP_TAG, "屏保独立监视任务已启动 (PSRAM 栈 %u words)", (unsigned)SS_SUPER_STACK);
}

