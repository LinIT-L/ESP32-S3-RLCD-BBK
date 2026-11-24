#pragma once

/* ============================================================================
 * espnow_proto — ESP-NOW 应用层帧格式 (LinTOS)
 *
 * 来源与依据 (为什么这么设计):
 *   ① 帧字段 (type / seq / crc / magic / payload) 直接对齐 ESP-IDF 官方示例
 *      examples/wifi/espnow/main/espnow_example.h 的 `example_espnow_data_t` ——
 *      该示例声明为 Public Domain / CC0, 可自由使用; 校验方式也照抄官方:
 *      `esp_crc16_le(UINT16_MAX, buf, len)`, 计算前把 crc 字段清零。
 *   ② 帧类型语义取 espressif/esp-now (Apache-2.0) v2.5.3 里
 *      `ESPNOW_DATA_TYPE_*` 的**子集**(DATA/ACK/BIND/CONTROL), 字段与命名自研,
 *      不搬运其源码 —— 该组件整体引入会带 protocomm/protobuf-c/console/fatfs
 *      和 `-Wl,--wrap=esp_log*` 全局日志劫持, 与 LinTOS 冲突(见交付报告)。
 *
 * 为什么必须有这一层 (而不是直接把应用字节丢给 esp_now_send):
 *   · 收包要能区分「控制帧(发现/配对)」与「应用数据」, 否则 Lua 应用会把
 *     别人的发现广播当成自己的协议数据;
 *   · 要能校验完整性 (CRC) 与乱序 (seq), 无线环境必须做;
 *   · 要能承载可靠性 (ACK), 广播没有底层 ACK, 单播有但只在驱动层。
 *
 * 帧布局 (大端序字段以本机内存序为准, 不跨架构共享):
 *   ┌──────────┬──────┬─────┬──────┬──────┬──────┬──────────┐
 *   │ magic 4B │type1B│ver1B│seq 2B│crc 2B│len 2B│ payload  │
 *   └──────────┴──────┴─────┴──────┴──────┴──────┴──────────┘
 *   头固定 12 字节; payload ≤ 238 (ESP-NOW 单包上限 250)。
 * ==========================================================================*/
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_crc.h"
#include "espnow_link.h"

#define ESPNOW_PROTO_MAGIC   0x4C4E4F57u   /* 'L''N''O''W' */
#define ESPNOW_PROTO_VER     1
/* ESPNOW_NAME_LEN 定义在 espnow_link.h (设备名是 link/peer 共用常量) */

/* 帧类型: 只保留 LinTOS 真正要用的子集, 新增类型往后加, 不复用旧号 */
typedef enum {
    ESPNOW_FT_DATA      = 0,   /* 应用数据 (唯一会交给 Lua 的类型) */
    ESPNOW_FT_ACK       = 1,   /* 预留: 应用层 ACK, payload[0..1]=被确认的 seq (当前未发送) */
    ESPNOW_FT_BIND_REQ  = 2,   /* 配对请求 payload={flags, lmk[16]?} */
    ESPNOW_FT_BIND_RSP  = 3,   /* 配对/解绑应答 payload={结果码} */
    ESPNOW_FT_DISCOVER  = 4,   /* 发现请求: 广播, payload={flags} */
    ESPNOW_FT_ANNOUNCE  = 5,   /* 发现应答: 单播, payload={name_len, name[], flags} */
    ESPNOW_FT_MAX       = 6,
} espnow_frame_type_t;

/* BIND_REQ / BIND_RSP 的 flags */
#define ESPNOW_BIND_F_ENC   0x01   /* 请求启用 ESP-NOW 原生加密 (LMK) */
#define ESPNOW_BIND_F_UNBIND 0x02  /* 解除配对 */

typedef struct {
    uint32_t magic;
    uint8_t  type;
    uint8_t  ver;
    uint16_t seq;
    uint16_t crc;
    uint16_t len;
} __attribute__((packed)) espnow_hdr_t;

#define ESPNOW_HDR_LEN   ((int)sizeof(espnow_hdr_t))
#define ESPNOW_APP_MAX   (ESPNOW_MAX_PAYLOAD - ESPNOW_HDR_LEN)   /* 238 */

/* 解析结果: 指向原始缓冲, 不拷贝 (调用方保证缓冲在生命周期内有效) */
typedef struct {
    uint8_t  type;
    uint8_t  ver;
    uint16_t seq;
    const uint8_t *payload;
    int      len;
} espnow_frame_t;

/* 组包: 写入 out (容量 cap), 返回总长度; <0 = 参数/容量错。
 * seq 由调用方给; payload 可为 NULL 只要 len==0。 */
int espnow_proto_pack(uint8_t *out, int cap, uint8_t type, uint16_t seq,
                      const void *payload, int len);

/* 解包 + 校验 (magic/ver/len/CRC 全部通过才返回 true)。
 * 校验时把 crc 字段当 0 参与计算 —— 与官方示例一致。 */
bool espnow_proto_parse(const uint8_t *buf, int len, espnow_frame_t *out);

/* 每类帧独立的发送序号 (单调递增, 回绕到 0 后继续, 与官方示例一致) */
uint16_t espnow_proto_next_seq(uint8_t type);
