/* ============================================================================
 * power_stat.c — 电源统计 (见 power_stat.h 的说明)
 * ==========================================================================*/
#include <string.h>
#include <time.h>

#include "power_stat.h"
#include "esp_attr.h"
#include "esp_rtc_time.h"   /* esp_rtc_get_time_us(): RTC 计时, 深睡期间继续走 */
#include "esp_sleep.h"      /* esp_sleep_get_wakeup_cause(): 数"到底醒了几次" */
#include "esp_system.h"    /* esp_reset_reason(): 区分冷启动 / 深睡唤醒 */
#include "esp_log.h"
#include "usb_hid.h"        /* usb_hid_host_mounted(): USB 插入检测 (不做充电检测) */

#define TAG "PSTAT"

#define PS_MAGIC 0x50535432u   /* "PST2": 校验 RTC 内存是否为本模块写过 (掉电后是乱码) */

typedef struct {
    uint32_t magic;
    uint32_t awake_s;         /* 上次充电以来 醒着累计秒 */
    uint32_t sleep_s;         /* 上次充电以来 深睡累计秒 */
    uint64_t seg_start_us;    /* 当前"醒着段"起点 (RTC 计时); 0 = 当前不在醒着段 */
    uint64_t sleep_enter_us;  /* 进深睡那一刻 (RTC 计时); 0 = 不在睡眠中 */
    /* ★ 2026-10-20: 待机耗电排查 —— 数深睡唤醒次数。
     *   "放了 12 小时电池就空了" 只有两种可能: ① 一直在睡但睡眠电流大 ② 根本没在睡(被反复唤醒)。
     *   这两个数字直接把②排除或坐实: 正常应「唤醒次数 ≈ 1~2 次/天」。 */
    uint32_t wake_cnt;        /* 上次充电以来 EXT1 唤醒次数 */
    uint32_t sleep_min_s;     /* 最短一次深睡时长 (秒) */
    uint32_t sleep_max_s;     /* 最长一次深睡时长 (秒) */
    uint8_t  last_cause;      /* 上次唤醒原因 (esp_sleep_wakeup_cause_t) */
} ps_state_t;

static RTC_NOINIT_ATTR ps_state_t s_ps;

/* ============================================================================
 * ★ 2026-10-26 linit: 唤醒/入睡事件日志 (带绝对时刻)
 *   起因: 只有"醒着 3.73h / 深睡 8.83h / 唤醒 10 次"这种累计值,
 *         无法回答"是哪一次唤醒、几点醒的、每次醒了多久"。
 *   做法: RTC 内存里放 24 条环形日志 (24×12B + 12B 头 = 300B), 跨深睡保留;
 *         入睡时记一条(时长=这次醒着多久), 唤醒时记一条(时长=刚睡了多久)。
 *   读取: esptool dump_mem <s_log 地址>  /  串口 `pwr` 命令打印。
 * ==========================================================================*/
#define PS_LOG_MAGIC 0x50534C47u   /* "PSLG" */

/* 事件类型 */
#define PS_EV_SLEEP 1u             /* 入睡: t=入睡时刻, dur_s=这次醒着多长 */
#define PS_EV_WAKE  2u             /* 唤醒: t=唤醒时刻, dur_s=刚睡了多长, cause=唤醒源 */
#define PS_EV_BOOT  3u             /* 上电/掉过电: t=开机时刻, cause=复位原因 */

typedef struct {
    uint32_t magic;
    uint32_t head;                 /* 下一条写入下标 */
    uint32_t total;                /* 累计条数 (判断转过几圈) */
    power_stat_ev_t ev[PS_LOG_N];
} ps_log_t;

static RTC_NOINIT_ATTR ps_log_t s_log;

static void ps_log_ensure(void)
{
    if (s_log.magic != PS_LOG_MAGIC) {
        memset(&s_log, 0, sizeof(s_log));
        s_log.magic = PS_LOG_MAGIC;
    }
    /* ★★ 2026-10-27 修「刷完整片开机就 panic、无限重启」(实测 StoreProhibited @ ps_log_put):
     *   s_log 放在 RTC_NOINIT 内存里, **跨固件版本保留**(刷 flash 不会清 RTC RAM)。
     *   换版本后若本结构/PS_LOG_N 变过, 旧数据的 magic 可能仍然匹配, 但 head 是旧布局的
     *   错位值(或上电乱码) ⇒ &s_log.ev[head] 写到非法地址 ⇒ 开机就 panic。
     *   magic 只能证明"这块内存被本模块写过", 证明不了"布局和现在一致",
     *   所以 head 必须再校验一次范围 —— 纠正成 0 远好于崩溃。 */
    if (s_log.head >= (uint32_t)PS_LOG_N) {
        s_log.head  = 0;
        s_log.total = 0;
        memset(s_log.ev, 0, sizeof(s_log.ev));
    }
}

static void ps_log_put(uint8_t kind, uint32_t dur_s, uint8_t cause)
{
    ps_log_ensure();
    power_stat_ev_t *e = &s_log.ev[s_log.head];
    e->t     = (uint32_t)time(NULL);
    e->dur_s = dur_s;
    e->kind  = kind;
    e->cause = cause;
    e->rsv   = 0;
    s_log.head = (s_log.head + 1u) % PS_LOG_N;
    s_log.total++;
}

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
        /* ★ 日志: 记一条"上电/掉过电", 这样读日志时不会把冷启动误认成深睡唤醒 */
        ps_log_put((uint8_t)PS_EV_BOOT, 0, (uint8_t)esp_reset_reason());
        return;
    }
    /* 结算上一次深睡: 唤醒后 RTC 计时已走过睡眠那段 */
    uint32_t slept = 0;
    if (s_ps.sleep_enter_us != 0 && now > s_ps.sleep_enter_us) {
        slept = (uint32_t)((now - s_ps.sleep_enter_us) / 1000000ull);
        s_ps.sleep_s += slept;
        if (s_ps.sleep_min_s == 0 || slept < s_ps.sleep_min_s) s_ps.sleep_min_s = slept;
        if (slept > s_ps.sleep_max_s) s_ps.sleep_max_s = slept;
        s_ps.sleep_enter_us = 0;
    }
    /* ★ 唤醒计数: 只数 EXT1 (按键) 唤醒 —— 这是"待机被反复吵醒"的直接证据。 */
    s_ps.last_cause = (uint8_t)esp_sleep_get_wakeup_cause();
    if (s_ps.last_cause == ESP_SLEEP_WAKEUP_EXT1) s_ps.wake_cnt++;
    /* ★ 日志: 这次的唤醒时刻 + 刚睡了多久 + 唤醒源 */
    ps_log_put((uint8_t)PS_EV_WAKE, slept, s_ps.last_cause);
    s_ps.seg_start_us = now;           /* 开新的醒着段 */
    ESP_LOGD(TAG, "唤醒结算: 醒 %us 睡 %us 唤醒 %u 次",
             (unsigned)s_ps.awake_s, (unsigned)s_ps.sleep_s, (unsigned)s_ps.wake_cnt);
}

void power_stat_on_deep_sleep(void)
{
    uint64_t now = ps_now();
    if (s_ps.magic != PS_MAGIC) ps_clear(now);

    /* 结掉当前醒着段 */
    uint32_t awake = 0;
    if (s_ps.seg_start_us != 0 && now > s_ps.seg_start_us)
        awake = (uint32_t)((now - s_ps.seg_start_us) / 1000000ull);
    s_ps.awake_s += awake;
    s_ps.seg_start_us = 0;
    s_ps.sleep_enter_us = now;

    /* ★ 日志: 入睡时刻 + 这一次醒着持续了多久 (判断"醒了 22 分钟"的直接证据) */
    ps_log_put((uint8_t)PS_EV_SLEEP, awake, 0);

    ESP_LOGI(TAG, "进深睡: 醒 %us 睡 %us (本次醒着 %us)",
             (unsigned)s_ps.awake_s, (unsigned)s_ps.sleep_s, (unsigned)awake);
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

/* ★ 2026-10-26: 打印事件日志 (旧→新). 供串口 `pwr` 命令 / 诊断用. */
void power_stat_log_dump(void)
{
    if (s_log.magic != PS_LOG_MAGIC || s_log.total == 0) {
        ESP_LOGW("PWRLOG", "PWRLOG 无事件记录");
        return;
    }
    uint32_t n = (s_log.total < (uint32_t)PS_LOG_N) ? s_log.total : (uint32_t)PS_LOG_N;
    ESP_LOGW("PWRLOG", "PWRLOG 累计 %u 条, 打印最近 %u 条 (旧→新):",
             (unsigned)s_log.total, (unsigned)n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (s_log.head + (uint32_t)PS_LOG_N - n + i) % (uint32_t)PS_LOG_N;
        const power_stat_ev_t *e = &s_log.ev[idx];
        struct tm tm;
        time_t t = (time_t)e->t;
        localtime_r(&t, &tm);
        const char *kind = (e->kind == PS_EV_SLEEP) ? "入睡"
                         : (e->kind == PS_EV_WAKE)  ? "唤醒" : "上电";
        ESP_LOGW("PWRLOG", "  %02d-%02d %02d:%02d:%02d %s 时长=%us 原因=%u",
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                 kind, (unsigned)e->dur_s, (unsigned)e->cause);
    }
}

/* ★ 2026-10-18 linit: 串口诊断 —— 打印"上次充电以来"的醒着/睡眠累计时长。
 *   linit 报"500mAh 12 小时耗光", 需要直接证据判断设备到底睡了多久。 */
void power_stat_diag(void)
{
    power_stat_t st;
    power_stat_get(&st);
    ESP_LOGW("PWRSTAT", "PWRSTAT awake=%us (%.1fh)  sleep=%us (%.1fh)  睡眠占比=%.1f%%",
             (unsigned)st.awake_s, st.awake_s / 3600.0f,
             (unsigned)st.sleep_s, st.sleep_s / 3600.0f,
             (st.awake_s + st.sleep_s) ? (100.0f * st.sleep_s / (st.awake_s + st.sleep_s)) : 0.0f);
    /* 唤醒次数: 判断"待机耗电"是睡眠电流大, 还是被反复唤醒 ★ 2026-10-20 */
    if (s_ps.magic == PS_MAGIC) {
        uint32_t avg = s_ps.wake_cnt ? (s_ps.sleep_s / s_ps.wake_cnt) : 0;
        ESP_LOGW("PWRSTAT", "PWRSTAT 唤醒=%u 次 (EXT1)  平均每次睡 %u s  最短 %u s  最长 %u s  上次原因=%d",
                 (unsigned)s_ps.wake_cnt, (unsigned)avg,
                 (unsigned)s_ps.sleep_min_s, (unsigned)s_ps.sleep_max_s,
                 (int)s_ps.last_cause);
    }
    power_stat_log_dump();   /* ★ 2026-10-26: 事件日志 (几点醒的/睡了多久) */
}
