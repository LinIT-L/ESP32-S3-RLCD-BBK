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
#include <stdint.h>
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

/* ==================== 商店 (am_store.c) ====================
 * ⚠ 2026-11-03: 应用（Lua/LPK）的发现与安装状态已经全部归 `am_app_core`。
 *   下面这些接口是**页面用的薄层**：列表投影 + 确认框 + 下载/安装/卸载动作。
 *   已删除：am_store_id_of / am_store_matchkey_of / am_store_activate
 *          （身份由 am_core_key_of 唯一给出，不再有第二套合并键）。 */
void  am_store_prefetch(void);            /* 触发一次重扫 + 按需刷新索引 */
bool  am_store_tick(void);                /* 每帧取货; 返回 true = 数据有更新 */
bool  am_store_ready(void);               /* 数据已就绪, 可列出 */
int   am_store_count(void);               /* 商店条目 = 应用段(核心) + 资源段 */
const os_app_t *am_store_app(int k);
bool  am_store_owns(const os_app_t *a);   /* a 是否商店管理的条目（应用或资源） */
bool  am_store_installed_of(const os_app_t *a);
const char *am_store_fname_of(const os_app_t *a); /* 包文件名; 非商店条目返回 NULL */
/* ★ 2026-11-01: 商店条目的**唯一状态来源**（定义见 am_store_state.h）。
 *   按钮文案/第二行文案/详情/点击行为全部由它派生，任何地方都不许再自己判一遍。 */
#include "am_store_state.h"
am_st_t am_store_state_of(const os_app_t *a);
/* 商店行第二行文案: 应用 → LAPP 简介 / 包大小; 资源 → 索引 desc / 大小 */
const char *am_store_sub_of(const os_app_t *a);
/* 商店行右侧按钮文案 —— 由 am_store_state_of() 派生: 获取/安装/更新/卸载/进行中 */
const char *am_store_btn_of(const os_app_t *a);
void  am_store_toggle(ui_ctx_t *ctx, const os_app_t *a);        /* 商店行: 只装/卸, 不启动应用 */
/* 商店行「详情」弹窗: 点行(右侧按钮以外)时显示分类/版本/作者/大小/状态/说明 */
void  am_store_detail(ui_ctx_t *ctx, const os_app_t *a);
void  am_store_uninstall_ask(ui_ctx_t *ctx, const os_app_t *a); /* 长按/按钮 → 弹卸载确认 */
void  am_store_uninstall_row(ui_ctx_t *ctx, const os_app_t *a); /* 长按入口（与应用/资源分流） */
void  am_store_invalidate(void);          /* 目录变化 → 下次重扫 */

/* ==================== 商店「云端」 (am_store_net.c, 2026-10-04) ====================
 * 商店从"只扫本地"升级为"联网拉索引 + 一键下载"。
 * 索引 = <mirror>/index.json (由 tools/store_build.py 生成), 里面**只存相对 path**
 * 并内嵌 mirrors[] —— 加源/换源只改服务器上的 json, 固件一行都不用动。
 * 分工: 本模块只管"拿网络数据 + 落盘到商店源目录"; 列表/确认框/安装全在 am_store.c。
 * 下载完成后**完全走既有流程**: am_store_invalidate() → 重扫 → 它就变成"本地包"了。
 * ⚠ `am_rpkg_t` / AM_RPKG_MAX / AM_MIRROR_* 定义在 am_store_json.h (纯 C, 可宿主单测)。 */
#include "am_store_json.h"

/* 状态机: 0=空闲 1=拉索引中 2=就绪 3=拉索引失败 4=下载中 5=下载成功 6=下载失败 */
enum { ARS_IDLE = 0, ARS_FETCHING, ARS_READY, ARS_FAIL, ARS_DOWNLOADING, ARS_OK, ARS_DLFAIL };

void  am_rstore_prefetch(void);          /* 已联网且没拉过 → 后台拉索引 (幂等) */
bool  am_rstore_tick(void);              /* 商店页每帧调; true = 列表该重建 (边沿触发) */
void  am_rstore_ack(void);               /* UI 处理完 OK/DLFAIL 后回落到 READY */
int   am_rstore_state(void);             /* ARS_* */
int   am_rstore_count(void);
/* ★ 2026-11-01: 索引**内容**代次（每解析出一份新的 pkgs[] 就 +1）。
 *   商店的云端视图按它重建 —— 只比条数会漏掉"条数不变但内容换了"的情况。 */
uint32_t am_rstore_gen(void);
const am_rpkg_t *am_rstore_get(int i);
void am_rstore_clear_cache(void);
int  am_store_dl_permille_of(const os_app_t *a);   /* ★ 该条目下载进度(无则 -1) */
bool am_store_auto_install(const char *fname);
bool am_store_take_uninst_done(void);

/* 2026-10-30: 云端索引刚被替换成新的一份（取一次即清）。
 * 页面 poll 拿到后重建列表并置脏 —— 否则新上架的资源要重进商店才看得见。 */
bool am_store_take_idx_updated(void);

/* ★ 2026-10-31: 应用在 TF 卡 ↔ 内置 之间移动。
 *   app_where: 0=未安装 1=内置 2=TF卡
 *   move_to_internal / move_to_sd: 成功 true；失败 err 给中文原因（空间不足/源丢失/校验不符）。
 *   都是「复制 → 校验大小 → 删源」，任何一步失败都不会丢原文件。 */
int  am_store_app_where(const os_app_t *a);   /* 0=未安装 1=内置 2=TF卡 */
bool am_store_movable(const os_app_t *a);     /* 只有 TF 上的 Lua 应用能搬家 */
uint32_t am_store_internal_free(void);
bool am_store_move_to_internal(const os_app_t *a, char *err, size_t errlen);
bool am_store_move_to_sd(const os_app_t *a, char *err, size_t errlen);

/* 长按确认框：问"移动到内置空间?"，确认后执行移动。 */
void am_move_internal_ask(ui_ctx_t *ctx, const os_app_t *a);
void am_store_refresh_index_if_due(void);   /* ★ 进商店时按需刷新索引 */
void am_rstore_refresh_force(void);
bool am_rstore_refresh_due(void);   /* ★ 卸载刚完成(取一次即清) */   /* ★ 下载完自动安装 */   /* ★ 清商店缓存(长按分类图标 3s) */
bool  am_rstore_dl_start(int i);         /* 后台下载第 i 个包 → 商店源目录 */
int   am_rstore_dl_permille(void);       /* 0..1000; -1 = 总长未知 */
const char *am_rstore_dl_name(void);     /* 正在下载的包名 */
const char *am_rstore_dl_label(void);    /* 正在下载的中文名 */
int   am_rstore_dl_err(void);            /* 1=HTTP 2=连接 3=写盘 4=校验 */
int   am_rstore_dl_index(void);          /* 正在/刚下载完的包在索引里的下标; -1 = 无 */

/* ==================== 统一「下载任务」视图 (2026-10-31) ====================
 * 背景: 字体包 / Lua 包 / 其它商店资源原来各自拼 UI —— Lua 只有行内百分比,
 *   字体包连"安装中"阶段都看不出来。这里把**任务状态**收敛成一份只读快照,
 *   页面（商店任意分类）只负责把它画出来, 不再关心下的是什么。
 *
 * 契约:
 *   · 下载任务跑在独立任务里, 与页面是否可见无关 ⇒ 切分类/退页面都不影响进度;
 *   · am_dl_view() 只读共享状态, 可在渲染路径每帧调用 (不分配内存、不加锁);
 *   · DONE/FAIL 会保留 1.5s / 3s 供用户看清结果, 之后由 am_rstore_tick() 回收成 IDLE。
 */
enum {
    AM_DLP_IDLE = 0,   /* 无任务 */
    AM_DLP_CONNECT,    /* 连接中 (DNS/TLS/等首包) */
    AM_DLP_DOWNLOAD,   /* 下载中 */
    AM_DLP_INSTALL,    /* 安装中 (资源包解包 / 落盘校验) */
    AM_DLP_DONE,       /* 完成 */
    AM_DLP_FAIL,       /* 失败 */
};
typedef struct {
    int      phase;        /* AM_DLP_* */
    int      permille;     /* 0..1000; <0 = 总长未知 */
    uint32_t got;          /* 已下载字节 */
    uint32_t total;        /* 预期总字节; 0 = 未知 */
    uint32_t bps;          /* 瞬时速度 字节/秒; 0 = 未知 */
    bool     asset;        /* true = 资源包(字体等), 有独立"安装中"阶段 */
    /* ★ 2026-10-31: 多文件直接下载的"文件计数"进度。
     *   total_files == 0 表示这次不是多文件安装(zip 包/普通 .lua), 界面别显示。 */
    int      done_files;
    int      total_files;
    char     label[48];    /* 中文名 (可能为空) */
    char     err[40];      /* 失败原因 (phase==FAIL 时有效) */
} am_dl_view_t;

/* true = 当前有一个下载任务(含刚完成/刚失败)。out 可为 NULL (只问"有没有")。 */
bool am_dl_view(am_dl_view_t *out);
/* 失败原因码 → 中文短句 (供 toast / 行内提示复用)。 */
const char *am_dl_err_text(int err);

/* ==================== Lua 应用运行入口 (am_lua.c) ====================
 * ⚠ 2026-11-03: 扫描/列表/注册职责已删除（全部归 am_app_core）。
 *   本模块现在只负责"把某个条目启动起来"：设 boot 路径 + need=wifi 门禁 + 进 Lua 页。
 *   已删除：am_lua_prefetch / am_lua_tick / am_lua_ready / am_lua_append / am_lua_invalidate */
/* 若 a 是 Lua/LPK 条目 → 设好启动路径并进入 Lua 页, 返回 true；
 * 内置 C 应用返回 false（调用方走 os_push(a->page)）。**应用管家与主菜单共用**这一处实现。 */
bool am_lua_open_if_entry(ui_ctx_t *ctx, const os_app_t *a);
bool am_lua_is_entry(const os_app_t *a, const char **path_out); /* 是否可启动的脚本项(回填路径) */
/* 按路径启动 LAPP (只知道文件路径的入口): 会现读包内 `need=wifi` 并走同一套 Wi-Fi 门禁。 */
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
