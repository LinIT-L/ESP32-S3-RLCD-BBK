/**
 * @file gps.h
 * @brief u-blox MAX-M10S GNSS 驱动 (UART + UBX 协议) —— **模块化 API**.
 *
 * 接线 (来源: 用户网表 Netlist_Schematic1_2026-09-29.enet, 器件 U5):
 *   GPS.TXD(2) → ESP U0RXD → **GPIO44**   (ESP 收)
 *   GPS.RXD(3) → ESP U0TXD → **GPIO43**   (ESP 发)
 *   GPS.EXTINT(5)          → **GPIO47**   (备用唤醒脚)
 *   GPS.VCC(8) / V_BCKP(6) / VCC_RF(14) → VCC3V3 (常通电)
 *   ⚠ GPS.V_IO(7) 网表**未接** —— V_IO 是模块的 I/O 电源域, 不接则 UART/EXTINT 全不工作,
 *     本驱动会正常发指令但模块收不到。硬件需把 pin7 接 3V3(VIO_SEL 已接 3V3 = 3.3V 逻辑域)。
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

#ifdef __cplusplus
}
#endif
#endif /* GPS_H */
