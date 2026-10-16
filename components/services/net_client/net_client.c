/* net_client.c — LinTOS 唯一 HTTP(S) 客户端实现 (见 net_client.h 的设计说明) */
#include "net_client.h"
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

/* ★ 搬运缓冲必须**内部 RAM**: 写 SPIFFS 时 flash 驱动会关 Flash+PSRAM cache,
 *   那个窗口里读 PSRAM 源缓冲会取到垃圾 (商店那套踩过, 见 am_store_net.c 文件头③)。 */
static char s_chunk[8192];

static const char *DEFAULT_UA = "LinTOS/1.0 (ESP32-S3)";

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
    FILE *fp = NULL;
    if (s_path[0]) {
        fp = fopen(s_path, "wb");
        if (!fp) { ESP_LOGW(TAG, "打不开落盘文件 %s", s_path); e = ESP_FAIL; }
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
                    int n = esp_http_client_read(cl, s_chunk, sizeof(s_chunk));
                    if (n <= 0) break;
                    if (fwrite(s_chunk, 1, (size_t)n, fp) != (size_t)n) {
                        ESP_LOGW(TAG, "写文件失败 %s (卡满?)", s_path);
                        e = ESP_FAIL;
                        break;
                    }
                    if (s_want_crc) s_crc = crc32_upd(s_crc, (const uint8_t *)s_chunk, (size_t)n);
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
        }
        esp_http_client_close(cl);
        esp_http_client_cleanup(cl);
    }
    if (fp) {
        fclose(fp);
        if (e != ESP_OK) remove(s_path);      /* 半截文件一定删, 别让上层拿到假数据 */
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
    s_http = 0; s_len = 0; s_err = 0; s_got = 0;
    s_cancel = false;
    s_want_crc = req->want_crc;
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
