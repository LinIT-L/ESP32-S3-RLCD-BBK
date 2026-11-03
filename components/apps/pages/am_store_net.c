/* ============================================================
 * am_store_net.c — 应用中心 · 商店「云端」: 拉索引 + 下载包
 * (2026-10-04)
 *
 * 背景: 商店原先只扫**本地** (/store 根 + /sdcard/store_pkg 的 .lua), 用户拿新包
 *       必须自己把文件拷进设备。这里补上联网那一环 —— 拉一份 index.json 拿到
 *       "线上有哪些包 / 多大 / crc32 / 相对路径", 点一下就下到**商店源目录**,
 *       之后完全走既有的"扫描 → 安装"流程 (am_store.c 的安装逻辑一行都不用改)。
 *
 * 关键设计 (对应 docs/应用商店-多源托管方案.md):
 *   ★ index.json 里只存**相对 path**, 并内嵌 mirrors[]。设备按序试 mirrors[0..n-1],
 *     一个都不通才报失败。**加源/换源只改服务器上的 json, 固件不用重刷。**
 *   ★ 网络全在**后台任务**里跑 (TLS 一次往返 1~3 秒, 放 UI 线程必冻帧);
 *     UI 每帧只读几个 volatile 状态字。
 *   ★ index.json 的解析在 am_store_json.c (纯 C, 宿主机可单测)。
 *
 * ★★ 三条硬约束 (都是从 luat_lib_net.c 的 fetch 实现上抄来的教训):
 *   ① **必须先联网**: lwip 是 wifi_manager_enable() 里 esp_netif_init() 建的,
 *      从没联过网时 g_tcpip_mbox 是空的, 这时 getaddrinfo 会
 *      `assert failed: tcpip_send_msg_wait_sem (Invalid mbox)` → panic → 整机重启。
 *   ② **任务栈必须内部 RAM**: TLS 握手期间会短暂禁中断/关 cache, PSRAM 栈会炸
 *      (esp_task_stack_is_sane_cache_disabled)。
 *   ③ **落盘缓冲必须内部 RAM**: 写 SPIFFS 时 flash 驱动会关 Flash+PSRAM cache,
 *      这个窗口里读 PSRAM 源缓冲会取到垃圾 (SD 卡没这问题, 但统一走内部缓冲最省心)。
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include "psa/crypto.h"   /* ★ SHA-256 流式校验：IDF6 的 mbedtls 4.x 公开入口是 PSA */

/* ---------------- SHA-256（流式，4 KB 分块，不进大内存） ----------------
 * 为什么不用 mbedtls_sha256_*：IDF 6 的 mbedtls 已是 4.x，sha256.h 变成私有头；
 * 公开入口是 PSA。文件级校验只在安装前跑一次，PSA 的流式接口完全够用。 */
static bool am_sha256_file(const char *path, char out[65])
{
    if (!path || !out) return false;
    out[0] = 0;
    if (psa_crypto_init() != PSA_SUCCESS) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) { fclose(f); return false; }

    /* ⚠ 缓冲走堆，不放栈：下载任务只有 20 KB 栈（32 KB 会创建失败，实测过），
     *   这里再压 4 KB 栈帧会逼近上限。 */
    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) { psa_hash_abort(&op); fclose(f); return false; }
    size_t n = 0;
    bool ok = true;
    while ((n = fread(buf, 1, 4096, f)) > 0) {
        if (psa_hash_update(&op, buf, n) != PSA_SUCCESS) { ok = false; break; }
    }
    free(buf);
    uint8_t dg[32];
    size_t dlen = 0;
    if (ok && psa_hash_finish(&op, dg, sizeof(dg), &dlen) == PSA_SUCCESS && dlen == 32) {
        static const char *H = "0123456789abcdef";
        for (int i = 0; i < 32; i++) { out[i*2] = H[dg[i] >> 4]; out[i*2+1] = H[dg[i] & 15]; }
        out[64] = 0;
    } else {
        psa_hash_abort(&op);
        ok = false;
    }
    fclose(f);
    return ok;
}

#include <stdbool.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "am_internal.h"
#include "am_asset.h"   /* ★ 2026-10-25: 资源包(zip)解包 */
#include "am_asset_multi.h"  /* ★ 2026-10-31: 资源包多文件直接下载(有 manifest 时优先) */
#include "os_sources.h"
#include "store_index_builtin.h"   /* ★ 固件内置商店索引(离线可用) */
#include "am_store_json.h"
#include "am_lpk.h"        /* ★ 2026-11-01: type=lpk 单文件资源包安装收尾 */
#include "sd_scan.h"
#include "wifi_manager.h"
#include "net_client.h"
#include "str_trunc.h"   /* str_trunc(): 短字段的有意截断拷贝 */

#define TAG "AMSTORENET"

#define AM_IDX_MAX      (16 * 1024)         /* index.json 正文上限 */
#define AM_DL_MAX       (2 * 1024 * 1024)   /* 单包上限 (防服务器给错东西跑飞) */

/* 商店源目录: 与 am_store.c 的 AM_STORE_SRC1/SRC2 必须一致 */
#define AM_SRC_SPIFFS   "/store"
#define AM_SRC_SD       "/sdcard/store_pkg"

/* ==================== 状态 (后台任务写, UI 读) ==================== */
static volatile int s_state    = ARS_IDLE;
static volatile int s_permille = -1;     /* 0..1000; -1 = 总长未知 */
static volatile int s_dl_err   = 0;      /* ARS_DLFAIL 时的原因码, 供日志/文案 */
static volatile bool s_dl_unknown_len = false;  /* 服务器未给长度 */
static char s_dl_name[32];               /* 正在下载的包名 */
static char s_dl_label[48];              /* 正在下载的中文名 */

/* ★ 2026-10-31: 统一「下载任务」视图 —— 字体包与 Lua 包共用同一套状态字。
 *   页面(商店任意分类)只读这几个 volatile 画进度, 与下载任务本身解耦:
 *   任务在独立 task 里跑, 页面切分类/退出都不影响它。 */
static volatile int      s_dl_phase = AM_DLP_IDLE;
static volatile uint32_t s_dl_got   = 0;    /* 已下载字节 */
static volatile uint32_t s_dl_total = 0;    /* 预期总字节 (0=未知) */
static volatile uint32_t s_dl_bps   = 0;    /* 瞬时速度 字节/秒 */
static volatile bool     s_dl_asset = false;/* 资源包(有解包阶段) */
static volatile int      s_dl_idx   = -1;   /* 索引下标 (UI 精确匹配"正在下载的那一行") */
/* ★ 2026-10-31: 多文件安装的文件计数 (total_files==0 = 不是多文件流程) */
static volatile int      s_dl_files_done  = 0;
static volatile int      s_dl_files_total = 0;
static char s_dl_errmsg[40];                /* 失败原因中文 (仅在任务结束时写) */

/* 解析结果 */
static char s_mir[AM_MIRROR_MAX][AM_MIRROR_LEN];
static int  s_mir_n = 0;
static EXT_RAM_BSS_ATTR am_rpkg_t s_rpkg[AM_RPKG_MAX];
static int  s_rpkg_n = 0;
/* ★ 2026-11-01: 索引**内容**代次。原来商店那边只看"条数变没变"决定要不要重建视图，
 *   而服务器换版时条数常常不变（同一个包换了个文件）⇒ 视图不重建 ⇒ 行还指着旧的
 *   索引下标，版本/路径全错位（表现为"更新按钮时有时无、状态闪变"）。
 *   现在每次把新索引提交进 s_rpkg[] 就 +1，商店按代次重建。 */
static volatile uint32_t s_idx_gen = 1;
static bool type_is_lpk(const char *t) { return t && strcmp(t, "lpk") == 0; }
static bool type_is_lua(const char *t) { return t && strcmp(t, "lua") == 0; }

/* ★★★ 2026-11-01 linit：**索引去重（lpk 优先）**。
 *   现网索引里同一个应用有两条 —— 迁移期留下的旧 `.lua` 包 + 新的 `.lpk`：
 *     录音机 / 航班雷达 / 矢量地图 / 自行车码表 / BIOS模拟器 / 人生指南 各两份。
 *   客户端两条都收 ⇒
 *     · 商店里每个应用出现两行（一行"下载 .lua"，一行"下载 .lpk"）；
 *     · 装完桌面出现两个同名图标（一个「录音机」一个 录音机）；
 *     · 删掉一个另一个还在 ⇒ 用户说的"图标删不掉"。
 *   规则：同名条目里只要有 lpk，就丢掉 lua 那条（lpk 带 app_id/双版本，是新格式）。
 *   ⚠ 只在内存里过滤，不改服务器索引、不动包文件。 */
static int idx_dedup(int n)
{
    int out = 0;
    for (int i = 0; i < n; i++) {
        bool drop = false;
        const char *nm = s_rpkg[i].name;
        if (type_is_lua(s_rpkg[i].type) && nm && nm[0]) {
            for (int j = 0; j < n; j++) {
                if (j == i || !type_is_lpk(s_rpkg[j].type)) continue;
                if (s_rpkg[j].name[0] && strcmp(nm, s_rpkg[j].name) == 0) { drop = true; break; }
            }
        }
        if (drop) {
            ESP_LOGW(TAG, "索引去重: 丢弃旧格式 %s（已有同名 lpk，以 lpk 为准）", nm);
            continue;
        }
        if (out != i) s_rpkg[out] = s_rpkg[i];
        out++;
    }
    return out;
}
static void idx_commit(int n) { s_rpkg_n = idx_dedup(n); s_idx_gen++; }
uint32_t am_rstore_gen(void) { return s_idx_gen; }
/* ★ 2026-10-30: 索引"这一轮确实换过了"的事件位。
 *   商店界面是脏帧渲染，索引在后台换成新的以后没人置 dirty ⇒ 用户要切一次分类
 *   （或退出重进）才看到新上架的资源。页面 poll 里 take 一次即可。 */
static volatile bool s_idx_updated;
static volatile bool s_fetched = false;  /* 拉过至少一次 (成功或失败), 防重入 */
#define RETRY_MS      10000              /* 索引拉取失败后的重试退避 */
static volatile bool     s_retry_wait;   /* true = 上次失败, 等退避到点 */
static volatile uint32_t s_retry_ms;     /* 上次失败时刻 (ms, esp_timer) */

/* 缓冲: 索引正文放 PSRAM (纯 socket 读); 落盘块**必须内部 RAM** (见文件头 ③) */
static EXT_RAM_BSS_ATTR char s_idx[AM_IDX_MAX + 1];

/* 拼 URL: 手写长度运算, 不用 snprintf / strncat ——
 *   snprintf 在"源是长度未知的指针"时会被 -Wformat-truncation 一直警告;
 *   strncat 又会触发 **-Werror=stringop-truncation** (IDF 默认把它当错误, 直接编不过)。 */
static void url3(char *dst, size_t cap, const char *a, const char *b, const char *c) {
    if (cap == 0) return;
    size_t o = 0;
    const char *parts[3] = { a, b, c };
    for (int i = 0; i < 3 && o + 1 < cap; i++) {
        if (!parts[i]) continue;
        size_t l = strlen(parts[i]);
        if (l > cap - 1 - o) l = cap - 1 - o;   /* 截断也要留 1 字节给结尾 NUL */
        memcpy(dst + o, parts[i], l);
        o += l;
    }
    dst[o] = 0;
}

/* ==================== 后台: 拉索引 ==================== */
static void am_idx_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "拉索引: 内部 free=%u largest=%u psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* ★ 第 0 步: 先拿任意一个镜像的 body 当"引导" —— 但 mirrors[] 就在 body 里,
     *   所以得先试一个已知源。做法: 先铺**固件内置源清单** (os_sources.c —— 商店与
     *   Lua 地图应用共用的唯一真相源), 再用上次 index.json 解析出的 mirrors 补位。
     *   首次运行时 s_mir_n 为 0, 纯靠内置源; 内置源通了, 它返回的 mirrors[]
     *   就覆盖/扩展后续要用的源列表 (所以"加源"通常连固件都不用刷)。 */
    /* ★ 2026-10-25: 这张表挪到 PSRAM 静态 —— 它是本任务最大的局部数组(1.3KB),
     *   原来占栈, 与本函数里另外两三份 mir[][] 叠加后 6KB 栈不够, 实测启动即
     *   "A stack overflow in task am_idx"。单任务使用, 静态无并发问题。 */
    static EXT_RAM_BSS_ATTR char tries[AM_MIRROR_MAX + 1][AM_MIRROR_LEN];
    int  ntry = 0;
    for (int i = 0; i < os_sources_count() && ntry < AM_MIRROR_MAX + 1; i++)
        snprintf(tries[ntry++], AM_MIRROR_LEN, "%s", os_sources_get(i));
    for (int i = 0; i < s_mir_n && ntry < AM_MIRROR_MAX + 1; i++)
        str_trunc(tries[ntry++], AM_MIRROR_LEN, s_mir[i]);

    int rc = -99;
    bool have_builtin = false;
    /* ★★★ 2026-10-17 linit: 先用【固件内置索引】铺底 —— 离线/网络不通也能看到应用列表 */
    {
        char mir[AM_MIRROR_MAX][AM_MIRROR_LEN];
        int  mn = 0, pn = 0;
        if (am_store_json_parse(STORE_INDEX_BUILTIN, s_rpkg, AM_RPKG_MAX, &pn,
                                mir, AM_MIRROR_MAX, &mn) == 0) {
            memcpy(s_mir, mir, sizeof(s_mir));
            s_mir_n = mn; idx_commit(pn);
            have_builtin = true;
            ESP_LOGI(TAG, "★ 内置索引已加载: %d 个包(离线可用)", pn);
        } else {
            ESP_LOGW(TAG, "内置索引解析失败(编译进固件的那份有问题)");
        }
    }
    for (int i = 0; i < ntry; i++) {
        char url[AM_MIRROR_LEN + 32];
        url3(url, sizeof(url), tries[i], "/index.json", NULL);
        /* ★ 2026-10-15 联网收口: 改走 net_client (唯一 HTTP 客户端), 不再自己建 client/任务 */
        netc_req_t r = { 0 };
        r.url = url; r.timeout_ms = 10000;
        r.body = s_idx; r.body_cap = AM_IDX_MAX;      /* 索引 ≤16KB, 一份内存就够 */
        int total = 0, status = 0;
        esp_err_t e = ESP_FAIL;
        if (netc_start(&r)) {
            uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
            while (netc_busy() && (uint32_t)(esp_timer_get_time() / 1000) - t0 < 15000)
                vTaskDelay(pdMS_TO_TICKS(50));
            if (netc_state() == NETC_DONE) { e = ESP_OK; }
            else e = (esp_err_t)(netc_err() ? netc_err() : ESP_FAIL);
            status = netc_http(); total = netc_len();
            netc_reset();
        }
        if (total >= AM_IDX_MAX) total = AM_IDX_MAX - 1;
        if (total > 0) s_idx[total] = 0; else s_idx[0] = 0;
        ESP_LOGI(TAG, "引导源%d: e=%d http=%d len=%d url=%s", i, (int)e, status, total, url);
        if (total > 0 && status == 200) {
            char mir[AM_MIRROR_MAX][AM_MIRROR_LEN];
            int  mn = 0, pn = 0;
            int pr = am_store_json_parse(s_idx, s_rpkg, AM_RPKG_MAX, &pn,
                                         mir, AM_MIRROR_MAX, &mn);
            if (pr == 0) {
                /* ★★★ 2026-10-18 linit: **把拉到的索引缓存到 apps 安装分区**(SPIFFS /store) ✓
                 *   ⇒ 下次开机/离线时优先读它 ⇒ 不用联网也能看到"上次联网的列表" ✓
                 *   (以前只有编译进固件的内置索引 ⇒ 云端加了新应用, 离线就永远看不到) */
                {
                    /* ★★★ 2026-10-30: 改成「先写 .tmp 再替换」。
                     *   原来 fopen(...,"wb") 直接截断旧缓存再写 —— 写到一半掉电/SPIFFS 满，
                     *   上一份完整的索引就没了，下次冷启动只能退回固件内置那份（老包）。
                     *   这里保证：只有新内容**完整写出**才替换，失败时旧缓存分毫不动。 */
                    FILE *fc = fopen("/store/index.json.tmp", "wb");
                    if (fc) {
                        size_t w = fwrite(s_idx, 1, (size_t)total, fc);
                        fclose(fc);
                        if (w == (size_t)total) {
                            remove("/store/index.json");     /* FATFS/SPIFFS 的 rename 不覆盖 */
                            if (rename("/store/index.json.tmp", "/store/index.json") == 0)
                                ESP_LOGI(TAG, "索引已缓存到 /store/index.json (%u B)", (unsigned)w);
                            else
                                ESP_LOGW(TAG, "索引缓存替换失败(rename), 保留旧缓存");
                        } else {
                            remove("/store/index.json.tmp");
                            ESP_LOGW(TAG, "索引缓存写入不完整(%u/%d) ⇒ 丢弃, 旧缓存保留",
                                     (unsigned)w, total);
                        }
                    } else {
                        ESP_LOGW(TAG, "索引缓存写入失败(SPIFFS 满? 无所谓, 用内置索引)");
                    }
                }
                /* ★★★ 2026-10-17 修: 云端索引的 mirrors 可能来自 CDN 旧缓存(含已下线源) ⇒
                 *   不能直接覆盖! 云端源打头, 再把**固件内置源**追加进来去重 ⇒
                 *   旧缓存再也不会把设备带向死服务器。 */
                int k = 0;
                for (int a = 0; a < mn && k < AM_MIRROR_MAX; a++)
                    str_trunc(s_mir[k++], AM_MIRROR_LEN, mir[a]);
                for (int a = 0; a < os_sources_count() && k < AM_MIRROR_MAX; a++) {
                    const char *u = os_sources_get(a);
                    bool dup = false;
                    for (int b = 0; b < k; b++) if (strcmp(s_mir[b], u) == 0) { dup = true; break; }
                    if (!dup) snprintf(s_mir[k++], AM_MIRROR_LEN, "%s", u);
                }
                s_mir_n = k;
                idx_commit(pn);
                /* ★★★ 2026-10-17: 云端索引解析出 0 个包 ⇒ 说明索引损坏/是旧格式(如 crc32 被写成数字),
                 *   此时**回退到固件内置索引** —— 坏索引再也不会把商店清空。 */
                if (pn == 0) {
                    char mir2[AM_MIRROR_MAX][AM_MIRROR_LEN];
                    int mn2 = 0, pn2 = 0;
                    if (am_store_json_parse(STORE_INDEX_BUILTIN, s_rpkg, AM_RPKG_MAX, &pn2,
                                            mir2, AM_MIRROR_MAX, &mn2) == 0 && pn2 > 0) {
                        idx_commit(pn2);
                        memcpy(s_mir, mir2, sizeof(s_mir));
                        s_mir_n = mn2;
                        ESP_LOGW(TAG, "云端索引 0 个包(格式异常) ⇒ 已回退内置索引: %d 个包", pn2);
                    }
                }
                ESP_LOGI(TAG, "索引 OK: %d 个包, %d 个镜像源, 跳过 %d 条",
                         s_rpkg_n, s_mir_n, am_store_json_skipped());
                s_idx_updated = true;      /* 通知 UI: 列表该重建了 */
                rc = 0;
                break;
            }
            rc = pr;
            ESP_LOGD(TAG, "索引解析失败 rc=%d (跳过 %d 条)", pr, am_store_json_skipped());
        }
    }
    if (rc == 0 || have_builtin) {     /* ★ 有内置索引 ⇒ 即使网络全挂也进 READY(离线商店) */
        if (rc != 0) ESP_LOGW(TAG, "云端索引失败, 使用内置索引(离线模式)");
        __sync_synchronize();          /* 先写完数据, 再放状态 */
        s_state = ARS_READY;
    } else {
        ESP_LOGW(TAG, "索引拉取/解析全部失败 rc=%d (10s 后允许重试)", rc);
        /* ★ 2026-10-15: 失败后必须**允许重试** —— 原来 s_fetched 一直是 true,
         *   于是"第一次没拉到就永远待添加, 只能重启"。改成置回 false + 退避计时,
         *   由 am_rstore_prefetch() 隔 RETRY_MS 再试一次 (服务器抖动/刚开机没网都能自愈)。 */
        s_retry_ms = (uint32_t)(esp_timer_get_time() / 1000);
        s_state = ARS_FAIL;
        s_fetched = false;
        s_retry_wait = true;
    }
    vTaskDelete(NULL);
}

#define RETRY_MS        10000      /* 索引拉取失败后多久允许重试 */
static volatile bool     s_retry_wait;
static volatile uint32_t s_retry_ms;

/* ★ 2026-10-18: 距上次索引刷新是否已过 60 秒(且当前没在拉) */
bool am_rstore_refresh_due(void) {
    if (s_state == ARS_FETCHING || s_state == ARS_DOWNLOADING) return false;
    static int64_t last_ms = 0;
    int64_t now = esp_timer_get_time() / 1000;
    if (last_ms == 0 || now - last_ms > 60000) { last_ms = now; return true; }
    return false;
}
/* ★ 强制重新拉一次索引(后台, 不阻塞; 失败不清旧数据) */
void am_rstore_refresh_force(void) {
    s_fetched = false;
    s_retry_wait = false;
    am_rstore_prefetch();
}

void am_rstore_prefetch(void) {
    if (s_fetched || s_state == ARS_FETCHING || s_state == ARS_DOWNLOADING) return;
    /* ★ 失败退避: 上电/服务器抖动导致第一次没拉到 → 隔 RETRY_MS 再试, 不必重启 */
    if (s_retry_wait &&
        (uint32_t)(esp_timer_get_time() / 1000) - s_retry_ms < RETRY_MS) return;
    /* ★★★ 2026-10-18 linit: **没联网也要有列表!**
     *   原来 WiFi 判断在加载内置索引之前 ⇒ 离线直接 return ⇒ 商店一片空白 ✗
     *   现在先把**固件内置索引**装上(离线可浏览 8 个应用), 再判断要不要联网刷新 ✓ */
    if (s_rpkg_n == 0) {
        char mir[AM_MIRROR_MAX][AM_MIRROR_LEN];
        int  mn = 0, pn = 0;
        bool got = false;

        /* ★★★ 2026-10-18 linit: **优先用 apps 分区里的缓存索引**(上次联网拉到的) ——
         *   离线/没网也能看到最新应用列表 ✓ 缓存坏了/不存在再退回固件内置 ✓ */
        FILE *fc = fopen("/store/index.json", "rb");
        if (fc) {
            size_t n = fread(s_idx, 1, AM_IDX_MAX - 1, fc);
            fclose(fc);
            s_idx[n] = 0;
            if (n > 16 && am_store_json_parse(s_idx, s_rpkg, AM_RPKG_MAX, &pn,
                                              mir, AM_MIRROR_MAX, &mn) == 0) {
                memcpy(s_mir, mir, sizeof(s_mir));
                s_mir_n = mn; idx_commit(pn);
                got = true;
                ESP_LOGI(TAG, "★ 离线: 已用**上次联网缓存**索引 %d 个包(/store/index.json %u B)",
                         pn, (unsigned)n);
            }
        }
        if (!got && am_store_json_parse(STORE_INDEX_BUILTIN, s_rpkg, AM_RPKG_MAX, &pn,
                                        mir, AM_MIRROR_MAX, &mn) == 0) {
            memcpy(s_mir, mir, sizeof(s_mir));
            s_mir_n = mn; idx_commit(pn);
            ESP_LOGI(TAG, "★ 离线: 已启用内置索引 %d 个包", pn);
        }
    }
    if (!wifi_manager_is_connected()) return;   /* ★ 见文件头 ①: 没联网一律不发请求 */
    s_retry_wait = false;
    s_fetched = true;
    s_state = ARS_FETCHING;
    /* 任务栈必须内部 RAM (TLS 会短暂禁中断; 8KB 与 luat_lib_net 的 fetch 任务同规格) */
    /* ★ 2026-10-18: 栈 8192 → **6144**(TLS 握手实测够用) ——
     *   内部 RAM 紧张时 8KB 任务会创建失败 ⇒ 索引永远拉不到 ✓ */
    /* 栈 6144 → 12288: 拉索引这条链路要容纳 net_client(HTTP+TLS 已不算)
     *   + json 解析 + 两三份 mir[][] 局部表, 6144 实测启动即栈溢出 panic。 */
    if (xTaskCreatePinnedToCore(am_idx_task, "am_idx", 12288, NULL, 4, NULL, 0) != pdPASS) {
        ESP_LOGD(TAG, "索引任务创建失败(内存不足)");
        s_state = ARS_FAIL;
        s_fetched = false;
    }
}

/* ★ 2026-10-17 linit: 清掉商店缓存(索引/镜像/包列表), 下次进商店会重新拉取。
 *   入口: 应用商店左栏"商店"图标长按 3 秒。 */
void am_rstore_clear_cache(void) {
    s_fetched    = false;
    s_retry_wait = false;
    s_state      = ARS_IDLE;
    s_rpkg_n     = 0;
    s_idx_gen++;                 /* 清空也算一次内容变化 */
    s_mir_n      = 0;
    ESP_LOGI(TAG, "商店缓存已清空(用户长按), 下次请求将重新拉取索引");
}

int am_rstore_state(void) { return s_state; }
int am_rstore_count(void) { return s_rpkg_n; }
const am_rpkg_t *am_rstore_get(int i) {
    return (i >= 0 && i < s_rpkg_n) ? &s_rpkg[i] : NULL;
}
int am_rstore_dl_permille(void) { return s_permille; }
const char *am_rstore_dl_name(void) { return s_dl_name; }
const char *am_rstore_dl_label(void) { return s_dl_label; }
int am_rstore_dl_err(void) { return s_dl_err; }
int am_rstore_dl_index(void) { return s_dl_idx; }
bool am_rstore_dl_unknown_len(void) { return s_dl_unknown_len; }

/* 失败原因码 → 中文 (1=HTTP 2=连接 3=写盘 4=校验; 其它=未知) */
const char *am_dl_err_text(int err) {
    switch (err) {
    case 1:  return "\xe6\x9c\x8d\xe5\x8a\xa1\xe5\x99\xa8\xe6\x8b\x92\xe7\xbb\x9d";  /* 服务器拒绝 */
    case 2:  return "\xe8\xbf\x9e\xe6\x8e\xa5\xe8\xb6\x85\xe6\x97\xb6";               /* 连接超时 */
    case 3:  return "\xe5\x86\x99\xe5\x85\xa5\xe5\xa4\xb1\xe8\xb4\xa5";               /* 写入失败 */
    case 4:  return "\xe6\xa0\xa1\xe9\xaa\x8c\xe5\xa4\xb1\xe8\xb4\xa5";               /* 校验失败 */
    default: return "\xe6\x9c\xaa\xe7\x9f\xa5\xe9\x94\x99\xe8\xaf\xaf";               /* 未知错误 */
    }
}

/* 统一「下载任务」视图: 每帧可调, 只读 volatile, 不分配内存。
 * ⚠ 只有 s_dl_phase 非 IDLE 才算"有任务"; DONE/FAIL 由 am_rstore_ack() 收尾。 */
bool am_dl_view(am_dl_view_t *out) {
    int ph = s_dl_phase;
    if (ph == AM_DLP_IDLE) return false;
    if (!out) return true;
    memset(out, 0, sizeof(*out));
    out->phase    = ph;
    out->permille = s_permille;
    out->got      = s_dl_got;
    out->total    = s_dl_total;
    out->bps      = s_dl_bps;
    out->asset    = s_dl_asset;
    out->done_files  = s_dl_files_done;
    out->total_files = s_dl_files_total;
    snprintf(out->label, sizeof(out->label), "%s", s_dl_label);
    snprintf(out->err,   sizeof(out->err),   "%s", s_dl_errmsg);
    return true;
}

/* 状态机推进: 由商店页每帧调, 返回 true = 列表该重建了。
 * ★ 必须**边沿触发**: 早先写成"状态是 READY 就返回 true" → 上层每帧重建列表, 白烧 CPU。 */
static int s_reported = ARS_IDLE;
/* ★ 2026-10-31: 完成/失败结果在屏幕上保留一会儿再收 —— 否则 ack 的同一帧就清了,
 *   用户根本看不到"完成 100%"和失败原因。 */
static volatile uint32_t s_dl_end_ms;
bool am_rstore_tick(void) {
    am_rstore_prefetch();          /* 已联网且没拉过 → 顺手把索引拉起来 */
    /* ★ 2026-10-31 linit「失败提示一直停留、无法操作」：
     *   原来 s_dl_end_ms 只在 am_rstore_ack() 里补设。只要 UI 没走到 ack 那条分支
     *   （用户切到别的分类 / 页面回调没执行），FAIL 就永远不收 —— 状态条上那句
     *   "失败 连接超时"会一直挂着。这里兜底：一进 FAIL 就记时刻，3 秒后自动收掉。 */
    if (s_dl_phase == AM_DLP_FAIL && s_dl_end_ms == 0)
        s_dl_end_ms = (uint32_t)(esp_timer_get_time() / 1000);
    const int st = s_state;
    const bool edge = (st != s_reported) &&
                      (st == ARS_READY || st == ARS_FAIL || st == ARS_OK || st == ARS_DLFAIL);
    s_reported = st;
    /* 结束态超时回收: 完成留 **1s**, 失败留 3s (够看清原因)
     * ★ 2026-11-03 linit: 原来完成留 1.5s，"已保存/已完成"状态在底栏挂太久，
     *   看着像卡住。改成 1s。失败仍留 3s —— 那是错误原因，用户要看清。 */
    if (s_dl_end_ms) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t hold = (s_dl_phase == AM_DLP_FAIL) ? 3000u : 1000u;
        if (now - s_dl_end_ms >= hold) {
            __sync_synchronize();
            s_dl_phase = AM_DLP_IDLE;
            s_dl_idx   = -1;
            s_dl_got   = 0;
            s_dl_total = 0;
            s_dl_bps   = 0;
            s_dl_files_done  = 0;
            s_dl_files_total = 0;
            s_dl_errmsg[0] = 0;
            s_dl_end_ms = 0;
            __sync_synchronize();
        }
    }
    return edge;
}
void am_rstore_ack(void) {
    __sync_synchronize();
    if (s_state == ARS_OK || s_state == ARS_DLFAIL) {
        s_state = ARS_READY;
        /* 不在这里清 s_dl_phase —— 交给 am_rstore_tick 的保留期 */
        if (s_dl_end_ms == 0) s_dl_end_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
}

/* crc32 已移到 net_client (netc_crc32): 下载时 want_crc=true, 收完直接比 —— 
 * 全工程共用同一份实现, 保证与 tools/store_build.py 的 binascii.crc32 一致。 */

/* ★★★ 2026-10-17: 包文件名可能是中文(linit 定: 用文件名做应用显示名) ⇒
 *   拼 URL 前必须百分号编码 —— esp_http_client 不做编码, 直接把 UTF-8 塞进 URL 会 400/404。 */
static void am_url_encode(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = "0123456789ABCDEF"[c >> 4];
            out[o++] = "0123456789ABCDEF"[c & 0x0F];
        }
    }
    out[o] = 0;
}

/* ==================== 后台: 下载一个包 ==================== */
/* 解包进度 0..1000 → 总进度 70%..100% */
static void am_asset_prog_70200(int permille)
{
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    s_permille = 700 + permille * 300 / 1000;
}

/* ★ 多文件安装进度: 字节与文件数同时上报。
 *   这条路径没有"解包"阶段（下载即安装），所以进度就是 0..1000 的真实字节比。 */
static void am_mf_prog_cb(const am_mf_prog_t *p)
{
    if (!p) return;
    s_dl_got         = p->done_bytes;
    s_dl_total       = p->total_bytes;
    s_dl_files_done  = p->done_files;
    s_dl_files_total = p->total_files;
    s_dl_phase       = AM_DLP_DOWNLOAD;
    if (p->total_bytes)
        s_permille = (int)((uint64_t)p->done_bytes * 1000u / p->total_bytes);
}

static void am_dl_task(void *arg) {
    int idx = (int)(intptr_t)arg;
    /* ★ 2026-11-01: 先把索引条目**快照**下来再开始下载。
     *   原来直接用 &s_rpkg[idx]：而索引可以在下载过程中被后台任务整表替换
     *   （服务器上架新包 → 60 秒节流到点 → am_idx_task 重解析）。
     *   那时 pk 指向的已经是**另一份数据**，下载完就会按错的 path/target/ver
     *   去落盘和写安装记录 —— 表现为"更新完版本记录对不上"或"下完装不上"。
     *   快照之后，本次任务全程只认自己这一份（am_rpkg_t ≈ 500 B，20KB 栈够用）。 */
    am_rpkg_t pk_snap = s_rpkg[idx];      /* 结构体赋值：字段长度信息不变，编译器仍能校验 */
    const am_rpkg_t *pk = &pk_snap;
    /* str_trunc：来源是索引字段（定长但编译器已按"快照"放宽了取值范围），
     * 目标缓冲本来就装不下长名字 —— 这是设计上的截断，用 str_trunc 表明意图。 */
    str_trunc(s_dl_name,  sizeof(s_dl_name),  pk->name);
    str_trunc(s_dl_label, sizeof(s_dl_label), pk->label);
    s_permille = -1;
    s_dl_err = 0;
    s_dl_unknown_len = false;
    /* 统一视图初值: 连接中。total 先按索引声明的大小, 实际收到的以 netc 为准。 */
    s_dl_idx       = idx;
    s_dl_phase     = AM_DLP_CONNECT;
    s_dl_got       = 0;
    s_dl_total     = pk->size;
    s_dl_bps       = 0;
    s_dl_errmsg[0] = 0;
    /* ★ 2026-11-01: "资源包"现在有两种装法 —— zip 解包(asset) 与 单文件 LPK(lpk)。
     *   页面上的"资源包"视图(下载/安装两段式)对两者都适用, 但 LPK 没有解压阶段。 */
    s_dl_asset     = am_pkg_is_res(pk->type);
    s_dl_files_done  = 0;
    s_dl_files_total = 0;

    /* 目标目录: 优先 TF 卡 (不占宝贵的 4MB SPIFFS; 且写卡不受"禁 flash cache"影响),
     * 没插卡才退回 SPIFFS 根 —— 与 am_store.c 的两个扫描源一一对应。 */
    /* ★ 2026-10-25: 资源包 (type=asset) 不落"商店源目录", 直接落 TF 卡根,
     *   下完就地解到 target, 解完删 zip —— 它不是一个可安装的 .lua。 */
    const bool is_asset = am_pkg_is_res(pk->type);
    const bool is_lpk   = am_pkg_is_lpk(pk->type);
    const bool to_sd = sd_is_mounted();
    const char *dir = to_sd ? AM_SRC_SD : AM_SRC_SPIFFS;
    if (to_sd) mkdir(AM_SRC_SD, 0777);
    char dst[192];
    if (is_lpk) {
        /* ★★★ 2026-11-01 修 B1：LPK 有**独立**落盘路径，不再拼资源包的 target。
         *   原来 dst = "<target>/<stem>.lpk"，而新索引条目没有 target ⇒ dst 变成
         *   根目录 "/x.lpk"，且 am_lpk_finish(dst,"") 直接返回 false ⇒ 必然"安装失败"。
         *   现在统一落 /sdcard/apps/staging/，由安装器校验后原子搬到
         *   installed/<app_id>/ —— staging 里留的是**未校验**的中间态，失败即删。 */
        char stem[64];
        am_lpk_stem(pk->path, stem, sizeof(stem));
        mkdir("/sdcard/apps", 0777);
        mkdir("/sdcard/apps/staging", 0777);
        snprintf(dst, sizeof(dst), "/sdcard/apps/staging/%s.lpk", stem);
    } else if (is_asset) {
        const char *base = strrchr(pk->path, '/');
        base = base ? base + 1 : pk->path;
        snprintf(dst, sizeof(dst), "/sdcard/%.150s", base);   /* 精度=有意截断 */
    } else {
        char fname[48];
        snprintf(fname, sizeof(fname), "%.40s.lua", pk->name);   /* LAPP 包名上限 40 */
        snprintf(dst, sizeof(dst), "%s/%s", dir, fname);
    }
    ESP_LOGI(TAG, "下载 %s -> %s (%u B)%s", pk->name, dst, (unsigned)pk->size,
             is_asset ? " [资源包]" : "");

    /* ★★★ 2026-10-31 (linit) 方案 B: 资源包「多文件直接下载」★★★
     *   索引条目里给了 manifest 就先走这条路:
     *     卡上只写一遍(不再 41 MB zip + 41 MB 解包), 逐文件校验, 断点续传。
     *   清单拉不到/不合规 ⇒ 原样回落到下面的 ZIP 下载 + 解包 ——
     *   所以"服务器还没出清单"或"要回滚"都不会影响设备可用性。 */
    if (is_asset && pk->manifest[0] && pk->target[0]) {
        int mrc = AM_MF_NO_MANIFEST;
        for (int i = 0; i < s_mir_n && mrc == AM_MF_NO_MANIFEST; i++) {
            s_dl_phase = AM_DLP_DOWNLOAD;
            mrc = am_asset_multi_install(s_mir[i], pk->manifest, pk->target, pk->path,
                                         am_mf_prog_cb);
            if (mrc == AM_MF_NO_MANIFEST)
                ESP_LOGW(TAG, "镜像%d 没有可用清单", i);
        }
        if (mrc == AM_MF_OK) {
            s_permille = 1000;
            s_dl_bps   = 0;
            s_dl_name[0] = 0;
            __sync_synchronize();
            s_state = ARS_OK;
            s_dl_phase = AM_DLP_DONE;
            netc_ka_close();          /* 收掉复用连接 + mbedTLS 上下文 */
            vTaskDelete(NULL);
            return;
        }
        if (mrc != AM_MF_NO_MANIFEST) {
            s_permille = -1;
            s_dl_err   = 4;
            snprintf(s_dl_errmsg, sizeof(s_dl_errmsg),
                     "\xe5\xae\x89\xe8\xa3\x85\xe5\xa4\xb1\xe8\xb4\xa5");   /* 安装失败 */
            s_dl_bps   = 0;
            s_dl_phase = AM_DLP_FAIL;
            s_dl_name[0] = 0;
            __sync_synchronize();
            s_state = ARS_DLFAIL;
            netc_ka_close();
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGW(TAG, "所有镜像都没有清单 ⇒ 回退 ZIP 下载: %s", pk->name);
        s_dl_files_done  = 0;
        s_dl_files_total = 0;
        s_permille = -1;
    }

    /* ★★★ 2026-10-17 修: 初值必须非 0 —— 否则下面的"重试循环"一次都不进(死了一个源就整体失败) */
    int rc = 9;   /* 9=未尝试 / 0=OK 1=HTTP 2=连接 3=写盘 4=校验 */
    uint32_t crc = 0;
    int total = 0, status = 0;

    /* ★★★ 2026-10-17 修: 原为 `rc == 0`(只要成功就继续) ⇒ 逻辑反了:
     *   第一个镜像失败后循环立刻退出, 永不重试后续镜像。改为 `rc != 0`(失败才继续试下一个)。 */
    for (int i = 0; i < s_mir_n && rc != 0; i++) {
        char url[AM_MIRROR_LEN + 256];
        char enc[224];
        am_url_encode(pk->path, enc, sizeof(enc));   /* ★ 中文文件名 ⇒ %XX */
        url3(url, sizeof(url), s_mir[i], "/", enc);
        if (i > 0) ESP_LOGD(TAG, "换源重试: %s", url);

        /* ★ 2026-10-15 联网收口: 流式落盘 + crc32 全交给 net_client;
         *   本任务只剩"等 + 报进度 + 校验", 不再自己建 client 与读循环。 */
        netc_req_t r = { 0 };
        r.url = url; r.timeout_ms = 15000;
        r.sink_path = dst;
        r.want_crc = true;                       /* 边收边算 crc32, 收完直接比 */
        /* ★ 2026-10-30: 把"期望体积/CRC"下推给 net_client —— 让它**先校验再替换**。
         *   原来是"先替换, 再在调用方比 crc", 一旦 crc 不匹配, 目标位置已被新文件占住,
         *   只能靠 remove(dst) 补救(把旧版本也删了)。现在校验不通过只删 .part,
         *   目标文件(可能是可用的旧版本)原样保留。 */
        r.expect_size = pk->size;
        r.expect_crc  = pk->crc;
        status = 0;
        if (!netc_start(&r)) { rc = 2; break; }  /* 客户端忙 → 当连接失败 */
        s_permille = 0;
        s_dl_phase = AM_DLP_CONNECT;             /* 每次换源都回到"连接中" */
        s_dl_bps   = 0;
        uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
        /* ★ 2026-10-31: 速度统计基准 (每 500ms 采一次差) */
        uint32_t spd_ms = t0, spd_got = 0;
        /* 资源包动辄 20MB+, 180s 不够 (Wi-Fi 200KB/s ⇒ 26MB 约 130s, 余量太小),
         * 给 asset 单独放宽到 10 分钟; 普通 .lua 包仍是 180s。 */
        const uint32_t dl_limit_ms = is_asset ? 600000u : 180000u;   /* asset 含 lpk */
        while (netc_busy() && (uint32_t)(esp_timer_get_time() / 1000) - t0 < dl_limit_ms) {
            int g = netc_got();
            /* 统一视图: 已收字节 / 总字节 / 阶段 / 速度 —— 字体与 Lua 完全同一套 */
            uint32_t nowms = (uint32_t)(esp_timer_get_time() / 1000);
            if (g > 0) {
                s_dl_got = (uint32_t)g;
                if (s_dl_phase == AM_DLP_CONNECT) s_dl_phase = AM_DLP_DOWNLOAD;
            }
            if (nowms - spd_ms >= 500) {
                uint32_t d = (uint32_t)g >= spd_got ? (uint32_t)g - spd_got : 0;
                uint32_t dt = nowms - spd_ms;
                s_dl_bps = dt ? (uint32_t)((uint64_t)d * 1000u / dt) : 0;
                spd_ms = nowms; spd_got = (uint32_t)g;
            }
            /* ★ 2026-10-25 linit: 资源包把进度条按 7:3 切开 ——
             *   下载只走 0..70%, 剩下 30% 留给"解压安装", 到 100% 才是真完成。
             *   普通 .lua 包仍按 0..100% (下载完即装完)。 */
            if (pk->size > 0) {
                /* LPK: 下载即安装(只多一步验包), 所以走满 96%; zip 仍按 7:3 切 */
                int span = is_lpk ? 960 : (is_asset ? 700 : 1000);
                s_permille = (int)((int64_t)g * span / (int64_t)pk->size);
            } else {
                /* ⚠ 2026-10-30 更正: 上一轮注释写"JSON 没有 size/crc"是**错的** ——
                 *   am_store_json.c:196-204 确实解析了 size(j_num) 和 crc32(strtoul hex)。
                 *   这里保留 else 分支只是**兜底**: 旧索引/畸形条目缺 size 时不至于卡在 0。
                 *   真正的"进度不动"根因尚未定位 (怀疑 netc_start 失败或镜像不可达), 见报告。 */
                s_dl_unknown_len = true;
                int cap = is_lpk ? 950 : (is_asset ? 690 : 990);
                int grow = (int)(g / 2048);            /* 每 2KB 涨 1‰, 封顶 */
                if (grow > cap) grow = cap;
                if (grow > s_permille) s_permille = grow;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        status = netc_http(); total = netc_len(); crc = netc_crc32();
        if (netc_state() == NETC_DONE)             rc = 0;
        else if (status != 0 && status != 200)     rc = 1;    /* HTTP 非 200 */
        else                                       rc = 2;    /* 连接/超时/截断 */

        /* ★ 2026-10-30 下载诊断日志: 一次下载只打 1 行, 明确区分失败阶段。
         *   排查"点下载没反应/进度不动"时看这一行就够:
         *     status=0        → 根本没拿到 HTTP 响应 (DNS/TLS/连接失败/超时)
         *     status=200 但 total=0 → 连上了但没数据
         *     total>0 但 != size    → 传了一半就断 (截断)
         *     state!=DONE     → netc 侧异常中断
         *   ⚠ 不含任何 token/密码; 只在单次下载结束时打一行。 */
        /* err = esp_err_t 原始码 (net_client.c:148 记录): 区分 DNS(-1/超时)、
         * TLS(0x6xxx)、连接拒绝、文件写入失败等 —— 光看 http=0 分不出来。 */
        ESP_LOGW(TAG, "DL[%d/%d] %s: state=%d http=%d got=%d/%u crc=%08x/%08x rc=%d err=0x%x",
                 i + 1, s_mir_n, pk->name,
                 (int)netc_state(), status, total, (unsigned)pk->size,
                 crc, pk->crc, rc, (unsigned)netc_err());
        netc_reset();
        /* ★ 必须同时比 size 和 crc32: 只比 size 挡不住"截断成等长的错误内容",
         *   只比 crc 又不够直观 (长度变了 crc 也会变, 但 size 一眼看出差多少) */
        /* ⚠ 2026-10-30 更正: 上一轮说"JSON 无 size/crc ⇒ 必然失败"是错的。
         *   am_store_json.c 已完整解析两者, 且 crc==0 的条目会被直接判非法而不进列表
         *   (见 :204 `if (rk.crc == 0) ok = false`)。这里的条件只是"无期望值时不误判"的兜底。 */
        if (rc == 0 && ((pk->size > 0 && total != (int)pk->size) ||
                        (pk->crc != 0 && crc != pk->crc))) {
            ESP_LOGD(TAG, "校验失败: 收到 %d B crc=%08x / 期望 %u B crc=%08x",
                     total, (unsigned)crc, (unsigned)pk->size, (unsigned)pk->crc);
            rc = 4;
        }
    }
    if (rc != 0) {
        /* ★ 2026-10-30: 这里**不再 remove(dst)**。
         *   现在的契约是: net_client 失败时只删 .part, 目标位置要么不存在(首次下载),
         *   要么是上一份**完好的旧版本** —— 在这里 remove 反而会把好文件删掉。 */
        s_dl_err = rc;
        s_permille = -1;
        /* ★ 2026-10-31: 统一视图 —— 失败原因落到 errmsg, 页面直接显示中文, 不用再猜码 */
        snprintf(s_dl_errmsg, sizeof(s_dl_errmsg), "%s", am_dl_err_text(rc));
        s_dl_bps   = 0;
        s_dl_phase = AM_DLP_FAIL;
        ESP_LOGW(TAG, "下载失败 rc=%d (%s)", rc, dst);
        /* ★★★ 2026-10-18 修: 失败也必须**清掉"正在下载"的名字** ——
         *   否则列表那行永远停在"下载中" ✗ (linit 报"一直显示下载中无法下载") */
        s_dl_name[0] = 0;
        __sync_synchronize();
        s_state = ARS_DLFAIL;
    } else {
        s_permille = 1000;
        ESP_LOGI(TAG, "下载完成: %s (%d B)", dst, total);
        /* ★★★ 2026-10-18 修顺序: **先用名字去自动安装, 再清空** ——
         *   之前先清 s_dl_name 再用 ⇒ fn 变成 ".lua" ⇒ 永远装不上 ✗ */
        if (is_lpk) {
            /* ★ 2026-11-01: 单文件 LPK —— 下载即完成, **没有解压阶段**。
             *   这里只做三件事: 验包(头/索引/必备资源) + 写安装标记 + 记模式=lpk。
             *   验包不过就删掉刚下的文件并报"安装失败" —— 不留"看起来装好了"的状态。 */
            s_permille = 960;
            s_dl_phase = AM_DLP_INSTALL;
            s_dl_bps   = 0;

            /* ★ 2026-11-01 修 B3：size/CRC32 之外再验 SHA-256（流式读文件，不进内存）。
             *   索引里有 sha256 就必须一致；没有则按旧协议（size+CRC32）并在日志里标明，
             *   不静默降级。 */
            if (pk->sha256[0]) {
                char got[65] = {0};
                if (!am_sha256_file(dst, got) || strcasecmp(got, pk->sha256) != 0) {
                    ESP_LOGE(TAG, "SHA-256 不符: got=%s want=%s ⇒ 删除 %s",
                             got[0] ? got : "(计算失败)", pk->sha256, dst);
                    remove(dst);
                    s_state = ARS_DLFAIL;
                    s_permille = -1;
                    s_dl_err = 4;
                    snprintf(s_dl_errmsg, sizeof(s_dl_errmsg),
                             "\xe6\xa0\xa1\xe9\xaa\x8c\xe5\xa4\xb1\xe8\xb4\xa5(SHA-256)");  /* 校验失败(SHA-256) */
                    s_dl_phase = AM_DLP_FAIL;
                    vTaskDelete(NULL); return;
                }
                ESP_LOGI(TAG, "SHA-256 OK %s", got);
            } else {
                ESP_LOGW(TAG, "索引无 sha256（旧协议）: 仅 size+CRC32 校验 %s", dst);
            }

            /* ★ 2026-11-01: 把**索引声明的版本**一并交给安装器写进 registry
             *   （store_ver/store_build）。若服务器只抬了索引、没重新打包，
             *   包内 manifest 会落后于索引；只按 manifest 比版本会导致
             *   "更新成功但仍显示更新、点了又下一次"永远退不掉。 */
            char sv[16];
            snprintf(sv, sizeof(sv), "%u", (unsigned)pk->ver);
            /* ★ 2026-11-02: 把索引里的**服务器文件 mtime/crc/size** 一并写进 registry ——
             *   这是"以后凭什么判有没有新版"的唯一依据。用的是服务器文件的时间，
             *   绝不是本地下载时间。 */
            bool ok = am_lpk_finish_meta2(dst, pk->target, sv, pk->build,
                                          pk->mtime, pk->crc, pk->size);
            s_dl_name[0] = 0;
            __sync_synchronize();
            s_state = ok ? ARS_OK : ARS_DLFAIL;
            if (ok) {
                s_permille = 1000;
                s_dl_phase = AM_DLP_DONE;
            } else {
                ESP_LOGE(TAG, "LPK 验包失败, 删除 %s", dst);
                remove(dst);
                s_permille = -1;
                s_dl_err   = 4;
                snprintf(s_dl_errmsg, sizeof(s_dl_errmsg),
                         "\xe5\xae\x89\xe8\xa3\x85\xe5\xa4\xb1\xe8\xb4\xa5");   /* 安装失败 */
                s_dl_phase = AM_DLP_FAIL;
                vTaskDelete(NULL); return;
            }
        } else if (is_asset) {
            /* 资源包: 就地解压到 target, 完事删掉 zip (TF 卡省空间)
             * ★ 解压阶段把进度从 70% 接到 100% (回调里映射) */
            s_permille = 700;
            /* ★ 2026-10-31: 进入"安装中"阶段 —— 字体包解包 20MB 要好一会儿,
             *   原来界面还写着"下载中 70%"不动, 用户以为卡住。 */
            s_dl_phase = AM_DLP_INSTALL;
            s_dl_bps   = 0;
            bool ok = am_asset_install_cb(dst, pk->target, am_asset_prog_70200);
            ESP_LOGI(TAG, "资源包解包 %s → %s : %s", dst, pk->target, ok ? "OK" : "失败");
            remove(dst);
            s_dl_name[0] = 0;
            __sync_synchronize();
            s_state = ok ? ARS_OK : ARS_DLFAIL;
            if (ok) {
                s_dl_phase = AM_DLP_DONE;
            } else {
                s_permille = -1;
                s_dl_err   = 4;
                snprintf(s_dl_errmsg, sizeof(s_dl_errmsg),
                         "\xe5\xae\x89\xe8\xa3\x85\xe5\xa4\xb1\xe8\xb4\xa5");   /* 安装失败 */
                s_dl_phase = AM_DLP_FAIL;
                vTaskDelete(NULL); return;
            }
        } else {
            extern bool am_store_auto_install(const char *fname);
            char nm[48], fn[56];
            snprintf(nm, sizeof(nm), "%s", s_dl_name);
            bool inst_ok = false;
            if (nm[0]) {
                snprintf(fn, sizeof(fn), "%s.lua", nm);
                inst_ok = am_store_auto_install(fn);
                if (inst_ok) ESP_LOGI(TAG, "已自动安装: %s", fn);
                else         ESP_LOGW(TAG, "自动安装失败: %s", fn);
            }
            s_dl_name[0] = 0;   /* 装完再清 ⇒ 列表第二行从"下载中"恢复成应用说明 ✓ */
            __sync_synchronize();
            s_state = ARS_OK;
            s_dl_phase = AM_DLP_DONE;
            if (!inst_ok) {
                /* 包下完了但装不上(空间不足/权限) —— 状态必须说清, 不能报"已安装" */
                s_dl_err = 3;
                snprintf(s_dl_errmsg, sizeof(s_dl_errmsg),
                         "\xe5\xae\x89\xe8\xa3\x85\xe5\xa4\xb1\xe8\xb4\xa5");   /* 安装失败 */
                s_dl_phase = AM_DLP_FAIL;
            }
        }
    }
    vTaskDelete(NULL);
}

bool am_rstore_dl_start(int i) {
    if (i < 0 || i >= s_rpkg_n) {
        ESP_LOGW(TAG, "dl_start 拒绝: 索引越界 i=%d rpkg_n=%d", i, s_rpkg_n);
        return false;
    }
    if (s_state == ARS_FETCHING || s_state == ARS_DOWNLOADING) {
        ESP_LOGW(TAG, "dl_start 拒绝: 状态忙 state=%d (1=正在刷索引 4=正在下载)", s_state);
        return false;
    }
    if (!wifi_manager_is_connected()) {
        ESP_LOGW(TAG, "dl_start 拒绝: WiFi 未连接");
        return false;
    }
    s_permille = -1;
    s_state = ARS_DOWNLOADING;
    /* 统一视图: 任务一开就进入"连接中", 页面立刻有反馈 (不必等首包) */
    s_dl_end_ms    = 0;
    s_dl_idx       = i;
    s_dl_phase     = AM_DLP_CONNECT;
    s_dl_got       = 0;
    s_dl_total     = s_rpkg[i].size;
    s_dl_bps       = 0;
    s_dl_asset     = (strcmp(s_rpkg[i].type, "asset") == 0);
    s_dl_files_done  = 0;
    s_dl_files_total = 0;
    s_dl_errmsg[0] = 0;
    /* ★★★ 2026-10-30 修「下载字体后设备立即重启」的根因 ★★★
     *   本任务下载完**就地解包**：am_dl_task → am_asset_install_cb → mz_zip_reader_extract_to_callback。
     *   反汇编实测各层栈帧（xtensa-esp32s3-elf-objdump，entry a1, N）：
     *       am_dl_task                          0x470 =  1136 B
     *       am_asset_install_cb                 0xa40 =  2624 B
     *       mz_zip_reader_extract_to_callback   0x25c0 = 9664 B   ← 大头
     *                                        合计 ≈ 13424 B
     *   原来这里给 8192 B ⇒ **溢出 5.2KB**，踩坏相邻 TCB/heap 后 panic 复位。
     *   真机现场完全吻合：SD 根目录留下完整的 font_wqy14.zip / font_fusion12.zip
     *   （那是另一个任务 netc 写好并 rename 的），但 /sdcard/fonts 目录没建出来
     *   （mkdirs 在解包前，崩溃时 FAT 目录项还在 cache 里，复位即丢），
     *   设备 uptime 只有十分钟级 —— 就是崩溃重启，不是主动重启。
     *
     *   给 20480：13424 之外还留 ~7KB 给 miniz 深层调用（iter_read / 中央目录解析）
     *   和编译器的余量。任务只在下载期间存在（结束即 vTaskDelete），非常驻。
     *   内部 RAM 余量：下载时 netc(8192)+am_dl(20480) 共 28KB，实测 internal free 185KB，够。 */
    /* ★★★ 2026-10-31 (linit) 定稿：**20 KB**。
     *
     *   演进过程（每一步都有真机证据，别再凭感觉调大）：
     *     8 KB  → 解包时栈溢出重启（stored 路径实测需 ≈13.4 KB）
     *     20 KB → 够 stored；但当时服务器上的 pal.zip 是 deflate，
     *             走 miniz 时在 miniz_zip.c:1909 空指针崩（miniz 内部要 64 KB 缓冲）
     *     32 KB → **任务根本创建不出来**：设备内部 RAM largest free block 只有 31744 B，
     *             32 KB 栈 + TCB 超过它 ⇒ xTaskCreate 失败 ⇒ 商店点"下载"只弹一句
     *             "无法开始下载"（而失败日志是 ESP_LOGD，早就被 MAXIMUM_LEVEL 裁掉了，
     *             所以完全静默，特别难查）。
     *
     *   现在 am_asset.c 的 stored 条目**不再经过 miniz**（直接从 zip 搬字节，
     *   全程只用 4 KB 静态缓冲），所以 20 KB 富余得很；
     *   deflate 条目直接判失败（不会再走 miniz、不会崩）。
     *   ⇒ 20 KB 是这里的正解，不要加大：加大反而创建不出任务。 */
    /* ★★★ 2026-11-01（内存专项）：**按包型给栈**，不要再一律 20 KB。
     *
     *   实测现场：internal free=42 KB / largest=22.5 KB 时，
     *     am_dl(20480) 把这唯一的大块吃掉 → 紧接着 netc 要 8 KB 连续块就拿不到
     *     ⇒ "NETC: 任务创建失败" ⇒ 商店显示下载失败/连接超时（不是网络问题）。
     *
     *   20 KB 是给 **zip 资源包**的：mz_zip_reader_extract_to_callback 单帧 9.6 KB
     *   （反汇编实测，见下面的历史记录）。而 **LPK / .lua 包根本不过 miniz**：
     *   LPK 走 lpk1 的流式解包（4 KB 缓冲），.lua 只是搬文件 ⇒ 12 KB 足够，
     *   余下 10 KB 正好留给 netc 的 8 KB 栈 + TCB。
     *   ⇒ 内存紧的时候，第三方应用的下载不再被"自己把内存占满"卡死。 */
    const uint32_t dl_stack = s_dl_asset ? 20480u : 12288u;

    /* 下载开始前先放掉**闲置的 TLS 会话**：keep-alive 的一条连接要占几十 KB 内部 RAM，
     * 而下载马上要新建自己的连接，留着纯属浪费（还会把大块切碎）。
     * 关掉后 heap 会立刻多出一块连续空间，两个任务都能建出来。 */
    netc_ka_close();
    ESP_LOGI(TAG, "下载前内存: 内部 free=%u largest=%u (栈需求 %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)dl_stack);

    BaseType_t tk = xTaskCreatePinnedToCore(am_dl_task, "am_dl", dl_stack,
                                            (void *)(intptr_t)i, 4, NULL, 0);
    /* 建不出来再试一次：上面刚放掉会话，heap 可能刚好合并出连续块（不额外占资源）。 */
    if (tk != pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(120));
        tk = xTaskCreatePinnedToCore(am_dl_task, "am_dl", dl_stack,
                                     (void *)(intptr_t)i, 4, NULL, 0);
    }
    if (tk != pdPASS) {
        /* ★ 失败必须喊出来：原来这条是 ESP_LOGD，被 CONFIG_LOG_MAXIMUM_LEVEL=3 裁掉，
         *   结果"点下载只弹一句无法开始下载"完全查不到根因。 */
        ESP_LOGE(TAG, "下载任务创建失败(内部RAM不足): 需要 %u B 栈, 当前 free=%u 最大块 %u B",
                 (unsigned)dl_stack,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        s_state = ARS_READY;
        s_dl_phase = AM_DLP_IDLE;   /* 没起来就别留一个假的"连接中" */
        s_dl_idx   = -1;
        return false;
    }
    return true;
}

/* 取"索引刚更新过"事件(取一次即清)。页面 poll 拿到就重建列表 + 置脏。 */
bool am_store_take_idx_updated(void)
{
    if (!s_idx_updated) return false;
    s_idx_updated = false;
    return true;
}
