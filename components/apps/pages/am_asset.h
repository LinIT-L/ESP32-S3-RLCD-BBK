/**
 * am_asset.h — 商店「资源包」安装器 (ZIP 解包到 TF 卡指定目录)
 *
 * 用途: 仙剑奇侠传这类"不是真应用、只是一堆游戏数据"的包。
 *   index.json 里 type=asset + target=/sdcard/apps/app/pal ⇒ 下载完成自动解到这个目录,
 *   apps_registry 里该应用的 .requires 指向同一目录 ⇒ 解完立刻出现在"游戏"分类。
 *
 * **只支持 stored(不压缩) ZIP**，这是硬约束，不是建议。
 *   2026-10-31 真机两次复现（栈 20 KB / 32 KB 都一样）：deflate 条目解包时
 *   `Guru Meditation (LoadProhibited)`，PC 落在 miniz_zip.c:1909，
 *   原因是 miniz 内部的 64 KB I/O 缓冲在本机分配不到（内部 RAM 最大块 31 KB）。
 *   现在 stored 条目**根本不经过 miniz**：算出 local header 后的数据偏移直接搬字节；
 *   deflate 条目则直接判失败并打日志，不再尝试解压、更不会把设备搞重启。
 *   打包一律用 `tools/pack_asset.py`（内部 ZIP_STORED），别用系统 zip 工具。
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

/* 递归删除一个目录及其内容 (卸载资源包用). 返回 true = 目录已不存在。
 * ⚠ 只适用于**该包独占**的目录 (如 /sdcard/apps/app/pal)。
 *   共享目录 (/sdcard/fonts) 请用 am_asset_uninstall()。 */
bool am_asset_rmdir(const char *dir);

/* 2026-10-30: 按安装时写下的清单卸载，只删本包装进去的文件。
 * 共享目录(多包共存)必须用它 —— 否则删一个字体就把别人全删了。
 * 返回 false = 没有清单(旧包), 调用方可回退到 am_asset_rmdir()。 */
bool am_asset_uninstall(const char *zip_path, const char *target_dir);

/* 2026-10-30: 该包是否装了 fullpath 这个文件（按安装清单核对）。
 * 卸载前判断"正在使用的字体是否属于本包" —— 只比清单，不用目录前缀，
 * 否则同目录下装多个字体时会互相误拦。返回 false 也可能是旧包无清单。 */
bool am_asset_has_file(const char *zip_path, const char *target_dir, const char *fullpath);

/* 2026-10-31: 逐级建目录（mkdir -p）。多文件直接下载要给每个文件建父目录，
 * 与 zip 解包共用同一份实现，避免两处行为不一致。 */
void am_asset_mkdirs(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* AM_ASSET_H */
