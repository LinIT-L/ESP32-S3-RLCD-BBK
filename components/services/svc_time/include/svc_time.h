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

/* 联网后触发 SNTP 校时 (每天首次联网自动校, 异步, 成功后回落系统时间) */
void svc_time_ntp_sync(void);

#ifdef __cplusplus
}
#endif

#endif /* LIN_TOS_SVC_TIME_H */
