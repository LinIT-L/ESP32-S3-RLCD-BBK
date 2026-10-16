/* ============================================================================
 * power_stat.c — 电源统计 (见 power_stat.h 的说明)
 * ==========================================================================*/
#include <string.h>

#include "power_stat.h"
#include "esp_attr.h"
#include "esp_rtc_time.h"   /* esp_rtc_get_time_us(): RTC 计时, 深睡期间继续走 */
#include "esp_log.h"
#include "usb_hid.h"        /* usb_hid_host_mounted(): USB 插入检测 (不做充电检测) */

#define TAG "PSTAT"

#define PS_MAGIC 0x50535431u   /* "PST1": 校验 RTC 内存是否为本模块写过 (掉电后是乱码) */

typedef struct {
    uint32_t magic;
    uint32_t awake_s;         /* 上次充电以来 醒着累计秒 */
    uint32_t sleep_s;         /* 上次充电以来 深睡累计秒 */
    uint64_t seg_start_us;    /* 当前"醒着段"起点 (RTC 计时); 0 = 当前不在醒着段 */
    uint64_t sleep_enter_us;  /* 进深睡那一刻 (RTC 计时); 0 = 不在睡眠中 */
} ps_state_t;

static RTC_NOINIT_ATTR ps_state_t s_ps;

static inline uint64_t ps_now(void) { return esp_rtc_get_time_us(); }

static void ps_clear(uint64_t now)
{
    memset(&s_ps, 0, sizeof(s_ps));
    s_ps.magic = PS_MAGIC;
    s_ps.seg_start_us = now;
}

void power_stat_init(void)
{
    uint64_t now = ps_now();
    if (s_ps.magic != PS_MAGIC) {
        ps_clear(now);                 /* 首次上电 / 掉过电 → 从头开始 */
        ESP_LOGD(TAG, "初始化 (RTC 内存无有效状态)");
        return;
    }
    /* 结算上一次深睡: 唤醒后 RTC 计时已走过睡眠那段 */
    if (s_ps.sleep_enter_us != 0 && now > s_ps.sleep_enter_us) {
        s_ps.sleep_s += (uint32_t)((now - s_ps.sleep_enter_us) / 1000000ull);
        s_ps.sleep_enter_us = 0;
    }
    s_ps.seg_start_us = now;           /* 开新的醒着段 */
    ESP_LOGD(TAG, "唤醒结算: 醒 %us 睡 %us", (unsigned)s_ps.awake_s, (unsigned)s_ps.sleep_s);
}

void power_stat_on_deep_sleep(void)
{
    uint64_t now = ps_now();
    if (s_ps.magic != PS_MAGIC) ps_clear(now);

    /* 结掉当前醒着段 */
    if (s_ps.seg_start_us != 0 && now > s_ps.seg_start_us)
        s_ps.awake_s += (uint32_t)((now - s_ps.seg_start_us) / 1000000ull);
    s_ps.seg_start_us = 0;
    s_ps.sleep_enter_us = now;

    ESP_LOGI(TAG, "进深睡: 醒 %us 睡 %us",
             (unsigned)s_ps.awake_s, (unsigned)s_ps.sleep_s);
}

void power_stat_tick(void)
{
    static bool prev_usb = false;
    bool usb = usb_hid_host_mounted();
    if (usb && !prev_usb) {
        /* USB 插入 = 开始充电 → 三个统计从这一刻重新计时 (linit: 没有充电检测, 按 USB 插入算) */
        ps_clear(ps_now());
        ESP_LOGI(TAG, "USB 插入: 电源统计清零");
    }
    prev_usb = usb;
}

void power_stat_get(power_stat_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    if (s_ps.magic == PS_MAGIC) {
        uint64_t now = ps_now();
        uint32_t live = 0;
        if (s_ps.seg_start_us != 0 && now > s_ps.seg_start_us)
            live = (uint32_t)((now - s_ps.seg_start_us) / 1000000ull);
        out->awake_s = s_ps.awake_s + live;
        out->sleep_s = s_ps.sleep_s;
    }
    out->since_usb_s = out->awake_s + out->sleep_s;
    out->usb_now = usb_hid_host_mounted();
}
