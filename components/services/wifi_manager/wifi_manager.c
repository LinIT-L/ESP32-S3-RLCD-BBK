/* V1.0.46: Wi-Fi 连网功能 (STA 模式) — 连接路由器, 配置保存到 NVS */
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "bt_manager.h"   /* bt_manager_is_enabled(): BT 开着时扫描时序不能覆盖 */
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "svc_nvs.h"
#include "svc_time.h"
#include "esp_attr.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

#define TAG "WIFI_MANAGER"
#define NVS_NS   "wifi_cfg"
#define KEY_SSID "ssid"
#define KEY_PASS "pass"
#define KEY_STATE "state"   /* 用户显式开关意图: 1=开 0=关; 缺省=auto(有凭据才开) */

static bool s_inited = false;
static bool s_diag_done = false;   /* 一次性域名/TCP 诊断 */
static int8_t s_stable_power = 0;   /* ★ 连上时记下的发射功率(重连直接复用) */    /* esp_wifi_init 已调用 */
static bool s_enabled = false;   /* esp_wifi_start 已成功 */
static bool s_connecting = false;
static uint32_t s_conn_t0 = 0;   /* 发起连接的时刻(ms), 用于 15s 硬超时 */
static bool s_connected = false;
static uint8_t s_last_reason = 0;   /* 上次断开原因码(诊断/提示用) */
static int s_retry = 0;             /* 连续重连次数(密码错/成功才清零) */
static char s_status[48] = "未连接";
static char s_ip[16] = "";            /* 已连接时纯 IP */
static char s_ssid[33] = "";
static char s_pass[65] = "";

/* V1.0.46: 扫描结果 (放 PSRAM, 不占内部 RAM). 上限调高以配合 wifi_probe 万用表展示更多网络 */
#define MAX_AP 24
EXT_RAM_BSS_ATTR static wifi_ap_record_t s_ap[MAX_AP];
static int s_ap_count = 0;
static bool s_scan_done = false;
static uint32_t s_scan_t0 = 0;              /* 本次扫描发起时刻(ms): 超时兜底用 */
static bool s_scanning = false;             /* 已发出 scan_start 且还没收到 SCAN_DONE */
static bool s_connected_just_now = false;  /* 刚连接成功事件 (菜单提示用) */
static bool s_suppress_reconnect = false;  /* 扫描期间禁止自动重连 (避免重连干扰扫描) */

/* ---------- NVS 配置持久化 ---------- */
/* ================= 连接记录 (最多 5 条, 新的在最前) =================
 * 为什么单独存: "网络配置"页要能看到连过哪些网、并且能逐个忘记。
 * 自动连接仍然只用 s_ssid/s_pass(= 最近一次成功使用的那条), 这里只是历史。 */
#define WIFI_HIST_MAX 5
typedef struct { char ssid[33]; char pass[65]; } wifi_hist_t;
static wifi_hist_t s_hist[WIFI_HIST_MAX];
static int  s_hist_n = 0;
static bool s_hist_loaded = false;
#define KEY_HIST "hist"

static void hist_load(void)
{
    if (s_hist_loaded) return;
    s_hist_loaded = true;
    nvs_handle_t h;
    if (svc_nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_hist);
    if (svc_nvs_get_blob(h, KEY_HIST, s_hist, &len) == ESP_OK) {
        s_hist_n = (int)(len / sizeof(wifi_hist_t));
        if (s_hist_n > WIFI_HIST_MAX) s_hist_n = WIFI_HIST_MAX;
    }
    svc_nvs_close(h);
}
static void hist_save(void)
{
    nvs_handle_t h;
    if (svc_nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    svc_nvs_set_blob(h, KEY_HIST, s_hist, (size_t)s_hist_n * sizeof(wifi_hist_t));
    svc_nvs_commit(h);
    svc_nvs_close(h);
}
static void hist_push(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return;
    hist_load();
    int i, w = 0;
    for (i = 0; i < s_hist_n; i++)          /* 去掉旧的同名记录 */
        if (strcmp(s_hist[i].ssid, ssid) != 0) {
            if (w != i) s_hist[w] = s_hist[i];
            w++;
        }
    s_hist_n = w;
    int n = (s_hist_n < WIFI_HIST_MAX - 1) ? s_hist_n : WIFI_HIST_MAX - 1;
    for (i = n; i > 0; i--) s_hist[i] = s_hist[i - 1];   /* 整体后移, 新的放最前 */
    snprintf(s_hist[0].ssid, sizeof(s_hist[0].ssid), "%s", ssid);
    snprintf(s_hist[0].pass, sizeof(s_hist[0].pass), "%s", pass ? pass : "");
    s_hist_n = n + 1;
    hist_save();
}

static void save_cfg(void) {
    nvs_handle_t h;
    if (svc_nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    svc_nvs_set_str(h, KEY_SSID, s_ssid);
    svc_nvs_set_str(h, KEY_PASS, s_pass);
    svc_nvs_commit(h);
    svc_nvs_close(h);
    hist_push(s_ssid, s_pass);   /* 记进连接记录(去重、最近的排最前) */
    ESP_LOGI(TAG, "Wi-Fi 配置已保存: %s", s_ssid);
}

/* 清空已保存的 SSID/密码 (用于"连接失败→清空重输") */
void wifi_manager_clear_saved(void){
    nvs_handle_t h;
    if (svc_nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        svc_nvs_erase_key(h, KEY_SSID);
        svc_nvs_erase_key(h, KEY_PASS);
        svc_nvs_commit(h);
        svc_nvs_close(h);
    }
    s_ssid[0]=0; s_pass[0]=0; s_connected=false;
    s_status[0]=0; snprintf(s_status,sizeof(s_status),"未配置");
    ESP_LOGI(TAG, "Wi-Fi 已保存配置已清空, 请重新输入");
}

static void load_cfg(void) {
    nvs_handle_t h;
    if (svc_nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_ssid);
    if (svc_nvs_get_str(h, KEY_SSID, s_ssid, &len) != ESP_OK) s_ssid[0] = '\0';
    len = sizeof(s_pass);
    if (svc_nvs_get_str(h, KEY_PASS, s_pass, &len) != ESP_OK) s_pass[0] = '\0';
    svc_nvs_close(h);
    if (s_ssid[0]) {
        ESP_LOGI(TAG, "已加载保存的 Wi-Fi 配置: %s", s_ssid);
    }
}

int wifi_manager_history_count(void)
{
    hist_load();
    /* ★ 2026-10-20 修"连接记录为空": 历史只在 wifi_manager_connect() 里入栈,
     *   而**开机自动连接**走的是另一条路径(直接 set_config), 永远不触发 ⇒ 记录一直空。
     *   这里在首次查询时用当前凭据补一条兜底, 保证列表至少能看到正在用的网络。 */
    if (s_hist_n == 0 && s_ssid[0]) hist_push(s_ssid, s_pass);
    return s_hist_n;
}
bool wifi_manager_history_get(int i, char *ssid, size_t ssz, char *pass, size_t psz)
{
    hist_load();
    if (i < 0 || i >= s_hist_n) return false;
    if (ssid && ssz) snprintf(ssid, ssz, "%s", s_hist[i].ssid);
    if (pass && psz) snprintf(pass, psz, "%s", s_hist[i].pass);   /* NULL/0 = 只要 ssid */
    return true;
}
/* 忘记某个网络: 从历史里删掉; 如果是当前正在用的, 同时断开并清掉自动连接凭据 */
bool wifi_manager_forget(const char *ssid)
{
    if (!ssid || !ssid[0]) return false;
    hist_load();
    int i, w = 0;
    for (i = 0; i < s_hist_n; i++)
        if (strcmp(s_hist[i].ssid, ssid) != 0) {
            if (w != i) s_hist[w] = s_hist[i];
            w++;
        }
    if (w == s_hist_n) return false;        /* 没有这条 */
    s_hist_n = w;
    hist_save();
    if (strcmp(s_ssid, ssid) == 0) {        /* 正在用的那条 */
        s_ssid[0] = 0; s_pass[0] = 0;
        if (s_enabled) esp_wifi_disconnect();
        s_connected = false;
        s_connecting = false;
        s_retry = 0;
        snprintf(s_status, sizeof(s_status), "已忘记该网络");
    }
    ESP_LOGI(TAG, "已忘记网络: %s", ssid);
    return true;
}

void wifi_manager_get_saved(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz) {
    snprintf(ssid, ssid_sz, "%s", s_ssid);
    snprintf(pass, pass_sz, "%s", s_pass);
}

/* ---- 用户显式开关意图 (持久化, 重启后按它决定 WiFi 状态) ----
 * -1 = 从未手动设置 (默认: 有凭据才开机自动开);
 *  0 = 用户手动关了 ⇒ 开机不 init (省 ~69KB 内部 RAM);
 *  1 = 用户手动开了 ⇒ 开机 init+start (有凭据则顺带连接)。 */
int wifi_manager_get_want_on(void) {
    nvs_handle_t h;
    if (svc_nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return -1;
    uint8_t v = 0;
    esp_err_t e = svc_nvs_get_u8(h, KEY_STATE, &v);
    svc_nvs_close(h);
    return (e == ESP_OK) ? (int)v : -1;
}
void wifi_manager_set_want_on(bool on) {
    nvs_handle_t h;
    if (svc_nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    svc_nvs_set_u8(h, KEY_STATE, on ? 1 : 0);
    svc_nvs_commit(h);
    svc_nvs_close(h);
}

/* ---------- 事件处理 ---------- */
/* ★ 2026-10-20: 一次性网络诊断(解析域名 + 试连 443)搬到**独立任务**跑。
 *   原来直接写在 GOT_IP 事件处理里: getaddrinfo 阻塞时会把后续 WiFi 事件
 *   (扫描完成/断开/重连)全堵住 ⇒ 表现为"连上了但界面卡住、状态不刷新"。
 *   网络差的时候这里能阻塞好几秒, 所以必须挪出去。 */
static void wifi_diag_task(void *arg)
{
    (void)arg;
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int g = getaddrinfo("bbk.linit.cn", "443", &hints, &res);
    if (g == 0 && res) {
        char ip[32] = "?";
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        snprintf(ip, sizeof(ip), "%d.%d.%d.%d",
                 (int)((ntohl(sa->sin_addr.s_addr) >> 24) & 0xFF),
                 (int)((ntohl(sa->sin_addr.s_addr) >> 16) & 0xFF),
                 (int)((ntohl(sa->sin_addr.s_addr) >> 8) & 0xFF),
                 (int)(ntohl(sa->sin_addr.s_addr) & 0xFF));
        ESP_LOGI("WIFI_MANAGER", "诊断: bbk.linit.cn 解析到 %s", ip);
        int sk = socket(AF_INET, SOCK_STREAM, 0);
        if (sk >= 0) {
            int rc = connect(sk, res->ai_addr, res->ai_addrlen);
            ESP_LOGI("WIFI_MANAGER", "诊断: TCP 443 %s (rc=%d)", rc == 0 ? "可连 ✓" : "连不上 ✗", rc);
            close(sk);
        }
        freeaddrinfo(res);
    } else {
        ESP_LOGW("WIFI_MANAGER", "诊断: 解析 bbk.linit.cn 失败 g=%d", g);
    }
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* WiFi 启动后, 如果有保存的配置则发起连接 */
        if (s_ssid[0]) {
            ESP_LOGI(TAG, "STA 启动, 连接 %s ...", s_ssid);
            s_connecting = true;
            snprintf(s_status, sizeof(s_status), "连接中...");
            esp_wifi_connect();
        } else {
            snprintf(s_status, sizeof(s_status), "未配置");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        esp_wifi_set_ps(WIFI_PS_NONE);   /* ★ 断开后切回全醒: 扫描/重连才稳定 */
        esp_wifi_set_ps(WIFI_PS_NONE);   /* ★ 断开后切回全醒: 扫描/重连才稳定 */
        /* ★★ 2026-10-20: 断开原因分类 (原来一律"连接失败", 用户分不清是密码错还是信号差)。
         *   15/204 = 4 次握手超时 / 认证失败 ⇒ **密码几乎肯定不对**, 别再重试浪费 10s;
         *   201 = 找不到 AP(信号/信道变了);  2 = 认证过期;  205 = 被路由器拒绝。 */
        uint8_t rs = 0;
        if (data) rs = ((wifi_event_sta_disconnected_t *)data)->reason;
        s_last_reason = rs;
        bool pass_bad = (rs == 15 /*4WAY_HANDSHAKE_TIMEOUT*/ || rs == 204 /*HANDSHAKE_TIMEOUT*/
                      || rs == 202 /*AUTH_FAIL*/ || rs == 2 /*AUTH_EXPIRE*/);
        /* ⚠ 实测: 首连时序问题也会报 202(AUTH_FAIL) 一次就恢复 ⇒ 必须**连续两次**才算密码错 */
        if (pass_bad && s_retry >= 1) {
            s_retry = 5;                       /* 密码错: 停止重试 */
            snprintf(s_status, sizeof(s_status), "密码可能不对(%u)", (unsigned)rs);
            ESP_LOGW(TAG, "断开 reason=%u ⇒ 判定密码错误, 停止重试", (unsigned)rs);
            if (s_connecting) s_connecting = false;
            return;
        }
        if (rs == 201) snprintf(s_status, sizeof(s_status), "没找到该网络");
        else if (rs == 205) snprintf(s_status, sizeof(s_status), "被路由器拒绝");
        /* 自愈: 有保存配置且未成功 → 隔1s重试, 最多5次 (应对ESP32-S3偶发握手超时).
         * 扫描期间抑制 (s_suppress_reconnect): 扫描前主动 disconnect 会触发此重连,
         * 重连 esp_wifi_connect 与扫描冲突又阻塞事件任务, 导致扫描长时间不出结果. */

        if (!s_suppress_reconnect && s_ssid[0] && s_retry < 5) {
            s_retry++;
            /* ★ 2026-10-09: 重连时**逐步放开发射功率**。
             *   首次连接故意用 8.5dBm(34) 压住"一开 WiFi 就把 3V3 拉黑屏"的冲击;
             *   但连不上多半是信噪比不够, 于是逐次放开。
             * ★★ 2026-10-18 优化 (linit "WiFi 卡/连得慢"):
             *   ① **删掉 vTaskDelay(1000)** —— 原来在 WiFi 事件任务里阻塞 1 秒,
             *      把扫描/GOT_IP/断开事件全拖住 ⇒ "打开 WiFi 页卡" ✗
             *      改为**立即 esp_wifi_connect()**, IDF 自己会做退避;
             *   ② 第 1 次重试就直接上 19.5dBm(78) —— 本机实测 RSSI ≈ -87dBm,
             *      还从 8.5dBm 慢慢爬纯属浪费两轮;
             *   ③ 连上后把功率记为"已稳定", 后续断线不再回到 8.5dBm。 */
            int8_t want = (s_stable_power > 0) ? s_stable_power : 78;
            esp_wifi_set_max_tx_power(want);
            s_connecting = true;
            snprintf(s_status, sizeof(s_status), "连接中...");
            ESP_LOGI(TAG, "Wi-Fi 重连 (%d/5, TX=%d*0.25dBm)", s_retry, (int)want);
            esp_wifi_connect();          /* ★ 立即重连, 不再阻塞事件任务 */
        } else {
            s_retry = 0;
            if (s_connecting) {
                s_connecting = false;
                snprintf(s_status, sizeof(s_status), "连接失败");
                ESP_LOGD(TAG, "Wi-Fi 连接失败");
            } else {
                snprintf(s_status, sizeof(s_status), "已断开");
            }
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* ★★ 2026-10-20: STA 就绪瞬间补一次连接。原来 esp_wifi_start() 刚返回就 connect,
         *   那一刻 STA 尚未就绪 ⇒ 首次 auth 必失败(auth -> init 0x600), 要等自愈重连,
         *   白等 2~3 秒。这里在 STA_START 事件里再踢一次, 失败的那轮几乎立刻被覆盖。 */
        if (s_ssid[0] && !s_connected) {
            ESP_LOGI(TAG, "STA 就绪, 立即发起连接 %s", s_ssid);
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        /* ★ 连上后记住当前发射功率, 之后断线重连直接用它(不再从 8.5dBm 爬) */
        {
            int8_t pw = 0;
            if (esp_wifi_get_max_tx_power(&pw) == ESP_OK && pw > s_stable_power) s_stable_power = pw;
        }
        /* ★★★ 2026-10-17 linit: 拿到 IP 后**强制覆盖 DNS** —— 实测 routers 下发的 DNS
         *   在设备侧解析不了(商店下载瞬间 ESP_ERR_HTTP_CONNECT)。用国内公共 DNS:
         *   阿里 223.5.5.5 / 114DNS 114.114.114.114。注意: 仍不能解决"域名被墙", 只解决 DNS。 */
        {
            esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (nif) {
                esp_netif_dns_info_t d = { 0 };
                d.ip.type = ESP_IPADDR_TYPE_V4;
                d.ip.u_addr.ip4.addr = esp_ip4addr_aton("223.5.5.5");
                {   /* "能用就用": 路由器下发的 DNS 保持主用, 我们的只做备用 */
                    esp_netif_dns_info_t cur;
                    if (esp_netif_get_dns_info(nif, ESP_NETIF_DNS_MAIN, &cur) != ESP_OK ||
                        cur.ip.u_addr.ip4.addr == 0)
                        esp_netif_set_dns_info(nif, ESP_NETIF_DNS_MAIN, &d);
                }
                d.ip.u_addr.ip4.addr = esp_ip4addr_aton("114.114.114.114");
                esp_netif_set_dns_info(nif, ESP_NETIF_DNS_BACKUP, &d);
                ESP_LOGI("WIFI_MANAGER", "DNS 已强制设为 223.5.5.5 / 114.114.114.114");
                /* ★ 2026-10-20 诊断改后台任务(见 wifi_diag_task): 事件处理里不允许阻塞 */
                if (!s_diag_done) {
                    s_diag_done = true;
                    xTaskCreate(wifi_diag_task, "wifi_diag", 4096, NULL, 1, NULL);
                }
            }
        }
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_connected = true;
        s_connecting = false;
        s_connected_just_now = true;
        char ipbuf[16];
        esp_ip4addr_ntoa(&e->ip_info.ip, ipbuf, sizeof(ipbuf));
        snprintf(s_ip, sizeof(s_ip), "%s", ipbuf);
        snprintf(s_status, sizeof(s_status), "已连接 %s", ipbuf);
        ESP_LOGI(TAG, "Wi-Fi 已连接: %s", s_status);
        /* ★ 2026-10-26 linit: **每次联网都校一次时间** (原来"当天只校一次")。
         *   每次拿到 IP 都会让 SNTP 重发一次查询; 后台异步, 成功后自动 settimeofday,
         *   不阻塞连接流程。关屏保时 WiFi 会关掉, 解锁重连时这里正好又补校一次。 */
        svc_time_ntp_sync();

        /* ★★ 2026-10-26 linit「省电」: 连上并拿到 IP 后进入 DTIM 省电。
         *   原来全程 WIFI_PS_NONE(射频永远全醒 ~70~90mA), 那是为了扫描/连接稳定;
         *   但**连上之后不扫描了** ⇒ 可以按 AP 的 DTIM 间隔睡射频(降到 ~20~30mA)。
         *   断开/扫描时会切回 NONE(见 DISCONNECTED 分支与 scan 入口)。 */
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        ESP_LOGI(TAG, "WiFi 省电: 已切到 MIN_MODEM (按 DTIM 间隔睡射频)");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        /* 扫描完成: 取回 AP 列表 */
        s_scan_done = true;
        s_suppress_reconnect = false;   /* 扫描结束, 恢复自动重连能力 */
        uint16_t num = MAX_AP;
        if (esp_wifi_scan_get_ap_records(&num, s_ap) == ESP_OK) {
            s_ap_count = num;
        } else {
            s_ap_count = 0;
        }
        /* 按信号强度排序 (简单插入排序) */
        for (int i = 1; i < s_ap_count; i++) {
            wifi_ap_record_t tmp = s_ap[i];
            int j = i - 1;
            while (j >= 0 && s_ap[j].rssi < tmp.rssi) {
                s_ap[j + 1] = s_ap[j];
                j--;
            }
            s_ap[j + 1] = tmp;
        }
        ESP_LOGI(TAG, "扫描完成: %d 个网络", s_ap_count);
    }
}

/* V1.0.46: 扫描附近的 Wi-Fi 网络
 * V1.7.x: 统一扫描方式 (以网络分析为模板) — 刚开启 WiFi 后等射频稳定再扫,
 * 启动失败 (状态未就绪/忙) 自动延迟重试一次, 保证设置页"刚开 WiFi 立即扫描"也稳定快速. */
bool wifi_manager_scan_start(void) {
    /* ★★★ 2026-10-25 崩溃修复 (linit: "连网时一直正在扫描, 然后就重启了"):
     *   根因 = **内部 RAM 不足**, 不是扫描逻辑本身。
     *   实测(remcon heap): 蓝牙开着时 internal free=66KB、largest 只有 ~20KB 级;
     *   蓝牙关掉立刻回到 free=115KB。esp_wifi_scan_start 要新分配射频/coex 缓冲,
     *   malloc 失败 → ESP_ERROR_CHECK abort → 重启, 表现就是"卡在正在扫描然后黑屏重启"。
     *   策略: ① 先量最大连续块; ② 不够且蓝牙开着 ⇒ 临时关 BT 腾出 ~49KB;
     *         ③ 仍不够 ⇒ 直接返回 false 让 UI 提示, **绝不进 esp_wifi_scan_start 送死**。 */
    {
        /* 只诊断，不干预：碎片化高时打一条警告，方便现场定位；扫描照常尝试。
         * （2026-10-25: 曾在这里自动关蓝牙，实测 linit 的崩溃与蓝牙无关，已撤掉。）*/
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (largest < 24 * 1024)
            ESP_LOGW(TAG, "内部最大连续块仅 %u B (碎片偏高)", (unsigned)largest);
    }
    if (!s_inited) wifi_manager_init();
    if (!s_inited) return false;
    if (!s_enabled) {
        if (!wifi_manager_enable()) return false;
        vTaskDelay(pdMS_TO_TICKS(150));   /* 等射频稳定: 刚开启立即扫描会 NOT_STARTED/STATE */
    }
    /* 根因修复: 若 STA 正在"连接/连电源重试"保存的网, esp_wifi 不允许 scan
     * (返回 ESP_ERR_WIFI_STATE), 导致扫描卡死"正在扫描...". 扫描期间先抑制自动重连
     * (否则主动 disconnect 会触发重连 esp_wifi_connect, 与扫描冲突又阻塞事件任务,
     * 扫描长时间不出结果), 再强制断开一次让 WiFi 进入可扫描状态. */
    s_suppress_reconnect = true;
    if (s_connecting || !s_connected) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_scan_done = false;
    s_ap_count = 0;
    esp_wifi_set_ps(WIFI_PS_NONE);
    /* ★ 2026-10-20: 强制 20MHz。本机 AP 报的是 40U, 窄带在穿墙/干扰下重传少、握手更快;
     *   我们只跑网页小包, 带宽上限降低无感。 */
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);   /* 保险: 扫描期间禁用省电, 联网速度优先 */
    wifi_scan_config_t cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    /* ★★ 蓝牙开着时**绝不能覆盖 scan_time** (2026-10-02 实机定案):
     *   IDF 会直接报错 —— 日志原文
     *   "Error! Should use default active scan time parameter for WiFi scan when
     *    Bluetooth is enabled!!!!!!"
     *   原因: BT 与 WiFi 共用 2.4G 射频, 扫描驻留必须按 IDF 的时隙交错参数走;
     *   自己压到 40-200ms 会让扫描漏网 + 关联失败。
     *   实测症状 (linit 报): 手机热点扫不到、或扫到却连不上
     *   (`wifi:Coexist: Wi-Fi connect fail, apply reconnect coex policy` → 1s 重试 1/5..5/5)。
     *   所以: 蓝牙关着才用"快速扫描"提速; 蓝牙开着交回 IDF 默认。 */
    if (bt_manager_is_enabled()) {
        ESP_LOGD(TAG, "蓝牙开启中 → 扫描用 IDF 默认时序 (不覆盖 scan_time, 保 coex 交错)");
    } else {
        cfg.scan_time.active.min = 40;    /* 蓝牙关: 快速主动扫描, 全 13 信道 1~2s */
        cfg.scan_time.active.max = 200;
    }
    esp_err_t ret = esp_wifi_scan_start(&cfg, false);
    if (ret != ESP_OK) {
        /* 状态未就绪/忙: 延迟重试一次 (刚开 WiFi 常见), 仍失败才返回 false */
        ESP_LOGD(TAG, "扫描启动失败: %s, 200ms 后重试", esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(200));
        ret = esp_wifi_scan_start(&cfg, false);
    }
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "扫描启动失败: %s (s_enabled=%d)", esp_err_to_name(ret), s_enabled);
        s_suppress_reconnect = false;
        return false;
    }
    ESP_LOGI(TAG, "扫描已启动 (%s)", bt_manager_is_enabled() ? "IDF 默认时序/BT 开" : "active 40-200ms/BT 关");
    s_scanning = true;                                        /* 等 SCAN_DONE 或 12s 超时 */
    s_scan_t0  = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    return true;
}

void wifi_manager_scan_stop(void) {
    esp_wifi_scan_stop();
    /* ★★★ 2026-10-25 修 (官方文档明确要求):
     *   "the found APs are stored in WiFi driver dynamic allocated memory, and then can be
     *    freed in esp_wifi_scan_get_ap_records / esp_wifi_scan_get_ap_record / esp_wifi_clear_ap_list"
     *   扫描中途 stop 时 SCAN_DONE 不会来 ⇒ 没人取结果 ⇒ **驱动那块内存永远不还**。
     *   反复"扫一半就退"会把内部 RAM 一点点吃光。这里必须显式清。 */
    if (s_scanning) esp_wifi_clear_ap_list();
    s_scanning = false;
    s_scan_t0 = 0;
    s_scan_done = true;   /* 停止后视为完成, 避免轮询卡"扫描中" */
    s_suppress_reconnect = false;   /* 结束扫描, 恢复自动重连能力 */
    ESP_LOGI(TAG, "扫描已停止");
}

bool wifi_manager_is_scan_done(void) {
    /* ★★★ 2026-10-25 "一直正在扫描"兜底 (linit 报: 连网卡在正在扫描然后重启):
     *   WiFi 驱动在"扫描与连接流程相撞"等状态下**不派发 WIFI_EVENT_SCAN_DONE**,
     *   界面就会永远停在"正在扫描..."。这里超过 12s 强制收尾, 并**清掉驱动里那份
     *   扫描结果内存**(官方文档: 结果存在驱动动态内存里, 必须由 get_ap_records /
     *   get_ap_record / clear_ap_list 之一释放; 不取就永久占着)。
     *   不这么收尾的话: 用户反复进退扫描页 ⇒ 每次漏一块驱动内存 ⇒ 越用越少 ⇒ 崩。 */
    if (!s_scan_done && s_scanning && s_scan_t0 &&
        (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - s_scan_t0 > 12000) {
        ESP_LOGW(TAG, "扫描 12s 未收到 SCAN_DONE → 强制收尾并清驱动结果内存");
        esp_wifi_scan_stop();
        esp_wifi_clear_ap_list();
        s_scanning = false;
        s_scan_t0  = 0;
        s_ap_count = 0;
        s_scan_done = true;
        s_suppress_reconnect = false;
    }
    return s_scan_done;
}
int  wifi_manager_get_scan_count(void) { return s_ap_count; }

bool wifi_manager_get_scan_ssid(int idx, char *out, size_t sz) {
    if (idx < 0 || idx >= s_ap_count || !out || sz == 0) return false;
    snprintf(out, sz, "%s", s_ap[idx].ssid);
    return true;
}

const wifi_ap_record_t *wifi_manager_get_scan_records(void) {
    return s_ap_count > 0 ? s_ap : NULL;
}

/* 网络自愈监督任务 (定义在文件后部, 此处前置声明) */
static void wifi_supervisor_task(void *arg);

void wifi_manager_init(void) {
    if (s_inited) return;
    load_cfg();

    ESP_LOGI(TAG, "初始化 WiFi 子系统 (STA)...");
    esp_netif_init();
    /* V1.0.67: 防重复创建 (网页手柄 AP 可能已先初始化过 esp_wifi/esp_netif) */
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    /* ★★ 2026-10-20: WiFi 驱动**不再往 flash 写配置**。
     *   凭据本来就是我们自己存在 svc_cfg 里 (开机 get_saved 读回来再 set_config),
     *   驱动再存一份纯属浪费: 每次连网多一次 NVS 擦写, 还和收藏夹/PIN/板型抢同一个分区。
     *   nvs_enable=0 + WIFI_STORAGE_RAM ⇒ 配置只留内存, 断电即弃 (反正下次开机重设)。 */
    /* 结论(2026-10-20 实测): `nvs_enable=0` 会让 esp_wifi_connect() 返回 OK 但驱动不启动
     * 状态机(连 init->auth 都没有) ⇒ 驱动依赖 NVS 里的 STA 配置。**必须保持默认 1**。
     * 省 flash 磨损改用另一招: 见 wifi_manager_connect() 的"配置没变就不 set_config"。 */
    cfg.nvs_enable = 1;
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(ret));
        return;
    }
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    s_inited = true;
    /* 网络自愈监督任务 (只建一次): 持久重连, 见 wifi_supervisor_task */
    static bool s_sup_started = false;
    if (!s_sup_started) {
        s_sup_started = true;
        xTaskCreate(wifi_supervisor_task, "wifi_sup", 3072, NULL, 3, NULL);
    }
    ESP_LOGI(TAG, "WiFi 子系统初始化完成 (STA)");
}

/* ★ 2026-10-09: 用**已保存的凭据**给 STA 配置并发起连接。
 * 前提: esp_wifi 已 start。返回 true = connect 已发起 (连接结果是异步的, 看事件)。 */
static bool sta_connect_saved(void)
{
    if (s_ssid[0] == '\0') return false;
    wifi_config_t cfg = { 0 };
    memcpy(cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid) - 1);
    cfg.sta.ssid[sizeof(cfg.sta.ssid) - 1] = '\0';
    memcpy(cfg.sta.password, s_pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.password[sizeof(cfg.sta.password) - 1] = '\0';
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) return false;
    esp_wifi_disconnect();          /* 清掉旧状态, 保证这次 connect 真的重连 */
    s_connecting = true;
    s_conn_t0 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    snprintf(s_status, sizeof(s_status), "连接中...");
    if (esp_wifi_connect() != ESP_OK) { s_connecting = false; return false; }
    return true;
}

bool wifi_manager_enable(void) {
    if (s_enabled) return true;
    if (!s_inited) wifi_manager_init();
    if (!s_inited) return false;

    /* V1.0.67: 网页手柄可能切过 AP 模式, 这里强制回 STA */
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGD(TAG, "WiFi 启动失败");
        return false;
    }
    /* 关闭 WiFi 省电: 省电会间歇休眠射频, 导致扫描时快时慢/卡死在"扫描中".
     * 始终唤醒射频, 扫描才能稳定快速 (代价是待机功耗略增). */
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* 2026-09-29: **限制发射功率**, 降低"开启 WiFi 瞬间把 3V3 拉低"的冲击.
     * 背景: 用户实测开启 WiFi 会黑屏关机 (疑供电跌落; 掉电复位族).
     * 默认 20dBm 时 TX PA 的瞬态电流最大, 降到 8.5dBm(34 = 34/4 dBm) 可把峰值砍掉约一半,
     * 而扫描/近距离连接基本不受影响。要远距离时把 TX_PWR_QUARTER_DBM 调回 78(19.5dBm)。
     * 必须在 esp_wifi_start() 之后调用 (IDF 要求)。 */
    /* ★★ 2026-10-20 实测: 首连压到 8.5dBm(34) 会直接 **auth 失败**, 然后重试 19.5dBm 才成,
     *   白多花 2~3 秒 —— 这正是"连得慢"的主因。射频启动已延后到桌面之后, 电流冲击不再是瓶颈,
     *   所以首连直接用 19.5dBm。真出现掉电(黑屏)再回退这个值。 */
    #define TX_PWR_QUARTER_DBM 78    /* 1/4 dBm 单位: 78 = 19.5 dBm */
    esp_err_t tp_ret = esp_wifi_set_max_tx_power(TX_PWR_QUARTER_DBM);
    int8_t tp_now = 0;
    if (tp_ret == ESP_OK && esp_wifi_get_max_tx_power(&tp_now) == ESP_OK) {
        ESP_LOGD(TAG, "TX 功率限制: %d (1/4dBm) = %.1f dBm — 降低开 WiFi 的电流冲击",
                 (int)tp_now, tp_now / 4.0);
    } else {
        ESP_LOGD(TAG, "TX 功率限制失败: %s (继续, 但功耗冲击未缓解)",
                 esp_err_to_name(tp_ret));
    }

    s_enabled = true;
    ESP_LOGI(TAG, "WiFi 射频已启动 (省电已关闭)");

    /* ★ 2026-10-09 linit 需求: **打开 WiFi 就自动连上"上次保存的密码"**。
     *   原来 enable() 只"开射频"不连网 —— 上层(应用商店门禁 / 设置页)各自再连一次,
     *   既重复、又要和扫描抢状态, 还出现过"门禁看到 conn=0 就反复弹确认框"。
     *   现在统一收口在 enable(): 只要有存的凭据且当前没连, 立刻发起连接。 */
    if (!s_connected && !s_connecting && s_ssid[0]) {
        if (sta_connect_saved()) ESP_LOGI(TAG, "已自动连接保存的 WiFi: %s", s_ssid);
        else                     ESP_LOGD(TAG, "自动连接保存的 WiFi 失败: %s", s_ssid);
    }
    return true;
}

bool wifi_manager_disable(void) {
    if (!s_enabled) return true;
    esp_wifi_disconnect();
    esp_wifi_stop();
    /* V1.3.x: 完整释放 esp_wifi_init 分配的内部内存 (仅 stop 不释放, 开/关内存不回落).
     * 网页手柄 (web_gamepad) 直接初始化 esp_wifi AP 模式共享本子系统 —
     * 检测到 AP/APSTA 在用时不 deinit, 只停 STA 射频, 避免破坏手柄 AP. */
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK &&
        mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler);
        esp_wifi_deinit();
        s_inited = false;
        ESP_LOGI(TAG, "WiFi 已 deinit (内部内存已释放)");
    }
    s_enabled = false;
    s_connected = false;
    s_connecting = false;
    snprintf(s_status, sizeof(s_status), "未连接");
    ESP_LOGI(TAG, "WiFi 已关闭");
    return true;
}

bool wifi_manager_is_enabled(void) { return s_enabled; }
bool wifi_manager_is_connected(void) { return s_connected; }
bool wifi_manager_is_connecting(void) { return s_connecting; }

/* ★ 2026-10-15 linit "联网归网络模块管": 是否配过网络 (有保存凭据).
 * 门禁/商店据此判断"能不能等" —— 有配置就等模块自己连, 没配置才引导去设置。 */
bool wifi_manager_has_saved(void) { return s_ssid[0] != '\0'; }

/* ★ 2026-10-15: 网络模块自愈监督任务 —— **持久重连** (原来事件里只重试 5 次就永久放弃,
 * 之后设备一直离线到用户手动重连 = "打开一直是离线" 的根因之一)。
 * 每 5s 检查: 已开射频 + 有配置 + 未连且没在连 + 不在扫描 → 主动重连。 */
static void wifi_supervisor_task(void *arg) {
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));   /* 自愈 5s→2s: 断线后最迟 2s 内重试, 不再"卡在未连接" */
        /* ★ 2026-10-20: "一直正在连接" 的硬超时 —— 15s 还没有结果就放弃并提示,
         *   否则 AP 不回应时会永远停在连接中态。 */
        if (s_connecting && s_conn_t0 &&
            (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - s_conn_t0 > 15000) {
            s_connecting = false;
            s_conn_t0 = 0;
            snprintf(s_status, sizeof(s_status), "连接超时");
            esp_wifi_disconnect();
            ESP_LOGW(TAG, "连接超时 15s ⇒ 放弃本次连接");
        }
        if (s_enabled && s_ssid[0] && !s_connected && !s_connecting && !s_suppress_reconnect) {
            ESP_LOGI(TAG, "网络自愈: 主动重连 %s", s_ssid);
            sta_connect_saved();
        }
    }
}

/* ★ 2026-10-15: 开机自动联网 —— 有保存凭据就把射频拉起来并连上,
 * 上层(应用/商店)只管查 is_connected, **不再各自联网**。
 * 没配过网络则完全不动 (不开射频, 保持省电)。 */
void wifi_manager_autostart(void) {
    /* ★★★ 2026-10-18 linit 内存优化 + 状态持久化:
     *   ① 先读凭据(load_cfg 只读 NVS, 不 init/不分配) —— 没了"先 init 再发现没网"的白花 ~69KB;
     *   ② 尊重"用户手动开关"意图: 手动关过 ⇒ 开机不 init; 手动开过 ⇒ 开机就开;
     *      从未手动设置(默认) ⇒ 有凭据才自动开 (即"没手动关过又配过网 = 开机仍开")。 */
    load_cfg();
    int want = wifi_manager_get_want_on();
    if (want == 0) {
        ESP_LOGI(TAG, "开机: 用户已关闭 WiFi, 不启用 (省 ~69KB 内部 RAM)");
        return;
    }
    if (want < 0 && s_ssid[0] == '\0') {
        ESP_LOGI(TAG, "开机: 未配置网络, 不初始化 WiFi (省 ~69KB 内部 RAM)");
        return;
    }
    if (!s_inited) wifi_manager_init();
    if (!s_inited) return;
    if (!wifi_manager_enable()) { ESP_LOGW(TAG, "开机自动联网: 射频启动失败"); return; }
    if (s_ssid[0]) ESP_LOGI(TAG, "开机自动联网: 已发起连接 %s", s_ssid);
    else          ESP_LOGI(TAG, "开机: 已开启 WiFi (无保存凭据, 仅射频就绪)");
}


bool wifi_manager_consume_connected_event(void) {
    bool v = s_connected_just_now;
    s_connected_just_now = false;
    return v;
}
const char *wifi_manager_get_status(void) { return s_status; }

/* V1.6.x: 返回已连接 WiFi 的纯 IP 串 (未连接则为空串) */
bool wifi_manager_get_ip(char *out, size_t sz) {
    if (!out || sz == 0) return false;
    out[0] = '\0';
    if (!s_connected || !s_ip[0]) return false;
    snprintf(out, sz, "%s", s_ip);
    return true;
}

bool wifi_manager_connect(const char *ssid, const char *password) {
    if (!ssid || ssid[0] == '\0') return false;

    /* ★★ 2026-10-20 linit: "重复点别起冲突" —— 这里的幂等/去重很关键:
     *   原来的实现每次调用都会先 esp_wifi_disconnect() 再连, 于是"连的过程里再点一次"
     *   会把正在进行的连接打断、从零重来 ⇒ 越点越慢、还可能失败。
     *   ① 已经连着同一个 SSID   → 直接成功, 一下都不动 (页面只需提示"已连接")
     *   ② 正在连同一个 SSID     → 直接返回 false (不打断), 页面提示"正在连接"
     *   ③ 才是真正的新连接/切换 */
    if (s_ssid[0] && strcmp(s_ssid, ssid) == 0) {
        if (s_connected)  return true;
        if (s_connecting) return false;
    }

    bool switching = (s_enabled && (s_connected || s_connecting));   /* 换目标才需要断一下 */

    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", password ? password : "");
    save_cfg();

    if (!s_inited) wifi_manager_init();
    if (!s_inited) return false;

    if (!s_enabled) {
        if (esp_wifi_start() != ESP_OK) return false;
        esp_wifi_set_ps(WIFI_PS_NONE);   /* 关省电, 保证扫描/连接稳定 */
        s_enabled = true;
    }

    wifi_config_t cfg = { 0 };
    /* V1.3.x: 原 strncpy(sizeof-1) 对 32 字符 SSID/64 字符密码会截断且不补 NUL,
     * 在 -O2 (stringop-truncation) 下触发 Werror, 且驱动按 C 串读可能越界.
     * 改 memcpy 复制满尺寸-1 + 显式置 NUL, 保证驱动收到合法 C 串. */
    memcpy(cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid) - 1);
    cfg.sta.ssid[sizeof(cfg.sta.ssid) - 1] = '\0';
    memcpy(cfg.sta.password, s_pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.password[sizeof(cfg.sta.password) - 1] = '\0';
    /* ★★ 2026-10-20: 官方最快的连法 —— 快速扫描: 只找目标 SSID 所在信道就停,
     *   不再把 1~13 信道全扫一遍(全扫 1~2s, 还容易先连到同名的弱信号 AP)。 */
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.scan_method = WIFI_FAST_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.pmf_cfg.capable  = true;
    cfg.sta.pmf_cfg.required = false;

    /* ★★ 2026-10-20 真优化: **配置没变就不写**。
     *   原来每次 connect 都 set_config(写 NVS)+save_cfg(再写自己的), 重复连同一个网
     *   等于无意义地擦两次 flash。这里先读驱动里现有配置比对: 一致就跳过,
     *   于是"开机自动连/断线重连/重复点"都不再产生任何 flash 写。 */
    wifi_config_t cur;
    bool same_cfg = (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK &&
                     strcmp((const char *)cur.sta.ssid, (const char *)cfg.sta.ssid) == 0 &&
                     strcmp((const char *)cur.sta.password, (const char *)cfg.sta.password) == 0);
    if (same_cfg) {
        ESP_LOGI(TAG, "配置未变, 跳过 set_config (不写 flash)");
    } else {
        esp_err_t sc = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (sc != ESP_OK) ESP_LOGW(TAG, "set_config 失败: %s", esp_err_to_name(sc));
    }
    if (switching) esp_wifi_disconnect();   /* 只有换目标才断, 避免打断同一目标的进行中连接 */
    s_connecting = true;
    snprintf(s_status, sizeof(s_status), "连接中...");
    if (esp_wifi_connect() != ESP_OK) {
        s_connecting = false;
        snprintf(s_status, sizeof(s_status), "连接失败");
        return false;
    }
    ESP_LOGI(TAG, "正在连接 %s ...", s_ssid);
    return true;
}
