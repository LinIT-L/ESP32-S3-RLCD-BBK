/**
 * lpk1.h — LPK 自包含资源包读取器 (格式版本 1)
 *
 * 一句话: 把一个 Lua 程序 + 它的全部资源塞进**一个文件**, 不解压、不散落,
 * 运行时按偏移直接读。设备上只存在一个 .lpk。
 *
 * 为什么不用 ZIP
 * --------------
 *   ZIP 要么解包(散成一堆文件 + 卡上写两遍), 要么随机访问时反复解析中央目录。
 *   LPK 把索引放在文件头后面, 打开时**只读头 + 索引**(几十 KB 级), 之后每个资源
 *   就是 [off, off+size) 一段区间, 一次 seek 就能读 —— 对 TF 卡最友好。
 *
 * 二进制布局 (全部小端, 无压缩, 无 miniz 依赖)
 * --------------------------------------------
 *   ┌────────────────────────────┐ 0
 *   │ HEADER (64 B)              │
 *   ├────────────────────────────┤ 64
 *   │ LUA ENTRY (原始字节)        │  lua_off / lua_size
 *   ├────────────────────────────┤
 *   │ MANIFEST:                  │  man_off / man_size
 *   │   count × 32B 记录          │
 *   │   + 文件名池 (UTF-8)        │
 *   ├────────────────────────────┤
 *   │ RESOURCE DATA (原始字节)     │  每条记录给出 [off, off+size)
 *   └────────────────────────────┘  total_size
 *
 * HEADER (64 B):
 *   0  char[4] magic "LPK1"
 *   4  u16 version      = 1
 *   6  u16 hdr_size     = 64
 *   8  u32 total_size   整个文件字节数(必须与真实文件大小相等, 防截断)
 *   12 u32 lua_off
 *   16 u32 lua_size
 *   20 u32 lua_crc      Lua 主程序 CRC32
 *   24 u32 man_off
 *   28 u32 man_size
 *   32 u32 count        资源条数
 *   36 u32 data_off     第一个资源数据的起始偏移(对齐用)
 *   40 u32 pkg_crc      整包 CRC32(该字段自身按 0 参与计算)
 *   44 u32 flags        bit0 = 含资源; 其余保留
 *   48 u8[16] reserved  必须全 0
 *
 * ENTRY (32 B):
 *   0  u32 name_off     相对 man_off 的字符串池偏移
 *   4  u32 off          资源数据**绝对**偏移
 *   8  u32 size
 *   12 u32 crc32
 *   16 u16 name_len
 *   18 u8  type         0=raw(保留给未来压缩/加密类型)
 *   19 u8  reserved0
 *   20 u32 reserved1
 *   24 u32 reserved2
 *   28 u32 reserved3
 *
 * 本文件**不依赖 ESP-IDF** —— 宿主机直接编译同一份源码跑单测, 避免"测试副本漂移"。
 */
#ifndef LPK1_H
#define LPK1_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LPK_MAGIC        "LPK1"
#define LPK_VERSION      1
#define LPK_HDR_SIZE     64
#define LPK_ENTRY_SIZE   32
#define LPK_MAX_NAME     192      /* 单条相对路径上限(含 NUL) */
#define LPK_MAX_ENTRIES  8192     /* 条数上限, 防畸形包吃光内存 */

/* 打开时的返回码 (负值一律是"安全拒绝", 调用方不得继续用句柄) */
enum {
    LPK_OK          = 0,
    LPK_E_ARG       = -1,   /* 参数非法 */
    LPK_E_OPEN      = -2,   /* 打不开文件 */
    LPK_E_MAGIC     = -3,   /* 魔数不对 */
    LPK_E_VERSION   = -4,   /* 格式版本不支持 */
    LPK_E_HDR       = -5,   /* 头字段越界/不自洽 */
    LPK_E_TRUNC     = -6,   /* 文件被截断(size != total_size) */
    LPK_E_ENTRY     = -7,   /* 某条记录越界/字段不合法 */
    LPK_E_PATH      = -8,   /* 路径不合法(绝对/..穿越/反斜杠/控制字符) */
    LPK_E_NOMEM     = -9,   /* 调用方给的表太小 */
    LPK_E_IO        = -10,  /* 读失败 */
    LPK_E_AMBIG     = -11,  /* 仅大小写不同的重名条目, 无法判定 */
    LPK_E_NOENT     = -12,  /* 资源不存在 */
    LPK_E_ROFS      = -13,  /* 只读: 不允许写资源区 */
    LPK_E_ISDIR     = -14,  /* 目标是目录, 不能当文件打开 */
};

typedef struct {
    char     name[LPK_MAX_NAME];  /* 相对路径, '/' 分隔 */
    uint32_t off;                 /* 绝对文件偏移 */
    uint32_t size;
    uint32_t crc32;
    uint8_t  type;
} lpk_entry_t;

typedef struct {
    FILE    *fp;
    uint32_t total_size;
    uint32_t lua_off, lua_size, lua_crc;
    uint32_t data_off;
    uint32_t pkg_crc;
    uint32_t flags;
    uint32_t count;               /* 头里声明的条数 */
    int      loaded;              /* 已成功读入表里的条数 */
} lpk_t;

/* 打开并校验头 + 索引。entries 由调用方提供(LPK_MAX_ENTRIES 条 ≈ 8192*200B 太多,
 * 实际按元数据里的 count 分配即可); 传 NULL 则只读头不读表。
 * 失败时返回负的错误码, 且会把 fp 关掉 —— 调用方不需要再 close。
 *
 * 索引阶段除逐条边界检查外, 还会查「重复路径 / 大小写冲突」:
 *   完全同名两条        ⇒ LPK_E_ENTRY（包被拼过/改过, 整包拒绝）
 *   仅大小写不同两条    ⇒ LPK_E_AMBIG（无法判定该用哪一条）
 * 放在打开时而不是查找时, 是为了避免"能打开但读到哪份资源看打包顺序"这种偶发问题。
 * 代价 O(n²) 次折叠比较: 仙剑 109 条 ≈ 6 千次(<1 ms), 只在打开时付一次。 */
int  lpk_open(lpk_t *h, const char *path, lpk_entry_t *entries, int max_entries);

void lpk_close(lpk_t *h);

/* 查表（**精确**匹配）。返回下标, 未找到 -1。
 * 注意: 这是字节比较, 区分大小写。SDLPal 这类从 DOS 时代过来的引擎
 * 一律用小写文件名去开（磁盘上是 ABC.MKF），必须用 lpk_find_ci()。 */
int  lpk_find(const lpk_t *h, const lpk_entry_t *entries, const char *name);

/* 查表（**大小写不敏感**，ASCII 范围）。返回：
 *   >= 0            下标
 *   -1              未找到
 *   LPK_E_AMBIG   包里存在两条"仅大小写不同"的条目 —— 无法判定该用哪个
 *                  （打包器已禁止这种包，这里是读取端的最后一道闸门）
 * 实现: 先跑一次精确匹配（绝大多数命中走这条，零额外开销），
 * 只有没命中才做一遍 ASCII 折叠比较；不分配任何内存，不改包里的原始名字。 */
int  lpk_find_ci(const lpk_t *h, const lpk_entry_t *entries, const char *name);

/* 按区间读资源: off 与 len 都是**相对于该资源起始**的。
 * 越界/句柄已关/读不满 一律返回负码。buf 由调用方提供。 */
int  lpk_read(lpk_t *h, const lpk_entry_t *e, uint32_t off, void *buf, uint32_t len);

/* 读 Lua 入口到调用方缓冲(缓冲由调用方按 lua_size 分配; 这里不做 malloc)。 */
int  lpk_read_lua(lpk_t *h, void *buf, uint32_t cap, uint32_t *out_len);

/* 整包 CRC32 校验(读全文件, 慢; 只在"下载后验收"时用一次)。返回 0 = 一致。 */
int  lpk_verify_pkg(lpk_t *h);

/* 路径守卫: 与 am_manifest.c 的 am_mf_path_ok 同一套规则
 * (相对、无 .. 段、无反斜杠、无盘符/协议、无控制字符、不以 / 结尾)。
 * 这里独立实现一份, 因为本模块要能被宿主机单测且不依赖 apps 层。 */
bool lpk_path_ok(const char *rel);

/* crc32 (与 zlib/binascii.crc32 一致) —— 供上层复用, 避免多份实现漂移。 */
uint32_t lpk_crc32(uint32_t crc, const void *buf, uint32_t len);
uint32_t lpk_crc32_file(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* LPK1_H */
