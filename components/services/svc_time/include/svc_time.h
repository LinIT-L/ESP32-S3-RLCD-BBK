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
    TIME_GPS_VALID,        /* ★ 已通过 GPS 授时 —— 全系统最高优先级 */
} time_state_t;

/* ★★★ 2026-11-21 linit 定规【时间源优先级】★★★
 *   GPS 授时 > 网络(SNTP) > 外挂 RTC(DS3231) > 本地(内部 RC)
 *   规则:
 *     · 只有 GPS / 网络**开着**时才去取它们（不为了校时主动开 GPS 或点 WiFi）；
 *     · 其它情况一律只读 RTC —— 不联网、不开 GPS 也能有时间；
 *     · 低优先级**不得覆盖**高优先级：GPS 在跑时网络/ RTC 只比对不写入；
 *       GPS 停掉超过 GPS_FRESH_S 秒后，网络与 RTC 才重新有资格接管。 */
#define TIME_GPS_FRESH_S  300     /* GPS 授时结果的有效期(秒)。超时可被网络/RTC 接管 */

/* 时区 + 启动时的时间可信度判定（main.c 启动时调用一次，必须早于任何读时间） */
void svc_time_init(void);

/* ★★★ 2026-11-21 linit 定规【固件时间默认 = 固件更新时间】★★★
 *   main 在 svc_time_init() **之前** 用 os_build_epoch() 注入构建时刻（UTC epoch，0=未知）。
 *   用途:
 *     ① 本地时间无效时，用它作为系统默认时间（状态 PROVISIONAL，联网/GPS 照常覆盖）；
 *     ② 作为"RTC 落后于镜像 ⇒ 芯片未校准"的判据基准（svc_rtc 调 svc_time_fw_epoch()）。
 *   ⛔ 放在 services 层是为了不反向依赖 system/os（分层守卫会拦）。 */
void    svc_time_set_fw_epoch(int64_t epoch);
int64_t svc_time_fw_epoch(void);

/* ★ 网络事件钩子：每次拿到 IP 调用（wifi_manager 的 IP_EVENT_STA_GOT_IP）。
 *   内部按策略决定"要不要真校"：
 *     · 本地时间无效       → 立即完整校准
 *     · 距上次成功 ≥ 6 小时 → 完整校准
 *     · 其它               → 跳过（打 TIME_SYNC_SKIPPED_COOLDOWN）
 *   10 秒内重复调用、以及正在校准时重复调用都会被忽略。 */
void svc_time_ntp_sync(void);

/* 校时状态（兼容旧接口）: 本次开机成功次数 / 上次成功的 epoch(0=还没成功) */
void svc_time_ntp_status(uint32_t *count, long *last_epoch);

/* ★ GPS 授时（最高优先级）。字段为 UTC 年月日时分秒，来自 NMEA RMC/GGA。
 *   调用方: main 主循环每秒取一次 gps_nmea_get() 快照(只读, 不主动开 GPS)。
 *   保护: 年月日时分秒逐个校验; 早于 2024 年拒收; 与当前时间差 ≤1s 只更新时效。
 *   返回 true = 本次采纳(GPS 状态已置位)。 */
bool svc_time_set_from_gps_fields(int year, int mon, int day, int hour, int min, int sec);
bool svc_time_set_from_gps(int64_t epoch);

/* GPS 授时是否仍在有效期内(默认 300s)。为假时网络/RTC 才能接管。 */
bool svc_time_gps_fresh(void);

/* ★ 外挂 RTC（DS3231MZ+，svc_rtc 调用）：把芯片时间注入系统。
 *   本函数仍是全系统唯一 settimeofday 的入口（见文件头 ⛔ 约定）。
 *   保护：低于下限拒收；与当前系统时间相差 ≤60s 视为已同步，不抖动；
 *        系统时间已可信时，芯片比系统早 >600s ⇒ 判芯片掉电/旧值, 拒绝(防时间被拉回)。 */
void svc_time_set_from_rtc(int64_t epoch);

/* ★ 2026-11-24 手动设时（设置→时间 的滚轮保存走这里）。
 *   收口原来 page_settings 直接 settimeofday 的旁路: 设时间 + 置状态 + 落 NVS 一条路做完,
 *   保证"系统时间 / 时间状态 / RTC 回写"三者一致(调用方随后 svc_rtc_save_now 即可)。
 *   返回 false = 值低于下限, 未采纳。 */
bool svc_time_set_manual(int64_t epoch);

/* 当前状态 + 名字（"INVALID" / "RTC_VALID" / ...） */
time_state_t svc_time_state(void);
const char  *svc_time_state_name(void);

/* 时间是否可直接用于显示/证书校验（GPS_VALID / NETWORK_VALID / RTC_VALID） */
bool svc_time_is_valid(void);

/* 一行诊断（remcon `timed` 用），返回 buf；内容: 状态/本地时间/上次校准/间隔/计数 */
const char *svc_time_diag(char *buf, int n);

#ifdef __cplusplus
}
#endif

#endif /* LIN_TOS_SVC_TIME_H */
