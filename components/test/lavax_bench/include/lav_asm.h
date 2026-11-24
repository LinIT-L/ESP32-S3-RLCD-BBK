/* lav_asm.h — LavaX 字节码构造器（测试专用，不参与正式固件）
 *
 * 编码依据**全部来自源码**，不是猜的：
 *   - opcode 编号 = components/engines/lavax/core/source/lava.c 的 codes[]/codes2[] 下标
 *   - 基本指令 opcode 0..0x7F 直接用下标；系统调用 = 0x80 | codes2 下标
 *   - 操作数宽度：push_char/push_int/push_long 固定 1/2/4 字节；
 *     push_vaddr 系列与 set_sp/add_bp 按 RamBits 取 2 或 3 字节；
 *     c_jmp / c_jmpe / c_jmpn 的目标地址**恒为 3 字节小端**（见 c_jmp 实现）。
 *   - 文件头 16 字节：
 *       [0..3] 'L','A','V',18
 *       [8]    flag: bit4→RamBits=32, bit7→24, 否则 16;
 *                    bit5-6→图形模式(0x40=4色,0x60=8色,否则1色); bit0→pen
 *       [9]    宽 / 16      [10] 高 / 16
 *     字节码从偏移 16 开始（lPC = pLAVA + 16）。
 *
 * 该文件只被 tools/lavax_bench 使用，正式固件不编译。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* ---- 基本指令（codes[] 下标，0..116） ---- */
#define LOP_SN_ERR        0
#define LOP_PUSH_CHAR     1
#define LOP_PUSH_INT      2
#define LOP_PUSH_LONG     3
#define LOP_PUSH_VCHAR    4
#define LOP_PUSH_VINT     5
#define LOP_PUSH_VLONG    6
#define LOP_PUSH_GCHAR    7
#define LOP_PUSH_GINT     8
#define LOP_PUSH_GLONG    9
#define LOP_PUSH_ACHAR   10
#define LOP_PUSH_AINT    11
#define LOP_PUSH_ALONG   12
#define LOP_PUSH_STRING  13
#define LOP_PUSH_LVCHAR  14
#define LOP_PUSH_LVINT   15
#define LOP_PUSH_LVLONG  16
#define LOP_PUSH_LGCHAR  17
#define LOP_PUSH_LGINT   18
#define LOP_PUSH_LGLONG  19
#define LOP_PUSH_LACHAR  20
#define LOP_PUSH_LAINT   21
#define LOP_PUSH_LALONG  22
#define LOP_PUSH_LLONG   25
#define LOP_PUSH_TEXT    26
#define LOP_PUSH_GRAPH   27
#define LOP_PUSH_SUB0    28
#define LOP_CAL_INC      29
#define LOP_CAL_DEC      30
#define LOP_CAL_INC2     31
#define LOP_CAL_DEC2     32
#define LOP_CAL_ADD      33
#define LOP_CAL_SUB      34
#define LOP_CAL_AND      35
#define LOP_CAL_OR       36
#define LOP_PUSH_NOT     37
#define LOP_CAL_XOR      38
#define LOP_CAL_LAND     39
#define LOP_CAL_LOR      40
#define LOP_CAL_LNOT     41
#define LOP_CAL_MUL      42
#define LOP_CAL_DIV      43
#define LOP_CAL_MOD      44
#define LOP_CAL_LSHIFT   45
#define LOP_CAL_RSHIFT   46
#define LOP_CAL_EQU      47
#define LOP_CAL_NEQ      48
#define LOP_CAL_LE       49
#define LOP_CAL_GE       50
#define LOP_CAL_GREAT    51
#define LOP_CAL_LESS     52
#define LOP_C_LET        53
#define LOP_C_PTR        54
#define LOP_C_CPTR       55
#define LOP_POP_VAL      56
#define LOP_C_JMPE       57
#define LOP_C_JMPN       58
#define LOP_C_JMP        59
#define LOP_SET_SP       60
#define LOP_C_CALL       61
#define LOP_ADD_BP       62
#define LOP_SUB_BP       63
#define LOP_GOOD_EXIT    64
#define LOP_PRESET       65
#define LOP_PUSH_GBUF    66

/* ---- 系统调用（codes2[] 下标，实际 opcode = 0x80 | idx） ---- */
#define LSC(idx)          (0x80 | (idx))
#define LSC_PUTCHAR       0
#define LSC_GETCHAR       1
#define LSC_PRINTF        2
#define LSC_STRCPY        3
#define LSC_STRLEN        4
#define LSC_SETSCREEN     5
#define LSC_UPDATELCD     6
#define LSC_DELAY         7
#define LSC_WRITEBLOCK    8
#define LSC_SCROLL        9
#define LSC_TEXTOUT      10
#define LSC_BLOCK        11
#define LSC_RECTANGLE    12
#define LSC_EXIT         13
#define LSC_CLEARSCREEN  14
#define LSC_ABS          15
#define LSC_RAND         16
#define LSC_SRAND        17
#define LSC_LOCATE       18
#define LSC_INKEY        19
#define LSC_POINT        20
#define LSC_GETPOINT     21
#define LSC_LINE         22
#define LSC_BOX          23
#define LSC_CIRCLE       24
#define LSC_ELLIPSE      25
#define LSC_BEEP         26
#define LSC_STRCAT       38
#define LSC_STRCHR       39
#define LSC_STRCMP       40
#define LSC_STRSTR       41
#define LSC_TOLOWER      42
#define LSC_TOUPPER      43
#define LSC_MEMSET       44
#define LSC_MEMCPY       45
#define LSC_FOPEN        46
#define LSC_FCLOSE       47
#define LSC_FREAD        48
#define LSC_FWRITE       49
#define LSC_FSEEK        50
#define LSC_FTELL        51
#define LSC_FEOF         52
#define LSC_REWIND       53
#define LSC_GETC         54
#define LSC_PUTC         55
#define LSC_SPRINTF      56
#define LSC_GETMS        59
#define LSC_CHECKKEY     60
#define LSC_MEMMOVE      61
#define LSC_SETGRAPHMODE 75
#define LSC_SETBGCOLOR   76
#define LSC_SETFGCOLOR   77

/* ---- 图形模式位（文件头 [8]） ---- */
#define LAV_FLAG_RAM32   0x10
#define LAV_FLAG_RAM24   0x80
#define LAV_FLAG_GMODE4  0x40
#define LAV_FLAG_GMODE8  0x60
#define LAV_FLAG_PEN     0x01

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    int      rambits;     /* 16 / 24 / 32 */
    int      overflow;
    unsigned ops;         /* 已发射的指令条数（不含字符串数据字节） */
} lav_asm_t;

void   lav_asm_init(lav_asm_t *a, uint8_t *buf, size_t cap,
                    int rambits, int graph_mode, int width, int height);
void   lav_asm_reset_code(lav_asm_t *a);          /* 保留头，回到 code 起点 */

/* 基本发射 */
void   lav_op(lav_asm_t *a, uint8_t op);          /* 无操作数 */
void   lav_op_u8(lav_asm_t *a, uint8_t op, uint8_t v);
void   lav_op_u16(lav_asm_t *a, uint8_t op, uint16_t v);
void   lav_op_u32(lav_asm_t *a, uint8_t op, uint32_t v);
void   lav_bytes(lav_asm_t *a, const void *p, size_t n);

/* 地址类操作数（依 RamBits 发 2 或 3 字节） */
void   lav_addr(lav_asm_t *a, uint32_t addr);

/* 语义化封装 */
void   lav_push_int(lav_asm_t *a, int v);         /* push_int: 2 字节符号扩展 */
void   lav_push_long(lav_asm_t *a, int32_t v);    /* push_long: 4 字节 */
void   lav_push_byte(lav_asm_t *a, uint8_t v);    /* push_char: 1 字节 */
/* 压入"地址常量"：LavaX 里 push_a* 会先 pop 一个偏移量再加入地址。
 * 因此必须先 lav_push_int(a,0) 提供一个 0 偏移。 */
void   lav_push_addr_const(lav_asm_t *a, uint32_t addr, int width_bytes);
void   lav_push_string(lav_asm_t *a, const char *s);  /* 压字符串并写进 VM 内存 */

/* 跳转：返回需要回填的位置；目标为**绝对文件偏移** */
size_t lav_jmp_slot(lav_asm_t *a, uint8_t op);    /* op = C_JMP/C_JMPE/C_JMPN */
void   lav_patch(lav_asm_t *a, size_t slot, size_t target_abs);

size_t lav_pos(const lav_asm_t *a);
