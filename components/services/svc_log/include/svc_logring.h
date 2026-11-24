#ifndef LIN_TOS_SVC_LOGRING_H
#define LIN_TOS_SVC_LOGRING_H

/* svc_logring — 崩溃前日志自留证（RTC 保留内存环形缓冲）
 *
 * 背景：svc_crash 只留得住 backtrace 地址表，而 panic 上下文里的 backtrace
 * 在 Xtensa 上经常跳帧、不可靠。真正能定位问题的是**崩溃前最后几条日志**
 * （"正在做 X" → "X 失败" → 崩）。但串口不常插、日志滚屏也会丢现场。
 *
 * 做法：接管 esp_log 的输出函数，把每一行同时抄一份进 RTC 保留内存的环形
 * 缓冲。RTC 内存在 panic / 看门狗 / 软复位后都不清零，只有真掉电才丢；
 * 真掉电的情况由 svc_crash 在开机时把快照转存 NVS 兜住。
 *
 * 双环设计（关键）：
 *   · ALL 环 —— 最近 24 条（不分级别），用来看崩溃前最后的执行流
 *   · WE  环 —— 最近 16 条 WARN/ERROR，**不会被刷屏的 INFO 冲掉**
 * 很多崩溃路径最后几秒是每帧一条 INFO，单环会被瞬间冲干净，双环才有用。
 *
 * ⚠ 本模块在日志路径上，必须极轻：固定栈缓冲 + vsnprintf，不 malloc。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 启动后调用一次（svc_log_init 之后）。只做 RTC 环初始化，**不再接管日志钩子**。 */
void svc_logring_init(void);

/* ★ 2026-11-22: 由 svc_log 在**已经格式化好**之后调用（一行一条，不含换行）。
 *
 * 为什么不让本模块自己挂 esp_log_set_vprintf：
 *   那样每行日志会被格式化**两次** —— 本模块一次(栈上 192B + vsnprintf)，
 *   再转给 svc_log 的钩子又一次(vsnprintf + vprintf)。日志在**任意任务**里都可能
 *   触发，多出来的这层栈开销在 12KB 栈的任务里是实打实的风险，且完全没必要：
 *   svc_log 里已经有格式化结果，直接抄一份即可（本函数只做 strncpy，不格式化）。
 * ⛔ 不要把 esp_log_set_vprintf 的接管加回来。 */
void svc_logring_push(const char *line);

/* 打印两个环的全部内容到当前日志输出。给 `crash` 命令用。 */
void svc_logring_dump(const char *why);

/* 把快照追加写进调用方缓冲（供 svc_crash 转存 NVS）。
 * 返回写入的字节数（不含结尾 NUL）。 */
size_t svc_logring_snapshot(char *out, size_t outlen);

/* 清空（自测用）。 */
void svc_logring_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* LIN_TOS_SVC_LOGRING_H */
