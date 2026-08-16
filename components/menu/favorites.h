/* favorites.h - 游戏收藏管理 (按引擎独立, TF 卡文件持久化)
 * 存储位置: /sdcard/system/fav_*.txt (每个引擎一个独立文件)
 * 容量: 每个引擎最多 64 个游戏路径 */
#ifndef FAVORITES_H
#define FAVORITES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAVORITES_MAX       64      /* 每个引擎最多收藏 64 个游戏 */
#define FAVORITES_PATH_MAX  160     /* 单个游戏路径最大长度 */

/* 游戏引擎枚举 (收藏按引擎独立存储)
 * 注意: 平台名 NES, 使用 /sdcard/nes 目录 + .nes 文件. */
typedef enum {
    FAV_ENGINE_BBK = 0,   /* 电子词典 (.gam) */
    FAV_ENGINE_GB,        /* GB (.gb) */
    FAV_ENGINE_GBC,       /* GBC (.gbc) */
    FAV_ENGINE_FC,        /* NES (.nes) */
    FAV_ENGINE_AB,        /* arduboy (.hex) */
    FAV_ENGINE_BOOK,      /* 电子书 (.txt) */
    FAV_ENGINE_MAX
} fav_engine_t;

/* 初始化: 从 TF 卡加载所有引擎的收藏. 失败则清空. */
void favorites_init(void);

/* 根据游戏路径推断所属引擎 (按根目录前缀). 未知路径归为 BBK. */
fav_engine_t favorites_engine_for_path(const char *path);

/* 查询: 指定引擎下路径是否已收藏 */
bool favorites_contains(fav_engine_t e, const char *path);

/* 切换: 收藏或取消收藏 (指定引擎). 返回新的收藏状态 (true=已收藏, false=未收藏). */
bool favorites_toggle(fav_engine_t e, const char *path);

/* 添加: 强制添加 (已存在则忽略). 返回 true=成功添加, false=失败 (空路径/已存在/已满) */
bool favorites_add(fav_engine_t e, const char *path);

/* 删除 (指定引擎) */
void favorites_remove(fav_engine_t e, const char *path);

/* 枚举: 获取指定引擎的所有收藏路径, count 返回数量. 返回值是内部静态数组, 不要修改. */
const char *const *favorites_list(fav_engine_t e, int *count);

/* 数量 (指定引擎) */
int favorites_count(fav_engine_t e);

#ifdef __cplusplus
}
#endif

#endif
