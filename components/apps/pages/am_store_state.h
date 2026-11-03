/**
 * am_store_state.h — 应用商店「单一条目状态」判定（纯 C，零 ESP/OS 依赖）
 *
 * 一句话：商店里一行的状态**只允许有一个来源**。按钮文案、第二行文案、
 * 详情弹窗、点击行为全部从这里派生，任何一处都不得自己再判一遍。
 *
 * 为什么单独成文件
 * ----------------
 * 以前同一个应用的状态在四处各判一次（按钮 / 详情 / 点击 / 卸载），判据还不一样：
 *   · 按钮看 `am_asset_installed()`（按 target 目录 + .installed 标记）
 *   · 点击看 `am_pkg_installed()`（按 .lua 文件是否存在）
 *   · 卸载按 `.lpk` 后缀一律走新 registry
 * ⇒ 同一行能同时出现"已安装"和"未安装"两种结论。现在把判定收敛到本文件，
 *   并且**不带任何文件系统调用**，所以能在宿主机上把状态表和版本比较全打一遍
 *   （tests/store_state/），而不是靠"看着像对的"。
 *
 * 状态表（linit 2026-11-01 定义，不许自行增删语义）
 *   未安装              → 下载
 *   已安装·服务器没更新   → 打开   (2026-11-02 由「卸载」改为「打开」；卸载改为长按行)
 *   已安装·商店有新版本  → 更新   (linit 2026-11-01: 文案定稿「更新」，点了直接用服务器版本替换)
 *   下载中              → 下载中
 *   安装中              → 安装中
 *   更新中              → 更新中
 *   卸载中              → 卸载中
 *   安装失败            → 回到 下载 / 更新（由"任务结束 + 重新判一次"自然得到）
 */
#ifndef AM_STORE_STATE_H
#define AM_STORE_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AM_ST_NONE = 0,        /* 未安装                        → 获取   */
    AM_ST_INSTALLED,       /* 已安装且与商店版本一致         → 已安装 */
    AM_ST_UPDATE,          /* 已安装但商店有更新版本         → 更新   */
    AM_ST_ORPHAN,          /* 已安装但**服务器已无此条**      → 卸载   */
    AM_ST_DL,              /* 正在下载（首次安装）           → 下载中 */
    AM_ST_INSTALLING,      /* 正在安装                       → 安装中 */
    AM_ST_UPDATING,        /* 正在下载/安装新版本            → 更新中 */
    AM_ST_UNINSTALLING,    /* 正在卸载                       → 卸载中 */
    AM_ST__COUNT
} am_st_t;

/* 任务类型（叠加到基础状态上，不改变基础状态本身的含义） */
typedef enum {
    AM_TASK_NONE = 0,
    AM_TASK_DL,            /* 下载中（装新应用） */
    AM_TASK_INSTALL,       /* 下载完成后的落盘/解包阶段 */
    AM_TASK_UPDATE,        /* 更新（下载或落盘） */
    AM_TASK_UNINSTALL
} am_st_task_t;

/* ---- 版本比较 ----
 * 规则（2026-10-30 linit 发版规则，此处是**唯一实现**）：
 *   ① 云端有 build 且本地有 build → strcmp(build) > 0 即"更新"。
 *      build 是 12 位定长数字 YYYYMMDDHHMM，字典序 == 时间先后，不用转数值。
 *   ② 云端有 build、本地没有（旧包 LAPP 头里没这个字段）→ 视为老体系，提示一次更新，
 *      让用户升到带 build 的新包（刻意的迁移行为：统一版本号后 ver 会变，若退回
 *      比 ver，老用户就永远收不到更新）。
 *   ③ 云端没有 build（老索引）→ 退回 rver > lver；lver==0 视为 1（未知版本按最低算）。
 * 返回 1 = 商店版本更新；0 = 一致；-1 = 本地更新（正常不会出现）。
 * ⚠ 不能只比字符串形式的 ver，也不能只比文件大小 —— 那是"同版本不同构建"识别不出来的根源。 */
int am_st_ver_newer(const char *lbuild, unsigned lver,
                    const char *rbuild, unsigned rver);

/* 纯判定：给"已安装 + 本地版本 + 商店版本"，返回**基础**状态（不含任务叠加）。
 * installed=false 时忽略版本，恒返回 AM_ST_NONE。
 * ⚠ 2026-11-02 起本函数只作为**兜底**（索引里既没有 mtime 也没有 crc 时）。
 *   正常路径走下面的 am_st_decide_meta()。 */
am_st_t am_st_decide(bool installed,
                     const char *lbuild, unsigned lver,
                     const char *rbuild, unsigned rver);

/* ---- 唯一更新判据（2026-11-02 linit 定）----
 * 只认**服务器文件的实际修改时间**：安装成功时把索引里的 mtime 抄进安装记录
 * （绝不用本地下载时间），之后 `索引 mtime > 记录 mtime` 才算有新版本。
 *
 * 为什么必须带 crc/size：重新生成索引、重新打包但内容没变时 mtime 会变，
 * 那时应当**不提示更新**（内容一模一样），并把记录推进到新 mtime，
 * 否则用户会陷入"更新完还是更新"的死循环。这一条同时就是老应用的迁移通道：
 * 老应用没有记录，就用 size+CRC32 与索引核对，一致即认定"已是最新"并补记当前 mtime。
 *
 * 字段语义：
 *   has_rec/rec_*   安装时抄下来的商店信息（rec_mtime=0 表示老记录没有时间）
 *   loc_*           本地已装文件的实测值（只在需要核对/迁移时才算，可读不到）
 *   has_idx/idx_*   商店索引此刻给出的服务器文件信息
 *
 * backfill（可传 NULL）：返回 true 时调用方应把 idx_* 写进该应用的安装记录。
 *   —— 用于"老应用迁移"和"内容未变但 mtime 前移"两种情形，只补记录、不改状态。 */
typedef struct {
    bool     installed;
    bool     has_rec;
    uint32_t rec_mtime;      /* unix 秒 */
    uint32_t rec_crc;
    uint32_t rec_size;
    bool     loc_crc_ok;     /* 本地文件 crc 是否算出来了 */
    uint32_t loc_crc;
    uint32_t loc_size;
    bool     has_idx;
    uint32_t idx_mtime;      /* unix 秒; 0 = 索引没这个字段(老索引) */
    uint32_t idx_crc;
    uint32_t idx_size;
    /* ---- 2026-11-03: 「本地独有」判定所需的两个门 ----
     * orphan_ok : 这一行代表**设备上真实装着的商店应用**（本地行），
     *             只有这种行才允许得出"服务器已删除"的结论；
     *             云端行永远不设，免得把"索引解析失败"误判成"应用已下架"。
     * idx_ready : 商店索引**已经成功拉到过**（count>0）。离线/首次进店时为 false,
     *             此时 has_idx=false 只代表"还不知道", ⛔ 不能当成"服务器没有"。 */
    bool     orphan_ok;
    bool     idx_ready;
} am_st_meta_t;

am_st_t am_st_decide_meta(const am_st_meta_t *m, bool *backfill);

/* 把"正在执行的任务"叠加到基础状态上。base 必须是 am_st_decide() 的结果。
 *   task=AM_TASK_DL        → AM_ST_DL
 *   task=AM_TASK_INSTALL   → 首次装 AM_ST_INSTALLING / 更新 AM_ST_UPDATING
 *   task=AM_TASK_UPDATE    → AM_ST_UPDATING
 *   task=AM_TASK_UNINSTALL → AM_ST_UNINSTALLING
 * 其他 → base。 */
am_st_t am_st_with_task(am_st_t base, am_st_task_t task);

/* 状态 → 按钮文案（UTF-8）。未知状态返回"下载"，绝不返回 NULL。 */
const char *am_st_btn(am_st_t st);

/* ---- 2026-11-03: 显示名择优（纯 C，宿主机可测）----
 * 规则：元数据中文名 > 英文名 > 文件名。返回第一个非空项；全空返回 ""（绝不 NULL）。
 * ⚠ 显示名**不承担身份职责** —— 身份只由 am_app_core 的 key 决定。 */
const char *am_st_display_name(const char *meta_zh, const char *meta_en,
                               const char *fname);

/* 该状态是否"任务进行中"（点它不应该再起第二个任务）。 */
bool am_st_btn_busy(am_st_t st);

/* ---- registry/<app_id>.txt 解析（纯字符串，不碰文件系统）----
 * 文件内容由 am_lpkg.c 写，形如：
 *   app_id=com.lintos.recorder
 *   label=录音机
 *   version=2          ← 包内 manifest 自称的版本
 *   build=202610310919
 *   store_ver=3        ← 商店交付这一份时的索引版本（更新判定用这一对）
 *   store_build=202611011048
 *   store_mtime=1762000000   ← ★ 2026-11-02: 安装时服务器文件的实际修改时间(unix 秒)
 *   store_crc=824595c1       ← ★ 同上: 安装时服务器文件的 crc32(十六进制)
 *   store_size=221453        ← ★ 同上: 安装时服务器文件的字节数
 *   package_type=app
 *   runtime=lua
 *   entry=app/entry.lua
 *   source=/sdcard/apps/staging/com.lintos.recorder.lpk
 *
 * 为什么要有 store_ver/store_build 两行：
 *   服务器生成索引时如果**没有重新打包**（包体没变、只把索引里的 ver/build 抬了），
 *   包内 manifest 就会比索引旧。若只拿 manifest 版本去比，用户"更新成功"之后
 *   商店仍然显示「更新」—— 点一次下一次，永远退不掉。
 *   这里把"我是按索引的哪一版装进来的"如实记下来：更新判定用 store_*（与商店同一口径），
 *   两份不一致时由 am_lpkg_info() 报 WARN，提醒去重新生成索引，而不是静默糊过去。
 *
 * 只认 `键=值` 行；未知键忽略；同一键重复出现以**第一次**为准（后写的补丁行不覆盖）。
 * 返回 true = 至少解析出 app_id（其余字段可能为空，由调用方决定够不够用）。 */
typedef struct {
    char app_id[56];
    char label[48];
    char ver[16];
    char build[16];
    char store_ver[16];
    char store_build[16];
    char store_mtime[16];   /* ★ 2026-11-02: 安装时服务器文件 mtime (unix 秒, 十进制) */
    char store_crc[16];     /* ★ 同上: 安装时服务器文件 crc32 (十六进制) */
    char store_size[16];    /* ★ 同上: 安装时服务器文件字节数 (十进制) */
    char pkg_type[16];
    char runtime[16];
    char entry[128];
    char source[128];
} am_st_reg_t;

bool am_st_reg_parse(const char *txt, size_t len, am_st_reg_t *out);

/* ---- 代次守卫：防"旧异步结果覆盖新状态" ----
 * 典型场景：进入商店时后台判一次"已安装"，用户中途装了新版本，旧的查询结果晚到，
 * 如果直接写回去就会把刚确认的"已安装"覆盖成"未安装"。
 * 规则：每个状态单元带一个代次；写入方必须带上自己**发起时**的代次，
 *       小于单元当前代次的写入被丢弃，同代次允许写（同代次内部按到达顺序最后者胜）。
 * —— 这就是"异步查询必须防止旧请求覆盖新结果"的可测实现。 */
typedef struct {
    uint32_t gen;
    int32_t  val;
    bool     valid;
} am_st_cell_t;

/* 作废该单元（代次 +1，valid 清掉）。返回新代次。 */
uint32_t am_st_cell_invalidate(am_st_cell_t *c);

/* 带代次写入。返回 true = 已写入；false = 该结果已过期，被丢弃。 */
bool am_st_cell_put(am_st_cell_t *c, uint32_t gen, int32_t val);

#ifdef __cplusplus
}
#endif
#endif /* AM_STORE_STATE_H */
