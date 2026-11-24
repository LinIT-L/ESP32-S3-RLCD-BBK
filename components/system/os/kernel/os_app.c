/**
 * os_app.c — 应用注册表.
 *
 * 统一登记所有"可启动应用" (主菜单/应用管理共用同一张表).
 * 支持运行时注册 (os_app_register), 为 WASM 应用商店下载安装后动态加入主菜单预留.
 * 私有 state 全部 static 留本文件.
 */
#include "os.h"
#include <sys/stat.h>
#include <dirent.h>
#include "touch_panel.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "svc_nvs.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "sd_arb.h"     /* ★ 2026-11-24: SD 访问仲裁 */
#include <string.h>

#define TAG "OSAPP"

#define OS_APP_MAX 96   /* 2026-11-01: 34 内置 + TF 卡 Lua 最多 48 ⇒ 64 会溢出 */
#define APPN_NS "os_apps"
static const os_app_t *s_apps[OS_APP_MAX];
static int s_app_count = 0;
static bool s_hidden_visible = false;
/* 桌面图标增删代次 (用户"添加/移出主菜单"时 +1, 见 os_app_vis_gen) */
static uint32_t s_vis_gen = 1;
/* ★ 2026-11-23: 同步序号 + 每条被 sync 触及的序号。
 *   prune 只回收"序号落后于本轮"的条目 ⇒ 一次扫描的结果就是最终结果，
 *   不需要调用方再传一份 id 数组（省一次 96×72B 的拷贝与比对）。 */
static uint32_t s_sync_epoch = 0;
static uint32_t s_app_epoch[OS_APP_MAX];

/* 隐藏应用开关 (赞助页 5 连点切换): 打开后主菜单显示隐藏的引擎/应用管理 */
static void hidden_load(void) {
    int32_t v = 0;
    nvs_handle_t h;
    if (svc_nvs_open(APPN_NS, NVS_READONLY, &h) == ESP_OK) {
        if (svc_nvs_get_i32(h, "show_hidden", &v) != ESP_OK) v = 0;
        svc_nvs_close(h);
    }
    s_hidden_visible = (v != 0);
}
bool os_app_hidden_visible(void) {
    static bool s_loaded = false;
    if (!s_loaded) { hidden_load(); s_loaded = true; }
    return s_hidden_visible;
}
void os_app_toggle_hidden_visible(void) {
    s_hidden_visible = !s_hidden_visible;
    nvs_handle_t h;
    if (svc_nvs_open(APPN_NS, NVS_READWRITE, &h) == ESP_OK) {
        svc_nvs_set_i32(h, "show_hidden", s_hidden_visible ? 1 : 0);
        svc_nvs_commit(h);
        svc_nvs_close(h);
    }
    ESP_LOGI(TAG, "隐藏应用: %s", s_hidden_visible ? "显示" : "隐藏");
}
/* ───────── requires 存在性: 同目录批量确认 (2026-11-21 linit) ─────────
 * 实测数据: 进桌面 on_enter 稳定 41~42ms, 全部来自这里 ——
 *   每个带 requires 的应用各做一次 stat(), TF 卡(1-bit 模式)实测 4.6ms/次,
 *   卡上 12 个 Lua 应用 ⇒ 12×4.6 ≈ 55ms(日志时间戳也逐条印证)。
 * 这些 requires 绝大多数落在同一个目录(/sdcard/apps/app)里 ⇒ 一次 opendir
 * 读目录项比 12 次独立目录查找便宜一个数量级; 再叠 3s TTL,
 * 连续进出桌面就是 0 次 IO。
 * ⚠ 只用于"存不存在"判断: 文件在 3s 内被删, 图标最多多留 3s;
 *   拔卡/格式化由 sd_scan 的挂载状态另行驱动重扫, 不依赖这里。
 * ⚠ 槽位必须 >1: requires 既有 /sdcard/apps/app/xx.lua, 也有
 *   /sdcard/apps/app/pal(资源包, 同一个目录) 和其它路径 —— 单槽会来回抖动、
 *   每次未命中都重扫, 反而更慢。
 * ⚠ 必须放**内部 RAM**: 这张表可能在开机很早(PSRAM 尚未就绪 / 未清零)就被查询,
 *   实测把它放 EXT_RAM_BSS_ATTR 时 dir 字段可能没有 NUL 结尾, 打印/比较时越界读到
 *   不可访问地址 ⇒ 启动阶段 panic 重启(整机静默挂死, USB 只枚举不响应)。
 *   缩到 3 槽 × 48 项 ≈ 7KB, 用内部 RAM 换"任何时刻都安全"。 */
#define APATH_SLOTS    3
#define APATH_DIR_MAX  96
#define APATH_ENT_MAX  48
#define APATH_NAME_LEN 48
#define APATH_TTL_MS   3000

typedef struct {
    char     dir[APATH_DIR_MAX];
    char     names[APATH_ENT_MAX][APATH_NAME_LEN];
    int      n;
    bool     trunc;      /* 目录项超过 APATH_ENT_MAX ⇒ 未命中必须回退 stat */
    bool     valid;
    uint32_t ms;
} apath_slot_t;

static apath_slot_t s_ap[APATH_SLOTS];
static int s_ap_lru;      /* 下一个要覆盖的槽 */
static SemaphoreHandle_t s_ap_mtx;

/* ⚠ 必须串行化: os_app_visible() 会被 UI 线程(进桌面/应用管家重建)和后台
 *   am_core 扫描任务同时调用 —— 实测同一次重建里同一目录被扫 3 次、24ms 全花在
 *   重复 IO 上, 就是两边互相把对方的缓存槽盖掉了。持锁期间做一次目录扫描(≈6ms),
 *   对另一个线程是"等 6ms"而不是"再扫一遍", 净收益仍然是省的。 */
static void apath_lock(void)
{
    if (!s_ap_mtx) s_ap_mtx = xSemaphoreCreateMutex();
    if (s_ap_mtx) xSemaphoreTake(s_ap_mtx, portMAX_DELAY);
}
static void apath_unlock(void)
{
    if (s_ap_mtx) xSemaphoreGive(s_ap_mtx);
}

static bool apath_scan(apath_slot_t *s, const char *dir)
{
    uint64_t t0 = (uint64_t)esp_timer_get_time();
    DIR *d = opendir(dir);
    if (!d) return false;
    /* 先整块清掉: 保证 dir/names 全是合法 C 字符串(str 越界读会 panic) */
    memset(s, 0, sizeof(*s));
    s->trunc = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0)))
            continue;                                   /* . / .. */
        if (s->n >= APATH_ENT_MAX) { s->trunc = true; break; }
        /* 手动截断而不是 snprintf("%s"): 文件名可能 > APATH_NAME_LEN, 截断后必然
         * 匹配不上 ⇒ 把 trunc 置上, 让未命中回退 stat, 不能静默漏掉一个存在的应用。 */
        {
            size_t nl = strlen(e->d_name);
            if (nl >= APATH_NAME_LEN) { s->trunc = true; nl = APATH_NAME_LEN - 1; }
            memcpy(s->names[s->n], e->d_name, nl);
            s->names[s->n][nl] = 0;
        }
        s->n++;
    }
    closedir(d);
    snprintf(s->dir, sizeof(s->dir), "%s", dir);
    s->ms    = (uint32_t)(esp_timer_get_time() / 1000);
    s->valid = true;
    /* 只在真的慢时报(LOG D): 诊断 TF 卡速度时开 debug 即可; info 级别每 3 秒刷两条
     * 目录扫描属于噪音。 */
    uint64_t us = (uint64_t)esp_timer_get_time() - t0;
    ESP_LOGD(TAG, "目录扫描 %s: %d 项 %llu us", dir, s->n, (unsigned long long)us);
    return true;
}

static bool apath_exists_raw(const char *path)
{
    if (!path || !path[0]) return false;
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) {                      /* 相对路径 / 根目录: 老实 stat */
        struct stat st;
        return stat(path, &st) == 0;
    }
    size_t dl = (size_t)(slash - path);
    if (dl >= APATH_DIR_MAX) {
        struct stat st;
        return stat(path, &st) == 0;
    }
    char dir[APATH_DIR_MAX];
    memcpy(dir, path, dl);
    dir[dl] = 0;
    const char *base = slash + 1;

    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    apath_slot_t *hit = NULL;
    for (int i = 0; i < APATH_SLOTS; i++) {
        apath_slot_t *s = &s_ap[i];
        if (!s->valid) continue;
        if (strcmp(s->dir, dir) != 0) continue;
        /* ⚠ 必须用**有符号**差值: s->ms 记的是扫描结束时刻, 同一批重建里紧接着的
         *   下一次查询 now 可能比它小几微秒 ⇒ 无符号相减会回绕成 40 多亿 ⇒
         *   每次都判"过期"重扫(实测同一目录一次重建扫 3 次, 24ms 全花在这)。
         *   有符号差值把这种"未来时间"算成负数, 正确地当作**未过期**。 */
        if ((int32_t)(now - s->ms) > (int32_t)APATH_TTL_MS) break;
        hit = s;
        break;
    }
    if (!hit) {
        apath_slot_t *s = &s_ap[s_ap_lru];
        if (!apath_scan(s, dir)) {
            struct stat st;
            return stat(path, &st) == 0;                /* 目录打不开(无卡): 老实 stat */
        }
        s_ap_lru = (s_ap_lru + 1) % APATH_SLOTS;
        hit = s;
    }
    for (int i = 0; i < hit->n; i++)
        if (strcmp(hit->names[i], base) == 0) return true;
    if (hit->trunc) {                                   /* 目录太大没扫全 */
        struct stat st;
        return stat(path, &st) == 0;
    }
    return false;
}

/* 对外入口: 全程持锁 —— 见 apath_lock() 的说明(UI 线程与 am_core 后台会并发调) */
static bool apath_exists(const char *path)
{
    apath_lock();
    /* ★ 2026-11-24: 只有 /sdcard 下的路径需要 SD 仲裁 —— 应用资源也可能在
     *   SPIFFS(/store), 那条路与 TF 卡无关, 不能被"卡没挂"连坐隐藏。
     *   本函数会被 UI 线程和 am_core 后台扫描任务同时调用 —— 正是"USB 交卡时
     *   后台还在 opendir/stat"的典型来源, 也是 sd_arb 要收进来的入口之一。 */
    bool need_sd = (path != NULL && strncmp(path, "/sdcard", 7) == 0);
    bool entered = need_sd ? sd_arb_enter() : true;
    bool r = entered ? apath_exists_raw(path) : false;
    if (need_sd && entered) sd_arb_leave();
    apath_unlock();
    return r;
}

/* 只判断资源就绪: 供应用管家列表过滤用 (不碰 hidden/touch_only/主菜单策略) */
bool os_app_available(const os_app_t *a) {
    if (!a) return false;
    if (a->requires && a->requires[0]) {
        /* ★ 2026-11-01: 与 os_app_visible() 保持一致 —— **存在即可**(文件或目录)。
         *   原来这里只认目录，而 TF 卡 Lua 应用的 requires 是 .lua **文件** ⇒
         *   应用中心列表用本函数过滤，结果"主菜单有、应用中心没有"。
         *   资源包类(目录)行为不变。 */
        if (!apath_exists(a->requires)) return false;
    }
    return true;
}

bool os_app_visible_with(const os_app_t *a, bool requires_ok) {
    if (!a) return false;
    /* ★ 2026-11-08 linit「应用中心分类切换卡」: 依赖存在性由调用方给出，本函数**不碰文件系统**。
     *   hidden / touch_only / 主菜单策略与 os_app_visible() 完全一致
     *   （那两个列表本身是 RAM 缓存，比较成本可忽略）。
     *   用途: 应用中心手里已有"扫描时确认过的文件存在性"(am_app_t.storage_ok)，
     *   切分类时不必对每条 Lua/LPK 再 stat 一次 TF 卡。 */
    if (a->requires && a->requires[0] && !requires_ok) return false;
    if (a->touch_only && !touch_panel_is_present()) return false;
    if (os_app_in_hidden(a->id)) return false;
    if (os_app_in_mainmenu(a->id)) return true;
    return !a->hidden || (os_app_hidden_visible() && a->reveal);
}

bool os_app_visible(const os_app_t *a) {
    if (!a) return false;
    /* ★★★ 2026-10-25 资源包类应用: 依赖目录不在 ⇒ 整条不显示。
     *   (仙剑这类"只装资源、装完才出现"的应用靠这一条判断, 每帧调, 装完立刻出现.) */
    if (a->requires && a->requires[0]) {
        /* ★ 2026-11-01: 原来只认**目录**（资源包类应用）。TF 卡上的 Lua 单文件应用
         *   的 requires 是一个 .lua 文件路径 —— 放宽成"存在即可"(文件或目录都算)，
         *   这样"卡里有就显示、拔卡就不显示"对两种形态都成立。
         *   原资源包(仙剑 requires=/sdcard/apps/app/pal)行为不变。
         * ★ 2026-11-21: stat() 换成按目录批量确认(apath_exists) —— 见上方说明。 */
        bool ok = apath_exists(a->requires);
        /* 诊断: 每个 requires 应用只打一次 —— LOG D, info 级别下每次开机刷 8 条属于噪音 */
        static char s_logged[8][24]; static int s_logged_n;
        bool seen = false;
        for (int i = 0; i < s_logged_n; i++)
            if (strncmp(s_logged[i], a->id, 23) == 0) { seen = true; break; }
        if (!seen && s_logged_n < 8) {
            snprintf(s_logged[s_logged_n++], 24, "%s", a->id);
            ESP_LOGD("OS_APP", "%s requires=%s → exists=%d ⇒ %s",
                     a->id, a->requires, (int)ok, ok ? "显示" : "隐藏");
        }
        if (!ok) return false;
    }
    /* 依赖已在上方确认 ⇒ 共用同一套策略（hidden / touch_only / 主菜单） */
    return os_app_visible_with(a, true);
}
int os_app_visible_count(void) {
    int n = 0;
    for (int i = 0; i < s_app_count; i++)
        if (os_app_visible(s_apps[i])) n++;
    return n;
}
const os_app_t *os_app_visible_at(int idx) {
    int n = 0;
    for (int i = 0; i < s_app_count; i++) {
        if (!os_app_visible(s_apps[i])) continue;
        if (n == idx) return s_apps[i];
        n++;
    }
    return NULL;
}

/* ==== 主菜单自定义管理 (应用管家): 用户显式加入/移出主菜单的 id 列表 ====
 * NVS "os_apps/mainmenu" = 逗号分隔的 id 列表 (用户显式"添加到主菜单");
 * NVS "os_apps/mmhide"   = 逗号分隔的 id 列表 (用户显式"移出主菜单").
 * 可见性 = (默认非隐藏 或 在mainmenu) 且 不在 mmhide. */
#define MAINMENU_KEY "mainmenu"
#define MMHIDE_KEY   "mmhide"
/* 加入/隐藏列表是**一个**逗号分隔串 (一个 NVS 键, 不受 NVS 15 字符键长限制)。
 * ⚠ 256 太紧: Lua 应用 id 形如 "lua:pomodoro.lua" (16 字符+逗号 ≈ 17B),
 *   装十几个 Lua 应用再逐个加进主菜单就会**静默截断** (strncat 到顶) → 后面的项丢失。
 *   2026-09-22 放宽到 512 (NVS str 上限 4000B, 够用)。 */
#define MAINMENU_MAX 512

/* ★★★ 2026-10-18 linit 反馈"切分类卡慢"的根因:
 *   原来**每次判断可见性都 nvs_get_str**(跨 flash 页, 2~5ms), 应用管家 20 个应用
 *   × 2 个列表 = 40+ 次 ⇒ 40~100ms 卡顿 ✗
 *   改为 **RAM 缓存**: 首次读入静态缓冲, 之后纯内存比对; 写列表时置脏。 */
/* ★ 2026-11-02: 缓存本身 1KB, 只在主循环/应用中心里做字符串比对, 不进 ISR、不做 DMA,
 *   移到 PSRAM 后内部 RAM 立省 1KB（命中后纯内存比较, 依然比 nvs_get_str 快两个量级）。 */
static EXT_RAM_BSS_ATTR char s_idlist_cache[2][MAINMENU_MAX];
static bool s_idlist_ok[2] = { false, false };
static int idlist_idx(const char *key) {
    if (strcmp(key, MMHIDE_KEY) == 0)   return 0;
    if (strcmp(key, MAINMENU_KEY) == 0) return 1;
    return -1;
}
static const char *idlist_get(const char *key) {
    int i = idlist_idx(key);
    if (i < 0) return "";
    if (!s_idlist_ok[i]) {
        size_t len = sizeof(s_idlist_cache[i]);
        s_idlist_cache[i][0] = '\0';
        nvs_handle_t h;
        if (svc_nvs_open(APPN_NS, NVS_READONLY, &h) == ESP_OK) {
            if (svc_nvs_get_str(h, key, s_idlist_cache[i], &len) != ESP_OK) s_idlist_cache[i][0] = '\0';
            svc_nvs_close(h);
        }
        s_idlist_ok[i] = true;
    }
    return s_idlist_cache[i];
}
static void idlist_dirty(const char *key) {
    int i = idlist_idx(key);
    if (i >= 0) s_idlist_ok[i] = false;
}

/* 读逗号分隔 id 列表, 判断 id 是否在其中 (走 RAM 缓存) */
static bool id_in_list(const char *key, const char *id) {
    if (!id) return false;
    const char *src = idlist_get(key);
    if (!src) return false;
    /* ★ 2026-10-20: 原来拷进 512 字节缓冲, 列表最长 1023 ⇒ 截断后漏判(应用被误隐藏)。
     *   这里只读不写, 直接在原串上扫, 彻底避免复制截断。 */
    const char *p = src;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == strlen(id) && strncmp(p, id, n) == 0) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}
/* 把 id 加入 (show=true) 或移出 (show=false) 某个逗号分隔列表 */
static void id_set_in_list(const char *key, const char *id, bool show) {
    if (!id) return;
    idlist_dirty(key);              /* ★ 写前失效, 保证读到最新的 NVS 内容 */
    char buf[MAINMENU_MAX];
    buf[0] = '\0';
    nvs_handle_t h;
    if (svc_nvs_open(APPN_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(buf);
        if (svc_nvs_get_str(h, key, buf, &len) != ESP_OK) buf[0] = '\0';
        svc_nvs_close(h);
    }
    bool already = id_in_list(key, id);
    if (show == already) return;
    char out[MAINMENU_MAX];
    out[0] = '\0';
    char *p = buf;
    while (*p) {
        char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (!(n == strlen(id) && strncmp(p, id, n) == 0)) {
            if (out[0]) strncat(out, ",", sizeof(out) - strlen(out) - 1);
            strncat(out, p, sizeof(out) - strlen(out) - 1);
        }
        if (!end) break;
        p = end + 1;
    }
    if (show) {
        if (out[0]) strncat(out, ",", sizeof(out) - strlen(out) - 1);
        strncat(out, id, sizeof(out) - strlen(out) - 1);
    }
    if (svc_nvs_open(APPN_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (out[0]) svc_nvs_set_str(h, key, out);
        else        svc_nvs_erase_key(h, key);
        svc_nvs_commit(h);
        svc_nvs_close(h);
    }
}

bool os_app_in_mainmenu(const char *id) { return id_in_list(MAINMENU_KEY, id); }
void os_app_set_mainmenu(const char *id, bool show) {
    id_set_in_list(MAINMENU_KEY, id, show);
    if (show) id_set_in_list(MMHIDE_KEY, id, false);   /* 加入时清除隐藏标记 */
    s_vis_gen++;                                        /* 主菜单要立刻重建可见缓存 */
    ESP_LOGI(TAG, "主菜单 %s: %s", show ? "加入" : "移出", id);
}
bool os_app_in_hidden(const char *id) { return id_in_list(MMHIDE_KEY, id); }
void os_app_set_hidden(const char *id, bool hide) {
    id_set_in_list(MMHIDE_KEY, id, hide);
    if (hide) id_set_in_list(MAINMENU_KEY, id, false);   /* 隐藏时清除加入标记 */
    s_vis_gen++;
    ESP_LOGI(TAG, "主菜单 %s: %s", hide ? "隐藏" : "恢复", id);
}
uint32_t os_app_vis_gen(void) { return s_vis_gen; }

static int app_find(const char *id)
{
    if (!id || !id[0]) return -1;
    for (int i = 0; i < s_app_count; i++)
        if (s_apps[i] && s_apps[i]->id && strcmp(s_apps[i]->id, id) == 0) return i;
    return -1;
}

/* 删除第 i 条（保持数组紧凑，序号一起搬） */
static void app_remove_at(int i)
{
    for (int j = i; j < s_app_count - 1; j++) {
        s_apps[j]      = s_apps[j + 1];
        s_app_epoch[j] = s_app_epoch[j + 1];
    }
    s_app_count--;
    s_apps[s_app_count]      = NULL;
    s_app_epoch[s_app_count] = 0;
    s_vis_gen++;
}

void os_app_register(const os_app_t *app)
{
    if (!app || !app->id || !app->id[0]) {
        ESP_LOGD(TAG, "register: bad app");
        return;
    }
    int i = app_find(app->id);
    if (i >= 0) {                       /* 同 id 覆盖 (商店更新安装) */
        s_apps[i] = app;
        s_app_epoch[i] = s_sync_epoch;
        ESP_LOGI(TAG, "app %s updated", app->id);
        return;
    }
    /* ⚠ 容量判断必须在"已存在"之后：表满时更新已有条目仍然要成功,
     *   否则"装了新应用却更新不了老的"（先注册的反而失效）。 */
    if (s_app_count >= OS_APP_MAX) {
        ESP_LOGW(TAG, "注册表已满(%d), 忽略 %s", OS_APP_MAX, app->id);
        return;
    }
    s_apps[s_app_count]      = app;
    s_app_epoch[s_app_count] = s_sync_epoch;
    s_app_count++;
    s_vis_gen++;
    ESP_LOGI(TAG, "app +%s (%s)%s", app->id, app->label, app->hidden ? " [隐藏]" : "");
}

int os_app_count(void)
{
    return s_app_count;
}

/* ★ 2026-11-23 linit 报障: 应用中心「运维」分类删不掉已卸载的 Lua 应用。
 *   根因 = 注册表只有 register、没有 unregister —— 条目一旦注册就永久留在
 *   s_apps[] 里, 文件删了也只是 requires 失效, 条目照旧出现在分类列表。
 *   这里补一个按 id 的移除, 由扫描换代方在"本轮已扫不到该条目"时调用。 */
void os_app_unregister(const char *id)
{
    int i = app_find(id);
    if (i < 0) return;
    app_remove_at(i);
    ESP_LOGW(TAG, "app -%s (已卸载, 从注册表移除)", id);
}

/* ════════════════════════════════════════════════════════════════════════
 * 对账式同步（详见 os.h 说明）
 * ------------------------------------------------------------------------
 * 现场问题（2026-11-23 定位）：
 *   · 注册表只加不减 ⇒ 删掉文件后条目永久残留；
 *   · 更糟的是条目里的 view 指针指向 am_core 的双缓冲，缓冲每轮扫描被重写
 *     ⇒ 残留条目的 id/label 会"变成别的应用"，表现为删不掉、点开跑错。
 * 现在：scan 结果 → 本文件一次 sync（全量刷新指针 + 打标记）→ prune（差集回收）。
 * 指针与内容始终来自**同一轮扫描**，不存在半新半旧。
 * ════════════════════════════════════════════════════════════════════════ */

void os_app_sync(const os_app_t *const *views, int n)
{
    s_sync_epoch++;
    if (s_sync_epoch == 0) {            /* 回绕：所有标记作废，重来一轮 */
        memset(s_app_epoch, 0, sizeof(s_app_epoch));
        s_sync_epoch = 1;
    }
    if (!views || n <= 0) return;

    for (int k = 0; k < n; k++) {
        const os_app_t *v = views[k];
        if (!v || !v->id || !v->id[0]) continue;

        int i = app_find(v->id);
        if (i >= 0) {
            s_apps[i]      = v;          /* 指向本轮读侧缓冲 ⇒ 指针永远有效 */
            s_app_epoch[i] = s_sync_epoch;
            continue;
        }
        if (s_app_count >= OS_APP_MAX) {
            /* 容量满：先回收一个"上一轮出现过、本轮还没出现"的动态条目腾位，
             * 绝不因为满就静默丢掉新应用（那会表现为"装了却看不到"）。 */
            int victim = -1;
            for (int j = 0; j < s_app_count; j++) {
                if (s_apps[j] && s_apps[j]->page == OS_PAGE_LUA &&
                    s_app_epoch[j] != s_sync_epoch) { victim = j; break; }
            }
            if (victim < 0) {
                ESP_LOGE(TAG, "注册表已满(%d), 无法加入 %s", OS_APP_MAX, v->id);
                continue;
            }
            ESP_LOGW(TAG, "注册表已满, 回收旧条目 %s 给 %s 腾位", s_apps[victim]->id, v->id);
            app_remove_at(victim);
        }
        s_apps[s_app_count]      = v;
        s_app_epoch[s_app_count] = s_sync_epoch;
        s_app_count++;
        s_vis_gen++;
        ESP_LOGI(TAG, "app +%s (%s)", v->id, v->label ? v->label : "");
    }
}

int os_app_prune(bool (*keep)(const char *id, void *ud), void *ud)
{
    if (s_sync_epoch == 0) return 0;    /* 还没同步过 ⇒ 没有"本轮"概念, 不动 */
    int removed = 0;
    for (int i = s_app_count - 1; i >= 0; i--) {
        const os_app_t *a = s_apps[i];
        if (!a || !a->id) continue;
        if (a->page != OS_PAGE_LUA) continue;           /* 内置页永不回收 */
        if (s_app_epoch[i] == s_sync_epoch) continue;   /* 本轮注册过 */
        if (keep && keep(a->id, ud)) continue;          /* 调用方判定要保留 */
        char dead[64];
        snprintf(dead, sizeof(dead), "%s", a->id);
        app_remove_at(i);
        removed++;
        ESP_LOGW(TAG, "app -%s (本轮扫描不存在, 从注册表移除)", dead);
    }
    return removed;
}

uint32_t os_app_sync_epoch(void)
{
    return s_sync_epoch;
}

const os_app_t *os_app_get(int idx)
{
    if (idx < 0 || idx >= s_app_count) return NULL;
    return s_apps[idx];
}
