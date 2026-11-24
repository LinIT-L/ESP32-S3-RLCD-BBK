#ifndef LIN_TOS_SVC_CRASH_H
#define LIN_TOS_SVC_CRASH_H

/* svc_crash — 崩溃现场自留证
 *
 * 目的: 用户报"突然重启"时, 不必依赖当时是否插着串口。panic 发生时把现场
 * （异常类型 / 出错地址 / 任务名 / backtrace 地址表）写进 **RTC 保留内存**,
 * 该区域在 panic 软复位、看门狗复位后都不会被清零；下一次开机读出来打印,
 * 并转存一份到 NVS（真掉电也能留）。
 *
 * ⚠ panic 上下文里只允许纯内存写: 不 malloc、不 printf、不碰 flash。
 *   所有格式化/打印都推迟到重启后的 svc_crash_init()。
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ⚠ 只要 svc_crash_rec_t 的**字段布局**有任何变化，这个版本号必须 +1。
 *   否则旧固件留在 RTC/NVS 里的 blob 会被按新布局解读 —— 字段全部错位，
 *   打印出来的"复位原因/任务名"都是别的字段的字节（本轮踩过：ver 没升，
 *   reason 显示成 "nstruction"、reset_reason 显示成一个时间戳）。
 *   读侧(init / get)一律校验 ver == SVC_CRASH_VER，不匹配就当作无记录。 */
#define SVC_CRASH_VER       3
#define SVC_CRASH_FRAMES    12
#define SVC_CRASH_MAGIC     0x4C435243u   /* 'LCRC' 新现场, 尚未打印 */
#define SVC_CRASH_MAGIC_DONE 0x4C435244u  /* 'LCRD' 已打印过, 内容仍可读 */

typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t nframes;
    uint32_t exception;      /* panic_exception_t */
    uint32_t core;
    uint32_t addr;           /* 触发异常的指令地址 */
    uint32_t uptime_ms;      /* 崩溃时已开机多久（区分"开机即崩"和"跑了几小时才崩"） */
    uint32_t reset_reason;   /* 本次开机看到的复位原因 esp_reset_reason_t */
    char     reason[32];
    char     desc[64];
    char     task[16];
    uint32_t frames[SVC_CRASH_FRAMES];
} svc_crash_rec_t;

/* 开机早期调用一次: 若上次有崩溃现场则打印 + 转存 NVS。无崩溃时静默。 */
void svc_crash_init(void);

/* 取上一次崩溃现场（不论是否已打印过）。返回 false = 没有记录。 */
bool svc_crash_get(svc_crash_rec_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LIN_TOS_SVC_CRASH_H */
