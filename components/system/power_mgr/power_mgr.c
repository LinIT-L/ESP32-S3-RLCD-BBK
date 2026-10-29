/* ============================================================================
 * power_mgr.c — 系统电源管理: 深睡/关机的唯一实现
 *
 * 从 components/ui/wallpapers/sleep.c 整体迁出 (2026-10-28, 第 5 阶段 Step 3)。
 * **代码与注释逐行照搬, 关闭顺序 / 延时 / 判断条件一字未改** —— 这条路径上每
 * 一条注释都是一次实测换来的, 迁移时不允许"顺手优化"。
 *
 * 本文件负责: 停射频 → 关音频 → GPS 停靠 → 卸 SD/关触摸/关屏 → 唤醒源与引脚
 * hold → 电源域 → 降频 → 屏 deinit → 引脚隔离 → 诊断汇总 → esp_deep_sleep_start()。
 * 不负责: 关机画面(UI 层刷白后调本模块) / 电池统计(power_stat 通过 pre_sleep 钩子)。
 * ==========================================================================*/
#include <stdbool.h>
#include "board_pins.h"   /* ★按键引脚唯一真相源 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
   /* rtc_gpio_pullup_en: RTC 域上拉(深睡期间数字域掉电, 数字上拉会失效) */
#include "esp_attr.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "svc_nvs.h"     /* 复位历史落 NVS (供 remcon rst 取证) */
#include "power_mgr.h"
#include "st7305.h"
#include "bt_manager.h"
#include "wifi_manager.h"
#include "touch_panel.h"    /* touch_panel_power_down() / touch_panel_sleep() */
#include "audio_player.h"   /* audio_pa_force_off(): 进深睡前关功放并 hold 低电平 */
#include "gps.h"            /* gps_boot_park(): 进深睡前把 GPS 送进 backup 待机 */
#include "driver/usb_serial_jtag.h"   /* usb_serial_jtag_driver_uninstall(): 深睡前关 USB 控制台 */
#include "sd_scan.h"        /* sd_unmount(): 深睡卸卡, 卡进 idle */

#define TAG "PWR"

/* 物理键: PWR(GPIO1) / KEY 左(GPIO18) / BOOT 右(GPIO0), 低电平按下。 */
#define PWR_KEY     (board_pins_get()->btn_pwr)   /* ★ 按板型表 */
#define KEY_LEFT    (board_pins_get()->btn_left)
#define KEY_RIGHT   (board_pins_get()->btn_right)

#define PWR_MASK       (1ULL << PWR_KEY)
/* 唤醒掩码 = PWR(GPIO1) + KEY 左(GPIO18)。**故意不含 GPIO0(BOOT 键)**:
 * 实测(2026-09-19) GPIO0 作为 EXT1 唤醒源时, 深睡瞬间电平保不住(strapping 脚, 板上无外部上拉;
 * 关机前数字读=1, 深睡时变低), ext1_status=0x1 → 关机后立刻自我唤醒,
 * 现象正是"休眠自动关机后 1 秒又回桌面"。GPIO0 在正常按键扫描里仍照常作 BOOT 键使用。 */
#define ANY_KEY_MASK   (PWR_MASK | (1ULL << KEY_LEFT))
#define WAKE_LEVEL     ESP_EXT1_WAKEUP_ANY_LOW

#define MANUAL_HOLD_MS       500    /* 手动关机后, PWR 需保持按住多久才放行开机 */
#define KEY_RELEASE_WAIT_MS  3000   /* 等按键松开的最长时间(防卡死) */

/* 关机来源: 高 24 位 magic(掉电后 RTC 内存乱码 → 校验不通过), 低 8 位存来源。 */
#define ORIGIN_MAGIC   0x57000000u
#define ORIGIN_MASK    0xFFFFFF00u

RTC_NOINIT_ATTR static uint32_t s_off_origin;   /* 跨深睡保留的关机来源 */
static st7305_handle_t *s_lcd = NULL;
static void (*s_pre_sleep_hook)(void) = NULL;

/* ═══ 分阶段功耗诊断掩码 (2026-10-28, 任务书第五步) ═══
 * 默认 0x7F = 全部关闭 ⇒ **正式关机流程行为完全不变**。
 * 位: 0x01 屏 / 0x02 音频 / 0x04 SD / 0x08 触摸 / 0x10 GPS / 0x20 震动 / 0x40 引脚隔离
 * 用法: remcon `sleepstage 0x13` ⇒ 只关屏+音频+GPS, 其余外设保持原状, 立刻深睡。
 *       配合外接电流表, 就能逐项对账"到底是谁在耗电"。
 * ⚠ 默认值不能改: 正式关机必须全关。
 *
 * ★★★ 2026-10-28 补齐 (审查发现 0x20/0x40 只写在注释里, 代码里没有判断) ★★★
 *   现在每一位**只控制该阶段的硬件动作**, 关掉就必须真跳过, 不是只少打一行日志:
 *     0x01 屏      : st7305_sleep_in() + st7305_deinit()
 *     0x02 音频    : audio_player_sleep_for_off_early() + audio_player_sleep_for_off()
 *     0x04 SD      : sd_unmount() ×2 + 卸载后 SD 总线引脚(CLK低/CMD高/D0高)hold
 *     0x08 触摸    : touch_panel_power_down() + touch_panel_sleep()
 *     0x10 GPS     : gps_boot_park()(含 2 次重试) + gps_tx_hold_high()
 *     0x20 震动    : 震动脚钉低 + hold(禁浮空)
 *     0x40 引脚隔离: 未用脚关上下拉 + rtc_gpio_isolate
 *
 * ⚠ 没有掩码位的固定动作(所以 `sleepstage 0` 并不等于"真的什么都不关"):
 *     · bt_manager_disable() / wifi_manager_disable() —— 原设计里就没有射频位,
 *       任何档位都会执行。要单独测射频, 得先加一位(本次不动)。
 *
 * ⛔ 以下属于**睡眠安全必需**, 任何掩码都不能跳过(写死, 不受 mask 控制):
 *     · wait_keys_released()   —— 键还按着就睡会立刻自唤醒("关不掉")
 *     · wake_src_set()         —— EXT1 唤醒源, 跳了就永远叫不醒
 *     · wake_pins_hold()       —— 锁住唤醒键上拉, 否则深睡浮空→秒醒
 *     · RTC_PERIPH 供电域 ON   —— EXT1 唤醒依赖 RTC 域上拉
 *     · 诊断汇总后 150ms 延时  —— 让最后几行日志发出去, 否则取证全丢
 *     · esp_deep_sleep_start() —— 没有它就没有"关机"
 *   诊断掩码只用来分项测电流; 它**不能**造出一个"睡下去就醒不来"的组合。 */
static uint32_t s_stage_mask = 0x7F;
void power_mgr_set_stage(uint32_t m) { s_stage_mask = m & 0x7F; }
uint32_t power_mgr_get_stage(void) { return s_stage_mask; }

void power_mgr_set_pre_sleep_hook(void (*cb)(void)) { s_pre_sleep_hook = cb; }

/* 并发的第二次关机请求: 不重复跑流程, 直接永久阻塞 (关机路径不返回, 没有"返回"语义)。
 * ★ 2026-10-28 审查结论:
 *   · 调用点只有一个(main.c 的 power_off_ui), 但上游有三条路径 —— 手动关机(对话框回调)、
 *     自动关机(main 主循环 / 屏保到点)、remcon `shutdown`(独立任务), **确实可能来自不同任务**。
 *   · 正常路径 esp_deep_sleep_start() 不返回 ⇒ 标志**不需要也不能清**(清了就等于允许第二次)。
 *   · 异常返回(理论不可达, 见文件末尾)走软重启, 标志保持 true —— 重启后 RAM 全清。
 *   · 被拦下的任务 vTaskDelay(portMAX_DELAY): 不占 CPU、不持锁, 深睡/重启时随系统一起消失。 */
static bool s_off_entered = false;

/* ---- 唤醒键(GPIO)配置: 普通数字输入 + 上拉 ---- */
static void keys_config(uint64_t mask)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    /* gpio_config 配的是**数字域**上拉: 深睡时数字域掉电 → 上拉失效 → 引脚浮空,
     * EXT1(ANY_LOW) 会把浮空误判成"按下" → 关机瞬间自我唤醒(现象: 关一下又回桌面)。
     * 官方 ext_wakeup.c 在 SOC_RTCIO_INPUT_OUTPUT_SUPPORTED 的芯片上正是补 RTC 域上拉。
     * 注: 只开 RTC 上拉(rtc_gpio_pullup_en)不动 IO_MUX, 数字读键不受影响;
     *     不要在这里调 rtc_gpio_init() —— 那会把 pad 路由到 RTC 功能, 数字读就断了。 */
    if (mask & PWR_MASK)             rtc_gpio_pullup_en(PWR_KEY);
    if (mask & (1ULL << KEY_LEFT))   rtc_gpio_pullup_en(KEY_LEFT);
    if (mask & (1ULL << KEY_RIGHT))  rtc_gpio_pullup_en(KEY_RIGHT);
}

/* ---- 唤醒源设置(官方 ext_wakeup.c 写法): 先清空, 再注册 EXT1 掩码 ---- */
static void wake_src_set(uint64_t mask)
{
    keys_config(mask);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_err_t err = esp_sleep_enable_ext1_wakeup_io(mask, WAKE_LEVEL);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "EXT1 唤醒源设置失败: %s", esp_err_to_name(err));
    }
    /* EXT1 不在 IDF 的"自动保 RTC_PERIPH 供电"名单里(RTC_EXT0/RTC_GPIO 才自动开,
     * 见 sleep_modes.c), 不显式打开则该域掉电 → RTC 上拉失效 → 引脚浮空被误唤醒。
     * 代价是深睡电流略高, 对按键唤醒设备是必要开销。 */
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
}

/* ---- ★ 深睡期间"锁住唤醒键的当前配置" (2026-10-14 修"睡眠秒自醒") ----
 * 症状: 自动关机(掩码 GPIO1|GPIO4)一睡就醒; 手动关机(仅 GPIO1)能睡住。
 * 根因: `gpio_config()` 配的上拉属于**数字域**, 深睡时数字域掉电 → 上拉失效 → 引脚浮空;
 *       EXT1 是电平触发(ANY_LOW), 浮空脚被当成"按下" → 立刻自醒。
 *       GPIO1 浮空恰好落在高电平所以没事, GPIO4 浮空落在低电平 → 秒醒。
 *       (老板 LEFT=GPIO18 只是"运气好浮在高", 这坑一直在, 换到 GPIO4 才暴露。)
 * 官方手段 (IDF `driver/gpio.h`): `gpio_hold_en(pin)` 把该 pad 的**当前有效配置**(含上拉)锁存,
 *       再 `gpio_deep_sleep_hold_en()` 让锁存在**深睡期间**生效 ——
 *       原文: "the state of each pad holds is its active configuration (not pad's sleep configuration!)"。
 * ⛔ 不要用 `rtc_gpio_init()` 那条路: 它把 pad 切到 RTC 功能, 数字读键会失灵(0.7 踩过)。
 * 释放: 醒来后由 `app_board` 的 `gpio_hold_dis()` 统一解。 */
static void wake_pins_hold(uint64_t mask)
{
    if (mask & PWR_MASK)             gpio_hold_en(PWR_KEY);
    if (mask & (1ULL << KEY_LEFT))   gpio_hold_en(KEY_LEFT);
    if (mask & (1ULL << KEY_RIGHT))  gpio_hold_en(KEY_RIGHT);
    gpio_deep_sleep_hold_en();   /* 幂等; audio_pa_force_off() 也会调 */
}

/* 掩码内是否有键正被按住 */
static bool any_key_down(uint64_t mask)
{
    if ((mask & PWR_MASK)                && gpio_get_level(PWR_KEY)   == 0) return true;
    if ((mask & (1ULL << KEY_LEFT))      && gpio_get_level(KEY_LEFT)  == 0) return true;
    if ((mask & (1ULL << KEY_RIGHT))     && gpio_get_level(KEY_RIGHT) == 0) return true;
    return false;
}

/* 等唤醒键全部松开。EXT1 是电平触发: 键还按着就睡会立刻自醒 → 看起来"关不掉"。
 * 只在深睡**前**调用(此时 flash 缓存还在, 可以正常 vTaskDelay)。 */
static void wait_keys_released(uint64_t mask)
{
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    while (any_key_down(mask)) {
        if ((uint32_t)(esp_timer_get_time() / 1000) - t0 > KEY_RELEASE_WAIT_MS) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void power_mgr_pin_init(void)
{
    keys_config(ANY_KEY_MASK);
    wake_src_set(PWR_MASK);   /* 默认保守: 只有 PWR 能唤醒 */
}

void power_mgr_init(st7305_handle_t *lcd)
{
    s_lcd = lcd;
    power_mgr_pin_init();
}

/* ---- 关机(不返回) ----
 * 顺序: 停射频 → 关音频(early) → GPS 停靠 → 关音频(hold) → 卸 SD/关触摸/关屏
 *       → 等键松开 → 设唤醒掩码并 hold → 电源域 → 降频 → 屏 deinit
 *       → 二次卸 SD/触摸复位 → SD 引脚 hold → 隔离未用脚 → 诊断 → 深睡。
 * 深睡瞬间禁用 flash/PSRAM 缓存, 屏保监督任务(PSRAM 栈刷屏)由调用方先挂起。 */
void power_mgr_off(int origin)
{
    /* ★ 并发防重: 第二个调用者永久阻塞。关机路径本来就不返回, 这里不能"返回后继续跑",
     *   否则会出现两套流程交叉关外设 / 两次 esp_deep_sleep_start()。 */
    if (__atomic_exchange_n(&s_off_entered, true, __ATOMIC_SEQ_CST)) {
        ESP_LOGW(TAG, "重复关机请求被拦下 (流程已在进行)");
        vTaskDelay(portMAX_DELAY);
    }

    if (bt_manager_is_enabled())   bt_manager_disable();
    if (wifi_manager_is_enabled()) wifi_manager_disable();

    /* ★★★ 2026-10-28 linit「睡眠时候提前睡眠功放/麦克风驱动芯片/音频芯片」★★★
     *   必须放在 gps_boot_park() **之前**: GPS 停靠要 4~15 秒
     *   (装 UART → 等稳态 3s → PMREQ → 失败再走 PSMOO 等 MAXACQ 15s),
     *   而音频关断原来排在它后面 ⇒ 这十几秒里 ES8311(5~10mA)、ES7210(探到就跑, 20~30mA)、
     *   功放 全都还挂着。先关掉它们, 再做慢的 GPS。
     *   ⚠ early 版故意不做 gpio_deep_sleep_hold_en()(全局冻结 pad 会堵死随后的 GPS UBX),
     *     那次全局冻结仍由 GPS 之后的 audio_player_sleep_for_off() 补。 */
    if (s_stage_mask & 0x02) audio_player_sleep_for_off_early();

    /* ★ 功放使能必须显式关断并 hold 住低电平。
     *   深睡时 GPIO 默认变高阻, 若模块侧功放 SD 脚有外部上拉, 功放会在**整个睡眠
     *   期间保持工作**(几十 mA 级) —— "关机了还在耗电"的典型来源。
     *   必须在进深睡**之前**做 (esp_deep_sleep_start 之后没有代码会执行)。 */
    /* ★ 2026-10-09: GPS (MAX-M10S) 也必须显式送进"软件待机", 并把 ESP 侧 TX 钉住。
     *   为什么必须在这里做:
     *   ① 深睡期间**没有任何代码会执行** ⇒ 若此刻 GPS 还在连续跟踪, 它会整夜保持
     *      ~10mA(峰值 25mA); 而待机只要 46µA —— 差 200 倍。这是"关机还在掉电"的
     *      另一大来源 (此前只治了功放 GPIO46 那一路)。
     *   ② GPS 的唤醒源是 **UART RX**。深睡时 GPIO 变高阻, TX 浮空/拾噪会被 GPS 当成
     *      起始位把它唤醒 —— 醒后再没人让它睡回去。`gpio_hold_en` 把 TX 冻在 UART
     *      的空闲高电平上, 从根上不给它边沿。
     *   ⚠ 顺序: 先 gps_sleep() (要发 UBX, 需要 UART 还能工作) 再 hold。 */
    /* ★★★ 2026-10-27 linit 实测「关机还有 0.1W」根因修复 ★★★
     *   原来这里调 gps_sleep(), 而 gps_sleep() 第一行就是 `if (!s_ready) return;`
     *   —— GPS 是选装件, 平时根本没 init(s_ready=false) ⇒ **PMREQ 从来没发出去**,
     *   MAX-M10S 整夜按 10~25mA 连续跟踪。实测: 断电关掉 GPS 后 25mA → 3mA。
     *   改用 gps_boot_park(): 自带「装 UART → 听 300ms → 该发才发 PMREQ → 卸驱动」,
     *   不依赖 s_ready, 冷启动/热启动都能落到 backup(46µA)。 */
    /* ★★★ 2026-10-28 linit「休眠前也要让 GPS 睡眠」★★★
     *   原来这里只调一次 gps_boot_park(), **不看返回值** —— 而它内部只发 1 轮 PMREQ,
     *   冷启动/正在搜星的模块很可能把第一帧丢掉(实测过: 第 1 次被忽略, 37 秒后重试才成)。
     *   一旦失败, 深睡期间没有任何代码会执行 ⇒ GPS 整夜连续跟踪 10~25mA
     *   (实测: 深睡占比 96.8% 却 24h 掉 424mV ≈ 30mA, 就是这个)。
     *   现在失败补 2 次(每次约 4 秒)。代价: 关机最多多花 8 秒, 而屏幕此时已全白, 用户无感。 */
    if (s_stage_mask & 0x10) {
        esp_err_t pr = gps_boot_park();
        /* ★★★ 2026-10-29 (深睡 20mA 定位): BUSY 不是"失败", 是"另一个停靠正在跑"。
         *   最常见的就是 app_board 的开机停靠(实测 1.9~6.1 秒)。原来的做法是 800ms×3 次重试,
         *   在"开机 2 秒内关机"时三个窗口**全部撞在 BUSY 上** ⇒ 直接放弃 ⇒ GPS 电源常通
         *   (硬件文档: 只有 PMREQ 能省电), 带着 10~25mA 进深睡。
         *   现在: BUSY 时按 100ms 轮询等它跑完(上限 8 秒), 等到了再正常发 PMREQ。
         *   ⚠ 只对 NOT_FINISHED 等 —— "模块没就绪/没插"仍是 INVALID_STATE, 等也没用。 */
        for (int i = 0; i < 80 && pr == ESP_ERR_NOT_FINISHED; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
            pr = gps_boot_park();
        }
        if (pr == ESP_ERR_NOT_FINISHED)
            ESP_LOGW(TAG, "GPS 停靠等待 8s 仍被占用(开机停靠未结束)");

        for (int i = 0; i < 2 && pr != ESP_OK; i++) {
            ESP_LOGW(TAG, "GPS 停靠失败(%s) ⇒ 第 %d 次重试", esp_err_to_name(pr), i + 1);
            vTaskDelay(pdMS_TO_TICKS(800));
            pr = gps_boot_park();
        }
        if (pr != ESP_OK)
            ESP_LOGE(TAG, "GPS 最终仍未进待机(%s) ⇒ 深睡期间它会一直耗电, 只能靠硬件断电",
                     esp_err_to_name(pr));
        else if (!gps_sleep_verified())
            /* ★ 2026-10-29: 只凭 sleep_verified 判断, 不引用 GPS 睡眠状态机(gps_sm_t) ——
             *   状态机属于 GPS 驱动的独立改造, 本组件只依赖 gps.h 的既有接口,
             *   否则电源模块会被迫连带整块 GPS 改动一起入库。 */
            ESP_LOGW(TAG, "GPS 已省电但**未经确认** ⇒ 若为 PSMOO 则平均约 0.1mA, 需实测");
        else
            ESP_LOGI(TAG, "GPS 已确认进入待机");
        /* TX 钉高交给驱动统一做(幂等): 软件待机的唤醒源是 UART RX, pad 浮空即被叫醒 */
        gps_tx_hold_high();
    } else {
        ESP_LOGW(TAG, "[stage] 跳过 GPS 停靠 (0x10 未置位)");
    }

    /* ⚠ 必须放在 GPS 之后调用: 这行内部会 gpio_deep_sleep_hold_en() + hold 功放脚,
     *   把 pad 冻住。放在前面会把 UART TX 也冻上, 上面那条 UBX 就发不出去
     *   —— "GPS 送待机" 会静默失败, 深睡期间 GPS 继续 ~10mA。(2026-10-09 修顺序) */
    if (s_stage_mask & 0x02) audio_player_sleep_for_off();

    /* ★★★ 2026-10-29: 关机前**唯一一次**日志落盘 —— 位置必须在 sd_unmount() 之前。
     *   sd_unmount() 会把 s_mounted 置 false (sd_scan.c:690), 之后 sd_is_mounted()==false,
     *   日志再也写不进这张卡 —— 所以这里就是最后的机会。
     *   · 不重试、不阻塞: 失败只记录, 绝不影响关机流程。
     *   · 下面这几条提示本身是在刷新**之后**产生的, 所以**不会**进这次写出的文件
     *     (只会在串口上看到) —— 这是时序决定的, 不是 bug。
     *   · 触摸 / 屏 deinit / 引脚隔离 / 诊断汇总 全在 sd_unmount() 之后, 一律不落盘
     *     (已知且接受: 不挪动关断顺序)。 */
    {
        /* svc_log 不在本组件 PRIV_REQUIRES 里(本轮不改 CMake), 用局部声明取用 */
        extern esp_err_t svc_log_flush_once(void);
        esp_err_t lr = svc_log_flush_once();
        if (lr == ESP_OK)
            ESP_LOGI(TAG, "关机前日志已落盘 (/sdcard/data/log/bbk.log)");
        else if (lr == ESP_ERR_INVALID_STATE)
            ESP_LOGW(TAG, "关机前日志未落盘: SD 未挂载, 缓冲留在 RAM");
        else if (lr == ESP_ERR_INVALID_SIZE)
            ESP_LOGW(TAG, "关机前日志未落盘: 文件已达 1MB (旧日志保留未删)");
        else if (lr == ESP_ERR_TIMEOUT)
            ESP_LOGW(TAG, "关机前日志未落盘: 等锁超时, 缓冲留在 RAM");
        else
            ESP_LOGW(TAG, "关机前日志落盘失败(%s), 这一段可能丢失", esp_err_to_name(lr));
    }

    /* ================= 睡前"能关的全关" (2026-10-24) =================
     * 目标: 除 MCU 的 RTC 域外, 所有外设进各自最低功耗态。
     * 顺序有讲究: 必须在最后一帧刷完之后, 在 esp_deep_sleep_start 之前。 */
    /* ① TF 卡: 只声明没调用过 —— 卡在深睡里一直是待机态(0.5~2mA)。
     *    先卸载文件系统再断 VFS, 避免卡侧还挂着命令队列。 */
    if (s_stage_mask & 0x04) sd_unmount();
    /* ② 触摸 IC: RST 拉低 + hold。唤醒源是 EXT1 按键, 不靠触摸, 睡死没副作用。 */
    if (s_stage_mask & 0x08) touch_panel_power_down();
    /* ③ 屏: **完全关闭** (st7305_sleep_in, 默认 mode 2 = 0x28 + 0x10)。
     *    ★ 2026-10-26 linit「深睡完全关闭屏幕」: Display OFF + Sleep In ⇒ 连内部升压
     *    VGH/VGL 一起断, 面板待机 ~1-5µA。
     *    ⚠ 代价已知: 本板 lcd_rst 没接, 万一面板锁死只能拔电池(重刷无效)。
     *    要回到"可恢复的关显示"用 `remcon slptest 4`(只发 0x28)。 */
    /* ★ 2026-10-29: sleep_in 的返回值原来直接被丢掉 —— lcd_lock 超时(120ms)时面板
     *   **根本没进 sleep**, 仍在扫描(50~100µA), 而汇总里照样写"已关"。
     *   现在接住结果, 汇总按真实状态输出。se 在下面汇总里用。 */
    esp_err_t se = ESP_OK;
    if (s_stage_mask & 0x01) {
        se = st7305_sleep_in(s_lcd);
        if (se != ESP_OK)
            ESP_LOGE(TAG, "屏 sleep_in 失败(%s) ⇒ 面板仍在扫描(50~100µA)", esp_err_to_name(se));
    } else {
        ESP_LOGW(TAG, "[stage] 跳过屏 sleep_in (0x01 未置位)");
    }
    /* ★ 2026-10-28: 不再写死"全部关断"—— 分阶段诊断时它会是假话。
     *   逐项真实状态统一由下面的"睡前诊断汇总"按掩码给出。 */
    ESP_LOGI(TAG, "外设关断阶段完成 (stage mask=0x%02X, 屏 sleep mode=%d) — 明细见睡前汇总",
             (unsigned)s_stage_mask, st7305_get_sleep_mode());

    /* ★ 2026-10-26 linit「深睡唤醒只要电源键, 其他通通屏蔽」:
     *   原来自动关机时用 ANY_KEY_MASK(PWR + LEFT) ⇒ 碰左键也会唤醒。
     *   现在**固定只用 PWR**（自动/手动关机一致）, 触摸/左键/右键都不再是唤醒源。
     *   注: 右键(GPIO0)本来就被排除（strapping 脚, 深睡电平保不住会自唤醒）; */
    const uint64_t mask = PWR_MASK;
    s_off_origin = ORIGIN_MAGIC | (uint32_t)((unsigned)origin & 0xFFu);

    /* 诊断: 关机前的三键电平(1=松开 0=按下)。某键恒 0 = 浮空/被外部拉低 → 会被 EXT1 误唤醒。 */
    ESP_LOGI(TAG, "关机前键电平: PWR(%d)=%d LEFT(%d)=%d RIGHT(%d)=%d",
             (int)PWR_KEY, gpio_get_level(PWR_KEY),
             (int)KEY_LEFT, gpio_get_level(KEY_LEFT),
             (int)KEY_RIGHT, gpio_get_level(KEY_RIGHT));

    wait_keys_released(mask);
    wake_src_set(mask);
    wake_pins_hold(mask);   /* ★ 锁住唤醒键的上拉配置, 否则深睡数字域掉电→浮空→秒醒 */

    ESP_LOGI(TAG, "关机: 进入深睡 (来源=%s, 唤醒=PWR 键按住 0.5s, 掩码=0x%llX)",
             (origin == POWER_OFF_AUTO) ? "休眠到点" : "手动",
             (unsigned long long)mask);
    if (s_pre_sleep_hook) s_pre_sleep_hook();   /* 结账"醒着段", 记下入睡时刻 (电池信息页用) */

    /* ★★★ 2026-10-18 linit: **深睡省电加深** —— 把 IDF 里能关的电源域全关掉。
     *   这些是纯软件开关, 不占内存, 实测能省几十 µA 到 ~1mA:
     *   · RTC_SLOW_MEM / RTC_FAST_MEM: 深睡不用 RTC 内存(没开 ULP) ⇒ 断电 ✓
     *   · XTAL: 深睡用 RC32K 就够, 主晶振断电 ✓
     *   · VDDSDIO: flash/PSRAM 供电域断电(数据已在 flash 里, 醒来重新初始化) ✓
     *   · RTC_PERIPH 必须 ON —— EXT1 唤醒依赖 RTC 域里的上拉, 关了就叫不醒 ✗ */
    /* ★★★ 2026-10-29 (深睡 20mA 排查): 这 4 个返回值原来**全部丢弃** ——
     *   一旦某个域没关成功(尤其 MODEM), WiFi/BLE 射频会一直供着, 实测可到 20mA 级,
     *   而日志里一个字都看不出来, 等于把最大的嫌疑点做成了黑盒。
     *   现在逐项记录, 让"射频域到底断没断"变成可观测事实。 */
    {
        esp_err_t pd;
        pd = esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
        if (pd != ESP_OK)
            ESP_LOGE(TAG, "电源域 RTC_PERIPH 置 ON 失败(%s) ⇒ EXT1 唤醒可能失效!",
                     esp_err_to_name(pd));
        pd = esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_OFF);
        if (pd != ESP_OK)
            ESP_LOGW(TAG, "电源域 XTAL 断电失败(%s)", esp_err_to_name(pd));
        pd = esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_OFF);
        if (pd != ESP_OK)
            ESP_LOGW(TAG, "电源域 VDDSDIO 断电失败(%s) ⇒ Flash/PSRAM 仍供电",
                     esp_err_to_name(pd));
        pd = esp_sleep_pd_config(ESP_PD_DOMAIN_MODEM, ESP_PD_OPTION_OFF);
        if (pd != ESP_OK)
            ESP_LOGE(TAG, "电源域 MODEM 断电失败(%s) ⇒ WiFi/BLE 射频可能仍供电(20mA 级!)",
                     esp_err_to_name(pd));
        else
            ESP_LOGI(TAG, "电源域: RTC_PERIPH=ON, XTAL/VDDSDIO/MODEM=OFF 全部生效");
    }

    /* ★ 2026-10-24: 入睡前把 CPU 降到最低档。
     *   ⚠ 说清楚: **深睡期间 CPU 是断电的, 调频对深睡电流没有影响**。
     *   这里降频是为了 ①入睡前的收尾阶段少耗一点 ②万一这版被改成"浅睡"也已经是低频。
     *   真正有用的是屏保态降频(=醒着但没人在看), 那个在 wallpapers.c 的监督任务里。 */
    {
        esp_pm_config_t pm = { .max_freq_mhz = 80, .min_freq_mhz = 80, .light_sleep_enable = false };
        esp_pm_configure(&pm);
    }

    /* ★★★ 2026-10-18 linit: 微雪 LCD RST(GPIO41) 平时被强制拉高("释放"电平),
     *   深睡前把它**拉低**并保持 —— 屏已 deinit, RST 高会让面板复位电路持续耗电 ✓
     *   (其他板 lcd_rst 不接/不同, 不动)
     * ★ 2026-10-28: 属"屏(0x01)"阶段 —— 0x01 不给就跳过, 否则 `sleepstage 0` 仍会动屏。 */
    {
        const board_pins_t *bpw2 = board_pins_get();
        if ((s_stage_mask & 0x01) && bpw2->id == BOARD_ID_WEIXUE && bpw2->lcd_rst >= 0) {
            gpio_set_level((gpio_num_t)bpw2->lcd_rst, 0);
            gpio_hold_en((gpio_num_t)bpw2->lcd_rst);   /* 深睡期间保持低 */
            ESP_LOGI(TAG, "微雪: LCD RST(GPIO%d) 已拉低并 hold(省电)", bpw2->lcd_rst);
        }
    }

    /* ★ 屏幕进低功耗(SPI 停 + 面板待机): 原来只清屏没 deinit ⇒ 面板还在扫描,
     *   实测 ST7305 正常工作 ~50-100µA, 待机 ~1-5µA ✓ (清屏已由 UI 层在请求前做过)
     * ★ 2026-10-28: deinit 与 sleep_in 同属"屏(0x01)"阶段 —— 掩码不给 0x01 时两步都跳过,
     *   否则 `sleepstage 0` 会变成"面板没关但 SPI 已停"的半关状态, 测出来的电流没法用。 */
    if (!(s_stage_mask & 0x01)) {
        ESP_LOGW(TAG, "[stage] 跳过屏 deinit (0x01 未置位)");
    } else if (s_lcd) {
        st7305_deinit(s_lcd);
        ESP_LOGI(TAG, "屏已 deinit(面板待机)");
    }

    ESP_LOGI(TAG, "深睡电源域: RTC_PERIPH=ON(唤醒用), XTAL/VDDSDIO/MODEM=OFF");

    /* ★★★ 2026-10-18 linit: **TF 卡必须卸载** —— 否则卡在深睡期间仍保持"传输态",
     *   待机电流可达**几 mA**(来源: "SD 卡的待机电流可能很大(几十毫安!), 进入深睡前
     *   一定要正确卸载并切断电源")。卸载后卡进入 idle, 耗电降到 µA 级 ✓
     *   (深睡唤醒=完整重启 ⇒ 会重新 mount ✓) */
    /* ★ 2026-10-28: 第二次卸载同样受 0x04 控制 —— 原先这里是**无条件**调用,
     *   于是 `sleepstage 0x13` 照样把卡卸了, "只关屏+音频+GPS"根本不成立。 */
    if (s_stage_mask & 0x04) sd_unmount();
    else ESP_LOGW(TAG, "[stage] 跳过 SD 二次卸载 (0x04 未置位)");

    /* ★ 2026-10-20: 触摸 IC 按入复位态 + I2C 上拉关掉。
     *   CST836U 常温工作电流 ~1mA 级, 深睡期间没人复位它就一直扫屏。
     *   (tp_rst 已在 app_board 的开机 gpio_deep_sleep_hold_dis() 里有释放项)
     * ★ 2026-10-28: 同样受 0x08 控制(原先无条件, 0x13 下触摸照样被复位)。 */
    if (s_stage_mask & 0x08) touch_panel_sleep();
    else ESP_LOGW(TAG, "[stage] 跳过触摸进复位 (0x08 未置位)");
    /* ★ 2026-10-20: 卸载后 SD 总线闲着 —— 把 CLK 钉低、CMD/D0 钉高(卡的空闲态),
     *   避免引脚浮空被卡片当成时钟/命令噪声而反复醒来。
     * ★ 2026-10-28: 归入 SD(0x04) 阶段 —— 不卸卡就不该动 SD 的引脚, 否则 0x13 档
     *   仍然改动了 SD 总线电平, 测出来的"SD 原状"是假的。 */
    if (s_stage_mask & 0x04) {
        const board_pins_t *bps = board_pins_get();
        if (bps && bps->sd_clk >= 0) {
            gpio_set_level((gpio_num_t)bps->sd_clk, 0); gpio_hold_en((gpio_num_t)bps->sd_clk);
            if (bps->sd_cmd >= 0) { gpio_set_level((gpio_num_t)bps->sd_cmd, 1); gpio_hold_en((gpio_num_t)bps->sd_cmd); }
            if (bps->sd_d0  >= 0) { gpio_set_level((gpio_num_t)bps->sd_d0,  1); gpio_hold_en((gpio_num_t)bps->sd_d0);  }
        }
    } else {
        ESP_LOGW(TAG, "[stage] 跳过 SD 总线引脚 hold (0x04 未置位)");
    }

    /* ★★★ 2026-10-18 linit: **隔离所有不用的引脚** —— 官方文档明确推荐:
     *   "Some ESP32-S3 IOs have internal pullups/pulldowns, which are enabled by default.
     *    如果外部电路驱动这些脚, 电流会经过上下拉电阻 ⇒ 增加深睡电流。
     *    调 rtc_gpio_isolate() 可避免。"
     *   ⇒ 除了【唤醒键(必须有上拉)】【GPS TX(已 hold 高)】【功放脚(已 hold 低)】,
     *     其余全部隔离 ✓ 这一步通常是深睡电流从 mA 级压到 µA 级的关键。 */
    {
        const int keep[3] = { PWR_KEY, KEY_LEFT, KEY_RIGHT };
        const gps_config_t *gc2 = gps_default_config();
        /* GPS 没启用 ⇒ 它的脚按"普通未用脚"处理(隔离), 不要留在白名单里 */
        int gpstx = (gps_is_ready() && gc2 && gc2->pin_tx >= 0) ? gc2->pin_tx : -1;
        const board_pins_t *bp = board_pins_get();
        int papin = bp ? bp->pa_pin : -1;

        /* ★★★ 2026-10-28 震动马达控制脚必须**钉低 + hold**, 绝不能浮空 ★★★
         * docs/硬件与IO.md: "震动 TM6604 + 1508 线性马达, EN 靠电阻**常开**
         *   (R4/R16 10k→VBAT)。⛔ 禁 gpio_reset_pin()、**禁浮空**。"
         * 而下面这个"隔离未用脚"的循环对 vib 做了 关上下拉 + rtc_gpio_isolate
         * ⇒ 引脚完全悬空。TM6604 的使能是电阻常开的(固件管不着), 输入悬空就停在
         * 中间电平, 驱动级长期半导通 —— 这是"深睡后仍有 mA 级电流"的一条确定来源。
         * 静止态正确做法: 输出低 + hold(与功放脚 pa_pin 同一套处理)。
         * ⚠ 必须在 audio_player_sleep_for_off() 之后(它内部已调 gpio_deep_sleep_hold_en),
         *   否则 hold 不覆盖整个深睡期。 */
        int vibpin = bp ? bp->vib : -1;
        if (!(s_stage_mask & 0x20)) {
            /* 诊断档: 震动脚完全是原状 —— 不钉低、也不进下面隔离循环(白名单里仍排除它)。
             * 这样"震动脚这一项到底耗不耗电"才有对比意义。 */
            ESP_LOGW(TAG, "[stage] 跳过震动脚钉低 (0x20 未置位)");
        } else if (vibpin >= 0) {
            gpio_set_direction((gpio_num_t)vibpin, GPIO_MODE_OUTPUT);
            gpio_set_level((gpio_num_t)vibpin, 0);
            gpio_hold_en((gpio_num_t)vibpin);
            ESP_LOGI(TAG, "震动脚(GPIO%d) 已钉低并 hold(禁浮空)", vibpin);
        }

        /* ★★★★ 2026-10-18 linit **严重回归修复**: 之前对**所有**引脚去上下拉+隔离,
         *   把 I2C(触摸/编解码)、屏、SD、触摸 INT/RST 这些**功能脚**也动了 ✗
         *   ⇒ 深睡唤醒后 I2C 探测失败 ⇒ 板型识别回落到默认(小智) ⇒ 触摸失灵 + 按键错位 ✓✓
         *   ⇒ 现在**只处理真正未使用的脚**: board_pins 里定义过的功能脚全部跳过 ✓ */
        int fn[40]; int fn_n = 0;
        #define KEEP_PIN(x) do { int _v=(int)(x); if (_v >= 0) fn[fn_n++ < 40 ? fn_n : 39] = _v; } while (0)
        if (bp) {
            KEEP_PIN(bp->lcd_dc);  KEEP_PIN(bp->lcd_cs);  KEEP_PIN(bp->lcd_sck); KEEP_PIN(bp->lcd_mosi);
            KEEP_PIN(bp->lcd_rst);
            KEEP_PIN(bp->tp_sda);  KEEP_PIN(bp->tp_scl);  KEEP_PIN(bp->tp_int);  KEEP_PIN(bp->tp_rst);
            KEEP_PIN(bp->codec_sda); KEEP_PIN(bp->codec_scl);
            KEEP_PIN(bp->sd_clk);  KEEP_PIN(bp->sd_cmd);  KEEP_PIN(bp->sd_d0);
            KEEP_PIN(bp->sd_cs);   KEEP_PIN(bp->sd_sck);  KEEP_PIN(bp->sd_mosi); KEEP_PIN(bp->sd_miso);
        }
        int niso = 0;
        /* ★ 2026-10-28: 0x40(引脚隔离) 未置位 ⇒ 循环体一次都不执行(整段跳过)。
         *   写成循环条件而不是外面再套一层 if, 是为了不把这 18 行整体缩进一级 ——
         *   纯缩进会让 diff 更难审, 而这条路径最怕"看着像改了其实没改"。 */
        for (int g = 0; (s_stage_mask & 0x40) && g <= 48; g++) {
            if (g == keep[0] || g == keep[1] || g == keep[2]) continue;  /* 唤醒键: 必须留上拉 */
            if (g == gpstx || g == papin || g == vibpin) continue;       /* 已 hold 固定电平 */
            int skip = 0;
            for (int j = 0; j < fn_n && j < 40; j++) if (fn[j] == g) { skip = 1; break; }
            if (skip) continue;                                          /* ★ 功能脚一律不动 */
            /* ★★★ 关键(来源: "10 Things Draining Your ESP32 Battery"):
             *   "Each enabled internal pull-up/pull-down sources current through a ~45kΩ
             *    resistor — that's 10..50 µA PER PIN, and they stay enabled through deep
             *    sleep unless explicitly disabled."
             *   ⇒ **先关内部上下拉**(全部引脚, 包括非 RTC 脚) ✗ 之前只做 isolate
             *     (rtc_gpio_isolate 只对 RTC 引脚 0~21 生效, 22~48 是空操作) ✓
             *   ⇒ RTC 引脚再额外 isolate(切断数字域到 pad 的通路) */
            gpio_pullup_dis((gpio_num_t)g);
            gpio_pulldown_dis((gpio_num_t)g);
            if (g <= 21) rtc_gpio_isolate((gpio_num_t)g);
            niso++;
        }
        if (s_stage_mask & 0x40)
            ESP_LOGI(TAG, "已关上下拉+隔离 %d 个引脚(唤醒键/GPS_TX/PA 除外)", niso);
        else
            ESP_LOGW(TAG, "[stage] 跳过未用引脚隔离 (0x40 未置位)");
    }

    /* ═══════════════════════════════════════════════════════════════════════
     * 睡前诊断汇总 (2026-10-28, 任务书第四节)
     *   目的: 每次关机把"每个外设到底关没关"一次列清, 便于和电流测量逐项对账。
     *   ⚠ 这是**软件状态**, 不是电流证据。任务书明确要求: 不许用日志推断实际电流。
     *   ⚠ 必须在 esp_deep_sleep_start() 之前打完 —— 深睡期间没有任何代码会执行。
     * ═══════════════════════════════════════════════════════════════════════ */
    {
        const board_pins_t *bpz = board_pins_get();
        int pa_z  = bpz ? bpz->pa_pin : -1;
        int vib_z = bpz ? bpz->vib   : -1;
        ESP_LOGW(TAG, "═══ 睡前诊断汇总 (stage mask=0x%02X) ═══", (unsigned)s_stage_mask);
        ESP_LOGW(TAG, " BT      : %s", bt_manager_is_enabled()   ? "!! 仍开" : "已关");
        ESP_LOGW(TAG, " WiFi    : %s", wifi_manager_is_enabled() ? "!! 仍开" : "已关");
        ESP_LOGW(TAG, " GPS     : ready=%d verified=%d",
                 (int)gps_is_ready(), (int)gps_sleep_verified());
        ESP_LOGW(TAG, " 功放PA  : %s (GPIO%d)", audio_pa_is_on() ? "!! 仍开" : "已关", pa_z);
        ESP_LOGW(TAG, " 音频    : %s", (s_stage_mask & 0x02) ? "ES8311/ES7210/I2S 已关 (early + 全局 hold)"
                                                              : "[stage] 跳过 (0x02 未置位)");
        /* ★ 2026-10-28: 这几行必须跟着掩码说真话 —— 分项测电流时,
         *   日志说"已关"而实际跳过了, 会直接把测量结论带偏。 */
        ESP_LOGW(TAG, " SD卡    : %s", (s_stage_mask & 0x04) ? "已卸载 + SDMMC host 已 deinit, CLK低/CMD高/D0高"
                                                              : "[stage] 跳过 (0x04 未置位)");
        ESP_LOGW(TAG, " 触摸    : %s", (s_stage_mask & 0x08) ? "RST 已拉低 + 已进复位态"
                                                              : "[stage] 跳过 (0x08 未置位)");
        ESP_LOGW(TAG, " 屏      : %s mode=%d",
                 !(s_stage_mask & 0x01) ? "[stage] 跳过(0x01 未置位)"
                 : (se == ESP_OK ? "sleep_in + deinit" : "!! sleep_in 失败(面板仍在扫描)"),
                 st7305_get_sleep_mode());
        ESP_LOGW(TAG, " 震动    : %s GPIO%d", (s_stage_mask & 0x20) ? "已钉低 + hold" : "[stage] 跳过(0x20 未置位)", vib_z);
        ESP_LOGW(TAG, " 引脚    : %s", (s_stage_mask & 0x40) ? "非功能脚已关上下拉 + rtc_gpio_isolate"
                                                              : "[stage] 跳过 (0x40 未置位)");
        ESP_LOGW(TAG, " 电源域  : RTC_PERIPH=ON, XTAL / VDDSDIO / MODEM = OFF");
        ESP_LOGW(TAG, " 唤醒源  : EXT1 mask=0x%llX (ANY_LOW, 仅 PWR)",
                 (unsigned long long)mask);
        ESP_LOGW(TAG, " ⚠ 以上均为软件状态; 实际电流必须外接表测, 不得据此推断");
        ESP_LOGW(TAG, "═══════════════════");
    }
    /* ★ 深睡前必须让 UART 把日志真正发出去:
     *   esp_deep_sleep_start() 立刻断电, 缓冲里没发完的日志直接丢失 ——
     *   实测就出现过"诊断汇总一行都没抓到"。10 行 ≈ 600B, 115200bps ≈ 52ms, 给 150ms 余量。 */
    vTaskDelay(pdMS_TO_TICKS(150));

    /* ★★★ 2026-10-29 (极限省电, 用户明确授权): 深睡前关闭 USB Serial/JTAG。
     *   用户要求: "睡眠时关闭 USB Serial/JTAG、关闭 8MB/16MB 等全部, 开机时启动"。
     *
     *   实际能做到的层次(按 IDF 公开 API 的边界):
     *     ① 驱动层: usb_serial_jtag_driver_uninstall() —— 卸 VFS / 中断 / 收发环形缓冲,
     *        这是公开 API 能关的最深一层。
     *     ② PHY 层: API `usb_serial_jtag_driver_uninstall()` **故意不关** usb_pad_enable
     *        (IDF 原注释: "USJ stdout 可能还依赖它"), 关它需要 include HAL 私有头
     *        (hal/usb_serial_jtag_ll.h), 而该头依赖 `__DECLARE_RCC_ATOMIC_ENV` 私有宏,
     *        直接 include 编译不过 ⇒ **不硬来**。
     *        但这不是问题: PHY 的 pad 使能位在**深睡复位时归零**, 所以深睡期间 pad 本来
     *        就不工作; 我们只需要保证驱动层不残留中断/任务在跑。
     *
     *   "开机时启动"不需要任何代码: 深睡 = 完整复位 ⇒ ROM 在启动最开始重新枚举 USB,
     *   IDF 在 core init 阶段重新注册 VFS, app_main 的 svc_log_init() 再接管日志钩子。
     *   唤醒源是 EXT1(PWR), 与 USB 无关, 关掉不影响唤醒。
     *   位置必须在 150ms 日志延时**之后** —— 先让最后一帧日志发出去, 再断串口。 */
    /* ★ 返回值说明(2026-10-29): 这行之后串口立刻断, 所以"检查返回值并打日志"
     *   在物理上不成立 —— 日志通道已经被自己关掉了。显式 (void) 表示"接住但不上报",
     *   依据: IDF 对"未安装就卸载"返回 ESP_OK(只是打一条 E), 其余失败(中断释放失败等)
     *   也不影响随后的深睡 —— 深睡是完整复位, USB 外设会被 ROM 从头重配。 */
    (void)usb_serial_jtag_driver_uninstall();

    esp_deep_sleep_start();
    /* ⛔ 理论不可达: esp_sleep.h:638 把 esp_deep_sleep_start() 标了 __attribute__((noreturn))。
     *   万一它真的返回(深睡配置被破坏 / 芯片差异), 此刻**外设已全部关断、唤醒键已 hold、
     *   屏已 deinit** —— 绝不能清 s_off_entered 之后接着跑: 那是带着关掉的硬件继续运行,
     *   而且之后所有关机请求都会被防重入拦死。按项目既有兜底(esp_restart)重新初始化。 */
    ESP_LOGE(TAG, "esp_deep_sleep_start() 异常返回 ⇒ 软重启恢复 (防重入标志保持 true)");
    esp_restart();
    /* 不返回 */
}

/* ---- 开机最早期门控 ----
 * ★ 2026-10-27 linit「自动关机和长按电源键效果一模一样」:
 *   原来两条路径行为不同 —— MANUAL 要求 PWR 按住 ≥0.5s(不足判误触立即回深睡),
 *   而 AUTO 直接放行(碰一下就开机)。现在**统一按 MANUAL 的规矩办**:
 *   不管哪种关机, 唤醒都必须按住 PWR ≥0.5s, 不足一律判误触 → 立即回深睡。
 *   非关机唤醒(上电/复位) 本来就不带 s_off_origin 标记, 仍然直接放行。 */
void power_mgr_wake_gate(void)
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) return;
    if ((s_off_origin & ORIGIN_MASK) != ORIGIN_MAGIC) return;      /* 非本模块记录 → 放行 */

    keys_config(PWR_MASK);
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    while ((uint32_t)(esp_timer_get_time() / 1000) - t0 < MANUAL_HOLD_MS) {
        if (gpio_get_level(PWR_KEY) != 0) {
            /* 松手了 / 按住不足 0.5s → 误触: 直接回深睡(保持手动语义) */
            wait_keys_released(PWR_MASK);
            s_off_origin = ORIGIN_MAGIC | (s_off_origin & 0xFFu);   /* 保留原来源(自动/手动), 只重写 magic */
            wake_src_set(PWR_MASK);
            /* 进深睡前的统一不变量: 功放必须关断并 hold 低电平
             * (这条路径在 app_board_init 之前, 但同样不能漏 —— 睡眠期间功放耗电是
             *  "关机了还在掉电" 的典型来源)。 */
            /* ★ 2026-10-09: GPS (MAX-M10S) 也必须显式送进"软件待机", 并把 ESP 侧 TX 钉住。
     *   为什么必须在这里做:
     *   ① 深睡期间**没有任何代码会执行** ⇒ 若此刻 GPS 还在连续跟踪, 它会整夜保持
     *      ~10mA(峰值 25mA); 而待机只要 46µA —— 差 200 倍。这是"关机还在掉电"的
     *      另一大来源 (此前只治了功放 GPIO46 那一路)。
     *   ② GPS 的唤醒源是 **UART RX**。深睡时 GPIO 变高阻, TX 浮空/拾噪会被 GPS 当成
     *      起始位把它唤醒 —— 醒后再没人让它睡回去。`gpio_hold_en` 把 TX 冻在 UART
     *      的空闲高电平上, 从根上不给它边沿。
     *   ⚠ 顺序: 先 gps_sleep() (要发 UBX, 需要 UART 还能工作) 再 hold。 */
    gps_sleep();
    /* ★ 2026-10-20: GPS 是选装件(默认关) ⇒ 没开启就一个脚都别碰,
     *   免得去 hold 一个可能已被别的东西占用的引脚。 */
    if (gps_is_ready()) {
        const gps_config_t *gc = gps_default_config();
        if (gc && gc->pin_tx >= 0) {
            gpio_hold_en((gpio_num_t)gc->pin_tx);   /* 冻住当前电平 (UART 空闲 = 高) */
            ESP_LOGI(TAG, "GPS 已送待机, TX(GPIO%d) 已 hold 在高电平", gc->pin_tx);
        }
    }

    /* ⚠ 必须放在 GPS 之后调用: 这行内部会 gpio_deep_sleep_hold_en() + hold 功放脚,
     *   把 pad 冻住。放在前面会把 UART TX 也冻上, 上面那条 UBX 就发不出去
     *   —— "GPS 送待机" 会静默失败, 深睡期间 GPS 继续 ~10mA。(2026-10-09 修顺序) */
    audio_player_sleep_for_off();
            wake_pins_hold(PWR_MASK);   /* 同样要锁住 PWR 上拉, 否则浮空秒醒 */
            if (s_pre_sleep_hook) s_pre_sleep_hook();   /* 误触回睡也要结账 (醒着段不能白算) */
            esp_deep_sleep_start();   /* 不返回 */
            /* 同上: noreturn, 真返回了就走软重启, 而不是带着已关的外设继续开机 */
            ESP_LOGE(TAG, "误触回睡: esp_deep_sleep_start() 异常返回 ⇒ 软重启恢复");
            esp_restart();
            /* 不返回 */
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* 按住满 0.5s → 放行开机; 等松手, 避免残余按键事件连发 */
    wait_keys_released(PWR_MASK);
}

bool power_mgr_woke_from_off(void)
{
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1;
}

int power_mgr_wake_origin(void)
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) return -1;
    if ((s_off_origin & ORIGIN_MASK) != ORIGIN_MAGIC) return -1;
    return (int)(s_off_origin & 0xFFu);
}

/* 复位/唤醒原因 → 人话 (参考 esp-mosaico 的 low_power_wakeup 做法:
 *   "开机能知道自己是怎么醒的" 是排查睡眠问题的第一手信息; 数字码要翻手册, 没人记) */
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:  return "上电/换电池";
        case ESP_RST_EXT:      return "外部复位脚";
        case ESP_RST_SW:       return "软件重启(esp_restart)";
        case ESP_RST_PANIC:    return "异常panic";
        case ESP_RST_INT_WDT:  return "中断看门狗";
        case ESP_RST_TASK_WDT: return "任务看门狗";
        case ESP_RST_WDT:      return "其它看门狗";
        case ESP_RST_DEEPSLEEP:return "深睡唤醒(正常)";
        case ESP_RST_BROWNOUT: return "掉电/电压跌落(BROWNOUT)";
        case ESP_RST_SDIO:     return "SDIO";
        case ESP_RST_USB:      return "USB 复位(串口/烧录工具)";
        case ESP_RST_JTAG:     return "JTAG";
        case ESP_RST_EFUSE:    return "efuse 错误";
        case ESP_RST_PWR_GLITCH: return "电源毛刺";
        case ESP_RST_CPU_LOCKUP: return "CPU 锁死(双异常)";
        default:               return "未知";
    }
}
static const char *wake_cause_str(esp_sleep_wakeup_cause_t w)
{
    switch (w) {
        case ESP_SLEEP_WAKEUP_UNDEFINED: return "非深睡(上电/复位)";
        case ESP_SLEEP_WAKEUP_EXT1:      return "EXT1(按键)";
        case ESP_SLEEP_WAKEUP_TIMER:     return "定时器";
        case ESP_SLEEP_WAKEUP_GPIO:      return "GPIO";
        case ESP_SLEEP_WAKEUP_UART:      return "UART";
        case ESP_SLEEP_WAKEUP_TOUCHPAD:  return "触摸板";
        default:                         return "其它";
    }
}

/* ★ 2026-10-20: 把"异常复位"记进 NVS(只记非 USB 复位 —— USB 复位是烧录/开串口造成的,
 *   会盖掉我们要查的那条)。收藏夹开了 USB HID 之后串口就没了, 崩了也看不到 panic,
 *   只能靠这条"事后取证": 重启完串口回来, 发 `rst` 就能看到上次是 PANIC 还是 WDT。 */
static void rst_hist_record(esp_reset_reason_t r)
{
    if (r == ESP_RST_USB || r == ESP_RST_POWERON) return;   /* 烧录/上电, 不算异常 */
    nvs_handle_t h;
    if (svc_nvs_open("pst", NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t hist[8] = {0};
    size_t n = sizeof(hist);
    svc_nvs_get_blob(h, "rst", hist, &n);
    uint8_t idx = hist[0];
    if (idx >= 8) idx = 0;
    hist[1 + idx] = (uint8_t)r;
    hist[0] = (uint8_t)((idx + 1) % 8);
    svc_nvs_set_blob(h, "rst", hist, sizeof(hist));
    svc_nvs_commit(h);
    svc_nvs_close(h);
}

void power_mgr_boot_diag(void)
{
    esp_reset_reason_t r        = esp_reset_reason();
    esp_sleep_wakeup_cause_t w  = esp_sleep_get_wakeup_cause();
    int o = power_mgr_wake_origin();
    rst_hist_record(r);
    /* ext1_status: 触发本次唤醒的键掩码(按 GPIO 号; 0 = 非 EXT1 唤醒)。 */
    uint64_t ws = (w == ESP_SLEEP_WAKEUP_EXT1) ? esp_sleep_get_ext1_wakeup_status() : 0;
    const char *which = (ws & PWR_MASK) ? "PWR"
                      : (ws & (1ULL << KEY_LEFT))  ? "左键"
                      : (ws & (1ULL << KEY_RIGHT)) ? "右键"
                      : (w == ESP_SLEEP_WAKEUP_EXT1) ? "(无键位, 疑似噪声误触!)" : "-";
    ESP_LOGI(TAG, "开机: %s (%d) | 唤醒=%s (%d) | 关机来源=%s | 唤醒键=%s status=0x%llX "
                  "(PWR%d=%d LEFT%d=%d RIGHT%d=%d)",
             reset_reason_str(r), (int)r,
             wake_cause_str(w), (int)w,
             (o == POWER_OFF_MANUAL) ? "手动" : (o == POWER_OFF_AUTO) ? "休眠到点" : "非关机唤醒",
             which, (unsigned long long)ws,
             (int)PWR_KEY,     (ws & PWR_MASK) ? 1 : 0,
             (int)KEY_LEFT,    (ws & (1ULL << KEY_LEFT)) ? 1 : 0,
             (int)KEY_RIGHT,   (ws & (1ULL << KEY_RIGHT)) ? 1 : 0);
}
