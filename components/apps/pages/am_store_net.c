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
#include "os_sources.h"
#include "store_index_builtin.h"   /* ★ 固件内置商店索引(离线可用) */
#include "am_store_json.h"
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

/* 解析结果 */
static char s_mir[AM_MIRROR_MAX][AM_MIRROR_LEN];
static int  s_mir_n = 0;
static EXT_RAM_BSS_ATTR am_rpkg_t s_rpkg[AM_RPKG_MAX];
static int  s_rpkg_n = 0;
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
            s_mir_n = mn; s_rpkg_n = pn;
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
                s_rpkg_n = pn;
                /* ★★★ 2026-10-17: 云端索引解析出 0 个包 ⇒ 说明索引损坏/是旧格式(如 crc32 被写成数字),
                 *   此时**回退到固件内置索引** —— 坏索引再也不会把商店清空。 */
                if (pn == 0) {
                    char mir2[AM_MIRROR_MAX][AM_MIRROR_LEN];
                    int mn2 = 0, pn2 = 0;
                    if (am_store_json_parse(STORE_INDEX_BUILTIN, s_rpkg, AM_RPKG_MAX, &pn2,
                                            mir2, AM_MIRROR_MAX, &mn2) == 0 && pn2 > 0) {
                        s_rpkg_n = pn2;
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
                s_mir_n = mn; s_rpkg_n = pn;
                got = true;
                ESP_LOGI(TAG, "★ 离线: 已用**上次联网缓存**索引 %d 个包(/store/index.json %u B)",
                         pn, (unsigned)n);
            }
        }
        if (!got && am_store_json_parse(STORE_INDEX_BUILTIN, s_rpkg, AM_RPKG_MAX, &pn,
                                        mir, AM_MIRROR_MAX, &mn) == 0) {
            memcpy(s_mir, mir, sizeof(s_mir));
            s_mir_n = mn; s_rpkg_n = pn;
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
bool am_rstore_dl_unknown_len(void) { return s_dl_unknown_len; }

/* 状态机推进: 由商店页每帧调, 返回 true = 列表该重建了。
 * ★ 必须**边沿触发**: 早先写成"状态是 READY 就返回 true" → 上层每帧重建列表, 白烧 CPU。 */
static int s_reported = ARS_IDLE;
bool am_rstore_tick(void) {
    am_rstore_prefetch();          /* 已联网且没拉过 → 顺手把索引拉起来 */
    const int st = s_state;
    const bool edge = (st != s_reported) &&
                      (st == ARS_READY || st == ARS_FAIL || st == ARS_OK || st == ARS_DLFAIL);
    s_reported = st;
    return edge;
}
void am_rstore_ack(void) {
    __sync_synchronize();
    if (s_state == ARS_OK || s_state == ARS_DLFAIL) s_state = ARS_READY;
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

static void am_dl_task(void *arg) {
    int idx = (int)(intptr_t)arg;
    const am_rpkg_t *pk = &s_rpkg[idx];
    snprintf(s_dl_name, sizeof(s_dl_name), "%s", pk->name);
    snprintf(s_dl_label, sizeof(s_dl_label), "%s", pk->label);
    s_permille = -1;
    s_dl_err = 0;
    s_dl_unknown_len = false;

    /* 目标目录: 优先 TF 卡 (不占宝贵的 4MB SPIFFS; 且写卡不受"禁 flash cache"影响),
     * 没插卡才退回 SPIFFS 根 —— 与 am_store.c 的两个扫描源一一对应。 */
    /* ★ 2026-10-25: 资源包 (type=asset) 不落"商店源目录", 直接落 TF 卡根,
     *   下完就地解到 target, 解完删 zip —— 它不是一个可安装的 .lua。 */
    const bool is_asset = (strcmp(pk->type, "asset") == 0);
    const bool to_sd = sd_is_mounted();
    const char *dir = to_sd ? AM_SRC_SD : AM_SRC_SPIFFS;
    if (to_sd) mkdir(AM_SRC_SD, 0777);
    char dst[192];
    if (is_asset) {
        const char *base = strrchr(pk->path, '/');
        base = base ? base + 1 : pk->path;
        snprintf(dst, sizeof(dst), "/sdcard/%s", base);
    } else {
        snprintf(dst, sizeof(dst), "%s/%s.lua", dir, pk->name);
    }
    ESP_LOGI(TAG, "下载 %s -> %s (%u B)%s", pk->name, dst, (unsigned)pk->size,
             is_asset ? " [资源包]" : "");

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
        uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
        /* 资源包动辄 20MB+, 180s 不够 (Wi-Fi 200KB/s ⇒ 26MB 约 130s, 余量太小),
         * 给 asset 单独放宽到 10 分钟; 普通 .lua 包仍是 180s。 */
        const uint32_t dl_limit_ms = is_asset ? 600000u : 180000u;
        while (netc_busy() && (uint32_t)(esp_timer_get_time() / 1000) - t0 < dl_limit_ms) {
            int g = netc_got();
            /* ★ 2026-10-25 linit: 资源包把进度条按 7:3 切开 ——
             *   下载只走 0..70%, 剩下 30% 留给"解压安装", 到 100% 才是真完成。
             *   普通 .lua 包仍按 0..100% (下载完即装完)。 */
            if (pk->size > 0) {
                int span = is_asset ? 700 : 1000;
                s_permille = (int)((int64_t)g * span / (int64_t)pk->size);
            } else {
                /* ⚠ 2026-10-30 更正: 上一轮注释写"JSON 没有 size/crc"是**错的** ——
                 *   am_store_json.c:196-204 确实解析了 size(j_num) 和 crc32(strtoul hex)。
                 *   这里保留 else 分支只是**兜底**: 旧索引/畸形条目缺 size 时不至于卡在 0。
                 *   真正的"进度不动"根因尚未定位 (怀疑 netc_start 失败或镜像不可达), 见报告。 */
                s_dl_unknown_len = true;
                int cap = is_asset ? 690 : 990;
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
        if (is_asset) {
            /* 资源包: 就地解压到 target, 完事删掉 zip (TF 卡省空间)
             * ★ 解压阶段把进度从 70% 接到 100% (回调里映射) */
            s_permille = 700;
            bool ok = am_asset_install_cb(dst, pk->target, am_asset_prog_70200);
            ESP_LOGI(TAG, "资源包解包 %s → %s : %s", dst, pk->target, ok ? "OK" : "失败");
            remove(dst);
            s_dl_name[0] = 0;
            __sync_synchronize();
            s_state = ok ? ARS_OK : ARS_DLFAIL;
            if (!ok) { s_permille = -1; vTaskDelete(NULL); return; }
        } else {
            extern bool am_store_auto_install(const char *fname);
            char nm[48], fn[56];
            snprintf(nm, sizeof(nm), "%s", s_dl_name);
            if (nm[0]) {
                snprintf(fn, sizeof(fn), "%s.lua", nm);
                if (am_store_auto_install(fn)) ESP_LOGI(TAG, "已自动安装: %s", fn);
                else                           ESP_LOGW(TAG, "自动安装失败: %s", fn);
            }
            s_dl_name[0] = 0;   /* 装完再清 ⇒ 列表第二行从"下载中"恢复成应用说明 ✓ */
            __sync_synchronize();
            s_state = ARS_OK;
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
    if (xTaskCreatePinnedToCore(am_dl_task, "am_dl", 20480, (void *)(intptr_t)i, 4, NULL, 0) != pdPASS) {
        ESP_LOGD(TAG, "下载任务创建失败(内存不足)");
        s_state = ARS_READY;
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
