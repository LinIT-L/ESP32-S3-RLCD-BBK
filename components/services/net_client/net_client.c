/* net_client.c — LinTOS 唯一 HTTP(S) 客户端实现 (见 net_client.h 的设计说明) */
#include "net_client.h"
#include "netc_fs_replace.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/errno.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define TAG "NETC"

#define NETC_URL_MAX   256
#define NETC_PATH_MAX  192

/* 请求参数 (netc_start 时一次性拷进来; 之后只读) */
static char   s_url[NETC_URL_MAX];
static char   s_path[NETC_PATH_MAX];
static char   s_ua[64];
static char  *s_body;          /* 调用方缓冲 (PSRAM) */
static int    s_cap;
static int    s_timeout;
static bool   s_no_redirect;

/* 运行态 (任务写, 任何上下文读) */
static volatile int  s_state;   /* netc_state_t */
static volatile int  s_http, s_len, s_err, s_got;
static volatile bool s_cancel;
static bool     s_want_crc;
static uint32_t s_crc;

/* crc32 (zlib/以太网, **16 项半字节表**): 必须与 tools/store_build.py 的 binascii.crc32
 * 逐位一致, 否则商店下载完会"校验失败"。表与算法与 am_store_net.c 的同源实现完全相同。 */
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

/* ★ 落盘搬运缓冲: 必须**内部 RAM** (写 SPIFFS 时 flash 驱动会关 Flash+PSRAM cache,
 *   那个窗口里读 PSRAM 源缓冲会取到垃圾 —— 商店那套踩过, 见 am_store_net.c 文件头③)。
 *   改为**按需分配**: 只在"下载落盘"时 malloc、任务结束即 free ⇒ 平时不占这 8KB。
 *   (原来 static char s_chunk[8192] 是常驻内部 BSS, 不用也占。) */
#define NETC_CHUNK  8192

static const char *DEFAULT_UA = "LinTOS/1.0 (ESP32-S3)";

/* ============ 落盘替换 / 恢复 (2026-10-30) ============
 * 核心实现在 **netc_fs_replace.c**（不依赖 ESP-IDF，主机测试直接编译同一份源码，
 * 不再有"测试副本与生产代码漂移"的问题）。这里只做三件事：
 *   ① 构造 `.bak` 路径（本模块单请求槽，一个静态缓冲就够）；
 *   ② 打日志（结果码 → ESP_LOG）；
 *   ③ 对外暴露 netc_recover_path()，让应用中心扫描复用同一套恢复逻辑。
 *
 * 事实依据（不是 POSIX 直觉）:
 *   - 落盘目标是 /sdcard/... ⇒ FATFS(VFS) ⇒ vfs_fat_rename() 直接调 f_rename()
 *     (esp-idf-v60/components/fatfs/vfs/vfs_fat.c:959-966);
 *   - f_rename() 对 FAT/FAT32 **目标已存在时返回 FR_EXIST, 不覆盖** (ff.c:5244);
 *   - IDF 6.0.3 **没有** CONFIG_FATFS_VFS_RENAME_REPLACES_DESTINATION 这个选项。
 */
static char s_bak_path[sizeof(s_path) + 8];

/* 下载路径专用（单请求槽，无并发） */
static void netc_recover_file(const char *final_path)
{
    if (!final_path || !final_path[0]) return;
    snprintf(s_bak_path, sizeof(s_bak_path), "%s.bak", final_path);
    switch (netc_fs_recover(final_path, s_bak_path)) {
    case NETC_REC_RESTORED:
        ESP_LOGW(TAG, "掉电恢复: %s 已从 .bak 复位", final_path);
        break;
    case NETC_REC_CLEANED:
        ESP_LOGI(TAG, "清理陈旧 .bak: %s", final_path);
        break;
    case NETC_REC_FAILED:
        ESP_LOGE(TAG, "掉电恢复失败: %s (.bak 保留)", final_path);
        break;
    default:
        break;
    }
}

int netc_recover_path(const char *final_path)
{
    if (!final_path || !final_path[0]) return (int)NETC_REC_NONE;
    /* 自带栈缓冲(≈204B)：不碰 s_bak_path ⇒ 与正在进行的下载**零共享状态**,
     * 调用方(应用中心扫描任务 8KB 栈)即使与 netc 任务并发也无竞争。 */
    char bak[NETC_PATH_MAX + 8];
    snprintf(bak, sizeof(bak), "%s.bak", final_path);
    netc_rec_t r = netc_fs_recover(final_path, bak);
    switch (r) {
    case NETC_REC_RESTORED: ESP_LOGW(TAG, "掉电恢复(扫描触发): %s 已复位", final_path); break;
    case NETC_REC_CLEANED:  ESP_LOGI(TAG, "清理陈旧 .bak(扫描触发): %s", final_path);   break;
    case NETC_REC_FAILED:   ESP_LOGE(TAG, "掉电恢复(扫描触发)失败: %s", final_path);     break;
    default: break;
    }
    return (int)r;
}

/* 完整性闸门: 期望值非 0 才校验 */
static uint32_t s_expect_crc;
static uint32_t s_expect_size;


static void netc_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "请求: 内部 free=%u largest=%u psram=%u url=%s%s",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             s_url, s_path[0] ? " -> 文件" : "");
    esp_http_client_config_t cfg = {
        .url = s_url,
        .timeout_ms = s_timeout,
        .buffer_size = 1024,        /* 内部 RAM 最紧张, 省 1KB (响应体我们自己搬) */
        .disable_auto_redirect = s_no_redirect,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .user_agent = s_ua[0] ? s_ua : DEFAULT_UA,
    };
    int total = 0, status = 0, clen = -1;
    esp_err_t e = ESP_OK;
    /* 落盘才需要 8KB 搬运缓冲 —— 按需分配内部 RAM, 用完即释放 (不下载就不占) */
    char *chunk = NULL;
    int chunk_sz = NETC_CHUNK;
    if (s_path[0]) {
        /* ★ 2026-10-30 修「下载立刻失败/进度 0」的一个高概率来源:
         *   本任务已占 8KB 栈, 若再硬要 8KB 连续内部 RAM (合计 16KB), 在 ESP32-S3
         *   内存紧张时(带 WiFi + 音乐 + 商店列表) heap_caps_malloc 常直接 NULL,
         *   于是 e=ESP_FAIL → 状态 ERR → 上层看到"下载失败、进度一动不动"。
         *   这里改为**降级分配**: 8KB → 2KB → 1KB, 只要拿到一块就能继续;
         *   全拿不到才算真失败 (此时日志会明确指出是内部 RAM 不足)。 */
        chunk = heap_caps_malloc(NETC_CHUNK, MALLOC_CAP_INTERNAL);
        if (!chunk) {
            chunk_sz = 2048;
            chunk = heap_caps_malloc(chunk_sz, MALLOC_CAP_INTERNAL);
            if (chunk) ESP_LOGW(TAG, "内部RAM紧张: 落盘缓冲降级为 %d 字节", chunk_sz);
        }
        if (!chunk) {
            chunk_sz = 1024;
            chunk = heap_caps_malloc(chunk_sz, MALLOC_CAP_INTERNAL);
            if (chunk) ESP_LOGW(TAG, "内部RAM很紧张: 落盘缓冲降级为 %d 字节", chunk_sz);
        }
        if (!chunk) { ESP_LOGE(TAG, "内部RAM耗尽: 连 1KB 落盘缓冲都分配不到"); e = ESP_FAIL; }
    }
    FILE *fp = NULL;
    /* ★ 2026-10-30: 落盘改**临时文件 + 成功后原子改名**。
     *   原实现直接 fopen(最终路径,"wb"): 下到一半掉电/硬复位, 就会在商店源目录里
     *   留一个半截文件 —— 上层按"文件已存在"当待安装, 体积/CRC 全错且不会自愈。
     *   改法: 先写 `<最终路径>.part`, 只有 状态码 200 + 长度相符 才 rename 到最终路径;
     *   任何失败都只删 .part, **不动**可能已存在的旧文件(以前失败会 remove 最终路径,
     *   把上一版好文件一起删掉)。对调用方完全透明: 入参/出参路径语义不变。 */
    /* 静态单槽: 与 s_path 同生命周期(本模块本就只有"单请求槽"),
     * **不放任务栈** —— netc 任务只有 8KB 栈且要跑 mbedtls, 这里再占数百字节会挤 TLS。 */
    static char part_path[sizeof(s_path) + 8];
    part_path[0] = 0;
    if (e == ESP_OK && s_path[0]) {
        netc_recover_file(s_path);        /* 上次掉电遗留的 .bak 先复位 */
        snprintf(part_path, sizeof(part_path), "%s.part", s_path);
        fp = fopen(part_path, "wb");
        if (!fp) { ESP_LOGW(TAG, "打不开落盘文件 %s", part_path); e = ESP_FAIL; }
    }
    esp_http_client_handle_t cl = NULL;
    if (e == ESP_OK) cl = esp_http_client_init(&cfg);
    if (!cl && e == ESP_OK) { ESP_LOGW(TAG, "client init 返回 NULL (URL/配置非法?) url=%s", s_url); e = ESP_FAIL; }
    if (cl) {
        e = esp_http_client_open(cl, 0);
        if (e != ESP_OK) ESP_LOGW(TAG, "open 失败 e=%d(%s) url=%s", (int)e, esp_err_to_name(e), s_url);
        if (e == ESP_OK) {
            esp_http_client_fetch_headers(cl);
            status = esp_http_client_get_status_code(cl);
            clen = (int)esp_http_client_get_content_length(cl);
            for (;;) {
                if (s_cancel) { e = ESP_ERR_INVALID_STATE; break; }
                if (fp) {
                    int n = esp_http_client_read(cl, chunk, chunk_sz);
                    if (n <= 0) break;
                    if (fwrite(chunk, 1, (size_t)n, fp) != (size_t)n) {
                        ESP_LOGW(TAG, "写文件失败 %s (卡满?)", s_path);
                        e = ESP_FAIL;
                        break;
                    }
                    if (s_want_crc) s_crc = crc32_upd(s_crc, (const uint8_t *)chunk, (size_t)n);
                    total += n;
                    s_got = total;
                } else {
                    int room = s_cap - 1 - total;
                    if (room <= 0) break;
                    int n = esp_http_client_read(cl, s_body + total, room);
                    if (n <= 0) break;
                    if (s_want_crc) s_crc = crc32_upd(s_crc, (const uint8_t *)(s_body + total), (size_t)n);
                    total += n;
                    s_got = total;
                }
            }
            /* ★ 状态码 + 长度双校验: 4xx/5xx 也会"成功收完"; 长度不符 = 被截断 (断流/代理超时) */
            if (e == ESP_OK && status != 200) {
                ESP_LOGW(TAG, "HTTP %d (非 200) len=%d url=%s", status, total, s_url);
                e = ESP_FAIL;
            }
            if (e == ESP_OK && clen > 0 && total != clen) {
                ESP_LOGW(TAG, "长度不符 收 %d / 应 %d (被截断?) url=%s", total, clen, s_url);
                e = ESP_FAIL;
            }
            /* ★★★ 2026-10-30 完整性闸门 —— **必须**在替换阶段之前:
             *   期望体积/CRC 非 0 才校验; 不通过 ⇒ e=FAIL ⇒ 下面只删 .part,
             *   目标位置(可能是上一版完好的文件)**一个字节都不动**。
             *   调用方 am_store_net 收完还会再比一次, 这里是刻意双保险:
             *   一处是"替换前"的硬闸门(保证旧文件不被坏数据顶掉),
             *   一处是"调用方"的验收(保证 UI/安装逻辑拿到的是对的字节数)。 */
            if (e == ESP_OK && s_expect_size && (uint32_t)total != s_expect_size) {
                ESP_LOGW(TAG, "校验失败: 体积 %d != 期望 %u (不替换, 旧文件保留) url=%s",
                         total, (unsigned)s_expect_size, s_url);
                e = ESP_FAIL;
            }
            if (e == ESP_OK && s_expect_crc && s_crc != s_expect_crc) {
                ESP_LOGW(TAG, "校验失败: crc %08x != 期望 %08x (不替换, 旧文件保留) url=%s",
                         (unsigned)s_crc, (unsigned)s_expect_crc, s_url);
                e = ESP_FAIL;
            }
        }
        esp_http_client_close(cl);
        esp_http_client_cleanup(cl);
    }
    if (fp) {
        fclose(fp);
        if (e == ESP_OK) {
            /* ★ 2026-10-30: 替换阶段不再直接 rename(part, final)。
             *   **FatFs 的 f_rename 在目标已存在时返回 FR_EXIST, 不会覆盖**
             *   (esp-idf-v60/components/fatfs/src/ff.c:5243-5245:
             *    `res = (djn.obj.sclust==djo.obj.sclust && djn.dptr==djo.dptr) ? FR_NO_FILE : FR_EXIST;`)
             *   ⇒ 重下已存在的包时, 直接 rename 会失败。这里改成"旧文件先挪成 .bak,
             *   新文件就位后再删 .bak", 任何一步失败都回滚, **全过程中不删除旧版本**。
             *   诚实说明: 这**不是** POSIX 原子替换, 而是"可恢复的非原子替换";
             *   掉电最坏结果是 final 暂时缺失(.bak 里有完整旧版), 由下次访问恢复。 */
            if (part_path[0]) {
                snprintf(s_bak_path, sizeof(s_bak_path), "%s.bak", s_path);
                if (!netc_fs_replace(part_path, s_path, s_bak_path)) {
                    ESP_LOGW(TAG, "替换失败 %s → %s (旧版本已保留/回滚)", part_path, s_path);
                    e = ESP_FAIL;
                }
            }
        } else {
            remove(part_path);    /* 半截文件只删临时件, 保留可能存在的旧文件 */
        }
    } else if (s_body && s_cap > 0) {
        s_body[total < s_cap ? total : s_cap - 1] = 0;
    }
    if (e != ESP_OK)
        ESP_LOGW(TAG, "失败: e=%d(%s) http=%d len=%d url=%s",
                 (int)e, esp_err_to_name(e), status, total, s_url);
    else
        ESP_LOGI(TAG, "成功: http=%d len=%d url=%s", status, total, s_url);
    s_len = total; s_http = status; s_err = (int)e;
    __sync_synchronize();                     /* 先写完数据再放状态 */
    s_state = (e == ESP_OK) ? NETC_DONE : NETC_ERR;
    if (chunk) free(chunk);                   /* 释放落盘缓冲 (下不再常驻内部 RAM) */
    vTaskDelete(NULL);
}

bool netc_start(const netc_req_t *req) {
    if (!req || !req->url || !req->url[0]) return false;
    if (s_state == NETC_RUN) return false;                 /* 单请求槽: 忙 */
    bool to_mem  = (req->body != NULL && req->body_cap > 1);
    bool to_file = (req->sink_path != NULL && req->sink_path[0]);
    if (!to_mem && !to_file) return false;

    snprintf(s_url, sizeof(s_url), "%s", req->url);
    snprintf(s_path, sizeof(s_path), "%s", to_file ? req->sink_path : "");
    snprintf(s_ua, sizeof(s_ua), "%s", req->user_agent ? req->user_agent : "");
    s_body     = to_mem ? req->body : NULL;
    s_cap      = to_mem ? req->body_cap : 0;
    s_timeout  = req->timeout_ms > 0 ? req->timeout_ms : 10000;
    s_no_redirect = req->no_redirect;
    s_expect_crc  = req->expect_crc;      /* 0 = 不校验 (向后兼容) */
    s_expect_size = req->expect_size;
    /* ★ 有期望 crc 就必须边收边算: 否则 s_crc 恒为 0, 闸门必然误判失败。
     *   (原来只看 want_crc, 两者由不同调用方字段控制, 容易配错) */
    s_want_crc = req->want_crc || (req->expect_crc != 0);
    s_http = 0; s_len = 0; s_err = 0; s_got = 0;
    s_cancel = false;
    s_crc = 0;
    __sync_synchronize();
    s_state = NETC_RUN;
    /* 任务栈内部 RAM: TLS 握手短暂关中断/禁 cache, PSRAM 栈会炸 */
    if (xTaskCreatePinnedToCore(netc_task, "netc", 8192, NULL, 4, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "任务创建失败 (内部 RAM 不足?)");
        s_state = NETC_ERR; s_err = (int)ESP_ERR_NO_MEM;
        return false;
    }
    return true;
}

netc_state_t netc_state(void) { return (netc_state_t)s_state; }
bool         netc_busy(void)  { return s_state == NETC_RUN; }
int          netc_http(void)  { return s_http; }
int          netc_len(void)   { return s_len; }
int          netc_err(void)   { return s_err; }
int          netc_got(void)   { return s_got; }
uint32_t     netc_crc32(void) { return s_crc; }
const char  *netc_url(void)   { return s_url; }

void netc_reset(void) {
    if (s_state == NETC_DONE || s_state == NETC_ERR) s_state = NETC_IDLE;
}

void netc_cancel(void) {
    if (s_state == NETC_RUN) s_cancel = true;
}
