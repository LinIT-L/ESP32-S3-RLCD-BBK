/**
 * am_asset.h — 商店「资源包」安装器 (ZIP 解包到 TF 卡指定目录)
 *
 * 用途: 仙剑奇侠传这类"不是真应用、只是一堆游戏数据"的包。
 *   index.json 里 type=asset + target=/sdcard/apps/app/pal ⇒ 下载完成自动解到这个目录,
 *   apps_registry 里该应用的 .requires 指向同一目录 ⇒ 解完立刻出现在"游戏"分类。
 *
 * 只支持 **stored(不压缩) ZIP** —— 打包用 `python3 tools/pack_asset.py` 保证 method=0。
 * 理由: 解压只需要"搬运字节", 不用链 inflate 库, 固件零新依赖、零解压内存峰值。
 */
#ifndef AM_ASSET_H
#define AM_ASSET_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 解包进度回调: permille = 0..1000 (按已写出的文件数/总条目数估算) */
typedef void (*am_asset_prog_cb_t)(int permille);

/* 把 zip_path 里的所有文件解到 target_dir (自动建子目录).
 * 返回 true = 全部条目都成功写出。失败不会删除已解出的部分 (便于排查)。 */
bool am_asset_install(const char *zip_path, const char *target_dir);

/* 带进度版: 供商店把"下载 70% + 解压 30%"合成一条进度条 */
bool am_asset_install_cb(const char *zip_path, const char *target_dir,
                         am_asset_prog_cb_t cb);

/* 递归删除一个目录及其内容 (卸载资源包用). 返回 true = 目录已不存在。 */
bool am_asset_rmdir(const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* AM_ASSET_H */
