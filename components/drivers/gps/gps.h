/**
 * @file gps.h
 * @brief u-blox MAX-M10S GNSS 驱动 (UART + UBX 协议) —— **模块化 API**.
 *
 * 接线 (来源: 原理图网表 2026-10-24, 器件 U3):
 *   ① 小智一体版: GPS.TXD(2)→GPIO14 · GPS.RXD(3)→GPIO41 · EXTINT(5)→GPIO18
 *   ③ 标准扩展版: GPS.TXD(2)→U0RXD=GPIO44 · GPS.RXD(3)→U0TXD=GPIO43 · EXTINT(5)→GPIO47
 *   引脚一律取 `board_pins_get()`（三张表里 gps_tx/gps_rx/gps_extint），不在本文件写死。
 *   波特率: 模块 MAX-M10S-00B-01, 出厂 **38400**；驱动仍带自动波特率兜底。
 *   GPS.VCC(8) / VCC_RF(14) → VCC3V3 (常通电)；V_IO(7) 两版都已接 VCC3V3。
 *
 * 设计约定 (与其他驱动一致):
 *   - 句柄/引脚/波特率都在 gps_config_t 里, 不写死在调用方 → 换板只改一处;
 *   - 只暴露"电源状态 + 通用 UBX 收发"两层 API, 业务层(定位/授时)以后再叠;
 *   - 未初始化时所有 API 返回 ESP_ERR_INVALID_STATE, 不崩不静默.
 */
#ifndef GPS_H
#define GPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 电源状态 */
typedef enum {
    GPS_POWER_UNKNOWN = 0,   /* 未初始化 */
    GPS_POWER_ACTIVE,        /* 连续跟踪: I_VCC 6~8mA + I_V_IO 2.2mA ≈ 10mA, 峰值 25mA */
    GPS_POWER_SLEEP,         /* 软件待机: 46µA (数据手册 Table 14: V_IO=VCC=3.3V) */
} gps_power_state_t;

typedef struct {
    int      uart_num;       /* UARTx, 默认 UART_NUM_1 (console 走 USB, UART0 也可) */
    int      pin_tx;         /* ESP TX → GPS RXD, 默认 43 */
    int      pin_rx;         /* ESP RX ← GPS TXD, 默认 44 */
    int      pin_extint;     /* GPS EXTINT, 默认 47 (不用填 -1) */
    uint32_t baud;           /* 默认 9600 (backup 唤醒后会丢配置回到出厂默认, 见 gps.c) */
    bool     drive_extint;   /* true=把 EXTINT 当输出(可主动脉冲唤醒); false=输入+上拉(默认) */
} gps_config_t;

/* 默认配置 (GPIO43/44 + EXTINT 8, 9600) */
const gps_config_t *gps_default_config(void);

/* 初始化 UART/GPIO 并置 ACTIVE. cfg=NULL → 用默认配置. */
esp_err_t gps_init(const gps_config_t *cfg);

/* 是否已初始化 */
bool gps_is_ready(void);

/* 实际生效的波特率 (自动波特率识别后的值; 未初始化 = 配置值) */
uint32_t gps_baud(void);

/* 当前电源状态 */
gps_power_state_t gps_power_state(void);

/* 通用 UBX 发送 (自动加 B5 62 同步头 + 8-bit Fletcher 校验). 供将来叠业务用. */
esp_err_t gps_send_ubx(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len);

/* 通用读取 (原样字节流, 给 NMEA/UBX 解析器用). 返回读到的字节数, <0 出错. */
int gps_read(uint8_t *buf, size_t max_len, uint32_t timeout_ms);

/* 进"软件待机"(software standby, µA 级): UBX-RXM-PMREQ(backup+force).
 * MAX-M10S 的 MANUAL 2.7.3.2 明确要求 force 标志; 唤醒源=UART RX (无极性风险). */
esp_err_t gps_sleep(void);

/* 唤醒: 先给 UART RX 送一个无害的 UBX-MON-VER 轮询; 若配置了 drive_extint 再补一个脉冲
 * (覆盖 EXTINT 高/低两种有效极性). 返回 ESP_OK 时状态置 ACTIVE. */
esp_err_t gps_wakeup(void);

/* ★ 2026-10-24 应用退出用: gps_sleep() + **彻底释放** (卸 UART 驱动 + 还原引脚 + s_ready=0)。
 *   下次 gps_init()/app.gps.start() 会重装。幂等, 未初始化返回 ESP_OK。 */
esp_err_t gps_release(void);

/* ★ 2026-10-24 开机用: 只把模块送进待机(不 autobaud / 不起服务 / 不占后台)。
 *   静听 300ms 判定: 无输出=本来就在睡(不动它) / 有输出=送 PMREQ / 乱码=完整初始化兜底。
 *   结束后 UART 已卸载, 内部 RAM 归零。 */
esp_err_t gps_boot_park(void);

/* ★ 2026-10-27: 把 GPS 的 RXD 侧(TX脚)钉成输出高并 hold。
 *   软件待机的唤醒源是 UART RX, pad 浮空的噪声就能把它叫醒 ⇒ 睡前必须调用。*/
void gps_tx_hold_high(void);

/* ★ 2026-10-27: 是否"已确认入睡"(实测静默 1.2s)。管家任务据此决定要不要补一次，
 *   已确认就不再去装 UART(装 UART 会重配 TX pad, 电平跳变会把模块叫醒)。 */
bool gps_sleep_verified(void);

/* ★ 是否正在停靠/初始化(装 UART 期间)。兜底/管家看到它就别插手，
 *   否则两个任务同时发 PMREQ、同时删驱动 ⇒ 日志串味、甚至撞 spinlock。 */
bool gps_is_busy(void);

/* ★ 2026-10-27 接线诊断: 逐档波特率试听 + MON-VER 验回程, 只读不改配置。
 *   安全: s_ready/s_initing/解析服务占用时直接拒绝, 不会抢 UART 驱动。 */
void gps_diag(void);
void gps_probe_bauds(void);

#ifdef __cplusplus
}
#endif
#endif /* GPS_H */
