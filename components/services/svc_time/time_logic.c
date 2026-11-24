/**
 * time_logic.c — 时间可信度判定（纯 C，无 IDF 依赖）
 * 规则说明见 time_logic.h。
 */
#include "time_logic.h"

static int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

tl_boot_t tl_eval_boot(int64_t now, int64_t last_sync)
{
    /* ① 1970 附近：RTC 没走起来（刷机 / 断电 / 首次开机）*/
    if (now < TL_MIN_VALID) return TL_BOOT_INVALID;

    if (last_sync > 0 && last_sync >= TL_MIN_VALID) {
        /* ② 比"上次成功校准"还早 60 秒以上 ⇒ 时间倒退 ⇒ RTC 被重置过。
         *    60 秒是容差：同一次校准前后几十秒的抖动不算倒退。 */
        if (now + 60 < last_sync) return TL_BOOT_INVALID;

        /* ③ 比记录晚一年以上：可能是坏数据，也可能真的放了一年。
         *    不敢当"有效"，降级成 PROVISIONAL（联网后仍会校准）。*/
        if (now > last_sync + TL_OFFLINE_MAX_S) return TL_BOOT_PROVISIONAL;

        /* ④ 正常：时间只往前走了有限的一段 */
        return TL_BOOT_RTC_VALID;
    }

    /* ⑤ 没有校准记录：给个"像样"的评价，但不敢说是有效 */
    return TL_BOOT_PROVISIONAL;
}

tl_net_t tl_judge_net(int64_t got, int64_t projected, bool local_invalid)
{
    /* 服务器时间低于下限 ⇒ 坏数据，任何情况下都不采纳 */
    if (got < TL_MIN_VALID) return TL_NET_REJECT_LOW;

    /* 本地无效（1970 / 被重置）：无条件采纳。
     * 这里**不能**用"偏差过大"去拒绝 —— 1970 和 2026 差 56 年，那是恢复，不是异常。 */
    if (local_invalid) return TL_NET_ACCEPT_IMMEDIATE;

    int64_t off = got - projected;
    if (iabs64(off) > TL_OFFSET_MAX) return TL_NET_REJECT_FAR;

    return TL_NET_ACCEPT;
}

bool tl_need_full_sync(bool state_invalid, bool state_failed,
                       int64_t since_ok_s, int64_t since_rec_s)
{
    if (state_invalid) return true;    /* 本地无效：必须先校 */
    if (state_failed)  return true;    /* 上次失败：允许重试（调用方另有冷却）*/

    /* 本次开机已校过：按"距上次成功"算 6 小时 */
    if (since_ok_s >= 0) return since_ok_s >= TL_FULL_SYNC_S;

    /* 本次开机还没校过：按持久化记录算 */
    if (since_rec_s >= 0) return since_rec_s >= TL_FULL_SYNC_S;

    /* 既没校过也没记录（首次开机 / 擦过 flash）⇒ 校一次 */
    return true;
}
