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
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "am_internal.h"
#include "am_store_json.h"
#include "sd_scan.h"
#include "wifi_manager.h"

#define TAG "AMSTORENET"

#define AM_IDX_MAX      (16 * 1024)         /* index.json 正文上限 */
#define AM_DL_MAX       (2 * 1024 * 1024)   /* 单包上限 (防服务器给错东西跑飞) */
#define AM_DL_CHUNK     2048

/* 商店源目录: 与 am_store.c 的 AM_STORE_SRC1/SRC2 必须一致 */
#define AM_SRC_SPIFFS   "/store"
#define AM_SRC_SD       "/sdcard/store_pkg"

/* ==================== 状态 (后台任务写, UI 读) ==================== */
static volatile int s_state    = ARS_IDLE;
static volatile int s_permille = -1;     /* 0..1000; -1 = 总长未知 */
static volatile int s_dl_err   = 0;      /* ARS_DLFAIL 时的原因码, 供日志/文案 */
static char s_dl_name[32];               /* 正在下载的包名 */
static char s_dl_label[48];              /* 正在下载的中文名 */

/* 解析结果 */
static char s_mir[AM_MIRROR_MAX][AM_MIRROR_LEN];
static int  s_mir_n = 0;
static EXT_RAM_BSS_ATTR am_rpkg_t s_rpkg[AM_RPKG_MAX];
static int  s_rpkg_n = 0;
static volatile bool s_fetched = false;  /* 拉过至少一次 (成功或失败), 防重入 */

/* 缓冲: 索引正文放 PSRAM (纯 socket 读); 落盘块**必须内部 RAM** (见文件头 ③) */
static EXT_RAM_BSS_ATTR char s_idx[AM_IDX_MAX + 1];
static char s_dl_buf[AM_DL_CHUNK];

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
     *   所以得先试一个已知源。做法: 依次试**已知缺省源**里的第一个 + 已经解析出来的
     *   mirrors (首次运行时 s_mir_n 为 0, 用内置的 == store_build.py 的默认主源)。
     *   内置源通了, 它返回的 mirrors[] 就覆盖/扩展后续要用的源列表。 */
    static const char *k_boot = "https://bbk.linit.cn/bbk";
    char tries[AM_MIRROR_MAX + 1][AM_MIRROR_LEN];
    int  ntry = 0;
    snprintf(tries[ntry++], AM_MIRROR_LEN, "%s", k_boot);
    for (int i = 0; i < s_mir_n && ntry < AM_MIRROR_MAX + 1; i++)
        snprintf(tries[ntry++], AM_MIRROR_LEN, "%s", s_mir[i]);

    int rc = -99;
    for (int i = 0; i < ntry; i++) {
        char url[AM_MIRROR_LEN + 32];
        url3(url, sizeof(url), tries[i], "/index.json", NULL);
        esp_http_client_config_t cfg = {
            .url = url, .timeout_ms = 10000,
            .buffer_size = 1024,                 /* 内部 RAM 最紧张, 响应体我们自己拼 */
            .disable_auto_redirect = false,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = false,
        };
        int total = 0, status = 0;
        esp_err_t e = ESP_FAIL;
        esp_http_client_handle_t cl = esp_http_client_init(&cfg);
        if (cl) {
            e = esp_http_client_open(cl, 0);
            if (e == ESP_OK) {
                esp_http_client_fetch_headers(cl);
                status = esp_http_client_get_status_code(cl);
                if (status == 200) {
                    for (;;) {
                        int n = esp_http_client_read(cl, s_idx + total, AM_IDX_MAX - total);
                        if (n <= 0) break;
                        total += n;
                        if (total >= AM_IDX_MAX) break;
                    }
                }
            }
            esp_http_client_close(cl);
            esp_http_client_cleanup(cl);
        }
        if (total > 0) s_idx[total] = 0; else s_idx[0] = 0;
        ESP_LOGI(TAG, "引导源%d: e=%d http=%d len=%d url=%s", i, (int)e, status, total, url);
        if (total > 0 && status == 200) {
            char mir[AM_MIRROR_MAX][AM_MIRROR_LEN];
            int  mn = 0, pn = 0;
            int pr = am_store_json_parse(s_idx, s_rpkg, AM_RPKG_MAX, &pn,
                                         mir, AM_MIRROR_MAX, &mn);
            if (pr == 0) {
                memcpy(s_mir, mir, sizeof(s_mir));
                s_mir_n = mn;
                s_rpkg_n = pn;
                ESP_LOGI(TAG, "索引 OK: %d 个包, %d 个镜像源, 跳过 %d 条",
                         s_rpkg_n, s_mir_n, am_store_json_skipped());
                rc = 0;
                break;
            }
            rc = pr;
            ESP_LOGW(TAG, "索引解析失败 rc=%d (跳过 %d 条)", pr, am_store_json_skipped());
        }
    }
    if (rc == 0) {
        __sync_synchronize();          /* 先写完数据, 再放状态 */
        s_state = ARS_READY;
    } else {
        ESP_LOGE(TAG, "索引拉取/解析全部失败 rc=%d", rc);
        s_state = ARS_FAIL;
    }
    vTaskDelete(NULL);
}

void am_rstore_prefetch(void) {
    if (s_fetched || s_state == ARS_FETCHING || s_state == ARS_DOWNLOADING) return;
    if (!wifi_manager_is_connected()) return;   /* ★ 见文件头 ①: 没联网一律不发请求 */
    s_fetched = true;
    s_state = ARS_FETCHING;
    /* 任务栈必须内部 RAM (TLS 会短暂禁中断; 8KB 与 luat_lib_net 的 fetch 任务同规格) */
    if (xTaskCreatePinnedToCore(am_idx_task, "am_idx", 8192, NULL, 4, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "索引任务创建失败(内存不足)");
        s_state = ARS_FAIL;
        s_fetched = false;
    }
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

/* ==================== crc32 (zlib/以太网, 半字节表) ====================
 * 必须与 tools/store_build.py 的 binascii.crc32 **逐位一致**, 否则一装就"校验失败"。
 * 这里用 16 项半字节表 (64 字节 flash) 而不是 256 项全表 (1KB) —— 空间换一点点 CPU。
 * 表与算法已用 tools/store_parse_test.py 对**全部真实包**逐一比对过。 */
static const uint32_t CRC_T[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};
static uint32_t crc32_upd(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ CRC_T[crc & 0x0Fu];
        crc = (crc >> 4) ^ CRC_T[crc & 0x0Fu];
    }
    return ~crc;
}

/* ==================== 后台: 下载一个包 ==================== */
static void am_dl_task(void *arg) {
    int idx = (int)(intptr_t)arg;
    const am_rpkg_t *pk = &s_rpkg[idx];
    snprintf(s_dl_name, sizeof(s_dl_name), "%s", pk->name);
    snprintf(s_dl_label, sizeof(s_dl_label), "%s", pk->label);
    s_permille = -1;
    s_dl_err = 0;

    /* 目标目录: 优先 TF 卡 (不占宝贵的 4MB SPIFFS; 且写卡不受"禁 flash cache"影响),
     * 没插卡才退回 SPIFFS 根 —— 与 am_store.c 的两个扫描源一一对应。 */
    const bool to_sd = sd_is_mounted();
    const char *dir = to_sd ? AM_SRC_SD : AM_SRC_SPIFFS;
    if (to_sd) mkdir(AM_SRC_SD, 0777);
    char dst[192];
    snprintf(dst, sizeof(dst), "%s/%s.lua", dir, pk->name);
    ESP_LOGI(TAG, "下载 %s -> %s (%u B)", pk->name, dst, (unsigned)pk->size);

    int rc = 0;   /* 0=OK 1=HTTP 2=连接 3=写盘 4=校验 */
    uint32_t crc = 0;
    int total = 0, status = 0, clen = 0;
    FILE *out = NULL;

    for (int i = 0; i < s_mir_n && rc == 0; i++) {
        char url[AM_MIRROR_LEN + 128];
        url3(url, sizeof(url), s_mir[i], "/", pk->path);
        if (i > 0) ESP_LOGW(TAG, "换源重试: %s", url);

        esp_http_client_config_t cfg = {
            .url = url, .timeout_ms = 15000,
            .buffer_size = 1024,
            .disable_auto_redirect = false,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .keep_alive_enable = false,
        };
        esp_http_client_handle_t cl = esp_http_client_init(&cfg);
        if (!cl) { rc = 2; break; }
        esp_err_t e = esp_http_client_open(cl, 0);
        if (e != ESP_OK) { rc = 2; goto next; }
        clen = (int)esp_http_client_fetch_headers(cl);
        status = esp_http_client_get_status_code(cl);
        if (status != 200) { rc = 1; goto next; }
        if (clen > 0) s_permille = 0;

        out = fopen(dst, "wb");
        if (!out) { rc = 3; goto next; }
        crc = 0; total = 0;
        for (;;) {
            int n = esp_http_client_read(cl, s_dl_buf, AM_DL_CHUNK);
            if (n < 0) { rc = 2; break; }
            if (n == 0) break;
            crc = crc32_upd(crc, (const uint8_t *)s_dl_buf, (size_t)n);
            if (fwrite(s_dl_buf, 1, (size_t)n, out) != (size_t)n) { rc = 3; break; }
            total += n;
            if (clen > 0) s_permille = (int)((int64_t)total * 1000 / clen);
            if (total > AM_DL_MAX) { rc = 3; break; }
        }
        fclose(out);
        out = NULL;
    next:
        esp_http_client_close(cl);
        esp_http_client_cleanup(cl);
        /* ★ 必须同时比 size 和 crc32: 只比 size 挡不住"截断成等长的错误内容",
         *   只比 crc 又不够直观 (长度变了 crc 也会变, 但 size 一眼看出差多少) */
        if (rc == 0 && (total != (int)pk->size || crc != pk->crc)) {
            ESP_LOGE(TAG, "校验失败: 收到 %d B crc=%08x / 期望 %u B crc=%08x",
                     total, (unsigned)crc, (unsigned)pk->size, (unsigned)pk->crc);
            rc = 4;
        }
    }
    if (out) fclose(out);
    if (rc != 0) {
        remove(dst);      /* 不许留半个包在商店源目录里 (会被当成"待安装") */
        s_dl_err = rc;
        s_permille = -1;
        ESP_LOGE(TAG, "下载失败 rc=%d (%s)", rc, dst);
        __sync_synchronize();
        s_state = ARS_DLFAIL;
    } else {
        s_permille = 1000;
        ESP_LOGI(TAG, "下载完成: %s (%d B)", dst, total);
        __sync_synchronize();
        s_state = ARS_OK;
    }
    vTaskDelete(NULL);
}

bool am_rstore_dl_start(int i) {
    if (i < 0 || i >= s_rpkg_n) return false;
    if (s_state == ARS_FETCHING || s_state == ARS_DOWNLOADING) return false;
    if (!wifi_manager_is_connected()) return false;
    s_permille = -1;
    s_state = ARS_DOWNLOADING;
    if (xTaskCreatePinnedToCore(am_dl_task, "am_dl", 8192, (void *)(intptr_t)i, 4, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "下载任务创建失败(内存不足)");
        s_state = ARS_READY;
        return false;
    }
    return true;
}
