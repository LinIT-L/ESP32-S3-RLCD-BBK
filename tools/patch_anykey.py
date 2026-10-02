import io
P = "/Users/linit/AI/LinTOS0.7/components/wallpapers/sleep.c"
s = open(P, encoding="utf-8").read()

old1 = """/* 唤醒脚 = 仅开机键 PWR(GPIO1), 与主界面手动软关机(do_soft_shutdown)完全一致.
 * 修复: 屏保睡眠若用 GPIO0/18 一起做 EXT1 唤醒源, 深睡时它们常态为低电平,
 * 入睡瞬间 EXT1 立即触发 → "睡眠中"一闪即重启. 故只留 GPIO1(PWR). */
#define S_WAKE_MASK ((1ULL << GPIO_NUM_1))

/* 软关机 = 深睡(复位式) = 睡眠. 深睡掉 RAM/时间, 等同"真关机".
 * 唤醒源 = 仅开机键 PWR(GPIO1)(ANY_LOW), 按下即全新启动进桌面. */
static void ss_soft_shutdown(void) {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    /* 睡眠=软关机(深睡复位式): 仅开机键(GPIO1)可唤醒, 与手动软关机一致. */
    esp_sleep_enable_ext1_wakeup_io(S_WAKE_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();   /* 不会返回; 唤醒=上电复位(全新启动) */
}"""
new1 = """/* 唤醒脚 = 三个物理键: PWR(GPIO1) + KEY 左(GPIO18) + BOOT 右(GPIO0).
 * V1.9 用户要求"睡眠时任意按键可唤醒". 历史教训: GPIO0/18 若不做 RTC 上拉,
 * 深睡时普通 GPIO 域掉电、上拉失效 → 引脚浮空判低 → EXT1 ANY_LOW 入睡即醒.
 * 故入睡前必须给三键都补 RTC 域上拉(深睡保持高电平, 只有真实按下变低才触发). */
#define S_WAKE_MASK ((1ULL << GPIO_NUM_1) | (1ULL << GPIO_NUM_18) | (1ULL << GPIO_NUM_0))

/* 入睡前给唤醒键补 RTC 上拉: 深睡时普通 GPIO 上拉失效, 只有 RTC 域上拉才保持
 * 高电平, 否则引脚浮空被 EXT1(ANY_LOW)误判为"按下"→ 入睡即醒重启.
 * 只用 pullup_en, 不切 INPUT_ONLY: GPIO0 切 RTC 输入会断开数字读路径(返回键失灵). */
static void ss_wake_pullup_all(void) {
    rtc_gpio_pullup_en(GPIO_NUM_1);
    rtc_gpio_pullup_en(GPIO_NUM_18);
    rtc_gpio_pullup_en(GPIO_NUM_0);
}

/* 软关机 = 深睡(复位式) = 睡眠. 深睡掉 RAM/时间, 等同"真关机".
 * 唤醒源 = 任意物理键(PWR/KEY/BOOT, EXT1 ANY_LOW), 按下即全新启动进桌面. */
static void ss_soft_shutdown(void) {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    ss_wake_pullup_all();   /* 三键 RTC 上拉, 防浮空误触发 */
    /* 睡眠=软关机(深睡复位式): 任意按键(GPIO1/18/0)可唤醒. */
    esp_sleep_enable_ext1_wakeup_io(S_WAKE_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();   /* 不会返回; 唤醒=上电复位(全新启动) */
}"""
assert old1 in s, "block1 not found"
s = s.replace(old1, new1)

old2 = """/* [电源收拢] 开机键唤醒引脚初始化 — 唯一唤醒源(PWR/Gpio1, EXT1 ANY_LOW), 从 input.c 收拢至此.
 * 必须让 PWR 在 RTC 域上拉: gpio_config 的上拉属普通 IO 域, 睡眠时若未在 RTC 域使能上拉,
 * 引脚趋悬空, EXT1 电平触发(ANY_LOW)会把悬空/毛刺判成低电平 → 没人按也报 EXT1 唤醒.
 * 顺序: init -> 方向 -> 上拉 (init 会清掉上拉配置, 故上拉必须最后).
 * GPIO0(BTN_RIGHT) 不做 rtc INPUT_ONLY 重配 — 会断开其数字读路径, 返回键读不到; 保持普通上拉.
 * 唤醒掩码用 S_WAKE_MASK(仅PWR), 与 ss_soft_shutdown 完全一致; 由 input_init 在开机时调用一次. */
void wp_power_pin_init(void) {
    rtc_gpio_init(GPIO_NUM_1);
    rtc_gpio_set_direction(GPIO_NUM_1, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(GPIO_NUM_1);
    esp_sleep_enable_ext1_wakeup_io(S_WAKE_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
}"""
new2 = """/* [电源收拢] 唤醒键引脚初始化 — 三键(PWR/KEY/BOOT, EXT1 ANY_LOW)唤醒, 从 input.c 收拢至此.
 * 必须让唤醒键在 RTC 域上拉: gpio_config 的上拉属普通 IO 域, 睡眠时若未在 RTC 域使能上拉,
 * 引脚趋悬空, EXT1 电平触发(ANY_LOW)会把悬空/毛刺判成低电平 → 没人按也报 EXT1 唤醒.
 * GPIO0(BTN_RIGHT) 不做 rtc INPUT_ONLY 重配 — 会断开其数字读路径, 返回键读不到; 仅补上拉.
 * 唤醒掩码用 S_WAKE_MASK(三键), 与 ss_soft_shutdown 完全一致; 由 input_init 在开机时调用一次. */
void wp_power_pin_init(void) {
    rtc_gpio_init(GPIO_NUM_1);
    rtc_gpio_set_direction(GPIO_NUM_1, RTC_GPIO_MODE_INPUT_ONLY);
    ss_wake_pullup_all();   /* 三键 RTC 上拉 (GPIO0/18 仅上拉, 保数字读) */
    esp_sleep_enable_ext1_wakeup_io(S_WAKE_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
}"""
assert old2 in s, "block2 not found"
s = s.replace(old2, new2)

old3 = """bool wp_power_handle_boot(void) {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_TIMER) { ss_soft_shutdown(); }
    if (cause == ESP_SLEEP_WAKEUP_EXT1) {
        uint32_t origin = wp_sleep_read_origin(WP_SLEEP_ORIGIN_WALL);
        wp_sleep_clear_origin();
        uint32_t t0 = esp_timer_get_time() / 1000;
        while ((esp_timer_get_time() / 1000 - t0) < 500) {
            if (gpio_get_level(GPIO_NUM_1) != 0) { ss_soft_shutdown(); }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return (origin == WP_SLEEP_ORIGIN_MANUAL) ? false : true;
    }
    return false;
}"""
new3 = """bool wp_power_handle_boot(void) {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_TIMER) { ss_soft_shutdown(); }
    if (cause == ESP_SLEEP_WAKEUP_EXT1) {
        uint64_t wmask = esp_sleep_get_ext1_wakeup_status();
        uint32_t origin = wp_sleep_read_origin(WP_SLEEP_ORIGIN_WALL);
        wp_sleep_clear_origin();
        /* V1.9: 任意键唤醒. 唤醒键若仍被按住, 等其松开(最长500ms)再进桌面,
         * 避免用户一直按着键进入桌面后按键连发/与开机动画冲突. 短按即唤醒. */
        uint32_t t0 = esp_timer_get_time() / 1000;
        while ((esp_timer_get_time() / 1000 - t0) < 500) {
            bool any_down = false;
            if (wmask & (1ULL << GPIO_NUM_1))  any_down |= (gpio_get_level(GPIO_NUM_1) == 0);
            if (wmask & (1ULL << GPIO_NUM_18)) any_down |= (gpio_get_level(GPIO_NUM_18) == 0);
            if (wmask & (1ULL << GPIO_NUM_0))  any_down |= (gpio_get_level(GPIO_NUM_0) == 0);
            if (!any_down) break;   /* 唤醒键已松开 → 正常进桌面 */
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return (origin == WP_SLEEP_ORIGIN_MANUAL) ? false : true;
    }
    return false;
}"""
assert old3 in s, "block3 not found"
s = s.replace(old3, new3)

open(P, "w", encoding="utf-8").write(s)
print("PATCH OK")
