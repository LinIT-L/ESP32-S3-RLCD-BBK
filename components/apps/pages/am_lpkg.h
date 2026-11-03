/**
 * am_lpkg.h — 第三方应用包（LPK）安装器 + 安装注册表（商店范围）
 *
 * 边界：只管**商店下载的第三方应用/资源包**。内置 C 应用完全不经过这里。
 *
 * 目录（与 docs/architecture/app_system.md 一致）
 *   /sdcard/apps/installed/<app_id>/   程序 + manifest + icon + 自有资源
 *   /sdcard/apps/data/<app_id>/        用户数据（卸载默认保留）
 *   /sdcard/apps/cache/<app_id>/       可删缓存
 *   /sdcard/apps/staging/              安装中转（失败即清）
 *   /sdcard/apps/quarantine/           校验失败隔离（取证）
 *   /sdcard/apps/registry/<app_id>.txt 安装记录（人可读）
 *   /sdcard/resources/<type>/<app_id>/ 资源包
 *
 * 身份：唯一以包内 manifest 的 app_id 为准。**不再用文件名/中文名/目录扫描结果**
 * 推导身份 —— 那正是以前"同一个应用出现两条、改了名就变成新应用"的根源。
 */
#ifndef AM_LPKG_H
#define AM_LPKG_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "os.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AM_LPKG_MAX   16      /* 同时可注册的第三方应用上限（每条约 240 B 静态） */

/* 安装一个 .lpk（应用包或资源包）。
 *   ① lpk_mf_load：容器 + manifest 全量校验（含 entry/icon 在包内）
 *   ② 解包到 staging/<app_id>.new（逐个条目写盘，带字节数核对）
 *   ③ 原子切换：旧版本先改名 .old，新目录就位后再删 .old
 *   ④ 写 registry + 重新扫描注册
 * 失败：清掉 staging，旧版本原样保留，不留"半装"状态。
 * why 返回具体原因（可为 NULL）。 */
esp_err_t am_lpkg_install(const char *lpk_path, char *why, size_t why_cap);

/* 同上，额外记录"这份包是从商店索引的哪一版装进来的"。
 * src_ver/src_build 传 NULL 表示不知道（手动安装/老调用方）⇒ 与 manifest 版本相同。
 * 用途见 am_store_state.h 里 registry 格式的说明：
 *   服务器若只抬索引版本、没重新打包，包内 manifest 会落后于索引；
 *   只按 manifest 比版本会导致"更新成功但仍显示更新"（永远退不掉）。
 * 所以 registry 同时记 manifest 版本 + 商店版本，更新判定用后者。 */
esp_err_t am_lpkg_install_src(const char *lpk_path, const char *src_ver,
                              const char *src_build, char *why, size_t why_cap);

/* ★ 2026-11-02: 同上，额外记录**服务器文件的身份**（mtime/crc32/size）。
 *   更新判定只认 src_mtime（服务器文件修改时间），src_crc/src_size 用于"内容其实没变"
 *   与老应用迁移的核对。三个都传 0 = 不知道来源（手动安装）⇒ 记录里写 0，
 *   商店侧退回 size+CRC32 核对，不会凭空产生"更新"。 */
esp_err_t am_lpkg_install_src2(const char *lpk_path, const char *src_ver,
                               const char *src_build,
                               uint32_t src_mtime, uint32_t src_crc, uint32_t src_size,
                               char *why, size_t why_cap);

/* 卸载。keep_data=true 时保留 /sdcard/apps/data/<app_id>/。
 * 资源包会顺带检查是否仍被其他应用依赖（当前仅日志告警，不做级联删除）。 */
esp_err_t am_lpkg_uninstall(const char *app_id, bool keep_data, char *why, size_t why_cap);

/* 该 app_id 是否已安装。
 * 判据（顺序固定，两处判定必须完全一致）：
 *   ① registry/<app_id>.txt 存在且能解析出 app_id  → 已安装
 *   ② registry 缺失/损坏, 但 installed/<app_id>/manifest.json 存在 → 已安装（并打 WARN）
 *   ③ 两者都没有 → 未安装
 * ⚠ 商店侧判断 `type=lpk` 的"已安装"必须用它，**不能**再用旧的
 *   `.installed.<stem>` / `.pkg.<stem>.lst` —— 那是 zip 资源包/无 manifest 老包的记账方式。 */
bool am_lpkg_is_installed(const char *app_id);

/* 已安装信息（含本地版本）—— 商店判"更新"的**唯一**本地数据源。
 * 返回 true = 已安装；out 里 version/build 可能为空串（旧包没写这俩字段时），
 * 此时调用方应按"未知版本"处理（不提示更新），而不是当成 0 去比。 */
typedef struct {
    char app_id[56];
    char ver[16];          /* 商店口径的版本（registry 的 store_ver，缺省=包内 version） */
    char build[16];        /* 同上（store_build 缺省=包内 build） */
    char mf_ver[16];       /* 包内 manifest 自称的版本（与 ver 不一致时会打 WARN） */
    char mf_build[16];
    bool meta_mismatch;    /* 索引版本 ≠ 包内版本（服务器索引需要重新生成） */
    /* ★ 2026-11-02: 安装时服务器文件的身份（registry 的 store_mtime/crc/size）。
     *   全 0 = 早期记录没有这三项 ⇒ 商店退回 size+CRC32 核对，不造"更新"。 */
    uint32_t rec_mtime;    /* unix 秒 */
    uint32_t rec_crc;      /* crc32 */
    uint32_t rec_size;     /* 字节数 */
    bool registry_ok;      /* registry 文件本身可读且自洽 */
    bool dir_ok;           /* installed/<app_id>/ 目录存在 */
} am_lpkg_info_t;
bool am_lpkg_info(const char *app_id, am_lpkg_info_t *out);

/* 从包路径/包名取 app_id 主干（"packages/com.lintos.x.lpk" → "com.lintos.x"）。
 * ⚠ 只去掉**最后一个**扩展名。早期实现截到**第一个** '.'，于是
 *   com.lintos.recorder.lpk 变成 "com" ⇒ 查 registry/com.txt 必然查不到 ⇒
 *   已安装的应用在商店里永远显示"下载"，卸载也删不掉（2026-11-01 根因）。
 * 返回 buf；失败返回空串。 */
const char *am_lpkg_stem_of(const char *path, char *buf, size_t cap);

/* 扫描 installed/ 并注册可启动应用到 os_app 注册表。返回注册条数。
 * 幂等：os_app_register 按 id 覆盖，不会重复。 */
int am_lpkg_scan(void);

/* ---- 给启动链路用：按 os_app_t 反查入口 ---- */
const char *am_lpkg_entry_of(const os_app_t *a);   /* 不是本体系的条目 → NULL */
/* ★ 2026-11-02: 按 app_id 求可启动入口的绝对路径（registry 的 entry + installed/<id>/）。
 *   商店里「打开」按钮用 —— 商店行持有的是自己的 os_app_t 副本，不能靠指针反查。
 *   返回 true = 文件真的存在且路径已写出。 */
bool        am_lpkg_entry_path(const char *app_id, char *out, size_t cap);bool        am_lpkg_need_wifi_of(const os_app_t *a);

/* ---- 诊断（串口 lpklist） ---- */
int         am_lpkg_count(void);
const char *am_lpkg_id_at(int i);
const char *am_lpkg_ver_at(int i);

#ifdef __cplusplus
}
#endif
#endif /* AM_LPKG_H */
