/* netc_fs_replace.h — 落盘"临时文件 → 正式文件"替换 / 掉电恢复 (纯文件操作)
 *
 * 为什么单独拆出来:
 *   这段逻辑的正确性只取决于"文件系统语义", 与 ESP-IDF / FreeRTOS 无关。
 *   拆成不依赖任何 ESP 头文件的实现后, **主机测试可以直接编译这个 .c**
 *   (tests/dl_replace), 不必再复制一份副本 —— 副本会和生产代码漂移。
 *
 * 事实依据 (不是 POSIX 直觉):
 *   - 落盘目标 /sdcard/... ⇒ FATFS(VFS) ⇒ vfs_fat_rename() 直调 f_rename()
 *     (esp-idf-v60/components/fatfs/vfs/vfs_fat.c:959-966);
 *   - f_rename() 对 FAT/FAT32 **目标已存在时返回 FR_EXIST, 不覆盖** (ff.c:5244);
 *   - IDF 6.0.3 **没有** CONFIG_FATFS_VFS_RENAME_REPLACES_DESTINATION 这个选项。
 *   ⇒ 想覆盖必须自己"先把旧文件改名腾位"。这是本模块存在的唯一理由。
 *
 * 三条不变量:
 *   ① 调用方只在数据完整之后才调 netc_fs_replace();
 *   ② 旧版本只被**改名**成 .bak, 全程不删除;
 *   ③ 任一步失败 → 立即回滚; 掉电 → netc_fs_recover() 把 .bak 复位。
 *
 * 本文件**无全局状态** ⇒ 可从任意任务并发调用(各用各的路径缓冲)。
 */
#ifndef NETC_FS_REPLACE_H
#define NETC_FS_REPLACE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* netc_fs_recover() 的结果 */
typedef enum {
    NETC_REC_NONE = 0,   /* 无需处理: 没有 .bak(绝大多数情况, 只花一次 stat) */
    NETC_REC_RESTORED,   /* 正式文件缺失, 已把 .bak 复位成正式文件 */
    NETC_REC_CLEANED,    /* 正式文件在, 已删掉陈旧的 .bak */
    NETC_REC_FAILED,     /* 该做的动作失败了(.bak 原样保留, 下次再试) */
} netc_rec_t;

/* 掉电恢复。两条规则:
 *   正式文件缺失 + .bak 存在 → rename(.bak → 正式)   ← 替换中途掉电的残留
 *   正式文件存在   + .bak 存在 → remove(.bak)         ← 替换成功后没来得及删
 * 不碰 .part(可能正在下载), 不碰其它任何文件。
 * final_path / bak_path 由调用方构造(本函数不分配、不使用任何静态缓冲)。 */
netc_rec_t netc_fs_recover(const char *final_path, const char *bak_path);

/* 把 part 提为正式文件。返回 true = 成功。
 *   - 正式文件存在: 先改名为 bak(失败则放弃本次替换, **旧文件不动**), 再 part → 正式;
 *   - 正式文件不存在: 直接 part → 正式;
 *   - 中间任何一步失败: 回滚旧版本并删掉 part, 返回 false。 */
bool netc_fs_replace(const char *part_path, const char *final_path, const char *bak_path);

#ifdef __cplusplus
}
#endif

#endif /* NETC_FS_REPLACE_H */
