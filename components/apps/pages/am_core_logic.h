/**
 * am_core_logic.h — 统一应用核心的**纯逻辑**部分（零 ESP / 零文件系统依赖）
 *
 * 为什么单独成文件：身份归一、状态判定、列表投影这三件事决定了"商店里该不该出现
 * 这一条、按钮写什么"，错了就是用户看到假状态。把它们从 am_app_core.c 里剥出来，
 * 就能在宿主机上用 clang 直接编译同一份源码跑断言（tests/app_core），
 * 而不是"在设备上点一遍看看像不像对的"。
 *
 * ⚠ 本文件与 am_core_logic.c **不得** 引用 ESP-IDF / FreeRTOS / os.h 的任何头
 *   （注意：连在注释里写 "esp_ 星号 斜杠" 都会提前结束注释块）。
 */
#ifndef AM_CORE_LOGIC_H
#define AM_CORE_LOGIC_H

#include <stdbool.h>
#include <stddef.h>

#include "am_store_state.h"   /* am_st_t / am_st_btn：同样是纯 C */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AM_KIND_LUA = 0,     /* 单文件 Lua 应用（LAPP 头） */
    AM_KIND_LPK,         /* LPK 应用（manifest.app_id 为身份） */
    AM_KIND_BUILTIN,     /* 固件内置 C 应用 */
    AM_KIND__COUNT
} am_core_kind_t;

/* 安装位置（位掩码，可同时存在） */
#define AM_LOC_NONE   0x00
#define AM_LOC_FLASH  0x01
#define AM_LOC_TF     0x02

/* ───────── 身份 ─────────
 * 规范化：去首尾空白、去尾部扩展名、ASCII 转小写；中文原样保留。
 * 身份键**带类型前缀**（lua: / lpk: / builtin:）⇒ 同名 Lua 与 LPK 永不合并。 */
void am_core_norm(const char *in, char *out, size_t cap);
void am_core_make_key(am_core_kind_t kind, const char *raw, char *out, size_t cap);

/* 两个身份键是否同一个应用。⛔ 严格相等，不做任何模糊/前缀/近似匹配。 */
bool am_core_same(const char *key_a, const char *key_b);

/* 索引里的 type 是否属于**资源包**（asset / lpk）——资源不进应用列表。 */
bool am_core_type_is_res(const char *index_type);

/* ───────── 安装状态 ─────────
 * installed   本地确实装着（Flash 或 TF）
 * in_catalog  服务器目录里有这一条
 * idx_ready   索引**成功拉到过**（离线时为 false ⇒ 绝不能判"服务器已删除"）
 * build_*     包内声明的构建时间（定长数字串，字典序 == 时间序）
 * ver_*       退回用的数字版本号字符串
 *
 * 返回值语义（见 am_store_state.h 的按钮表）：
 *   未安装 + 目录有          → AM_ST_NONE    （按钮「获取」/「安装」）
 *   已装   + 目录有 + 有新版 → AM_ST_UPDATE
 *   已装   + 目录有 + 无新版 → AM_ST_INSTALLED（按钮「卸载」）
 *   已装   + 目录没有 + 索引就绪 → AM_ST_ORPHAN（「卸载」，⛔ 不给「获取」）
 *   已装   + 索引没就绪       → AM_ST_INSTALLED（离线不当成下架）
 */
am_st_t am_core_decide(bool installed, bool in_catalog, bool idx_ready,
                       const char *build_local, const char *ver_local,
                       const char *build_remote, const char *ver_remote);

/** 服务器版本是否**比本地新**（未安装时用来区分「安装」还是「更新」）。
 *  build 优先，退回 ver；任一侧缺失 → false（不凭空造更新）。 */
bool am_core_remote_newer(const char *build_local, const char *ver_local,
                          const char *build_remote, const char *ver_remote);

/* ───────── 列表投影 ───────── */
/* 应用中心：有效 + 未隐藏 + 真装好了 + 存储可访问。未安装的 LPK 安装包不进。 */
bool am_core_center_ok(bool valid, am_core_kind_t kind, unsigned loc,
                       bool storage_ok, bool center_hide);

/* 商店：有效 + 不是内置 C 应用（内置不进商店）。 */
bool am_core_store_ok(bool valid, am_core_kind_t kind);

/* ───────── 注册表同步判据（os_app 对账） ─────────
 * 2026-11-23: 应用注册表只登记**真的装着**的应用。只在商店目录里、
 * 本地没有文件的条目（loc == AM_LOC_NONE）不得进入注册表 —— 否则它会以
 * "幽灵应用"的形式出现在主菜单/应用管家/分类列表里，且删不掉
 * （典型：索引里残留旧名字，文件早被改名/删除了）。
 * 内置 C 应用由 apps_registry 注册，本函数对它们返回 false（不重复登记）。 */
bool am_core_reg_ok(bool valid, am_core_kind_t kind, unsigned loc);

typedef enum {
    AM_REG_DROP = 0,   /* 本轮扫描不存在 ⇒ 从注册表回收 */
    AM_REG_KEEP = 1    /* 保留（本轮出现过，或所在位置这次没扫到） */
} am_reg_action_t;

/* 回收判据（纯逻辑，宿主机可测）：
 *   seen_now       本轮扫描里还有它
 *   prev_loc       上一条记录声明的安装位置（找不到旧记录传 0）
 *   flash_scanned  本轮内置分区目录**全部**打开成功
 *   tf_scanned     本轮 TF 卡目录**打不开/未挂载** ⇒ false
 * 规则：本轮没出现时，只有"完全没有任何来源被扫过"或"它所在的那个来源这次
 * 没扫成"才保留；其余情况判定为真正被删除。⛔ 绝不因为某个目录读失败就把
 * 那一整片应用一起注销。 */
am_reg_action_t am_core_reg_action(bool seen_now, unsigned prev_loc,
                                   bool flash_scanned, bool tf_scanned);

#ifdef __cplusplus
}
#endif
#endif /* AM_CORE_LOGIC_H */
