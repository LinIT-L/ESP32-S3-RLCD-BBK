/**
 * lpk_manifest.h — 第三方应用包内 manifest.json 的读取与校验
 *
 * 为什么单独一层
 * --------------
 *   LPK1 容器本身只是"一堆带名字的字节"（见 lpk1.h）。谁是可启动应用、谁是资源包、
 *   入口在哪、图标用哪个 —— 全部由包内 `manifest.json` 决定。
 *   把这层抽出来的两个理由：
 *     ① 安装器、注册层、打包工具三方必须用**同一套判据**，否则又会出现
 *        "工具能打、设备打不开" 或 "中心有、桌面没有"；
 *     ② 纯 C、零 ESP 依赖 ⇒ 宿主机直接编译同一份源码跑单测（tests/lpk），
 *        不用在真机上试错。
 *
 * 与容器的关系（不改 LPK1 二进制格式）
 * ------------------------------------
 *   manifest.json 与 icon.bin 都是**普通条目**，直接放进包的资源区：
 *     · 不需要新增字段、不需要 version=2、旧读取器完全兼容；
 *     · 设备端用 lpk_find_ci() 按名字取，和读 ABC.MKF 没有任何区别。
 *   本模块只做三件事：取出来 → 解析 → 校验（含"声明的入口/图标真的在包里吗"）。
 *
 * 安全边界（这些都是"包能生效"的必要条件，缺一即拒绝安装）
 * --------------------------------------------------------
 *   · schema_version 必须为 1
 *   · app_id 形如 com.lintos.recorder：小写、点分段、无空段、无 '..'
 *   · package_type ∈ {app, resource}
 *   · app 包必须有 runtime 与 entry；resource 包不参与启动
 *   · 路径一律相对、'/' 分隔，禁止绝对路径 / '..' / 反斜杠 / 控制字符
 *   · manifest 自身 ≤ LPK_MF_MAX（防止用超大 JSON 吃内存）
 *   · 声明了 icon 就必须在包里，且长度必须为 512（64×64 1bpp）
 */
#ifndef LPK_MANIFEST_H
#define LPK_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LPK_MF_NAME     "manifest.json"
#define LPK_MF_MAX      8192      /* manifest 自身大小上限（真实包 ≈ 600 B） */
#define LPK_MF_ICON_SZ  512       /* 64×64 1bpp，在包内必须是这个长度 */

/* manifest 字段上限。全部定长，避免解析时 malloc。 */
typedef struct {
    int      schema_version;
    char     app_id[64];
    char     name[48];          /* 英文/ASCII 名（可空） */
    char     label[48];         /* 中文显示名（必须非空） */
    char     description[128];
    char     category[16];      /* 见 lpk_mf_cat_id()：软件/游戏/学习/运维/系统/工具/壁纸 */
    char     package_type[12];  /* app | resource */
    char     runtime[12];       /* lua | native | engine （app 包必填） */
    char     entry[96];         /* app 包必填，包内相对路径 */
    char     version[16];
    char     build[16];         /* YYYYMMDDHHMM，12 位定长数字，字典序==时间序 */
    char     author[32];
    char     icon[48];          /* 包内相对路径，可空（空 = 用系统默认图标） */
    char     resource_type[16]; /* resource 包：font/theme/rom/gamedata/shared */
    char     install_target[64];/* resource 包建议值；固件另有白名单，不盲信 */
    char     min_firmware[16];
    bool     has_dependencies;  /* dependencies[] 非空 */
    bool     has_permissions;   /* permissions[] 非空 */
} lpk_mf_t;

/* 返回码（负值一律"安全拒绝"） */
enum {
    LPK_MF_OK        = 0,
    LPK_MF_E_ARG     = -1,   /* 参数非法 */
    LPK_MF_E_OPEN    = -2,   /* 包打不开 / 不是 LPK */
    LPK_MF_E_NOENT   = -3,   /* 包里没有 manifest.json */
    LPK_MF_E_BIG     = -4,   /* manifest 超过 LPK_MF_MAX */
    LPK_MF_E_JSON    = -5,   /* JSON 结构不合法（非对象/未闭合） */
    LPK_MF_E_FIELD   = -6,   /* 必填字段缺失或取值非法 */
    LPK_MF_E_PATH    = -7,   /* 包内路径不合法 */
    LPK_MF_E_NOENTRY = -8,   /* 声明的 entry 不在包里 */
    LPK_MF_E_NOICON  = -9,   /* 声明的 icon 不在包里 */
    LPK_MF_E_ICON    = -10,  /* icon 长度不是 512 */
};

/* 读 + 解析 + 校验（一站式）。why 用于回报具体原因（可为 NULL）。 */
int  lpk_mf_load(const char *lpk_path, lpk_mf_t *out, char *why, size_t why_cap);

/* 从内存里的 JSON 文本解析 + 校验（安装后扫描 installed/<app_id>/manifest.json 用）。
 * 与 lpk_mf_load 走**同一份** mf_parse/validate，保证"包里能过、装完也能过"。 */
int  lpk_mf_parse(const char *json, size_t len, lpk_mf_t *out, char *why, size_t why_cap);

/* 只校验结构，不碰文件（打包工具/单测用同一份规则）。 */
int  lpk_mf_validate(const lpk_mf_t *m, char *why, size_t why_cap);

/* 把 manifest 里的中文分类归一到稳定 ID（程序只认这个，不认中文）。
 *   "软件"/"程序"/"工具"? 见实现：工具与程序都归一为 software，
 *   因为设备顶部子分类把它们放在同一栏（app_match_sub 的 PROG 分支）。 */
const char *lpk_mf_cat_id(const char *cat);

/* 该包是否可启动（app + runtime 有效）。resource 包一律 false。 */
bool lpk_mf_launchable(const lpk_mf_t *m);

#ifdef __cplusplus
}
#endif
#endif /* LPK_MANIFEST_H */
