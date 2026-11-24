/**
 * os_file.h — 文件整体替换 / 掉电恢复 (最底层纯工具, 任何层都可依赖)
 *
 * 为什么需要单独一层 (不是"为了抽象而抽象"):
 *   本机 /sdcard 是 FatFs。`f_rename()` 在**目标已存在**时返回 FR_EXIST,
 *   **不会像 POSIX 那样覆盖** —— 源码实锤:
 *     esp-idf-v60/components/fatfs/src/ff.c:5243-5245
 *       res = follow_path(&djn, path_new);
 *       if (res == FR_OK)
 *           res = (djn.obj.sclust == djo.obj.sclust && djn.dptr == djo.dptr)
 *                 ? FR_NO_FILE : FR_EXIST;
 *   真机日志同样证实过 (page_wb.c: `WB: rename 失败: .../wb17200000.bmp`)。
 *
 *   ⇒ "写 tmp 再 rename(tmp, final)" 这个到处都在用的写法, 只要 final 已存在
 *     **必然失败**。于是各模块各写一套"先删再 rename"/"挪成 .bak 再换"补丁,
 *     其中"先删再 rename"在两步之间掉电就丢档。
 *
 *   本文件把正确顺序收敛成一份: 旧 → .old, tmp → 正式, 成功后才删 .old,
 *   任一步失败回滚; 并在加载前用 os_file_recover_auto() 把掉电留下的 .old 复位。
 *   这**不是** POSIX 原子替换, 而是"可恢复的非原子替换" —— 不做超出文件系统
 *   能力的承诺。
 *
 * 约束: 只 include libc 头 (stdio/stat/string), 不碰 ESP-IDF —— 主机测试可直接编译。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* .old 后缀 (掉电恢复用)。统一在这里定义, 别处不要再拼字面量。 */
#define OS_FILE_OLD_SUFFIX ".old"

typedef enum {
    OS_FS_REC_NONE = 0,   /* 无需动作 (.old 不存在, 或正式文件完好/无法判定谁更完整) */
    OS_FS_REC_RESTORED,   /* 正式文件缺失或为 0 字节空壳 ⇒ 已用 .old 复位 */
    OS_FS_REC_CLEANED,    /* 保留值: 当前实现**不再产生** (见 os_file.c 的保守化说明) */
    OS_FS_REC_FAILED,     /* 有 .old 但复位/删除失败 (调用方应告警) */
} os_fs_rec_t;

/* 掉电恢复 (显式路径版)。扫描/加载路径可安全高频调用。
 *
 * ★ 保守原则 (2026-11-22 第4回合定): **不删任何可能是有效数据的文件**。
 *   · 正式文件存在且非空 → 不动, .old 留着 (下次写入会覆盖它);
 *   · 正式文件是 0 字节空壳 → 用 .old 顶替;
 *   · 正式文件不存在 → .old 复位;
 *   · 残留的 .tmp **不提升也不删除** —— 无法区分"写完了"和"写一半",
 *     提升半截数据会被 MD/SMS 的加载函数当合法 savestate 读进去。
 *   也就是说: 恢复失败时宁可留下垃圾文件由人工判断, 也不替用户猜。 */
os_fs_rec_t os_file_recover(const char *final_path, const char *old_path);

/* 掉电恢复 (自动拼 "<final_path>.old")。返回同上。 */
os_fs_rec_t os_file_recover_auto(const char *final_path);

/* 把 tmp_path 就位到 final_path, 全程不删旧版本:
 *   ① final 存在 → 先挪成 old_path (清掉陈旧的 old_path)
 *   ② rename(tmp → final); 失败 → 用 old_path 回滚
 *   ③ 成功后才删 old_path
 * 三个路径必须在同一目录 (同一 FAT 卷; 跨卷 rename 不支持)。
 * 返回 true = 新文件就位。false = 失败且旧文件已回滚 (或 tmp 已清理)。 */
bool os_file_replace(const char *tmp_path, const char *final_path, const char *old_path);

/* 一次调用写完整个文件 (hdr 可为 NULL; hdr 与 body 顺序写出):
 *   写 "<path>.tmp" → fflush → fclose 检查 → stat 校验长度 → os_file_replace。
 * 返回 0 成功; 负值失败, 此时**旧文件保持原样**。
 * 断电语义: 最坏只留下 "<path>.tmp" 或 "<path>.old" (完整旧档),
 *          正式文件要么是旧的、要么是新的, 不会出现半截。 */
int os_file_write_atomic(const char *path, const void *hdr, size_t hdr_len,
                         const void *body, size_t body_len);

#ifdef __cplusplus
}
#endif
