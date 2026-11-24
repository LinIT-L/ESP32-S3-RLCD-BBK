/**
 * time_logic.h — 时间可信度判定的**纯函数**（不依赖 IDF，可在主机上跑测试）
 *
 * 把 svc_time.c 里三处"拍脑袋级别"的判断抽出来，这样刷机识别、6 小时节流、
 * 大偏差拒绝这些规则都能用测试证明，而不是只能烧板子看日志。
 *
 * ⛔ 这里只有算术和时间戳比较，不读写 NVS、不碰网络、不调用 time()。
 */
#ifndef LIN_TOS_TIME_LOGIC_H
#define LIN_TOS_TIME_LOGIC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 阈值（改这里，svc_time.c 也引用同一份）---- */
#define TL_MIN_VALID      1735689600LL   /* 2025-01-01 00:00:00 UTC */
#define TL_FULL_SYNC_S    (6 * 3600)     /* 完整校准间隔 */
#define TL_OFFSET_IGNORE 2               /* ≤2s 视为误差，不动时间 */
#define TL_OFFSET_MAX    1800            /* >30min 且本地可信 ⇒ 拒绝 */
#define TL_OFFLINE_MAX_S  (365LL * 86400)/* 与上次校准相差 1 年以上 ⇒ 可疑 */

/* ---- 启动时本地时间属于哪一类 ---- */
typedef enum {
    TL_BOOT_INVALID = 0,      /* 1970 附近 / 比上次校准记录还早 ⇒ 时间被重置 */
    TL_BOOT_PROVISIONAL,      /* 有个像样的值，但没有参照 */
    TL_BOOT_RTC_VALID,        /* 与上次校准记录连贯 */
} tl_boot_t;

/**
 * @param now        当前 time(NULL)
 * @param last_sync  持久化的"上次成功网络校准"（0 = 没有记录）
 * @return           TL_BOOT_*
 *
 * 判定顺序（顺序很重要）:
 *   ① now < 2025-01-01            → INVALID（刷机/断电后 RTC 归零）
 *   ② 有记录 && now + 60 < 记录   → INVALID（时间倒退了 ⇒ RTC 被重置）
 *   ③ 有记录 && now 比记录晚一年+ → PROVISIONAL（太离谱，不敢当有效）
 *   ④ 有记录 && 其余              → RTC_VALID
 *   ⑤ 无记录                      → PROVISIONAL
 */
tl_boot_t tl_eval_boot(int64_t now, int64_t last_sync);

/* ---- 网络时间该怎么处理 ---- */
typedef enum {
    TL_NET_ACCEPT_IMMEDIATE = 0,  /* 本地无效 ⇒ 直接采纳（1970→2026 是恢复，不是偏差）*/
    TL_NET_ACCEPT,                /* 偏差在可接受范围 */
    TL_NET_REJECT_LOW,            /* 服务器时间低于下限（明显是坏数据）*/
    TL_NET_REJECT_FAR,            /* 本地可信但偏差 > 30min ⇒ 拒绝并回滚 */
} tl_net_t;

/**
 * @param got            网络返回的时间（epoch）
 * @param projected      本地"本该是几点" = 同步前本地时间 + 已过去秒数
 * @param local_invalid  同步前本地时间是否无效
 */
tl_net_t tl_judge_net(int64_t got, int64_t projected, bool local_invalid);

/* ---- 这次联网要不要做完整校准 ---- */
/**
 * @param state_invalid   当前状态是 INVALID
 * @param state_failed    当前状态是 SYNC_FAILED（等重试）
 * @param since_ok_s      距上次**本次开机**成功校准的秒数（没校过传 -1）
 * @param since_rec_s     距**持久化记录**里上次校准的秒数（没有记录传 -1）
 * @return                true = 需要完整校准
 */
bool tl_need_full_sync(bool state_invalid, bool state_failed,
                       int64_t since_ok_s, int64_t since_rec_s);

#ifdef __cplusplus
}
#endif

#endif /* LIN_TOS_TIME_LOGIC_H */
