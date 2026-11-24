/* ============================================================================
 * espnow_proto.c — 帧组包/解包 (格式与来源见 include/espnow_proto.h)
 *
 * 实现要点:
 *   · CRC 分段计算 (头前 8B → 2B 零 → 头后 2B + payload), 不为了清零 crc 字段
 *     而拷贝整帧 —— 收包回调跑在 WiFi 任务上, 不给它 250B 栈临时变量。
 *   · 序号每类帧独立, 与 IDF 官方示例 s_example_espnow_seq[type]++ 一致。
 * ==========================================================================*/
#include <stddef.h>
#include "espnow_proto.h"

#define HDR_CRC_OFF   offsetof(espnow_hdr_t, crc)          /* 8 */
#define HDR_TAIL_OFF  (HDR_CRC_OFF + sizeof(uint16_t))      /* 10 */

int espnow_proto_pack(uint8_t *out, int cap, uint8_t type, uint16_t seq,
                      const void *payload, int len)
{
    if (!out || len < 0 || len > ESPNOW_APP_MAX) return -1;
    if (len > 0 && !payload) return -1;

    const int total = ESPNOW_HDR_LEN + len;
    if (cap < total) return -1;

    espnow_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = ESPNOW_PROTO_MAGIC;
    h.type  = type;
    h.ver   = ESPNOW_PROTO_VER;
    h.seq   = seq;
    h.crc   = 0;
    h.len   = (uint16_t)len;
    memcpy(out, &h, ESPNOW_HDR_LEN);
    if (len > 0) memcpy(out + ESPNOW_HDR_LEN, payload, (size_t)len);

    /* 与官方示例一致: crc 字段按 0 参与计算, 算完再填回 */
    static const uint8_t k_zero[sizeof(uint16_t)] = { 0, 0 };
    uint16_t crc = esp_crc16_le(UINT16_MAX, out, HDR_CRC_OFF);
    crc = esp_crc16_le(crc, k_zero, sizeof(k_zero));
    crc = esp_crc16_le(crc, out + HDR_TAIL_OFF, (uint32_t)(total - HDR_TAIL_OFF));
    memcpy(out + HDR_CRC_OFF, &crc, sizeof(crc));

    return total;
}

bool espnow_proto_parse(const uint8_t *buf, int len, espnow_frame_t *out)
{
    if (!buf || !out || len < ESPNOW_HDR_LEN || len > ESPNOW_MAX_PAYLOAD) return false;

    espnow_hdr_t h;
    memcpy(&h, buf, ESPNOW_HDR_LEN);
    if (h.magic != ESPNOW_PROTO_MAGIC) return false;      /* 不是本协议的包 */
    if (h.ver != ESPNOW_PROTO_VER)     return false;      /* 版本不兼容 */
    if (h.type >= ESPNOW_FT_MAX)       return false;      /* 未知类型, 直接丢弃 */
    if (h.len > ESPNOW_APP_MAX)        return false;
    if (ESPNOW_HDR_LEN + (int)h.len != len) return false; /* 长度自洽 */

    static const uint8_t k_zero[sizeof(uint16_t)] = { 0, 0 };
    uint16_t crc = esp_crc16_le(UINT16_MAX, buf, HDR_CRC_OFF);
    crc = esp_crc16_le(crc, k_zero, sizeof(k_zero));
    crc = esp_crc16_le(crc, buf + HDR_TAIL_OFF, (uint32_t)(len - HDR_TAIL_OFF));
    if (crc != h.crc) return false;

    out->type    = h.type;
    out->ver     = h.ver;
    out->seq     = h.seq;
    out->payload = buf + ESPNOW_HDR_LEN;
    out->len     = (int)h.len;
    return true;
}

uint16_t espnow_proto_next_seq(uint8_t type)
{
    static uint16_t s_seq[ESPNOW_FT_MAX];
    if (type >= ESPNOW_FT_MAX) return 0;
    return s_seq[type]++;
}
