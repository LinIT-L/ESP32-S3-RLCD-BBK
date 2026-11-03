/**
 * am_app_core.h — LinTOS 2.0 **统一应用管理核心**（唯一应用数据来源）
 *
 * ════════════════════════════════════════════════════════════════════════
 * 为什么要有这个文件
 * ------------------------------------------------------------------------
 * 2026-11-03 之前，应用管理有三套互不相干的扫描器和五套身份：
 *   · am_lua_scan   (am_lua.c)    → s_lua[]，id = "lua:<文件名>"
 *   · am_pkg_scan   (am_store.c)  → s_pkg[]，id = "pkg:<文件名>"
 *   · am_lpkg_scan  (am_lpkg.c)   → s_lp[] ，id = manifest.app_id
 * 前两者**扫同一批目录**，各自建表、各自判"装没装"，于是同一应用在应用中心
 * 和商店里是两条记录、两种状态 —— 这就是"商店显示获取、应用中心能打开"的根因。
 *
 * 本模块把这些收敛成**一份记录**：谁装的、装在哪、什么版本、有没有新版、
 * 能不能启动，只在这里判一次。页面（应用中心 / 商店 / 详情 / 卸载）只读。
 *
 * ════════════════════════════════════════════════════════════════════════
 * 2026-11-03 linit 定稿的五条规则（改动前先读这五条）
 * ------------------------------------------------------------------------
 * ① **本地扫描与服务器目录分离**
 *      本地 = 真文件 + 已安装状态；服务器 = 目录 + 远端版本 + 下载信息。
 *      两者只在本模块合并。`type=asset`（字体/主题/ROM 等资源包）**不进应用列表**。
 * ② **身份**
 *      优先 manifest.app_id；没有则规范化文件名（小写、去扩展名、trim）。
 *      身份**带类型前缀**（lua:/lpk:/builtin:）⇒ 同名 Lua 与 LPK 永不合并。
 *      同身份的 Flash / TF 副本合并成一条。
 *      ⛔ 不同 app_id 绝不因显示名相同而合并；⛔ 不做别名白名单、不做模糊匹配。
 * ③ **os_app 的职责不变**
 *      内置 C 注册、LPK 运行注册、桌面入口、系统运行能力仍归 os_app。
 *      本模块只负责发现 / 身份 / 安装状态 / 管理动作。
 *      ⛔ 不允许把 am_app_t 强转成 os_app_t —— 页面要的 `os_app_t` 是本结构里的
 *         `view` 成员（由核心填好、指向自己的字段），不是 reinterpret_cast。
 * ④ **LPK**
 *      已安装 → 应用中心 + 商店（卸载/更新）；有效但未安装 → **只进商店**（安装）。
 *      安装/更新/卸载一律复用 am_lpkg_* 的正式实现，本模块不自己解包。
 * ⑤ **TF 卡**
 *      安装事实（registry / 文件在不在）与**存储可用性**分开：
 *      拔卡只让 TF 位置"暂不可见"，不改安装事实、不产生幽灵条目；
 *      重新挂载后 am_core_invalidate() → 自动重扫，Flash 与服务器状态不受影响。
 * ════════════════════════════════════════════════════════════════════════
 */
#ifndef AM_APP_CORE_H
#define AM_APP_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os.h"              /* os_app_t */
#include "am_store_state.h"  /* am_st_t */
#include "am_core_logic.h"   /* 纯逻辑：身份 / 状态判定 / 列表投影（宿主机可测） */

#ifdef __cplusplus
extern "C" {
#endif

typedef am_core_kind_t am_kind_t;

/* 96 = 内置 35 + TF/Flash Lua 最多 48 + LPK 至多 16（与 os_app 的 OS_APP_MAX 对齐）。
 * ⚠ 曾经是 64 —— 光是「内置 35 + 服务器目录 17 + 本地 Lua 十几条」就会溢出，
 *   溢出后排在后面的应用会**静默消失**（表现就是"商店里少了几个"）。 */
#define AM_CORE_MAX        96
#define AM_CORE_KEY_LEN    72
#define AM_CORE_NAME_LEN   48
#define AM_CORE_PATH_LEN  192
#define AM_CORE_VER_LEN    16

typedef struct {
    /* ---- 身份 ---- */
    char      key[AM_CORE_KEY_LEN];      /* "lua:<norm>" / "lpk:<app_id>" / "builtin:<id>" —— 唯一身份 */
    char      rt_id[AM_CORE_KEY_LEN];    /* 运行期 id（写进 os_app 注册表 / NVS 桌面列表）：
                                          *   内置 = 原 id；Lua = "lua:<文件名>"；LPK = app_id。
                                          *   ⚠ 与 key 分开：key 带类型前缀保证不误合并，
                                          *     rt_id 必须与历史 NVS 里的 id 一致，桌面图标才不会丢。 */
    am_kind_t kind;
    bool      center_hide;               /* 应用中心不显示它（同 id 已有内置 C 应用时，
                                          *   LPK 只在商店里作为"可安装/可更新"条目存在） */

    /* ---- 名称（规则 ⑥：中文 > 英文 > 文件名；显示名不承担身份职责） ---- */
    char name_zh[AM_CORE_NAME_LEN];
    char name_en[AM_CORE_NAME_LEN];
    char name_show[AM_CORE_NAME_LEN];

    char cat_zh[16];                     /* 中文分类（游戏/软件/运维/学习/系统） */
    char desc[96];
    int  icon_idx;
    int  page;                           /* 仅内置 C：页面枚举（投影时写进 view.page） */

    /* ---- 有效性 ---- */
    bool valid;                          /* 包损坏 / LAPP 头缺失 / manifest 无效 ⇒ false */
    bool need_wifi;                      /* LAPP `need=wifi` 或 manifest permissions */

    /* ---- 安装事实（本地扫描得出，与服务器无关） ---- */
    uint8_t loc;                         /* AM_LOC_* 位掩码 */
    bool storage_ok;                     /* 该条目的位置当前可访问（TF 拔卡 ⇒ false） */
    char path_flash[AM_CORE_PATH_LEN];   /* 实际文件（内置分区） */
    char path_tf[AM_CORE_PATH_LEN];      /* 实际文件（TF 卡） */
    char pkg[56];                         /* 包文件名 "map.lua" / "pal.lpk"（下载/安装/删除共用） */
    char entry[AM_CORE_PATH_LEN];        /* 运行入口：Lua 文件 / LPK registry entry */
    char ver_local[AM_CORE_VER_LEN];
    char build_local[AM_CORE_VER_LEN];

    /* ---- 远端事实（服务器索引；索引未加载时 in_catalog=false 且 idx_ready=false） ---- */
    bool     in_catalog;
    bool     idx_ready;                  /* 索引**成功拉到过**才允许判"服务器已删除" */
    bool     updatable;
    int16_t  ridx;                       /* 索引下标（-1 = 索引里没有）；下载器按它取包 */
    char     pkg_remote[AM_CORE_PATH_LEN];/* 索引里的相对路径 pkgs/xxx.lua */
    char     ver_remote[AM_CORE_VER_LEN];
    char     build_remote[AM_CORE_VER_LEN];
    uint32_t mtime_remote;
    uint32_t crc_remote;
    uint32_t size_remote;

    /* ---- 页面投影（由核心填好，指向本结构自己的字段；**不是 cast**） ---- */
    os_app_t view;
} am_app_t;

/* ───────────── 扫描 / 刷新 ───────────── */

/** 目录、安装记录、TF 挂载状态变化后调用：下次 am_core_tick 重扫。幂等。 */
void am_core_invalidate(void);

/** 每帧调用。返回 true = 数据有变化，调用方应重建列表。 */
bool am_core_tick(void);

/** 数据是否已就绪（首轮扫描完成）。 */
bool am_core_ready(void);

/** 读侧数据代次：每次换代/定向改动 +1。
 *  页面列表存的是 &s_app[i].view **裸指针** —— 核心换代后旧指针全部失效，
 *  必须重建才能取到新的说明/按钮。页面构建列表时记下当时代次，
 *  代次变了就不能复用缓存列表（否则出现"卸载后说明消失、切分类才恢复"）。 */
uint32_t am_core_gen(void);

/** 启动时踢一次后台扫描（幂等）。 */
void am_core_boot(void);

/* ───────────── 读（页面唯一入口） ───────────── */

int             am_core_count(void);
const am_app_t *am_core_at(int i);
const am_app_t *am_core_find(const char *key);
const char     *am_core_key_of(const am_app_t *a);

/** 页面拿到的 `os_app_t*` 反查回统一记录（**唯一**的指针→记录映射）。
 *  传进来的必须是本模块 view 成员的地址；其它指针（内置注册表原件、栈上临时对象）
 *  一律返回 NULL，调用方据此走「不是统一核心管理的条目」分支。 */
const am_app_t *am_core_from_view(const os_app_t *view);

/** 该条目的**包文件名**（"map.lua" / "pal.lpk"）；无则空串。
 *  下载、安装、删除都要用同一个名字，别各处自己拼。 */
const char *am_core_pkg_name_of(const am_app_t *a);

/** 该条目的显示名（name_show 的指针）。 */
const char *am_core_label_of(const am_app_t *a);

/** 应用中心候选：内置 + 有效 + 已安装（未安装 LPK 包不进）。 */
int am_core_count_center(void);
const am_app_t *am_core_center_at(int i);

/** 商店候选：服务器目录 + 本地有效应用（内置 C 不进）。 */
int am_core_count_store(void);
const am_app_t *am_core_store_at(int i);

/** 统一安装状态（取代原先散在七处的判断）。 */
am_st_t am_core_state_of(const am_app_t *a);

/** 统一按钮文案（"获取 / 安装 / 更新 / 卸载"）。 */
const char *am_core_btn_of(const am_app_t *a);

/* ───────────── 动作（唯一入口；内部复用已验证的底层实现） ───────────── */

/** 安装：.lua 复制到 apps/<cat>/ ；.lpk 走 am_lpkg_install()。 */
esp_err_t am_core_install(const char *key, char *why, size_t why_cap);

/** 更新：优先用远端包（由调用方先下好），否则用本地包。不降级。 */
esp_err_t am_core_update(const char *key, char *why, size_t why_cap);

/** 卸载：清掉**所有**已确认属于该 key 的副本（Flash+TF+待装源+安装记录）。 */
esp_err_t am_core_remove(const char *key, bool keep_data, char *why, size_t why_cap);

/** 卸载成功后的**定向**状态更新：只把这一条改成"未安装"。
 *  ⛔ 不得用"重扫"代替它 —— 一次扫描失败就会把其它应用一起判成未安装，
 *     这正是"卸载一个 Lua，所有 Lua 都变成下载"的根源。
 *  调用后仍会安排一次后台重扫，但那只是校正，不承担"让按钮变对"的职责。 */
void am_core_mark_removed(const char *key);

/** 本地是否有"可安装但未安装"的包（LPK 安装包）。 */
bool am_core_has_local_pkg(const am_app_t *a);

/** 商店下载完成后调用：按下载下来的**文件名**找到对应记录并安装。
 *  fname 例："map.lua" / "pal.lpk"。返回 ESP_OK 表示已落到正式安装位置。 */
esp_err_t am_core_install_downloaded(const char *fname, char *why, size_t why_cap);

/** 在 Flash ↔ TF 之间搬运已安装的 Lua 应用（LPK 不支持，返回 INVALID_STATE）。 */
esp_err_t am_core_move_to(const char *key, bool to_flash, char *why, size_t why_cap);

/** 该条目启动前是否需要联网（LAPP need=wifi / manifest permissions）。 */
bool am_core_need_wifi_of(const am_app_t *a);

/* ───────────── 身份工具 ─────────────
 * am_core_norm / am_core_make_key / am_core_same 定义在 am_core_logic.h
 * （纯 C，宿主机可编译；本头文件已 include 它，这里不再重复声明）。 */

#ifdef __cplusplus
}
#endif
#endif /* AM_APP_CORE_H */
