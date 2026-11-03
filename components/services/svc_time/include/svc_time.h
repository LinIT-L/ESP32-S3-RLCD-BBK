/**
 * svc_time.h — 系统时间 / 校时统一服务
 *
 * 职责（全系统唯一）:
 *   ① 时区设置（UTC-8，北京时间显示）；
 *   ② 判断"当前系统时间可不可信"（刷机/断电/首次开机 都归到这一层）；
 *   ③ 联网后按策略用 SNTP 校准，并把"上次成功校准"持久化到 NVS；
 *   ④ 给诊断/其它模块查询时间状态。
 *
 * ⛔ 其它模块**不要**自己建 SNTP、不要自己 settimeofday —— 一律调本服务。
 *
 * 设计要点（2026-11-01 重做）:
 *   · 系统内部一律用 **UTC**；显示层靠 TZ 转北京时间。settimeofday 写的是 UTC。
 *   · **不再伪造默认时间**。原来"时间 < 2025 就设成 2026-09-01"会让"时间未知"
 *     看起来像"时间正确"，刷机后无从区分。现在明确标记 INVALID。
 *   · 本地时间无效时，网络时间**无条件采纳**（1970 → 2026 不是"巨大偏差"，是恢复）。
 *   · 本地时间可信时，网络时间与本地相差 > 30 分钟**先拒绝并回滚**，
 *     避免单个 NTP 源异常把设备时间带跑。
 */
#ifndef LIN_TOS_SVC_TIME_H
#define LIN_TOS_SVC_TIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 时间可信度状态（诊断/UI 用）---- */
typedef enum {
    TIME_INVALID = 0,      /* 本地时间无效：1970 附近 / 比上次校准记录还早（刷机、断电、首次开机）*/
    TIME_PROVISIONAL,      /* RTC 有个像样的值，但没有可信参照能证明它对 */
    TIME_RTC_VALID,        /* 与持久化记录连贯（时间只往前走），可当离线时间用 */
    TIME_NETWORK_VALID,    /* 已通过网络校准 */
    TIME_SYNCING,          /* 正在校准 */
    TIME_SYNC_FAILED,      /* 本次校准失败，保留原时间 */
} time_state_t;

/* 时区 + 启动时的时间可信度判定（main.c 启动时调用一次，必须早于任何读时间） */
void svc_time_init(void);

/* ★ 网络事件钩子：每次拿到 IP 调用（wifi_manager 的 IP_EVENT_STA_GOT_IP）。
 *   内部按策略决定"要不要真校"：
 *     · 本地时间无效       → 立即完整校准
 *     · 距上次成功 ≥ 6 小时 → 完整校准
 *     · 其它               → 跳过（打 TIME_SYNC_SKIPPED_COOLDOWN）
 *   10 秒内重复调用、以及正在校准时重复调用都会被忽略。 */
void svc_time_ntp_sync(void);

/* 校时状态（兼容旧接口）: 本次开机成功次数 / 上次成功的 epoch(0=还没成功) */
void svc_time_ntp_status(uint32_t *count, long *last_epoch);

/* 当前状态 + 名字（"INVALID" / "RTC_VALID" / ...） */
time_state_t svc_time_state(void);
const char  *svc_time_state_name(void);

/* 时间是否可直接用于显示/证书校验（NETWORK_VALID 或 RTC_VALID） */
bool svc_time_is_valid(void);

/* 一行诊断（remcon `timed` 用），返回 buf；内容: 状态/本地时间/上次校准/间隔/计数 */
const char *svc_time_diag(char *buf, int n);

#ifdef __cplusplus
}
#endif

#endif /* LIN_TOS_SVC_TIME_H */
