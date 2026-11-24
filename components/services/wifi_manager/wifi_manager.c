/* V1.0.46: Wi-Fi 连网功能 (STA 模式) — 连接路由器, 配置保存到 NVS */
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "bt_manager.h"   /* bt_manager_is_enabled(): BT 开着时扫描时序不能覆盖 */
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"   /* IPSTR / IP2STR（AP 模式打印 IP） */
#include "dns_server.h"          /* AP captive 门户的 DNS 劫持（与 AP 同生命周期） */
#include "lwip/ip4_addr.h"       /* IP4_ADDR */
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"   /* 射频操作串行化: 递归互斥量 */

#include "svc_nvs.h"
#include "svc_time.h"
#include "esp_attr.h"
#include "esp_timer.h"     /* 下载结束后延迟恢复 DTIM 省电用 */
#include <string.h>
#include <stdio.h>
#include <time.h>

#define TAG "WIFI_MANAGER"
#define NVS_NS   "wifi_cfg"
#define KEY_SSID "ssid"
#define KEY_PASS "pass"
#define KEY_STATE "state"   /* 用户显式开关意图: 1=开 0=关; 缺省=auto(有凭据才开) */
/* ★★★ 2026-11-22 linit「密码输错也保存了, 之后改不回来」根治 ★★★
 *   `ver` = 这份凭据**真的连上过**的标记 (只有 GOT_IP 才写 1)。
 *   老固件(以及所有早期版本)是"点了连接就存盘", 于是 NVS 里可能躺着一份从没连通过的
 *   错误密码; 页面又按"有保存密码"直接拿它去连 ⇒ 点一次失败一次, 永远回不到输入框。
 *   现在: get_saved() 对**未验证**的凭据不返回密码 ⇒ 页面必然弹键盘让用户重输。
 *   没有这个键 = 老存档 = 视为未验证 ⇒ 升级后第一次点该网络就能重新输密码。 */
#define KEY_VERIFIED "ver"
/* `wf_clean` = 已做过"清除驱动层 nvs.net80211 旧配置"的一次性迁移 */
#define KEY_WF_CLEAN "wf_clean"

/* ★★★ 2026-11-07 linit「统一极致省电」★★★
 * 旧的 s_ps_hold / s_ps_diag_off / s_ps_timer 已**删除**。
 *   原来下载、局域网页服务、串口诊断三条路各自把射频切成 WIFI_PS_NONE(70~90mA),
 *   结束后再靠 8 秒定时器恢复 —— 任何一条没走到恢复, 就永久停在最费电档
 *   (实测已确认: 扫描 / 频谱仪 / 断连三处都会把 NONE 留在设备里)。
 * 现在全项目**只有一个**正常运行省电档, 业务层不再拥有切换权:
 *   · 普通业务(下载/网页/检测/扫描) → 一律 wifi_manager_apply_ps()
 *   · 唯一例外: 频谱仪 promiscuous 连续抓包 → wifi_manager_rf_measure_begin/end()
 */
static int  s_rf_measure;                 /* >0 = 频谱仪正在测量(唯一允许 NONE 的窗口) */

/* 统一省电档的**唯一**施加点 (实现在文件后段, 事件回调里要用, 先声明) */
void wifi_manager_apply_ps(void);
static bool s_inited = false;
/* ★ 2026-11-01: AP 热点状态（由本模块统一持有；见 wifi_manager_ap_start） */
static bool                 s_ap_on  = false;
static dns_server_handle_t  s_ap_dns = NULL;
static bool s_diag_done = false;   /* 一次性域名/TCP 诊断 */
static int8_t s_stable_power = 0;   /* ★ 连上时记下的发射功率(重连直接复用) */    /* esp_wifi_init 已调用 */
static bool s_enabled = false;   /* esp_wifi_start 已成功 */

#define WIFI_PS_UNIFIED   WIFI_PS_MIN_MODEM

/* ==================== 射频操作的串行化 (2026-11-22) ====================
 * 问题: esp_wifi_init/deinit 会被**至少两个任务**调用 ——
 *   · 快捷栏 Wi-Fi 开关（qb_act 任务, core0, 内部 RAM 栈）
 *   · 设置页 / 应用中心门禁（UI 主循环, core1）
 * 本文件原来一把锁都没有: 一边在 esp_wifi_deinit() 释放驱动内部内存,
 * 另一边同时在 esp_wifi_start()/set_config() 里用它 ⇒ 悬空指针, 随机崩或被看门狗复位。
 *
 * 用**递归锁**: disable() 内部会调 force_off(), ap_start() 内部会调 enable(),
 * 非递归锁会在这种嵌套处自死锁。
 * 事件回调 (wifi_event_handler) 只做 connect/apply_ps, 不回本组入口, 不会重入。 */
static SemaphoreHandle_t s_op_lock = NULL;

static void wm_lock(void)
{
    if (!s_op_lock) s_op_lock = xSemaphoreCreateRecursiveMutex();   /* 开机单线程阶段首次进入 */
    if (s_op_lock) xSemaphoreTakeRecursive(s_op_lock, portMAX_DELAY);
}
static void wm_unlock(void)
{
    if (s_op_lock) xSemaphoreGiveRecursive(s_op_lock);
}

/* ★ 2026-11-08: 运行时可切的省电档。默认仍是统一档(MIN_MODEM) —— 实测 MAX_MODEM
 * 在局域网 ping 上丢包 37.5%, 不能当默认。这里只给"省电优先"场景一个开关,
 * 由 remcon `wifi psmode max|min` 现场切, 不写 NVS、重启回默认。 */
static wifi_ps_type_t s_ps_mode = WIFI_PS_UNIFIED;

/* ★ 2026-11-08 linit「按官方优化 Wi-Fi 省电」: 空闲自动降档
 *   官方模型是"用的时候快、不用的时候深睡", 而不是全局钉死一档:
 *     · 有网络活动 → MIN_MODEM (DTIM 醒来, 延迟正常)
 *     · 连续 20s 无活动 → MAX_MODEM (只在 listen_interval 个 beacon 后醒)
 *   这样待机时射频占空比大幅下降(实测 Wi-Fi 是唯一的 4°C 热源),
 *   而下发请求的瞬间自动回 MIN_MODEM, 不会像全局 MAX_MODEM 那样把下载拖死
 *   (项目 2026-10-31 记录: 全局深省电下 41.7MB 资源包传到 7.5MB 就断流)。
 *   用 `wifi psauto 0` 可退回"恒 MIN_MODEM"的旧行为。 */
#define PS_IDLE_MS        20000u   /* 无网络活动多久后降档 (ms) */
static volatile uint32_t s_last_act_ms = 0;
static bool s_ps_auto = true;

static bool s_connecting = false;
static uint32_t s_conn_t0 = 0;   /* 发起连接的时刻(ms), 用于 15s 硬超时 */
static bool s_connected = false;
static uint8_t s_last_reason = 0;   /* 上次断开原因码(诊断/提示用) */
static int s_retry = 0;             /* 连续重连次数(密码错/成功才清零) */
static char s_status[48] = "未连接";
static char s_ip[16] = "";            /* 已连接时纯 IP */
static char s_ssid[33] = "";
static char s_pass[65] = "";
/* ★ 2026-11-22 linit「密码输错不要保存」:
 *   这两份是**最后一次真正落过盘**的凭据快照 (来自 load_cfg 或 save_cfg).
 *   s_ssid/s_pass 可能已被新的连接尝试改掉, 但没连上之前一律不写 NVS ——
 *   只有 GOT_IP 才落盘. 于是"输错密码点连接"不会污染自动连接凭据/连接记录. */
static char s_saved_ssid[33] = "";
static char s_saved_pass[65] = "";
static bool s_cfg_pending = false;   /* s_ssid/s_pass 与已落盘内容不同 ⇒ 成功后要补存 */
/* ★ 2026-11-22: 当前凭据是否"真的连上过". false ⇒ get_saved() 不吐密码,
 *   强迫页面弹键盘重输, 避免拿一份没验证过的错误密码无限撞墙. */
static bool s_cfg_verified = false;

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
/* ★ 2026-11-02: 5 条历史（490 B）只在 WiFi 页面里用, 放 PSRAM。 */
static EXT_RAM_BSS_ATTR wifi_hist_t s_hist[WIFI_HIST_MAX];
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

/* 只把"已验证"标记写盘 (凭据本身没变时用, 例如开机自动连上了老凭据) */
static void mark_verified(void) {
    if (s_cfg_verified || !s_ssid[0]) return;
    nvs_handle_t h;
    if (svc_nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    svc_nvs_set_u8(h, KEY_VERIFIED, 1);
    svc_nvs_commit(h);
    svc_nvs_close(h);
    s_cfg_verified = true;
    ESP_LOGI(TAG, "凭据已验证(本次连上), 之后可直接复用: %s", s_ssid);
}

static void save_cfg(void) {
    nvs_handle_t h;
    if (svc_nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t e = svc_nvs_set_str(h, KEY_SSID, s_ssid);
    if (e == ESP_OK) e = svc_nvs_set_str(h, KEY_PASS, s_pass);
    /* ★ 能走到 save_cfg 只可能是 GOT_IP 之后 ⇒ 顺手打上"已验证" */
    if (e == ESP_OK) e = svc_nvs_set_u8(h, KEY_VERIFIED, 1);
    if (e == ESP_OK) e = svc_nvs_commit(h);
    svc_nvs_close(h);
    if (e != ESP_OK) {
        /* ★ 2026-11-22: 原来 commit 返回值被丢掉, 照样打"已保存"并写历史 ——
         *   而下次开机的自动连接完全依赖这份凭据, 写失败时用户永远连不上却查不出原因。 */
        ESP_LOGE(TAG, "Wi-Fi 配置保存失败(%s): %s ⇒ 本次不写连接记录", esp_err_to_name(e), s_ssid);
        return;
    }
    hist_push(s_ssid, s_pass);   /* 记进连接记录(去重、最近的排最前) */
    /* ★ 2026-11-22: 落盘成功才更新快照 —— s_cfg_pending 据此判断"还要不要补存" */
    snprintf(s_saved_ssid, sizeof(s_saved_ssid), "%s", s_ssid);
    snprintf(s_saved_pass, sizeof(s_saved_pass), "%s", s_pass);
    s_cfg_pending = false;
    s_cfg_verified = true;
    ESP_LOGI(TAG, "Wi-Fi 配置已保存: %s", s_ssid);
}

/* ★ 2026-11-22: 连接失败 → 丢弃**内存里**这份没验证过的凭据.
 *   不碰 NVS: 里面可能还存着上一次真正连通过的网络, 用户输错一次不该把旧的也弄丢。
 *   清掉之后自愈任务不会再拿错误密码反复重连, 页面可以直接让用户重输。 */
void wifi_manager_discard_pending(void) {
    if (!s_cfg_pending) return;      /* 当前凭据就是已落盘的那份: 不动 */
    /* ★ 这里**不回填** s_saved_*: 回填会让自愈任务拿"上一次保存的网"偷偷重连,
     *   用户正在重输密码, 却看到状态栏跳到另一个网络连上了 —— 更容易误解。
     *   正确做法: 内存凭据清空 + 不重连; NVS 原样保留 (下次开机照旧自动连它). */
    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    s_retry = 0;
    s_connecting = false;
    s_cfg_pending = false;
    if (!s_connected) snprintf(s_status, sizeof(s_status), "%s", "未连接");
    ESP_LOGW(TAG, "连接失败: 本次凭据未保存, 已从内存丢弃 (NVS 保留上次的: %s)",
             s_saved_ssid[0] ? s_saved_ssid : "(无)");
}

/* 清空已保存的 SSID/密码 (用于"连接失败→清空重输") */
void wifi_manager_clear_saved(void){    nvs_handle_t h;
    if (svc_nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        svc_nvs_erase_key(h, KEY_SSID);
        svc_nvs_erase_key(h, KEY_PASS);
        svc_nvs_erase_key(h, KEY_VERIFIED);
        svc_nvs_commit(h);
        svc_nvs_close(h);
    }
    s_ssid[0]=0; s_pass[0]=0; s_connected=false;
    s_saved_ssid[0]=0; s_saved_pass[0]=0;
    s_cfg_pending=false; s_cfg_verified=false;
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
    /* ★ 2026-11-22: 老存档没有这个键 ⇒ 视为**未验证** ⇒ 页面会要求重输密码。
     *   这正是"以前输错过一次就再也改不回来"的出口。 */
    uint8_t ver = 0;
    s_cfg_verified = (svc_nvs_get_u8(h, KEY_VERIFIED, &ver) == ESP_OK && ver == 1);
    svc_nvs_close(h);
    /* 快照与内存凭据对齐: 开机自动连接用的就是这份, 连上后不需要再重复落盘 */
    snprintf(s_saved_ssid, sizeof(s_saved_ssid), "%s", s_ssid);
    snprintf(s_saved_pass, sizeof(s_saved_pass), "%s", s_pass);
    s_cfg_pending = false;
    if (s_ssid[0]) {
        ESP_LOGI(TAG, "已加载保存的 Wi-Fi 配置: %s (%s)", s_ssid,
                 s_cfg_verified ? "已验证" : "未验证→点它需重输密码");
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
        /* ★ 2026-11-22: 一起清掉"已验证"标记, 否则下次还会把它当可用凭据 */
        s_cfg_verified = false;
        s_saved_ssid[0] = 0; s_saved_pass[0] = 0;
        s_cfg_pending = false;
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
    /* ★★★ 2026-11-22 关键: **未验证的密码不吐出去**。
     *   页面拿到空密码 ⇒ 必然弹键盘让用户重输 (不再"用保存的错误密码直连→失败→再点→再失败")。
     *   SSID 照给: 列表里还要显示这个网络名字。 */
    snprintf(pass, pass_sz, "%s", s_cfg_verified ? s_pass : "");
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
/* ★★★ 2026-11-22 修「密码正确也连不上」★★★
 *   sta_connect_saved() 定义在后面, 但 STA_START 事件处理要用 —— 先声明。
 *   为什么必须用它而不是裸 esp_wifi_connect(): 见 STA_START 分支的注释。 */
static bool sta_connect_saved(void);

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
        /* ★★★ 2026-11-22 修「密码正确也连不上」(linit 实测) ★★★
         *   这里原来是裸 `esp_wifi_connect()` + 自己置 s_connecting=true。
         *   在 WIFI_STORAGE_FLASH 时代它是能用的: 驱动 start 时会从 nvs.net80211
         *   把上次的 STA 配置读回来, 所以"配置已经在那儿"。
         *   但本版改成 WIFI_STORAGE_RAM 之后 —— **RAM 配置会被 stop() 丢掉**,
         *   而启动时驱动没有任何配置 ⇒ 这次 connect 拿着空 SSID 直接失败
         *   (ESP_ERR_WIFI_SSID), 更糟的是 s_connecting 已被置 true ⇒
         *   下面 enable_impl 里"没连且没在连才发起连接"的判断被跳过,
         *   ⇒ **整机根本没发起过一次带凭据的连接**, 界面一直"连接中...",
         *   15s 超时后才由自愈任务补上(或者一直失败)。
         *   修法: 统一走 sta_connect_saved() —— 它负责 set_config + connect,
         *   是"配置从哪来"的唯一权威路径。 */
        if (s_ssid[0] && s_suppress_reconnect) {
            /* ★ 2026-11-22: 扫描流程已经提前置了这个标志 —— 扫描期间射频必须独占,
             *   否则"自动连保存的网络"会和扫描抢空口 ⇒ 第一次扫描空结果。 */
            ESP_LOGI(TAG, "STA 启动但正在扫描 → 暂不自动连接 %s", s_ssid);
            snprintf(s_status, sizeof(s_status), "扫描中");
        } else if (s_ssid[0]) {
            ESP_LOGI(TAG, "STA 启动, 连接 %s ...", s_ssid);
            if (!sta_connect_saved()) {
                s_connecting = false;
                snprintf(s_status, sizeof(s_status), "连接失败");
                ESP_LOGW(TAG, "STA 启动后发起连接失败 (配置/状态异常), 等自愈任务重试");
            }
        } else {
            snprintf(s_status, sizeof(s_status), "未配置");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        /* ★★★ 2026-11-07 linit「统一极致省电」: 这里原来连写两遍
         *   `esp_wifi_set_ps(WIFI_PS_NONE)`(重复语句, 合并残留), 而且方向是错的 ——
         *   CONFIG_ESP_WIFI_STA_DISCONNECTED_PM_ENABLE=y 的作用正是"未连接时让 modem
         *   在射频空闲时睡觉", 一旦把 PS 设成 NONE, **这个编译进去的省电能力就被关掉了**。
         *   现在断开后仍回统一省电档, 由驱动决定空闲时是否 modem-sleep。
         *   (PS 只在**已关联**状态下决定收 beacon 的节奏; 未关联时的省电由上面那个 Kconfig 管) */
        wifi_manager_apply_ps();
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
            /* ★★★ 2026-11-22 linit「输错一次就再也改不回来」的直接出口:
             *   如果被判密码错的是**已落盘的**那份凭据, 撤销它的"已验证"标记 ⇒
             *   用户下次点这个网络时 get_saved() 不吐密码, 页面必然弹键盘重输。
             *   (未落盘的试用凭据不用管: 它本来就不在 NVS 里, 由页面 discard。) */
            if (s_cfg_verified && s_ssid[0] && strcmp(s_ssid, s_saved_ssid) == 0) {
                s_cfg_verified = false;
                nvs_handle_t h;
                if (svc_nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
                    svc_nvs_erase_key(h, KEY_VERIFIED);
                    svc_nvs_commit(h);
                    svc_nvs_close(h);
                }
                ESP_LOGW(TAG, "已保存凭据被判密码错 → 撤销已验证标记, 下次点该网络需重输");
            }
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
                /* ★ 2026-10-20 诊断改后台任务(见 wifi_diag_task): 事件处理里不允许阻塞
                 * ★ 2026-11-22: 建成功才置 s_diag_done (失败时下次 GOT_IP 还能再试) */
                if (!s_diag_done &&
                    xTaskCreate(wifi_diag_task, "wifi_diag", 4096, NULL, 1, NULL) == pdPASS)
                    s_diag_done = true;
            }
        }
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_connected = true;
        s_connecting = false;
        s_connected_just_now = true;
        char ipbuf[16];
        esp_ip4addr_ntoa(&e->ip_info.ip, ipbuf, sizeof(ipbuf));
        snprintf(s_ip, sizeof(s_ip), "%s", ipbuf);
        /* ★ 2026-10-30 linit: 状态栏只显示"已连接", 不再带 IP ——
         *   用户要求"连接成功不用显示 IP"; IP 仍存进 s_ip, 需要时(
         *   wifi_manager_get_ip() / 网络检测页)照样取得到, 只是状态文本干净了。 */
        snprintf(s_status, sizeof(s_status), "%s", "已连接");
        ESP_LOGI(TAG, "Wi-Fi 已连接: %s", s_status);
        /* ★ 2026-11-22: **连上了才落盘**。密码错的连接永远到不了这里,
         *   所以自动连接凭据与连接记录里不会出现没连通过的密码。 */
        if (s_cfg_pending) save_cfg();
        else mark_verified();   /* 开机自动连上老凭据: 补打"已验证", 以后可直接复用密码 */
        /* ★ 2026-10-26 linit: **每次联网都校一次时间** (原来"当天只校一次")。
         *   每次拿到 IP 都会让 SNTP 重发一次查询; 后台异步, 成功后自动 settimeofday,
         *   不阻塞连接流程。关屏保时 WiFi 会关掉, 解锁重连时这里正好又补校一次。 */
        svc_time_ntp_sync();

        /* ★★★ 2026-11-07 linit「统一极致省电」: 连上就上统一省电档。
         *   历史: 2026-10-26 起是"连上 MIN_MODEM / 断开与扫描切 NONE";
         *   2026-10-31 又加了"下载期间强制 NONE"。两条都把切档权散给了业务层,
         *   只要有一处没恢复, 射频就永久停在 70~90mA。现在只剩这一个入口。 */
        wifi_manager_apply_ps();
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
static bool wifi_manager_scan_start_impl(void) {
    /* ★★★ 2026-11-22 修「刚开 WiFi 第一次搜索提示没有网络, 再搜一次才行」(linit) ★★★
     *   根因: **扫描与"开射频自动连保存网络"抢跑**。
     *     enable_impl() 里 esp_wifi_start() 之后, 事件任务(优先级 20, 高于 UI)立刻处理
     *     WIFI_EVENT_STA_START, 那里会用保存的凭据发起连接; 而扫描紧接着才进来,
     *     先 esp_wifi_disconnect() 把它打断, 再 esp_wifi_scan_start()。
     *     射频刚刚被"认证握手 → 强断"折腾过, 主动扫描的 probe/驻留时序被破坏,
     *     ⇒ 第一次扫描经常返回 **0 个 AP**, 界面就显示"未找到网络"; 手动再搜一次时
     *     连接流程已经停下, 就正常了。
     *   修法: 把"本次要扫描"的意图**提前到开射频之前** ⇒ STA_START 与 enable 末尾的
     *     自动连接都会看 s_suppress_reconnect 跳过, 扫描全程独占射频。
     *   注意: 每个提前返回点都必须恢复它, 否则会永久抑制自动重连。 */
    s_suppress_reconnect = true;
    {
        /* 只诊断，不干预：碎片化高时打一条警告，方便现场定位；扫描照常尝试。
         * （2026-10-25: 曾在这里自动关蓝牙，实测 linit 的崩溃与蓝牙无关，已撤掉。）*/
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (largest < 24 * 1024)
            ESP_LOGW(TAG, "内部最大连续块仅 %u B (碎片偏高)", (unsigned)largest);
    }
    if (!s_inited) wifi_manager_init();
    if (!s_inited) { s_suppress_reconnect = false; return false; }
    if (!s_enabled) {
        if (!wifi_manager_enable()) { s_suppress_reconnect = false; return false; }
        vTaskDelay(pdMS_TO_TICKS(150));   /* 等射频稳定: 刚开启立即扫描会 NOT_STARTED/STATE */
    }
    /* 根因修复: 若 STA 正在"连接/连电源重试"保存的网, esp_wifi 不允许 scan
     * (返回 ESP_ERR_WIFI_STATE), 导致扫描卡死"正在扫描...". 扫描期间先抑制自动重连
     * (否则主动 disconnect 会触发重连 esp_wifi_connect, 与扫描冲突又阻塞事件任务,
     * 扫描长时间不出结果), 再强制断开一次让 WiFi 进入可扫描状态. */
    if (s_connecting || !s_connected) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_scan_done = false;
    s_ap_count = 0;
    /* ★★★ 2026-11-07 linit「统一极致省电」: 这里原来 `esp_wifi_set_ps(WIFI_PS_NONE)`,
     *   问题是**扫完没人恢复** —— 无线探测/设置页扫一次, 射频就永久停在 70~90mA 全醒档。
     *   扫描本身是"主动发探测请求 + 收回应", 与收 beacon 的省电节奏无关, MIN_MODEM 下
     *   一样能扫完(驱动扫描期间自己会把射频拉起来)。所以这里不再动省电档。 */
    /* ★ 2026-10-20: 强制 20MHz。本机 AP 报的是 40U, 窄带在穿墙/干扰下重传少、握手更快;
     *   我们只跑网页小包, 带宽上限降低无感。 */
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
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
        /* ★ 2026-11-22: 必须把"扫描完成"置真。12s 兜底要求 s_scanning==true,
         *   而这里 s_scanning 从来没被置过 ⇒ 原来界面会**永远停在"正在扫描"**, 只能重启。 */
        s_scan_done = true;
        return false;
    }
    ESP_LOGI(TAG, "扫描已启动 (%s)", bt_manager_is_enabled() ? "IDF 默认时序/BT 开" : "active 40-200ms/BT 关");
    s_scanning = true;                                        /* 等 SCAN_DONE 或 12s 超时 */
    s_scan_t0  = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    return true;
}

bool wifi_manager_scan_start(void)
{
    wm_lock();
    bool r = wifi_manager_scan_start_impl();
    wm_unlock();
    return r;
}

void wifi_manager_scan_stop(void) {
    wm_lock();
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
    wm_unlock();
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

/* ★★★ 2026-11-22 linit「密码输错也保存了, 之后改不回来」根治(上) ★★★
 *   真正的"偷偷保存"点在**驱动层**: `esp_wifi_set_config()` 在 WIFI_STORAGE_FLASH
 *   (IDF 默认) 下会把 SSID/密码写进 nvs.net80211 —— 跟我们自己的 svc_nvs 完全无关。
 *   所以就算我们把 save_cfg 改成"连上才存", 错误密码照样落盘, 重启后又复活。
 *   两手一起做:
 *     ① esp_wifi_set_storage(WIFI_STORAGE_RAM)：以后 set_config 只留内存;
 *     ② 本函数把**历史遗留**的那份擦掉一次 (esp_wifi_restore)。
 *   用 wf_clean 标记保证只擦一次, 不额外磨损 flash。 */
static void wifi_drv_cfg_migrate_once(void) {
    nvs_handle_t h;
    bool done = false;
    if (svc_nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t v = 0;
    done = (svc_nvs_get_u8(h, KEY_WF_CLEAN, &v) == ESP_OK && v == 1);
    svc_nvs_close(h);
    if (done) return;
    /* ★ 安全阀: 网页手柄(AP 模式)可能比我们先 init 了 esp_wifi。
     *   esp_wifi_restore() 会把 mode / config 一起重置 ⇒ 那种情况下必须让路,
     *   否则一开 WiFi 就把网页手柄的热点拆了。不写标记, 下次 STA 场景再迁。 */
    wifi_mode_t m = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&m) == ESP_OK &&
        (m == WIFI_MODE_AP || m == WIFI_MODE_APSTA)) {
        ESP_LOGW(TAG, "迁移跳过: 当前是 AP/APSTA 模式(网页手柄在用)");
        return;
    }
    /* ★ 先擦、成功了才落标记 —— 擦失败(极少见)下次开机还会重试,
     *   否则标记一写就再也没机会清掉那份错误密码。 */
    esp_err_t e = esp_wifi_restore();   /* 清 nvs.net80211 里保存的 sta 配置 */
    ESP_LOGW(TAG, "升级迁移: 清除驱动层遗留 WiFi 配置 (%s) —— 防错误密码重启复活",
             esp_err_to_name(e));
    if (e != ESP_OK) return;
    if (svc_nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    svc_nvs_set_u8(h, KEY_WF_CLEAN, 1);
    svc_nvs_commit(h);
    svc_nvs_close(h);
}

void wifi_manager_init_impl(void) {
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
    /* 顺序很关键: esp_wifi_restore() 会连 esp_wifi_set_mode 一起重置
     * (IDF 文档: 恢复 bandwidth/protocol/config/mode), 所以迁移必须在 set_mode 之前。
     * storage 是运行时设置, restore 不重置, 但放它前面更保险。
     * ⚠ storage=RAM 与上面 cfg.nvs_enable=0 **不是一回事**:
     *   nvs_enable 管驱动内部状态机(实测关掉会让 connect 返回 OK 但状态机不启动), 必须保持 1;
     *   storage=RAM 只决定 set_config 要不要落盘 —— 正是我们要的: 不落盘。 */
    wifi_drv_cfg_migrate_once();
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_inited = true;
    /* 网络自愈监督任务 (只建一次): 持久重连, 见 wifi_supervisor_task
     * ★ 2026-11-22: 先建成功再置标志 —— 原来先置 true, 建任务失败后本次开机
     *   再也不会尝试, "WiFi 连上断了不自动重连"就查不到原因。 */
    static bool s_sup_started = false;
    if (!s_sup_started) {
        if (xTaskCreate(wifi_supervisor_task, "wifi_sup", 3072, NULL, 3, NULL) == pdPASS)
            s_sup_started = true;
        else
            ESP_LOGW(TAG, "wifi_sup 任务创建失败 (内部 RAM 不足) ⇒ 本次开机无自动重连监督");
    }
    ESP_LOGI(TAG, "WiFi 子系统初始化完成 (STA)");
}

void wifi_manager_init(void)
{
    wm_lock();
    wifi_manager_init_impl();
    wm_unlock();
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
    /* ★★★ 2026-11-22 修: 这里原来是 WIFI_AUTH_WPA2_PSK, 与 connect_impl 的
     *   WIFI_AUTH_OPEN **两套口径**。
     *   threshold.authmode 是"扫到低于这个安全等级的 AP 就不用"的**过滤门限**,
     *   设成 WPA2_PSK 会滤掉: 开放网络 / WPA(老路由) / 部分 WPA3 过渡模式的
     *   隐蔽 BSS —— 表现为连不上且 reason 常是 201(找不到 AP)。
     *   而 connect_impl(用户手输密码那条)用的是 OPEN, 所以"手动能连、自动连不上"
     *   这种诡异现场正是这两个口径打架造成的。统一用 OPEN:
     *   threshold 只拦**完全没加密**的反向场景交给下方 authmode 本身, 不在这里过滤。 */
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) return false;
    /* ★ 诊断用: 只打密码长度, 不打密码本身。空密码连 WPA2 必然失败,
     *   有这一行就能一眼看出"驱动拿到的凭据是不是空的"。 */
    ESP_LOGI(TAG, "下发 STA 配置: ssid=%s pass_len=%u", s_ssid, (unsigned)strlen(s_pass));
    esp_wifi_disconnect();          /* 清掉旧状态, 保证这次 connect 真的重连 */
    s_connecting = true;
    s_conn_t0 = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    snprintf(s_status, sizeof(s_status), "连接中...");
    if (esp_wifi_connect() != ESP_OK) { s_connecting = false; return false; }
    return true;
}

static bool wifi_manager_enable_impl(void) {
    if (s_enabled) return true;
    if (!s_inited) wifi_manager_init();
    if (!s_inited) return false;

    /* V1.0.67: 网页手柄可能切过 AP 模式, 这里强制回 STA。
     * ★ 2026-11-01: 但如果热点正开着，必须用 APSTA —— 用 MODE_STA 会把热点关掉，
     *   而 s_ap_on 还留着 true ⇒ 网页手柄的页面立刻打不开。 */
    esp_wifi_set_mode(s_ap_on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGD(TAG, "WiFi 启动失败");
        return false;
    }
    /* ★★★ 2026-11-07 linit「统一极致省电」: 这里原来是 `WIFI_PS_NONE`
     *   (理由"省电会让扫描时快时慢")。但代价是**只要开过 WiFi 就一直全醒**,
     *   而那只是一次性扫描的观感问题。现在开射频即上统一省电档,
     *   扫描速率不受影响(扫描由驱动自己拉射频)。 */

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
    wifi_manager_apply_ps();          /* ★ 开射频即上统一省电档 (唯一策略入口) */
    ESP_LOGI(TAG, "WiFi 射频已启动 (统一省电档: %s)", wifi_manager_ps_name());

    /* ★ 2026-10-09 linit 需求: **打开 WiFi 就自动连上"上次保存的密码"**。
     *   原来 enable() 只"开射频"不连网 —— 上层(应用商店门禁 / 设置页)各自再连一次,
     *   既重复、又要和扫描抢状态, 还出现过"门禁看到 conn=0 就反复弹确认框"。
     *   现在统一收口在 enable(): 只要有存的凭据且当前没连, 立刻发起连接。 */
    /* ★ 2026-11-22: 扫描期间不自动连 —— 扫描流程会在调用 enable 之前先把
     *   s_suppress_reconnect 置真(见 scan_start_impl 顶部注释), 这里必须遵守,
     *   否则"开射频即连保存的网络"会和扫描抢空口 ⇒ 第一次扫描返回 0 个 AP。 */
    if (!s_suppress_reconnect && !s_connected && !s_connecting && s_ssid[0]) {
        if (sta_connect_saved()) ESP_LOGI(TAG, "已自动连接保存的 WiFi: %s", s_ssid);
        else                     ESP_LOGD(TAG, "自动连接保存的 WiFi 失败: %s", s_ssid);
    } else if (s_suppress_reconnect && s_ssid[0]) {
        ESP_LOGI(TAG, "扫描流程进行中 → 跳过开射频后的自动连接");
    }
    return true;
}

bool wifi_manager_enable(void)
{
    wm_lock();
    bool r = wifi_manager_enable_impl();
    wm_unlock();
    return r;
}

/* ==================== AP 热点（统一持有，APSTA） ====================
 * 设计要点见 wifi_manager.h。三条硬约束：
 *   ① **绝不**用 WIFI_MODE_AP —— 那会踢掉已连接的 STA；
 *      APSTA 下驱动自动把 AP 信道对齐到 STA 信道。
 *   ② 退出只关 AP（stop dhcps/dns + 模式回 STA），不碰 STA 连接。
 *   ③ 重复 start 幂等；stop 后 s_ap_on 必须复位，否则下次永远起不来。 */
esp_err_t wifi_manager_ap_start_impl(const char *ssid, const char *pass,
                                     uint8_t channel, uint8_t max_conn, bool captive)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    if (s_ap_on) return ESP_OK;

    if (!s_inited) wifi_manager_init();
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!wifi_manager_enable()) return ESP_FAIL;      /* 射频起（内部已处理 APSTA） */

    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) ap = esp_netif_create_default_wifi_ap();
    if (!ap) return ESP_FAIL;

    wifi_config_t cfg = { 0 };
    size_t sl = strlen(ssid);
    if (sl > sizeof(cfg.ap.ssid) - 1) sl = sizeof(cfg.ap.ssid) - 1;
    memcpy(cfg.ap.ssid, ssid, sl);
    cfg.ap.ssid_len = (uint8_t)sl;
    cfg.ap.channel  = channel ? channel : 6;
    cfg.ap.max_connection = max_conn ? max_conn : 4;
    cfg.ap.pmf_cfg.required = false;
    if (pass && strlen(pass) >= 8) {              /* WPA2 要求 ≥8 字符 */
        size_t pl = strlen(pass);
        if (pl > sizeof(cfg.ap.password) - 1) pl = sizeof(cfg.ap.password) - 1;
        memcpy(cfg.ap.password, pass, pl);          /* 有意截断：WPA2 密码上限 63 */
        cfg.ap.password[pl] = 0;
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.ap.password[0] = 0;
        cfg.ap.authmode    = WIFI_AUTH_OPEN;
    }

    /* ★ 关键：APSTA，保 STA 不掉线 */
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) return ESP_FAIL;
    if (esp_wifi_set_config(WIFI_IF_AP, &cfg) != ESP_OK) return ESP_FAIL;

    /* 网关固定 8.8.8.8：手机连上后浏览器直接访问它（好记，和旧行为一致） */
    esp_netif_ip_info_t ip = { 0 };
    IP4_ADDR(&ip.ip,      8, 8, 8, 8);
    IP4_ADDR(&ip.gw,      8, 8, 8, 8);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap);
    esp_netif_set_ip_info(ap, &ip);
    if (captive)
        esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                               "http://8.8.8.8/", strlen("http://8.8.8.8/"));
    esp_netif_dhcps_start(ap);

    if (captive) {
        dns_server_config_t dc = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
        s_ap_dns = start_dns_server(&dc);
        if (!s_ap_dns) ESP_LOGW(TAG, "captive DNS 启动失败（不影响网页访问）");
    }

    s_ap_on = true;
    ESP_LOGI(TAG, "AP 已开启: '%s' (%s, ch=%u, captive=%d) 已连接的 STA 保持不动",
             ssid, cfg.ap.authmode == WIFI_AUTH_OPEN ? "开放" : "WPA2",
             (unsigned)cfg.ap.channel, (int)captive);
    return ESP_OK;
}

esp_err_t wifi_manager_ap_start(const char *ssid, const char *pass,
                                uint8_t channel, uint8_t max_conn, bool captive)
{
    wm_lock();
    esp_err_t r = wifi_manager_ap_start_impl(ssid, pass, channel, max_conn, captive);
    wm_unlock();
    return r;
}

static void wifi_manager_ap_stop_impl(void)
{
    if (!s_ap_on) return;
    s_ap_on = false;
    if (s_ap_dns) { stop_dns_server(s_ap_dns); s_ap_dns = NULL; }
    /* 只关 AP，STA 继续用：回 STA 模式即可（AP netif 不再承载流量） */
    if (s_enabled) esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "AP 已关闭（STA 保持不变: %s）", s_connected ? "已连接" : "未连接");
}

void wifi_manager_ap_stop(void)
{
    wm_lock();
    wifi_manager_ap_stop_impl();
    wm_unlock();
}

bool wifi_manager_ap_is_on(void) { return s_ap_on; }

bool wifi_manager_ap_get_ip(char *out, size_t cap)
{
    if (!s_ap_on || !out || cap == 0) return false;
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) return false;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(ap, &ip) != ESP_OK) return false;
    snprintf(out, cap, IPSTR, IP2STR(&ip.ip));
    return true;
}

static bool wifi_manager_disable_impl(void) {
    /* ★ 2026-11-01: 关射频前先把本模块的 AP 关掉（AP 归本模块管）。
     *   不然下面 esp_wifi_stop() 会把热点一起停掉，但 s_ap_on 仍为 true，
     *   下次开热点会误判"已开"直接返回 ⇒ 热点再也起不来。 */
    if (s_ap_on) wifi_manager_ap_stop();
    /* ★★★ 2026-11-08 漏洞 #5（审计发现）★★★
     *   原来这里直接 `if (!s_enabled) return true;` —— 与 force_off 注释里记的是同一个坑:
     *   频谱仪(promiscuous)把模式切成 WIFI_MODE_NULL 再 esp_wifi_start(), 或网页手柄
     *   共享 AP 之后, s_enabled 可能已是 false 而射频**真的在工作** ⇒ 这个提前返回
     *   让"设置页关 WiFi"变成空操作, 射频继续 70~90mA。
     *   串口 `wifi off` 和快捷栏之前都绕过了这个洞, 但设置页仍走本函数 —— 现在补上:
     *   标志说"已关"但驱动还在, 就退回 force_off (只问驱动, 不看标志)。 */
    if (!s_enabled) { wifi_manager_force_off(); return true; }
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
    /* ★★★ 2026-10-28 【扫描状态必须一并复位】★★★
     *   esp_wifi_stop() 会把正在进行的扫描一并终止, 但扫描相关的三个标志原来没人清:
     *     s_suppress_reconnect / s_scanning / s_scan_t0。
     *   残留后果(明确可复现的路径):
     *     ① 用户在设置页点"扫描" ⇒ s_suppress_reconnect = true (见 scan_start)
     *     ② 无操作 1 分钟 ⇒ 进屏保 ⇒ wallpapers.c 调 wifi_manager_disable()
     *     ③ 退出屏保 ⇒ wallpapers 的 wifi_restore_task 调 enable(), 射频起来了
     *     ④ wifi_supervisor_task 的重连条件是
     *          `s_enabled && s_ssid[0] && !connected && !connecting && !s_suppress_reconnect`
     *        此时 s_suppress_reconnect 仍是 true ⇒ **永久跳过自动重连**
     *     ⇒ 现象: 解锁后 WiFi 显示"已开启", 却一直连不上; 要么再进设置页扫一次,
     *        要么重启, 才会恢复正常。
     *   (唯一的自愈途径是 UI 继续轮询 wifi_manager_is_scan_done() 的 12 秒超时,
     *    但进了屏保之后没有 UI 在轮询它。) */
    s_suppress_reconnect = false;
    s_scanning  = false;
    s_scan_t0   = 0;
    s_scan_done = true;    /* 让仍挂在"扫描中"的 UI 轮询立刻退出, 不必等 12s 超时 */
    snprintf(s_status, sizeof(s_status), "未连接");
    ESP_LOGI(TAG, "WiFi 已关闭");
    return true;
}

bool wifi_manager_disable(void)
{
    wm_lock();
    bool r = wifi_manager_disable_impl();
    wm_unlock();
    return r;
}

bool wifi_manager_is_enabled(void) { return s_enabled; }
bool wifi_manager_is_connected(void) { return s_connected; }
bool wifi_manager_is_connecting(void) { return s_connecting; }

/* ★ 2026-11-24: 省电模式「Wi-Fi 闲置自动关闭」的统一活动接口 (头文件有完整说明)。
 * 逐个对应任务书"必须检查并保护"的清单:
 *   · 正在连接 / 正在扫描 / 已连接 / 商店或 OTA 网络任务 → 已连接与各状态位覆盖;
 *   · 上传/下载/Lua 网络任务 → 一定发生在连接建立之后, 被 s_connected 覆盖;
 *     断开瞬间还有 60s 冷却窗口(s_last_act_ms), 避免"刚断就判闲置"。
 * ⛔ 不要用它判断"能不能关": 它只回答"现在有没有人用"。关闭决策在 wallpapers.c 的
 *    wp_eco_tick()(那里还要叠加"整机闲置时长"与省电模式开关)。 */
bool wifi_manager_is_busy(void)
{
    if (!s_enabled) return false;
    if (s_connected || s_connecting || s_scanning) return true;
    if (s_ap_on) return true;               /* 网页手柄/热点: 有设备可能正连着 */
    if (s_rf_measure > 0) return true;      /* 频谱仪连续抓包: 关射频会拆掉采集链 */
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (s_last_act_ms && (uint32_t)(now - s_last_act_ms) < 60000u) return true;
    return false;
}

/* ★★★ 2026-11-22 「网络状态只读接口」—— 页面/应用一律用这三个, 不许再自己调 esp_wifi_* ★★★
 *   背景: 除了管理器, 还有 4 处直接读驱动(设置页网络详情 / Lua 的 wifi.ssid / Lua 的
 *   net.mac / os_housekeeper)。分散直连的害处不只是"不统一":
 *     · 读接口本身无害, 但一旦有人顺手写成 stop/deinit, 就会把别的应用正在用的射频关掉;
 *     · os_housekeeper 更严重 —— 它直接 esp_wifi_scan_stop(), **漏了 clear_ap_list**,
 *       扫描中途停掉时驱动分配的 AP 列表内存永远不还(内部 RAM 缓慢泄漏, 见 scan_stop 注释)。
 *   所以把"能读什么"收敛成这三个函数, 底层实现只在这里维护。 */
bool wifi_manager_get_ap_info(int *rssi, int *auth, uint8_t *channel)
{
    bool ok = false;
    wm_lock();
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        if (rssi)    *rssi = ap.rssi;
        if (auth)    *auth = (int)ap.authmode;
        ok = true;
    }
    if (channel) {
        uint8_t ch = 0; wifi_second_chan_t sc;
        if (esp_wifi_get_channel(&ch, &sc) == ESP_OK) *channel = ch;
    }
    wm_unlock();
    return ok;
}

bool wifi_manager_get_ssid(char *out, size_t sz)
{
    if (!out || sz == 0) return false;
    out[0] = '\0';
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK || !ap.ssid[0]) return false;
    snprintf(out, sz, "%s", (const char *)ap.ssid);
    return true;
}

bool wifi_manager_get_mac(uint8_t mac[6])
{
    if (!mac) return false;
    return esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK;
}

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
        /* ★ 2026-11-08 官方式空闲降档: 连上但连续 20s 没有网络活动 ⇒ 转 MAX_MODEM。
         *   射频占空比从"每 102ms 醒一次"降到"每 listen_interval 个 beacon 醒一次",
         *   待机发热随之下降; 任何请求进来经 wifi_manager_note_net_activity() 立刻回档。 */
        if (s_ps_auto && s_connected && !s_connecting && s_rf_measure == 0) {
            uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (s_last_act_ms == 0) s_last_act_ms = now_ms;
            if (s_ps_mode != WIFI_PS_MAX_MODEM &&
                (uint32_t)(now_ms - s_last_act_ms) >= PS_IDLE_MS) {
                s_ps_mode = WIFI_PS_MAX_MODEM;
                wifi_manager_apply_ps();
                ESP_LOGI(TAG, "空闲 %us: 省电档降为 MAX_MODEM (待机省电)", (unsigned)(PS_IDLE_MS / 1000));
            }
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
/* ============ 统一 Wi-Fi 省电策略（全项目唯一入口, linit 2026-11-07）============
 * 定规:
 *   ① 正常联网**恒用** WIFI_PS_UNIFIED, 业务层不再拥有切档权;
 *   ② 下载 / 局域网页 / 扫描 / 网络检测 **一律不再切 NONE**;
 *   ③ 唯一例外 = 频谱仪的 promiscuous 连续抓包, 且只在测量期间 (见下方 rf_measure_*)。
 *
 * ⚠ 为什么默认 MIN_MODEM 而不是 MAX_MODEM（不是省电优先级问题, 是本机实测问题）:
 *   本项目 2026-10-31 的真机记录 —— 同一张卡、同一个 AP, 只差一个省电开关:
 *     · MIN_MODEM: 局域网 ping 丢包 37.5%, RTT 21~276ms; 41.7MB 资源包传到第 8 个
 *       文件(7.5MB)就断流, 之后连 TCP 都建不起来;
 *     · NONE:      丢包 0%, RTT 10~44ms, 同一个包一次跑完。
 *   MAX_MODEM 靠 wifi_sta_config_t.listen_interval 决定醒来的 beacon 间隔(默认 3),
 *   等于在上述问题之上再拉长睡眠窗口 —— 广播/组播会整帧丢失。
 *   ⇒ **未做真机验证前不选 MAX_MODEM**。验证通过后只改下面这一行即可全局切档。
 */
const char *wifi_manager_ps_name(void) {
    /* ★ 2026-11-08: 原来 switch 的是编译期宏 WIFI_PS_UNIFIED —— 运行期切档后
     *   这里仍报旧名字, 日志会骗人。改成读当前实际档位 s_ps_mode。 */
    switch (s_ps_mode) {
    case WIFI_PS_NONE:      return "NONE";
    case WIFI_PS_MAX_MODEM: return "MAX_MODEM";
    default:                return "MIN_MODEM";
    }
}

/* 把统一省电档施加到射频。**所有**切档路径都必须经过这里, 不许别处直接调
 * esp_wifi_set_ps()。 */
void wifi_manager_apply_ps(void) {
    /* ★ 2026-11-08: 原来第一行是 `if (!s_enabled) return;` —— 标志与真实射频状态
     *   脱节时(频谱仪把模式切成 WIFI_MODE_NULL 再 start, s_enabled 可能已是 false)
     *   省电档**根本不会被施加**, 射频就停在全醒档(70~90mA)。
     *   改成只问驱动: 没 init 才返回, 与 wifi_manager_force_off() 同一判据。 */
    wifi_mode_t m;
    if (esp_wifi_get_mode(&m) != ESP_OK) return;
    if (s_rf_measure > 0) {
        esp_wifi_set_ps(WIFI_PS_NONE);      /* 频谱仪测量窗口: 必须连续收 */
        return;
    }
    esp_err_t e = esp_wifi_set_ps(s_ps_mode);
    if (e != ESP_OK) ESP_LOGW(TAG, "设置省电档失败: %s", esp_err_to_name(e));
}

/* 运行期切省电档 (remcon `wifi psmode`): 1=MAX_MODEM 深省电 / 0=MIN_MODEM 默认
 * 手动切档会**关掉**自动降档, 否则 2s 后就被自动逻辑改回去。 */
void wifi_manager_set_ps_mode(int max_modem)
{
    s_ps_auto = false;
    s_ps_mode = max_modem ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM;
    wifi_manager_apply_ps();
    ESP_LOGI(TAG, "省电档切到 %s (手动固定, 自动降档已关; 本次运行有效)",
             max_modem ? "MAX_MODEM 深省电" : "MIN_MODEM 默认");
}

/* 手动开/关自动降档 (remcon `wifi psauto 0|1`)。打开时立刻回默认档。 */
void wifi_manager_set_ps_auto(int on)
{
    s_ps_auto = on ? true : false;
    if (s_ps_auto) {
        s_last_act_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        s_ps_mode = WIFI_PS_UNIFIED;
        wifi_manager_apply_ps();
    }
    ESP_LOGI(TAG, "空闲自动降档: %s", s_ps_auto ? "开 (20s 空闲→MAX_MODEM)" : "关");
}

bool wifi_manager_ps_auto_enabled(void) { return s_ps_auto; }

/* 网络活动上报: 任何要收发数据的路径(下载/网页/商店/校时)开头调一次。
 * 有活动立即回 MIN_MODEM —— 不能让用户等深省电档慢慢醒来。 */
void wifi_manager_note_net_activity(void)
{
    s_last_act_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (s_ps_auto && s_ps_mode != WIFI_PS_UNIFIED) {
        s_ps_mode = WIFI_PS_UNIFIED;
        wifi_manager_apply_ps();
        ESP_LOGI(TAG, "网络活动: 省电档回 %s", wifi_manager_ps_name());
    }
}

/* ===== 唯一的高功耗窗口: 频谱仪 promiscuous 连续抓包 =====
 * 计数而非 bool: 进出次数不配对时不会提前恢复, 也不会漏恢复。 */
void wifi_manager_rf_measure_begin(void) {
    if (s_rf_measure++ == 0) {
        ESP_LOGW(TAG, "射频测量开始: 临时关闭省电 (仅测量期间, 退出即恢复)");
        wifi_manager_apply_ps();
    }
}

void wifi_manager_rf_measure_end(void) {
    if (s_rf_measure == 0) return;
    if (--s_rf_measure > 0) return;
    ESP_LOGI(TAG, "射频测量结束: 恢复统一省电档 %s", wifi_manager_ps_name());
    wifi_manager_apply_ps();
}

bool wifi_manager_rf_measure_active(void) { return s_rf_measure > 0; }

/* ============ 关机/深睡专用：无条件停射频 (linit 2026-11-07) ============
 * 为什么不能只靠 wifi_manager_disable(): 它开头就是 `if (!s_enabled) return true;`。
 * 而频谱仪把 WiFi 切到 WIFI_MODE_NULL 再 start 之后, s_enabled 可能已是 false,
 * 射频却仍在工作 —— 深睡时这一路会被跳过, 整机就多出 ~20mA。
 * (同理 `WIFI active=0 connected=1` 也是同一个"标志与真实状态脱节"的根因。)
 * 本函数**不看开关、不看引用计数**, 只问驱动: 还在就停掉 + deinit。 */
static void wifi_manager_force_off_impl(void)
{
    wifi_mode_t m;
    if (esp_wifi_get_mode(&m) != ESP_OK) {
        /* 没 init 过(返回 ESP_ERR_WIFI_NOT_INIT): 射频根本不存在, 直接返回 */
        s_enabled = false;
        return;
    }
    esp_wifi_disconnect();
    esp_wifi_stop();
    /* ★ 2026-11-22: 这条路径原来只 deinit 不注销事件处理器 (正常关 WiFi 的
     *   wifi_manager_disable_impl() 会注销)。处理器挂在 default event loop 上,
     *   esp_wifi_deinit() 不会带走它们 ⇒ 深睡/关机走过一次 force_off 之后,
     *   下次 enable 又注册一遍, 同一个 handler 挂两份:
     *   每个 WIFI/IP 事件执行两次 (GOT_IP 会重复写 NVS、重复回调、连接状态被写两次)。
     *   保持与 disable 路径对称: 先注销再 deinit。 */
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler);
    esp_wifi_deinit();
    s_enabled   = false;
    s_inited    = false;
    s_connected = false;
    s_connecting = false;
    s_scanning  = false;
    s_scan_done = true;
    s_rf_measure = 0;
    ESP_LOGI(TAG, "射频已强制关闭 (深睡/关机路径: 不看开关与引用计数)");
}

void wifi_manager_force_off(void)
{
    wm_lock();
    wifi_manager_force_off_impl();
    wm_unlock();
}

/* 只读查询: 当前**驱动里真实生效**的省电档 (供串口 `wifi ps` / 诊断用, 不改任何状态) */
const char *wifi_manager_ps_current(void) {
    wifi_ps_type_t t = WIFI_PS_MIN_MODEM;
    if (esp_wifi_get_ps(&t) != ESP_OK) return "?";
    switch (t) {
    case WIFI_PS_NONE:      return "NONE";
    case WIFI_PS_MAX_MODEM: return "MAX_MODEM";
    default:                return "MIN_MODEM";
    }
}

bool wifi_manager_get_ip(char *out, size_t sz) {
    if (!out || sz == 0) return false;
    out[0] = '\0';
    if (!s_connected || !s_ip[0]) return false;
    snprintf(out, sz, "%s", s_ip);
    return true;
}

static bool wifi_manager_connect_impl(const char *ssid, const char *password) {
    if (!ssid || ssid[0] == '\0') return false;

    /* ★★ 2026-10-20 linit: "重复点别起冲突" —— 这里的幂等/去重很关键:
     *   原来的实现每次调用都会先 esp_wifi_disconnect() 再连, 于是"连的过程里再点一次"
     *   会把正在进行的连接打断、从零重来 ⇒ 越点越慢、还可能失败。
     *   ① 已经连着同一个 SSID   → 直接成功, 一下都不动 (页面只需提示"已连接")
     *   ② 正在连同一个 SSID     → 直接返回 false (不打断), 页面提示"正在连接"
     *   ③ 才是真正的新连接/切换 */
    /* ★★★ 2026-11-22 修「输完正确密码还是连不上, 按了回车像没反应」★★★
     *   原来的短路只比 **SSID**:
     *       if (s_ssid == ssid) { if (connected) return true; if (connecting) return false; }
     *   于是"同一个网络、但用户改了密码重连"会被当成重复点击**直接 return false**,
     *   新密码连 snprintf 都没走到 ⇒ 驱动里仍是旧(错)密码, 自愈任务拿它反复重连,
     *   用户看到的就是"密码明明是对的却一直连不上"。密码错一次的现场正是这样锁死的。
     *   现在必须 **SSID + 密码都相同** 才短路; 密码变了就继续走下面重新 set_config,
     *   并允许打断正在进行的(用旧密码的)连接。 */
    const char *new_pass = password ? password : "";
    bool same_target = (s_ssid[0] && strcmp(s_ssid, ssid) == 0);
    bool same_pass   = (strcmp(s_pass, new_pass) == 0);
    if (same_target && same_pass) {
        if (s_connected)  return true;                 /* 已连着同一个网: 什么都不用做 */
        if (s_connecting) return false;                /* 同一凭据正在连: 不打断 */
    }
    if (same_target && !same_pass)
        ESP_LOGW(TAG, "同一网络但密码已修改 → 打断旧连接, 用新密码重连: %s", ssid);

    /* 换目标 **或换了密码** 都需要断一下, 否则驱动沿用旧凭据 */
    bool switching = (s_enabled && (s_connected || s_connecting || (same_target && !same_pass)));

    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", new_pass);
    /* ★★★ 2026-11-22 linit「密码错误不要保存」★★★
     *   原来这里直接 save_cfg() ⇒ 密码打错也立刻写进 NVS + 连接记录,
     *   下次开机拿错误密码自动连接, 用户还得回设置里手动改。
     *   现在只把凭据放内存里试, **GOT_IP 成功后才落盘** (见事件处理);
     *   失败由 wifi_manager_discard_pending() 丢弃, NVS 保持上一次能用的那份。 */
    s_cfg_pending = (strcmp(s_ssid, s_saved_ssid) != 0 || strcmp(s_pass, s_saved_pass) != 0);
    if (s_cfg_pending) ESP_LOGI(TAG, "新凭据仅内存试用: %s (连上才保存)", s_ssid);

    if (!s_inited) wifi_manager_init();
    if (!s_inited) return false;

    if (!s_enabled) {
        if (esp_wifi_start() != ESP_OK) return false;
        s_enabled = true;
        wifi_manager_apply_ps();         /* ★ 统一省电档, 不再切 NONE */
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

bool wifi_manager_connect(const char *ssid, const char *password)
{
    wm_lock();
    bool r = wifi_manager_connect_impl(ssid, password);
    wm_unlock();
    return r;
}
