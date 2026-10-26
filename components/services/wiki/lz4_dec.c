/* lz4_dec.c — LZ4 block 解压 (无状态, 一次性)
 *
 * 帧格式 (LZ4 block spec):
 *   循环 { token:1B = 高4位 literal 长度 | 低4位 match 长度
 *          [literal 长度 =15 时 追加 255 累加直到 <255]
 *          literals
 *          若还有数据: offset:2B LE (1..65535), [match 长度同上, 最终 +4 (MINMATCH)]
 *        }
 *   最后一段只有 literals, 没有 offset —— 靠 "输入读完" 判断结束。
 *
 * 逐字节复制 match: 允许 offset=1 这种自重叠(游程), 必须按字节正序拷。
 */
#include "lz4_dec.h"

#define MINMATCH 4

int lz4_dec_block(const uint8_t *src, size_t src_len,
                  uint8_t *dst, size_t dst_cap, size_t *out_len)
{
    const uint8_t *ip = src;
    const uint8_t *const iend = src + src_len;
    uint8_t *op = dst;
    uint8_t *const oend = dst + dst_cap;

    while (ip < iend) {
        uint8_t token = *ip++;
        size_t lit = token >> 4;

        if (lit == 15) {
            uint8_t b;
            do {
                if (ip >= iend) return -1;
                b = *ip++;
                lit += b;
            } while (b == 255);
        }
        if (lit) {
            if ((size_t)(iend - ip) < lit) return -1;
            if ((size_t)(oend - op) < lit) return -2;
            for (size_t i = 0; i < lit; i++) op[i] = ip[i];
            ip += lit;
            op += lit;
        }

        if (ip >= iend) break;              /* 末段: 只有 literals */

        if ((size_t)(iend - ip) < 2) return -1;
        size_t offset = (size_t)ip[0] | ((size_t)ip[1] << 8);
        ip += 2;
        if (offset == 0 || offset > (size_t)(op - dst)) return -3;

        size_t mlen = token & 15;
        if (mlen == 15) {
            uint8_t b;
            do {
                if (ip >= iend) return -1;
                b = *ip++;
                mlen += b;
            } while (b == 255);
        }
        mlen += MINMATCH;
        if ((size_t)(oend - op) < mlen) return -2;

        const uint8_t *mp = op - offset;
        for (size_t i = 0; i < mlen; i++) op[i] = mp[i];
        op += mlen;
    }

    *out_len = (size_t)(op - dst);
    return 0;
}
