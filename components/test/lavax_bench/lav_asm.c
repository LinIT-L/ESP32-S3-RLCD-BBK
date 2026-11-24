/* lav_asm.c — LavaX 字节码构造器实现（测试专用） */
#include <string.h>
#include "lav_asm.h"

static void emit(lav_asm_t *a, uint8_t b)
{
    if (a->len >= a->cap) { a->overflow = 1; return; }
    a->buf[a->len++] = b;
}

void lav_asm_init(lav_asm_t *a, uint8_t *buf, size_t cap,
                  int rambits, int graph_mode, int width, int height)
{
    uint8_t flags = 0;
    a->buf = buf; a->cap = cap; a->len = 0; a->overflow = 0; a->ops = 0;
    a->rambits = rambits;

    if (rambits == 32)      flags |= LAV_FLAG_RAM32;
    else if (rambits == 24) flags |= LAV_FLAG_RAM24;

    if (graph_mode == 4)      flags |= LAV_FLAG_GMODE4;
    else if (graph_mode == 8) flags |= LAV_FLAG_GMODE8;

    memset(buf, 0, cap < 16 ? cap : 16);
    emit(a, 'L'); emit(a, 'A'); emit(a, 'V'); emit(a, 18);
    emit(a, 0); emit(a, 0); emit(a, 0); emit(a, 0);
    emit(a, flags);
    emit(a, (uint8_t)(width / 16));
    emit(a, (uint8_t)(height / 16));
    emit(a, 0); emit(a, 0); emit(a, 0); emit(a, 0); emit(a, 0);
}

void lav_asm_reset_code(lav_asm_t *a)
{
    if (a->len > 16) a->len = 16;
    a->overflow = 0;
}

void lav_bytes(lav_asm_t *a, const void *p, size_t n)
{
    const uint8_t *s = (const uint8_t *)p;
    while (n--) emit(a, *s++);
}

void lav_op(lav_asm_t *a, uint8_t op)          { a->ops++; emit(a, op); }
void lav_op_u8(lav_asm_t *a, uint8_t op, uint8_t v)   { a->ops++; emit(a, op); emit(a, v); }
void lav_op_u16(lav_asm_t *a, uint8_t op, uint16_t v){ a->ops++; emit(a, op); emit(a, v & 0xff); emit(a, (v >> 8) & 0xff); }
void lav_op_u32(lav_asm_t *a, uint8_t op, uint32_t v)
{
    a->ops++;
    emit(a, op);
    emit(a, v & 0xff); emit(a, (v >> 8) & 0xff);
    emit(a, (v >> 16) & 0xff); emit(a, (v >> 24) & 0xff);
}

/* 依 RamBits 发地址：16 位 2 字节，24/32 位 3 字节（get_vaddr 的读法） */
void lav_addr(lav_asm_t *a, uint32_t addr)
{
    emit(a, addr & 0xff);
    emit(a, (addr >> 8) & 0xff);
    if (a->rambits > 16) emit(a, (addr >> 16) & 0xff);
}

void lav_push_int(lav_asm_t *a, int v)
{
    uint16_t u = (uint16_t)v;
    lav_op_u16(a, LOP_PUSH_INT, u);
}

void lav_push_long(lav_asm_t *a, int32_t v)
{
    lav_op_u32(a, LOP_PUSH_LONG, (uint32_t)v);
}

void lav_push_byte(lav_asm_t *a, uint8_t v)
{
    lav_op_u8(a, LOP_PUSH_CHAR, v);
}

/* push_a* 走 get_gaddr()：先读立即地址，再 **从求值栈弹一个偏移量** 相加。
 * 所以必须先在栈上放一个 0，否则会触发 "address stack underflow"
 * （lavax.c 中 pop_address_offset 在 eval_top<4 时直接返回 0 并报错）。
 * 这里自动补那个 0，避免调用方踩这个坑。 */
void lav_push_addr_const(lav_asm_t *a, uint32_t addr, int width_bytes)
{
    uint8_t op = (width_bytes == 1) ? LOP_PUSH_ACHAR
               : (width_bytes == 2) ? LOP_PUSH_AINT
                                    : LOP_PUSH_ALONG;
    lav_push_int(a, 0);          /* 偏移 0 */
    lav_op(a, op);
    lav_addr(a, addr);
}

/* push_string: 先把 string_ptr 压栈，再把字节码里的字符串逐字节写进
 * lRam[string_ptr++]，遇到 0 结束（secret=0 时不异或）。 */
void lav_push_string(lav_asm_t *a, const char *s)
{
    size_t n = strlen(s) + 1;
    lav_op(a, LOP_PUSH_STRING);   /* 已 ++ops */
    lav_bytes(a, s, n);
}

size_t lav_jmp_slot(lav_asm_t *a, uint8_t op)
{
    size_t slot;
    lav_op(a, op);
    slot = a->len;
    emit(a, 0); emit(a, 0); emit(a, 0);   /* c_jmp 恒读 3 字节 */
    return slot;
}

void lav_patch(lav_asm_t *a, size_t slot, size_t target_abs)
{
    if (slot + 3 > a->cap) { a->overflow = 1; return; }
    a->buf[slot + 0] = (uint8_t)(target_abs & 0xff);
    a->buf[slot + 1] = (uint8_t)((target_abs >> 8) & 0xff);
    a->buf[slot + 2] = (uint8_t)((target_abs >> 16) & 0xff);
}

size_t lav_pos(const lav_asm_t *a) { return a->len; }
