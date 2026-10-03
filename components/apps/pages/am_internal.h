/**
 * @file am_internal.h
 * @brief 应用中心 (app_mgr) 模块内部共享接口 —— 仅供本目录下 am_*.c / page_app_manager.c 使用。
 *
 * 模块划分 (2026-09-20 重构, 原 1648 行单文件):
 *   page_app_manager.c  页面主逻辑: 分类/子分类、网格与列表渲染、触摸与按键分发
 *   am_store.c          商店: 扫包 / 安装 / 卸载
 *   am_lua.c            Lua 单文件程序扫描 (挂到各分类列表里)
 *
 * 2026-09-22: **page_main.c (主菜单) 也引用本头** —— 主菜单要能启动 Lua 应用
 * (否则 Lua 应用加不进主菜单: 启动时没人给它设 boot 路径)。
 *
 * 约定: 这几个文件之间**只经本头文件交互**; 各文件自己的 static 状态不出文件。
 */
#ifndef AM_INTERNAL_H
#define AM_INTERNAL_H

#include <stdbool.h>
#include "os.h"
#include "icons_main_enum.h"   /* ICON_* 语义化图标 id (os_pages.c 等共用) */

/* ==== 左侧分类 (主文件与商店/子模块共用) ====
 * ⚠ 枚举值 = 左栏显示顺序: 渲染按 am_cat_cy(值) 定行位, 图标按 s_cat_icon_map[值] 取.
 * 用户规范 2026-09-28: 顺序由 引擎/应用/运维/商店 → 引擎/运维/应用/商店 (应用与运维换位). */
#define AM_CAT_ENGINE  0   /* 引擎 (含原"独立游戏": 手柄/修改机) */
#define AM_CAT_OPS     1   /* 运维 */
#define AM_CAT_APP     2   /* 应用 */
#define AM_CAT_STORE   3   /* 商店 (本地应用包, 以后放新包) */
#define AM_CAT_COUNT   4

/* ==================== 主文件 (page_app_manager.c) 提供 ==================== */
int  am_cat(void);                     /* 当前左侧分类 (AM_CAT_*) */
int  am_sub(void);                     /* 当前顶部子分类 */
bool am_push_app(const os_app_t *a);   /* 列表尾追加 (超出上限返回 false) */
void am_rebuild_keep(void);            /* 重建列表并保留原滚动位置 */
/* 当前顶部子分类是否要这个 cat (程序/软件→软件, 系统/工具→系统, 游戏, 壁纸, 学习…)。
 * Lua 文件项与 C 注册项**走同一套判据**, 避免两处各写一份导致分类不一致。 */
bool am_sub_want_cat(const char *cat);

/* ==================== 商店 (am_store.c) ==================== */
void  am_store_prefetch(void);            /* 启动后台扫包 (幂等; 进页面/切商店分类时调用) */
bool  am_store_tick(void);                /* 每帧取货; 返回 true = 数据有更新(调用方应重建列表) */
bool  am_store_ready(void);               /* 扫描完成, 可列出 */
int   am_store_count(void);
const os_app_t *am_store_app(int k);      /* 第 k 个包的应用条目 */
bool  am_store_owns(const os_app_t *a);   /* a 是否商店里的包 */
bool  am_store_installed_of(const os_app_t *a);   /* 已装? (读缓存, 渲染可安全每帧调用) */
const char *am_store_fname_of(const os_app_t *a); /* 包文件名; 非商店包返回 NULL */
const char *am_store_desc_of(const os_app_t *a);  /* 包简介 (LAPP desc=); 无则 NULL (商店行第二行显示它) */
void  am_store_activate(ui_ctx_t *ctx, const os_app_t *a);      /* 已装→运行 / 未装→弹安装确认 */
void  am_store_toggle(ui_ctx_t *ctx, const os_app_t *a);        /* 商店行: 只装/卸, 不启动应用 */
void  am_store_uninstall_ask(ui_ctx_t *ctx, const os_app_t *a); /* 长按 → 弹卸载确认 */
void  am_store_invalidate(void);          /* 目录变化 → 下次重扫 */

/* ==================== Lua 程序扫描 (am_lua.c) ==================== */
void am_lua_prefetch(int cat);            /* 切分类时启动后台扫描 (幂等) */
bool am_lua_tick(void);                   /* 每帧取货; true = 数据有更新 */
bool am_lua_ready(int cat);               /* 该分类的 Lua 数据是否已就绪 */
void am_lua_append(bool ops_only);        /* 把已就绪的 Lua 项追加进列表
                                           * (ops_only=false 时**还会按当前子分类过滤 cat**) */
/* 若 a 是 Lua 文件项 → 设好启动路径并进入 Lua 页, 返回 true; 否则返回 false (调用方走普通 os_push)。
 * **应用管家与主菜单共用**这一处实现: 主菜单也要能启动 Lua 应用 (原先只有管家能 → 长按加不进主菜单)。 */
bool am_lua_open_if_entry(ui_ctx_t *ctx, const os_app_t *a);
bool am_lua_is_entry(const os_app_t *a, const char **path_out); /* 是否 Lua 项(回填启动路径) */
void am_lua_invalidate(void);             /* 目录变化 → 下次重扫 */
/* 按路径启动 LAPP (只知道文件路径的入口, 如商店"已装 → 运行"): 会现读包内 `need=wifi`
 * 并走同一套 Wi-Fi 门禁。普通 Lua 文件 (没打包) 等同直接进 Lua 页。 */
void am_lua_open_path(ui_ctx_t *ctx, const char *path);
/* 注册 Wi-Fi 门禁全局服务 (`need=wifi` 应用: 未联网时先弹窗连网再启动)。
 * 必须在开机注册阶段调用一次 (os_page_app_manager_register 里调)。 */
void am_lua_gate_register(void);

/* ==================== 崩溃定位探针 (2026-09-28) ====================
 * 现象: 第一次打开 Lua 应用即整机重启, 串口在复位瞬间丢字抓不到 panic.
 * 做法: 把"复位前走到哪一步"写进 RTC 保留内存 (复位不清零), 开机由 apps_register_all
 * 打印; 同时打印本次复位原因 (区分 panic/看门狗/掉电/软复位). */
void lua_probe_stage(int stage, const char *info);
void lua_probe_boot_report(void);

#endif /* AM_INTERNAL_H */
