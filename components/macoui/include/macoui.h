#pragma once
/* macoui.h — IEEE OUI 字典 查询 (appdata os_db 槽, mmap 只读, 二分查找).
 * 数据: components/macoui/oui_db.bin, 由 tools/gen_oui_db.py 生成.
 * 写入: merge_flash.sh @ appdata+0x540000 (os_db 区, 见 font_part.h 字库区结束之后).
 * [V1.0.9x: 字库区精简(删细宋)后 os_db 迁至 0x540000, 已与 font_part.h / merge_flash.sh 同步] */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OUI 字典在 appdata 分区内的偏移 (字库区结束之后的 os_db 区) */
#define MACOUI_DB_OFF  0x540000UL
#define MACOUI_DB_MAXLEN 0x80000UL   /* 槽位上限 512KB */

/* 用 3 字节 MAC 前缀查厂商名. 命中返回 true 并把 vendor 指向 static 缓冲/字典映射串. */
bool macoui_lookup(const uint8_t oui[3], const char **vendor);

#ifdef __cplusplus
}
#endif