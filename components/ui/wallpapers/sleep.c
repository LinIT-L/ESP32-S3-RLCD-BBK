/* ============================================================================
 * 电源模块 — 关机 = ESP-IDF 官方 deep sleep
 *
 * 参考(ESP-IDF v5.5):
 *   examples/system/deep_sleep/main/deep_sleep_example_main.c  (关机流程 + RTC 内存存数据)
 *   examples/system/deep_sleep/main/ext_wakeup.c               (EXT1 唤醒源注册)
 *   docs: api-reference/system/sleep_modes.html
 *
 * 本芯片(ESP32-S3)的深睡 GPIO 唤醒只有 EXT1:
 *   S3 无 SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP → 不能用 esp_deep_sleep_enable_gpio_wakeup(),
 *   只能 esp_sleep_enable_ext1_wakeup_io()。
 *
 * 两条关机路径, 唤醒条件不同(唤醒掩码在关机时按来源设置):
 *   MANUAL 手动(长按关机键 → 弹窗确认): 只认 PWR 键, 且必须**保持按住 ≥0.5s** 才开机。
 *          EXT1 是电平触发(按下即唤醒), 0.5s 保持只能由软件层判定:
 *          唤醒后 wp_power_wake_gate() 检查 PWR 是否仍被按住满 0.5s, 不足 → 立即回深睡。
 *   AUTO   自动(休眠时间到点)        : 认任意键(PWR/KEY/BOOT), 短按即开机, 无需保持。
 *
 * 关机来源用 RTC 内存保存(RTC_NOINIT_ATTR, 掉电才清; 官方示例同样用 RTC 内存跨睡眠存数据)。
 * ==========================================================================*/
#include <stdbool.h>
#include "board_pins.h"   /* ★按键引脚唯一真相源 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
/* ★ 深睡: 卸掉 TF 卡(待机可到 mA 级)。sd_scan 的头文件没在本组件 include 路径里 ⇒ 前置声明 */
void sd_unmount(void);
   /* rtc_gpio_pullup_en: RTC 域上拉(深睡期间数字域掉电, 数字上拉会失效) */
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "audio_player.h"   /* audio_pa_force_off(): 进深睡前关功放并 hold 低电平 */
#include "gps.h"            /* gps_sleep(): 进深睡前把 GPS 送进 backup 待机 (2026-10-09) */

#include "sleep.h"
#include "power_stat.h"     /* power_stat_on_deep_sleep(): 睡前结账 (电池信息页) */
#include "st7305.h"
#include "bt_manager.h"
#include "wifi_manager.h"

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

void wp_power_pin_init(void)
{
    keys_config(ANY_KEY_MASK);
    wake_src_set(PWR_MASK);   /* 默认保守: 只有 PWR 能唤醒 */
}

void wp_power_init(st7305_handle_t *lcd)
{
    s_lcd = lcd;
    keys_config(ANY_KEY_MASK);
    wake_src_set(PWR_MASK);
}

/* ---- 关机(不返回) ----
 * 顺序: 停射频 → 清屏(polling 同步刷出) → 记录来源 → 等键松开 → 设唤醒掩码 → 深睡。
 * 深睡瞬间禁用 flash/PSRAM 缓存, 屏保监督任务(PSRAM 栈刷屏)由调用方先挂起。 */
void wp_power_off(int origin)
{
    if (bt_manager_is_enabled())   bt_manager_disable();
    if (wifi_manager_is_enabled()) wifi_manager_disable();

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
    gps_sleep();
    {
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

    if (s_lcd) {
        st7305_clear(s_lcd, ST7305_COLOR_WHITE);
        st7305_flush_override(s_lcd);
    }

    const uint64_t mask = (origin == WP_OFF_AUTO) ? ANY_KEY_MASK : PWR_MASK;
    s_off_origin = ORIGIN_MAGIC | (uint32_t)((unsigned)origin & 0xFFu);

    /* 诊断: 关机前的三键电平(1=松开 0=按下)。某键恒 0 = 浮空/被外部拉低 → 会被 EXT1 误唤醒。 */
    ESP_LOGI(TAG, "关机前键电平: PWR(%d)=%d LEFT(%d)=%d RIGHT(%d)=%d",
             (int)PWR_KEY, gpio_get_level(PWR_KEY),
             (int)KEY_LEFT, gpio_get_level(KEY_LEFT),
             (int)KEY_RIGHT, gpio_get_level(KEY_RIGHT));

    wait_keys_released(mask);
    wake_src_set(mask);
    wake_pins_hold(mask);   /* ★ 锁住唤醒键的上拉配置, 否则深睡数字域掉电→浮空→秒醒 */

    ESP_LOGI(TAG, "关机: 进入深睡 (来源=%s, 唤醒=%s, 掩码=0x%llX)",
             (origin == WP_OFF_AUTO) ? "休眠到点" : "手动",
             (origin == WP_OFF_AUTO) ? "任意键" : "PWR 键按住 0.5s",
             (unsigned long long)mask);
    power_stat_on_deep_sleep();   /* 结账"醒着段", 记下入睡时刻 (电池信息页用) */

    /* ★★★ 2026-10-18 linit: **深睡省电加深** —— 把 IDF 里能关的电源域全关掉。
     *   这些是纯软件开关, 不占内存, 实测能省几十 µA 到 ~1mA:
     *   · RTC_SLOW_MEM / RTC_FAST_MEM: 深睡不用 RTC 内存(没开 ULP) ⇒ 断电 ✓
     *   · XTAL: 深睡用 RC32K 就够, 主晶振断电 ✓
     *   · VDDSDIO: flash/PSRAM 供电域断电(数据已在 flash 里, 醒来重新初始化) ✓
     *   · RTC_PERIPH 必须 ON —— EXT1 唤醒依赖 RTC 域里的上拉, 关了就叫不醒 ✗ */
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH,   ESP_PD_OPTION_ON);    /* 唤醒必需 */
    /* ⚠ ESP32-S3 的 IDF 里没有 RTC_SLOW_MEM / RTC_FAST_MEM 这两个域(仅 C6/H2 有) */
    esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL,         ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO,      ESP_PD_OPTION_OFF);

    /* ★ 屏幕进低功耗(SPI 停 + 面板待机): 原来只清屏没 deinit ⇒ 面板还在扫描,
     *   实测 ST7305 正常工作 ~50-100µA, 待机 ~1-5µA ✓ (清屏已在上面做过) */
    if (s_lcd) {
        st7305_deinit(s_lcd);
        ESP_LOGI(TAG, "屏已 deinit(面板待机)");
    }

    ESP_LOGI(TAG, "深睡电源域: RTC_PERIPH=ON(唤醒用), XTAL/VDDSDIO=OFF");

    /* ★★★ 2026-10-18 linit: **TF 卡必须卸载** —— 否则卡在深睡期间仍保持"传输态",
     *   待机电流可达**几 mA**(来源: "SD 卡的待机电流可能很大(几十毫安!), 进入深睡前
     *   一定要正确卸载并切断电源")。卸载后卡进入 idle, 耗电降到 µA 级 ✓
     *   (深睡唤醒=完整重启 ⇒ 会重新 mount ✓) */
    sd_unmount();

    /* ★★★ 2026-10-18 linit: **隔离所有不用的引脚** —— 官方文档明确推荐:
     *   "Some ESP32-S3 IOs have internal pullups/pulldowns, which are enabled by default.
     *    如果外部电路驱动这些脚, 电流会经过上下拉电阻 ⇒ 增加深睡电流。
     *    调 rtc_gpio_isolate() 可避免。"
     *   ⇒ 除了【唤醒键(必须有上拉)】【GPS TX(已 hold 高)】【功放脚(已 hold 低)】,
     *     其余全部隔离 ✓ 这一步通常是深睡电流从 mA 级压到 µA 级的关键。 */
    {
        const int keep[3] = { PWR_KEY, KEY_LEFT, KEY_RIGHT };
        const gps_config_t *gc2 = gps_default_config();
        int gpstx = (gc2 && gc2->pin_tx >= 0) ? gc2->pin_tx : -1;
        int papin = board_pins_get()->pa_pin;
        int niso = 0;
        for (int g = 0; g <= 48; g++) {
            if (g == keep[0] || g == keep[1] || g == keep[2]) continue;  /* 唤醒键: 必须留上拉 */
            if (g == gpstx || g == papin) continue;                      /* 已 hold 固定电平 */
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
        ESP_LOGI(TAG, "已关上下拉+隔离 %d 个引脚(唤醒键/GPS_TX/PA 除外)", niso);
    }

    esp_deep_sleep_start();
    /* 不返回 */
}

/* ---- 开机最早期门控 ----
 * MANUAL 关机唤醒: 要求 PWR 键保持按住 ≥0.5s, 不足(碰一下) → 判为误触, 立即回深睡。
 * AUTO 关机唤醒 / 非关机唤醒(上电等): 直接放行。 */
void wp_power_wake_gate(void)
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) return;
    if ((s_off_origin & ORIGIN_MASK) != ORIGIN_MAGIC) return;      /* 非本模块记录 → 放行 */
    if ((s_off_origin & 0xFFu) == (uint32_t)WP_OFF_AUTO) return;   /* 自动关机: 任意键直接开机 */

    keys_config(PWR_MASK);
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    while ((uint32_t)(esp_timer_get_time() / 1000) - t0 < MANUAL_HOLD_MS) {
        if (gpio_get_level(PWR_KEY) != 0) {
            /* 松手了 / 按住不足 0.5s → 误触: 直接回深睡(保持手动语义) */
            wait_keys_released(PWR_MASK);
            s_off_origin = ORIGIN_MAGIC | (uint32_t)WP_OFF_MANUAL;
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
    {
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
            power_stat_on_deep_sleep();   /* 误触回睡也要结账 (醒着段不能白算) */
            esp_deep_sleep_start();   /* 不返回 */
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* 按住满 0.5s → 放行开机; 等松手, 避免残余按键事件连发 */
    wait_keys_released(PWR_MASK);
}

bool wp_power_woke_from_off(void)
{
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1;
}

int wp_power_wake_origin(void)
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) return -1;
    if ((s_off_origin & ORIGIN_MASK) != ORIGIN_MAGIC) return -1;
    return (int)(s_off_origin & 0xFFu);
}

void wp_sleep_boot_diag(void)
{
    esp_reset_reason_t r        = esp_reset_reason();
    esp_sleep_wakeup_cause_t w  = esp_sleep_get_wakeup_cause();
    int o = wp_power_wake_origin();
    /* ext1_status: 触发本次唤醒的键掩码(按 GPIO 号; 0 = 非 EXT1 唤醒)。 */
    uint64_t ws = (w == ESP_SLEEP_WAKEUP_EXT1) ? esp_sleep_get_ext1_wakeup_status() : 0;
    ESP_LOGI(TAG, "开机: reset_reason=%d wakeup_cause=%d 关机来源=%s ext1_status=0x%llX "
                  "(PWR%d=%d LEFT%d=%d RIGHT%d=%d)",
             (int)r, (int)w,
             (o == WP_OFF_MANUAL) ? "手动" : (o == WP_OFF_AUTO) ? "休眠到点" : "非关机唤醒",
             (unsigned long long)ws,
             (int)PWR_KEY,     (ws & PWR_MASK) ? 1 : 0,
             (int)KEY_LEFT,    (ws & (1ULL << KEY_LEFT)) ? 1 : 0,
             (int)KEY_RIGHT,   (ws & (1ULL << KEY_RIGHT)) ? 1 : 0);
}
