/**
 * lpk_mode.h — 「这个游戏用哪个资源模式」的唯一判定点
 *
 * 为什么必须有一个显式判定
 * ------------------------
 * 仙剑现在有两种装法：
 *   · 模式 A（推荐）：卡上只有一个 `pal.lpk`，运行时挂到 `/lpk` 直接读，不解压；
 *   · 模式 B（兼容）：原来那套 ZIP 解压，109 个文件散在 `/sdcard/apps/app/pal/`。
 * 两种装法的数据完全可能**同时存在**（用户从 ZIP 升到 LPK，旧目录还没删）。
 * 如果按"先看包里有没有、没有再看盘上"，就会出现资源混用：
 * 一部分来自包、一部分来自盘，出问题时根本分不清读的是哪一份。
 *
 * 所以判定只有三条依据，且**顺序固定**：
 *   ① 安装记录文件 `<dir>/.install_mode`（内容 `lpk` 或 `zip`）—— 安装器写的，最权威；
 *   ② 各自的自检：LPK 要能过头部+索引+必备资源齐全；目录要能真的打开必备资源；
 *   ③ 记录缺失/失效时按"目录优先"回落（目录模式是历史行为，不能被新功能破坏）。
 *
 * ⛔ 不做的事：不去看"有没有某个临时文件/残留目录"来猜；
 *    LPK 自检失败时**不删**任何东西，只回落并把原因写进日志。
 */
#ifndef LPK_MODE_H
#define LPK_MODE_H

#include <stdbool.h>
#include <stddef.h>

#include "lpk1.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 安装记录文件名（放在数据目录里，点开头不碍事） */
#define LPK_MODE_RECORD ".install_mode"

typedef enum {
    LPK_RES_NONE = 0,   /* 两种都不成立 ⇒ 不能启动 */
    LPK_RES_DIR  = 1,   /* ZIP 解压出来的目录 */
    LPK_RES_LPK  = 2,   /* 单文件 LPK */
} lpk_res_mode_t;

/* 读安装记录：1 = lpk，0 = zip，-1 = 没有记录/读不出来 */
int lpk_mode_record_read(const char *dir);

/* 写安装记录（安装器用）：mode 非 0 写 "lpk"，否则写 "zip"。成功返回 0。 */
int lpk_mode_record_write(const char *dir, int mode);

/* 必备资源名（两模式共用一份清单，保证判定口径一致）。*n 返回条数。 */
const char *const *lpk_mode_required(int *n);

/* 目录模式自检：必备资源都能打开 ⇒ true。why 里写第一条缺失项。 */
bool lpk_mode_dir_ready(const char *dir, char *why, size_t cap);

/* LPK 自检：头部/索引/边界（lpk_open 全做）+ 必备资源都在索引里 ⇒ true。
 * tab/maxtab 由调用方给（设备侧从 PSRAM 分配），传 NULL 时临时 malloc。 */
bool lpk_mode_lpk_ready(const char *lpk_path, lpk_entry_t *tab, int maxtab,
                        char *why, size_t cap);

/* 综合判定（唯一入口）。why 里写"为什么选它/为什么都不行"。 */
lpk_res_mode_t lpk_mode_decide(const char *dir, lpk_entry_t *tab, int maxtab,
                               char *why, size_t cap);

const char *lpk_mode_str(lpk_res_mode_t m);

#ifdef __cplusplus
}
#endif

#endif /* LPK_MODE_H */
