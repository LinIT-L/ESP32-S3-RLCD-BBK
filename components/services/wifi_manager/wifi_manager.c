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
static bool s_connected = false;
static char s_status[48] = "未连接";
static char s_ip[16] = "";            /* 已连接时纯 IP */
static char s_ssid[33] = "";
static char s_pass[65] = "";

/* V1.0.46: 扫描结果 (放 PSRAM, 不占内部 RAM). 上限调高以配合 wifi_probe 万用表展示更多网络 */
#define MAX_AP 24
EXT_RAM_BSS_ATTR static wifi_ap_record_t s_ap[MAX_AP];
static int s_ap_count = 0;
static bool s_scan_done = false;
static bool s_connected_just_now = false;  /* 刚连接成功事件 (菜单提示用) */
static bool s_suppress_reconnect = false;  /* 扫描期间禁止自动重连 (避免重连干扰扫描) */

/* ---------- NVS 配置持久化 ---------- */
static void save_cfg(void) {
    nvs_handle_t h;
    if (svc_nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    svc_nvs_set_str(h, KEY_SSID, s_ssid);
    svc_nvs_set_str(h, KEY_PASS, s_pass);
    svc_nvs_commit(h);
    svc_nvs_close(h);
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
        /* 自愈: 有保存配置且未成功 → 隔1s重试, 最多5次 (应对ESP32-S3偶发握手超时).
         * 扫描期间抑制 (s_suppress_reconnect): 扫描前主动 disconnect 会触发此重连,
         * 重连 esp_wifi_connect 与扫描冲突又阻塞事件任务, 导致扫描长时间不出结果. */
        static int s_retry = 0;
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
                esp_netif_set_dns_info(nif, ESP_NETIF_DNS_MAIN, &d);
                d.ip.u_addr.ip4.addr = esp_ip4addr_aton("114.114.114.114");
                esp_netif_set_dns_info(nif, ESP_NETIF_DNS_BACKUP, &d);
                ESP_LOGI("WIFI_MANAGER", "DNS 已强制设为 223.5.5.5 / 114.114.114.114");
                /* ★ 2026-10-18 诊断: 解析商店主源域名 + 试连 443 —— 定位"设备连不上而电脑能连"
                 *   是 DNS 还是路由/防火墙问题。 */
                if (!s_diag_done) {
                    s_diag_done = true;
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
        /* V1.7.x: 每天首次联网自动校时 (后台异步, 自动 settimeofday; 不影响使用).
         * NVS 记上次校时日期: 当天重复连接不重复校时, 跨天/掉电丢时间后自动重校. */
        svc_time_ntp_sync();
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
    esp_wifi_set_ps(WIFI_PS_NONE);   /* 保险: 扫描期间禁用省电, 联网速度优先 */
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
    return true;
}

void wifi_manager_scan_stop(void) {
    esp_wifi_scan_stop();
    s_scan_done = true;   /* 停止后视为完成, 避免轮询卡"扫描中" */
    s_suppress_reconnect = false;   /* 结束扫描, 恢复自动重连能力 */
    ESP_LOGI(TAG, "扫描已停止");
}

bool wifi_manager_is_scan_done(void) { return s_scan_done; }
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
    #define TX_PWR_QUARTER_DBM 34    /* 1/4 dBm 单位: 34 = 8.5 dBm */
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
        vTaskDelay(pdMS_TO_TICKS(5000));
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
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_disconnect();  /* 清除旧连接状态 */
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
