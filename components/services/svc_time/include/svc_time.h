#ifndef LIN_TOS_SVC_TIME_H
#define LIN_TOS_SVC_TIME_H

// svc_time — 时间/校时统一服务
// 统一管理东八区 (北京时间) 时区、未联网时的默认时间兜底、以及联网后 SNTP 自动校时。
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 时区 + 默认时间兜底初始化 (开机时调用一次; 需在 setenv/tzset 后、任何读时间前) */
void svc_time_init(void);

/* 联网后触发 SNTP 校时 (★ 每次拿到 IP 都校一次, 异步, 成功后自动 settimeofday)
 * 首次调用初始化 SNTP, 之后每次调用 = 重启 SNTP 立刻重发查询; 10s 内重复调用忽略 */
void svc_time_ntp_sync(void);

/* 校时状态 (诊断用): 本次开机成功次数 / 上次成功时刻(epoch, 0=还没成功) */
void svc_time_ntp_status(uint32_t *count, long *last_epoch);

#ifdef __cplusplus
}
#endif

#endif /* LIN_TOS_SVC_TIME_H */
