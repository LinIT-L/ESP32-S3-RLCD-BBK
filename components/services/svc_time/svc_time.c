// svc_time — 系统时间 / 校时统一服务（2026-11-01 重做）
//
// 旧实现只有两件事: 设时区 + "时间 < 2025 就设成 2026-09-01" + 每次联网重启 SNTP。
// 它有三个硬伤，正是这次要解决的:
//   ① **伪造默认时间**: 刷机后 time() 是 1970, 被改写成 2026-09-01 ⇒ "时间未知"看起来像
//      "时间正确", 之后既没法判断该不该重新校时, 也没法解释"日期为什么不对"。
//   ② **没有可信度概念**: 不知道本地时间是 1970(刷机) 还是真走了 3 天(正常离线),
//      于是无法区分"该无条件采纳网络时间"和"网络时间可疑, 应拒绝"。
//   ③ **每次都全量重校**: 每次抖动式 GOT_IP 都重启 SNTP, 频繁联网时反复改系统时间。
//
// 现在的模型:
//   状态机  INVALID / PROVISIONAL / RTC_VALID / NETWORK_VALID / SYNCING / SYNC_FAILED
//   持久化  NVS(ns "time", key "rec") 只记"上次成功网络校准的 UTC"等少量字段, 成功才写
//   判定    启动时把 time() 与持久化记录对比 ⇒ 判断是"正常走时"还是"被重置"
//   校准    无效→立即校; 距上次 ≥6h→校; 其余跳过; 本地可信但偏差 >30min→拒绝并回滚
//
// ⚠ 关于"多源": IDF 的 SNTP 只能把多个服务器配成**候选**(谁先回用谁), 不暴露每个源的
//   时间戳/往返延迟。所以这里**不伪造**四时间戳算法, 而是:
//   ① 配 3 个源(阿里/腾讯/pool), 单点故障时自动换;
//   ② 收到时间后做**合理性校验**(下限/与本地记录的一致性), 不合理就回滚;
//   ⇒ 报告里也按这个口径写, 不宣称"多源中位数"。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "svc_time.h"
#include "time_logic.h"

static const char *TAG = "SVC_TIME";

/* ==================== 常量（改这里，别散落到别处） ==================== */
/* 阈值统一在 time_logic.h —— 那份是纯函数，主机测试会核对同样的数 */
#define TIME_MIN_VALID        TL_MIN_VALID
#define TIME_FULL_SYNC_S      TL_FULL_SYNC_S
#define TIME_OFFSET_IGNORE_S  TL_OFFSET_IGNORE
#define TIME_OFFSET_MAX_S     TL_OFFSET_MAX
#define TIME_MIN_GAP_S        10             /* 两次请求最小间隔（抖动保护） */
#define TIME_RETRY_S          300            /* 失败后 5 分钟才允许重试 */

#define TIME_NTP_SERVERS      3
static const char *const k_ntp[TIME_NTP_SERVERS] = {
    "ntp.aliyun.com",
    "time1.cloud.tencent.com",
    "pool.ntp.org",
};

#define TIME_NVS_NS   "time"
#define TIME_NVS_KEY  "rec"
#define TIME_REC_MAGIC 0x54494D45u   /* "TIME" */
#define TIME_REC_VER   1

typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t flags;      /* bit0: 曾经成功校准过 */
    int64_t  last_sync;  /* 上次成功网络校准的 UTC epoch */
    int64_t  last_boot;  /* 上次开机时的 UTC epoch（诊断用） */
    uint32_t sync_count; /* 终身成功次数 */
} time_rec_t;

/* ==================== 状态 ==================== */
static time_state_t s_st = TIME_INVALID;
static int64_t  s_last_sync   = 0;      /* 上次成功校准 epoch（NVS 恢复） */
static int64_t  s_last_boot   = 0;      /* 本次开机的本地时间（诊断） */
static uint32_t s_sync_count  = 0;      /* 本次开机成功次数 */
static uint32_t s_sync_life   = 0;      /* 终身成功次数 */
static int64_t  s_last_req_us = 0;
static int64_t  s_last_ok_us  = 0;      /* 上次成功的 esp_timer（算 6h 间隔用） */
static int64_t  s_last_fail_us = 0;
static bool     s_ntp_inited  = false;
static bool     s_syncing     = false;
static const char *s_reason   = "init";
static int64_t  s_gps_us      = 0;      /* ★ 上次 GPS 授时的 esp_timer(判时效用) */
static int64_t  s_fw_epoch    = 0;      /* ★ 固件更新时间(main 注入), 0=未知 */

void svc_time_set_fw_epoch(int64_t epoch) { s_fw_epoch = (epoch > 0) ? epoch : 0; }
int64_t svc_time_fw_epoch(void)           { return s_fw_epoch; }

/* 同步前的快照：用于"结果不合理 → 回滚" */
static int64_t  s_pre_epoch   = 0;
static int64_t  s_pre_us      = 0;

static const char *st_name(time_state_t s)
{
    switch (s) {
    case TIME_INVALID:       return "INVALID";
    case TIME_PROVISIONAL:   return "PROVISIONAL";
    case TIME_RTC_VALID:     return "RTC_VALID";
    case TIME_NETWORK_VALID: return "NETWORK_VALID";
    case TIME_SYNCING:       return "SYNCING";
    case TIME_SYNC_FAILED:   return "SYNC_FAILED";
    case TIME_GPS_VALID:     return "GPS_VALID";
    default:                 return "?";
    }
}

static void set_state(time_state_t st, const char *why)
{
    if (st != s_st) ESP_LOGI(TAG, "TIME_STATE %s → %s (%s)", st_name(s_st), st_name(st), why);
    s_st = st;
    s_reason = why;
}

/* ==================== NVS 持久化（只在成功校准/状态变化时写） ==================== */
/* 返回 false = NVS 还没就绪（开机早期）⇒ 调用方要稍后重试，
 *   绝不能当成"没有记录"，否则刷机识别会静默失效。 */
static bool rec_load(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(TIME_NVS_NS, NVS_READONLY, &h);
    if (e != ESP_OK) {
        /* ★ 2026-11-21 修: 必须分清两种失败, 原来一律报"尚未就绪"——
         *   ① ESP_ERR_NVS_NOT_FOUND = 这个命名空间还没写过(首次开机/刷机后)
         *      ⇒ 属于"就绪, 只是没有记录", 要返回 true, 否则 s_loaded 永远 false,
         *        之后每次查时间都会重跑 boot_evaluate, 反复刷 TIME_LOCAL_INVALID 警告。
         *   ② 其它(典型 ESP_ERR_NVS_NOT_INITIALIZED) ⇒ 真没就绪, 返回 false 等重试。 */
        if (e == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "TIME_PERSISTED_RECORD 无记录(首次开机) ⇒ 按无记录处理");
            return true;
        }
        /* T+2ms 调用时必然未就绪, 属**预期时序**不是异常 ⇒ INFO, 免得每次开机吓人 */
        ESP_LOGI(TAG, "NVS 尚未就绪(%s)，时间记录稍后再读", esp_err_to_name(e));
        return false;
    }
    time_rec_t r;
    size_t len = sizeof(r);
    if (nvs_get_blob(h, TIME_NVS_KEY, &r, &len) == ESP_OK && len == sizeof(r) &&
        r.magic == TIME_REC_MAGIC && r.ver == TIME_REC_VER) {
        s_last_sync  = r.last_sync;
        s_last_boot  = r.last_boot;
        s_sync_life  = r.sync_count;
        ESP_LOGI(TAG, "TIME_PERSISTED_RECORD last_sync=%lld last_boot=%lld count=%u",
                 (long long)r.last_sync, (long long)r.last_boot, (unsigned)r.sync_count);
        nvs_close(h);
        return true;
    } else {
        ESP_LOGW(TAG, "TIME_PERSISTED_RECORD 存在但无效（magic/ver/len 不符），按无记录处理");
    }
    nvs_close(h);
    return true;   /* NVS 能打开 ⇒ 就绪；记录无效按"无记录"处理 */
}

static void rec_save(void)
{
    nvs_handle_t h;
    if (nvs_open(TIME_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    time_rec_t r = {
        .magic      = TIME_REC_MAGIC,
        .ver        = TIME_REC_VER,
        .flags      = (uint16_t)(s_last_sync > 0 ? 1u : 0u),
        .last_sync  = s_last_sync,
        .last_boot  = s_last_boot,
        .sync_count = s_sync_life,
    };
    if (nvs_set_blob(h, TIME_NVS_KEY, &r, sizeof(r)) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

/* ==================== 启动判定 ==================== */
static void boot_evaluate(void)
{
    time_t t = time(NULL);
    s_last_boot = (int64_t)t;

    ESP_LOGI(TAG, "TIME_RTC_STATUS local=%lld", (long long)t);

    /* 判定规则在 time_logic.c（纯函数，主机测试覆盖 8 种组合）*/
    switch (tl_eval_boot((int64_t)t, s_last_sync)) {
    case TL_BOOT_INVALID:
        set_state(TIME_INVALID, "本地时间不可信");
        ESP_LOGW(TAG, "TIME_LOCAL_INVALID local=%lld last_sync=%lld", (long long)t, (long long)s_last_sync);
        if (s_last_sync > 0)
            ESP_LOGW(TAG, "TIME_FLASH_RESET_SUSPECTED 有校准记录但本地时间为 %lld ⇒ 刷机/断电后 RTC 归零",
                     (long long)t);
        break;
    case TL_BOOT_PROVISIONAL:
        set_state(TIME_PROVISIONAL, s_last_sync > 0 ? "比上次校准晚太久" : "无校准记录");
        break;
    case TL_BOOT_RTC_VALID:
        set_state(TIME_RTC_VALID, "与上次校准记录连贯");
        ESP_LOGI(TAG, "TIME_LOCAL_VALID 距上次校准 %lld 秒", (long long)((int64_t)t - s_last_sync));
        break;
    }

    /* ⚠ 这里**不写 NVS**：只在"成功校时"时落盘。
     *   开机写一次会让"看门狗复位循环"变成每几秒写一次 flash。*/
}

/* 惰性加载：svc_time_init() 在 app_main 很早就被调用，那时 **NVS 还没初始化**
 * （实测 svc_time_init 在 T+1ms，NVS 在 T+78ms）。所以这里允许失败后重试，
 * 首次被查询/联网时再补做一次判定 —— 否则"刷机识别"会静默退化成"永远没有记录"。 */
static bool s_loaded = false;

static void ensure_loaded(void)
{
    if (s_loaded) return;
    if (!rec_load()) return;      /* NVS 未就绪：下次再试，不改状态 */
    boot_evaluate();
    s_loaded = true;
}

void svc_time_init(void)
{
    /* ① 时区必须最早设：svc_log 等模块在 T+0ms 就会 localtime */
    setenv("TZ", "UTC-8", 1);
    tzset();

    /* ② 记录 + 可信度判定（NVS 没就绪时会推迟到首次查询）*/
    ensure_loaded();

    /* ★★★ 2026-11-21 linit 定规【固件时间默认 = 固件更新时间】★★★
     *   本地时间无效(RTC 掉电归零 / 首次刷机 / 1970) 时，以前会一直停在 INVALID，
     *   界面上就是"时间不对"。现在用构建戳把系统时间立起来：
     *     · 只是一块"能用的地基"，状态记 PROVISIONAL（不是 VALID）；
     *     · 联网/GPS 一到位照常覆盖，优先级不变；
     *     · 这样 RTC 判据("比固件还旧 ⇒ 未校准")也有参照物，不会再出现 2000-01-01。
     *   ⛔ 不改 settimeofday 规则：全系统仍只有本文件能写时间。 */
    if ((int64_t)time(NULL) < TIME_MIN_VALID) {
        int64_t fw = s_fw_epoch;          /* main 已用 os_build_epoch() 注入 */
        if (fw > 0) {
            struct timeval tv = { .tv_sec = (time_t)fw, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            set_state(TIME_PROVISIONAL, "固件更新时间为默认值");
            ESP_LOGW(TAG, "TIME_FW_DEFAULT 本地无效 ⇒ 采用固件更新时间 %lld", (long long)fw);
        } else {
            ESP_LOGW(TAG, "TIME_FW_DEFAULT 固件时间未知(未注入) ⇒ 保持 INVALID");
        }
    }

    if (s_loaded) {
        ESP_LOGI(TAG, "TIME_BOOT_CHECK state=%s reason=%s local=%lld last_sync=%lld",
                 st_name(s_st), s_reason, (long long)time(NULL), (long long)s_last_sync);
    } else {
        ESP_LOGI(TAG, "TIME_BOOT_CHECK 推迟（NVS 未就绪），首次查询时再判定");
    }
}

/* ==================== 校准 ==================== */
static void ntp_sync_cb(struct timeval *tv);

static bool ntp_start(void)
{
    if (!s_ntp_inited) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
            TIME_NTP_SERVERS,
            ESP_SNTP_SERVER_LIST(k_ntp[0], k_ntp[1], k_ntp[2]));
        cfg.sync_cb       = ntp_sync_cb;
        cfg.wait_for_sync = false;
        cfg.start         = false;   /* 由本服务显式 start */
        if (esp_netif_sntp_init(&cfg) != ESP_OK) {
            ESP_LOGW(TAG, "TIME_SYNC_FAILED SNTP 初始化失败，下次再试");
            return false;
        }
        s_ntp_inited = true;
        ESP_LOGI(TAG, "NTP 已初始化 (%d 个源: %s / %s / %s)",
                 TIME_NTP_SERVERS, k_ntp[0], k_ntp[1], k_ntp[2]);
    }
    return esp_netif_sntp_start() == ESP_OK;
}

static void ntp_sync_cb(struct timeval *tv)
{
    s_syncing = false;
    if (!tv || tv->tv_sec <= 0) {
        set_state(TIME_SYNC_FAILED, "回调无有效时间");
        s_last_fail_us = esp_timer_get_time();
        return;
    }

    if (svc_time_gps_fresh()) {
        ESP_LOGI(TAG, "TIME_GPS_PRIORITY 忽略网络时间 %lld (GPS 授时有效)", (long long)tv->tv_sec);
        set_state(TIME_GPS_VALID, "GPS 优先，忽略网络校时");
        return;
    }

    int64_t got       = (int64_t)tv->tv_sec;
    int64_t elapsed   = (esp_timer_get_time() - s_pre_us) / 1000000;
    int64_t projected = s_pre_epoch + elapsed;      /* 本地时间"本该"是几点 */
    bool local_invalid = (s_pre_epoch < TIME_MIN_VALID);

    ESP_LOGI(TAG, "TIME_NETWORK_REFERENCE got=%lld projected=%lld offset=%llds (delay≈%llds)",
             (long long)got, (long long)projected, (long long)(got - projected), (long long)elapsed);

    /* 判定规则在 time_logic.c（纯函数，主机测试覆盖 4 种结果）*/
    switch (tl_judge_net(got, projected, local_invalid)) {
    case TL_NET_REJECT_LOW:
        ESP_LOGW(TAG, "TIME_SERVER_RESULT 拒绝: %lld 低于 2025-01-01", (long long)got);
        if (!local_invalid) {
            struct timeval back = { .tv_sec = (time_t)projected, .tv_usec = 0 };
            settimeofday(&back, NULL);              /* 回滚到校准前 */
            ESP_LOGW(TAG, "TIME_SYNC_FAILED 已回滚");
        }
        set_state(TIME_SYNC_FAILED, "服务器时间低于下限");
        s_last_fail_us = esp_timer_get_time();
        return;

    case TL_NET_REJECT_FAR: {
        struct timeval back = { .tv_sec = (time_t)projected, .tv_usec = 0 };
        settimeofday(&back, NULL);
        ESP_LOGW(TAG, "TIME_SERVER_DISAGREEMENT 偏差 %llds > %ds，拒绝并回滚",
                 (long long)(got - projected), TIME_OFFSET_MAX_S);
        set_state(TIME_SYNC_FAILED, "网络时间与本机相差过大");
        s_last_fail_us = esp_timer_get_time();
        return;
    }

    case TL_NET_ACCEPT_IMMEDIATE:
        ESP_LOGW(TAG, "TIME_SYNC_IMMEDIATE 本地无效(%lld) → 采纳网络时间 %lld",
                 (long long)s_pre_epoch, (long long)got);
        break;

    case TL_NET_ACCEPT:
    default: {
        int64_t off = got - projected;
        if (off < 0) off = -off;
        if (off <= TIME_OFFSET_IGNORE_S)
            ESP_LOGI(TAG, "TIME_SYNC_SMOOTH 偏差 %llds，可忽略", (long long)(got - projected));
        else
            ESP_LOGI(TAG, "TIME_OFFSET_SECONDS %llds 已校正", (long long)(got - projected));
        break;
    }
    }

    /* 采纳 */
    s_last_sync  = got;
    s_last_ok_us = esp_timer_get_time();
    s_sync_count++;
    s_sync_life++;
    rec_save();
    set_state(TIME_NETWORK_VALID, local_invalid ? "本地无效，已用网络时间恢复" : "网络校准成功");
    ESP_LOGI(TAG, "TIME_SYNC_SUCCESS #%u epoch=%lld", (unsigned)s_sync_count, (long long)got);
}

void svc_time_ntp_sync(void)
{
    ensure_loaded();
    int64_t now_us = esp_timer_get_time();

    /* ★ 2026-11-21 优先级: GPS > 网络。GPS 授时还在时效内 ⇒ 根本不发起 SNTP,
     *   既不浪费流量/功耗, 也避免把 GPS 的绝对时间换成网络时间。 */
    if (svc_time_gps_fresh()) {
        ESP_LOGI(TAG, "TIME_SYNC_SKIPPED GPS 授时有效 ⇒ 不发起网络校时");
        return;
    }

    /* 正在校准时重复触发（重复 GOT_IP）→ 直接忽略，绝不并发两套 */
    if (s_syncing) {
        ESP_LOGI(TAG, "TIME_SYNC_CHECK 已在校准中 → 忽略重复请求");
        return;
    }
    /* 抖动保护：10 秒内重复 */
    if (s_last_req_us && (now_us - s_last_req_us) < (int64_t)TIME_MIN_GAP_S * 1000000) {
        ESP_LOGI(TAG, "TIME_SYNC_SKIPPED_COOLDOWN 距上次请求不足 %ds", TIME_MIN_GAP_S);
        return;
    }
    /* 失败后冷却 */
    if (s_last_fail_us && (now_us - s_last_fail_us) < (int64_t)TIME_RETRY_S * 1000000) {
        ESP_LOGI(TAG, "TIME_SYNC_SKIPPED_COOLDOWN 上次失败不足 %ds", TIME_RETRY_S);
        return;
    }

    /* 要不要做**完整**校准 —— 规则在 time_logic.c（纯函数，主机测试覆盖）*/
    int64_t since_ok  = (s_last_ok_us > 0)
                        ? (now_us - s_last_ok_us) / 1000000 : -1;
    int64_t since_rec = (s_last_sync > 0)
                        ? ((int64_t)time(NULL) - s_last_sync) : -1;

    if (!tl_need_full_sync(s_st == TIME_INVALID, s_st == TIME_SYNC_FAILED, since_ok, since_rec)) {
        ESP_LOGI(TAG, "TIME_SYNC_SKIPPED_COOLDOWN state=%s 距上次成功=%llds (阈值 %ds)",
                 st_name(s_st), (long long)since_ok, (int)TIME_FULL_SYNC_S);
        if (since_ok >= 0)
            ESP_LOGI(TAG, "TIME_NEXT_SYNC_SCHEDULED 剩余 %lld 秒",
                     (long long)(TIME_FULL_SYNC_S - since_ok));
        return;
    }

    /* 快照 + 立刻切状态：期间任何再次触发都会被上面的 s_syncing 挡住 */
    s_pre_epoch = (int64_t)time(NULL);
    s_pre_us    = now_us;
    s_last_req_us = now_us;
    s_syncing   = true;
    ESP_LOGI(TAG, "TIME_SYNC_CHECK 需要校准 (state=%s local=%lld since_ok=%lld since_rec=%lld)",
             st_name(s_st), (long long)s_pre_epoch, (long long)since_ok, (long long)since_rec);
    set_state(TIME_SYNCING, "网络到达，开始校准");
    ESP_LOGI(TAG, "TIME_SYNC_START servers=%d", TIME_NTP_SERVERS);

    if (!ntp_start()) {
        s_syncing = false;
        s_last_fail_us = esp_timer_get_time();
        set_state(TIME_SYNC_FAILED, "SNTP 启动失败");
        ESP_LOGW(TAG, "TIME_SYNC_FAILED 启动失败");
    }
}

/* ==================== GPS 授时（最高优先级） ====================
 * ★ 2026-11-21 linit 定规: GPS > 网络 > RTC > 本地。
 *   本文件不碰 UART、不主动开 GPS —— 只由 main 主循环送"快照字段"进来，
 *   所以"没开 GPS 应用"时这里一次都不会被调用，天然满足
 *   "GPS 有开才取，其它情况只读 RTC"。
 *   时效: 每次授时刷新 s_gps_us；超过 TIME_GPS_FRESH_S 秒后
 *        网络/RTC 才重新有资格接管（GPS 关掉后不会长期"霸占"时间源）。 */

/* 公历年月日 → 1970-01-01 起的天数（与 svc_rtc 同一算法，纯算术、与 TZ 无关） */
static int64_t days_from_civil_gps(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

bool svc_time_gps_fresh(void)
{
    if (s_gps_us <= 0) return false;
    return (esp_timer_get_time() - s_gps_us) < (int64_t)TIME_GPS_FRESH_S * 1000000;
}

bool svc_time_set_from_gps(int64_t epoch)
{
    ensure_loaded();
    if (epoch < TIME_MIN_VALID) return false;

    int64_t now  = (int64_t)time(NULL);
    int64_t diff = now - epoch;
    if (diff < 0) diff = -diff;

    if (now >= TIME_MIN_VALID && diff <= 1) {
        /* 已一致: 只刷时效戳，避免每秒 settimeofday 抖动 */
        s_gps_us = esp_timer_get_time();
        if (s_st != TIME_GPS_VALID) set_state(TIME_GPS_VALID, "GPS 授时(与本地一致)");
        return true;
    }

    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_gps_us     = esp_timer_get_time();
    s_last_boot  = epoch;
    s_last_sync  = epoch;        /* GPS 也是可信校准点 ⇒ 供下次开机"时间只往前走"判定 */
    rec_save();
    set_state(TIME_GPS_VALID, "GPS 授时");
    ESP_LOGW(TAG, "TIME_FROM_GPS epoch=%lld (原 %lld, 差 %llds)",
             (long long)epoch, (long long)now, (long long)(epoch - now));
    return true;
}

bool svc_time_set_from_gps_fields(int year, int mon, int day, int hour, int min, int sec)
{
    if (year < 2024 || year > 2199) return false;
    if (mon < 1 || mon > 12) return false;
    if (day < 1 || day > 31) return false;
    if (hour < 0 || hour > 23) return false;
    if (min < 0 || min > 59) return false;
    if (sec < 0 || sec > 60) return false;      /* 60 = 闰秒 */

    int64_t ts = days_from_civil_gps(year, (unsigned)mon, (unsigned)day) * 86400
               + (int64_t)hour * 3600 + (int64_t)min * 60 + sec;
    return svc_time_set_from_gps(ts);
}

/* ==================== 外挂 RTC ====================
 * ★ 2026-11-20: DS3231MZ+（I2C 0x68）由 svc_rtc 读写，读完调这里注入系统时间。
 *   settimeofday 仍然只出现在本文件（头文件 ⛔ 约定）。
 *   三道保护：① 低于下限（2025-01-01）一律不收；
 *            ② 与当前系统时间相差 ≤60s 视为"已经同步"，不来回抖时间；
 *            ③ ★ 2026-11-24 倒退保护：系统时间已可信(NETWORK/GPS/RTC_VALID) 时，
 *              芯片比系统早 >600s ⇒ 判芯片掉电/旧值/读错，拒绝采纳，
 *              避免旧 RTC 把已经校准过的时间拉回去。 */
#define TIME_RTC_DEADBAND_S   60
#define TIME_RTC_BACKWARD_S  600

void svc_time_set_from_rtc(int64_t epoch)
{
    ensure_loaded();
    /* ★ 2026-11-21 优先级: GPS > 网络 > RTC。GPS 有效期内 RTC 只作比对, 不写系统时间。 */
    if (svc_time_gps_fresh()) return;
    if (epoch < TIME_MIN_VALID) {
        ESP_LOGW(TAG, "TIME_FROM_RTC 拒绝: %lld 低于下限", (long long)epoch);
        return;
    }
    int64_t now  = (int64_t)time(NULL);
    int64_t diff = now - epoch;
    if (diff < 0) diff = -diff;

    if (now >= TIME_MIN_VALID && diff <= TIME_RTC_DEADBAND_S) {
        if (s_st != TIME_NETWORK_VALID) set_state(TIME_RTC_VALID, "外挂 RTC 与本地一致");
        return;
    }

    /* ③ 倒退保护: 只在"系统时间已经有可信来源"时生效。
     *   INVALID / PROVISIONAL(固件时间兜底) 必须让 RTC 覆盖 —— 那是恢复, 不是倒退。 */
    if (now >= TIME_MIN_VALID && epoch < now - TIME_RTC_BACKWARD_S &&
        (s_st == TIME_RTC_VALID || s_st == TIME_NETWORK_VALID || s_st == TIME_GPS_VALID)) {
        ESP_LOGW(TAG, "TIME_FROM_RTC 拒绝倒退: 芯片 %lld 比系统 %lld 早 %llds (>%ds, 状态 %s)",
                 (long long)epoch, (long long)now, (long long)(now - epoch),
                 TIME_RTC_BACKWARD_S, st_name(s_st));
        return;
    }

    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_last_boot = epoch;
    set_state(TIME_RTC_VALID,
              (now >= TIME_MIN_VALID) ? "外挂 RTC 修正本地时间" : "外挂 RTC 注入");
    ESP_LOGW(TAG, "TIME_FROM_RTC epoch=%lld (原 %lld, 差 %llds)",
             (long long)epoch, (long long)now, (long long)(epoch - now));
}

/* ★ 2026-11-24 手动设时: 收口设置页原来的直接 settimeofday。
 *   为什么必须走这里: 直接写时系统时间变了但 s_st / s_last_sync / NVS 还是旧的,
 *   于是 ① 下次开机可能被判定"时间倒退"而落到 INVALID;
 *        ② 周期校准会拿 RTC 旧值把用户刚设的时间覆盖掉。
 *   这里一次做完: 设时间 + 记基准 + 落盘 + 置 RTC_VALID(离线可信, 非网络校准)。
 *   ⚠ 调用方随后应调 svc_rtc_save_now() 把时间落到 DS3231, 否则断电就白设。 */
bool svc_time_set_manual(int64_t epoch)
{
    ensure_loaded();
    if (epoch < TIME_MIN_VALID) {
        ESP_LOGW(TAG, "TIME_MANUAL 拒绝: %lld 低于下限", (long long)epoch);
        return false;
    }
    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_last_boot = epoch;
    s_last_sync = epoch;          /* 用户明确设定 ⇒ 作为"时间只往前走"的新基准 */
    rec_save();
    set_state(TIME_RTC_VALID, "用户手动设定");
    ESP_LOGW(TAG, "TIME_MANUAL_SET epoch=%lld", (long long)epoch);
    return true;
}

/* ==================== 查询 ==================== */
void svc_time_ntp_status(uint32_t *count, long *last_epoch)
{
    if (count)      *count      = s_sync_count;
    if (last_epoch) *last_epoch = (long)s_last_sync;
}

time_state_t svc_time_state(void) { ensure_loaded(); return s_st; }
const char  *svc_time_state_name(void) { return st_name(s_st); }

bool svc_time_is_valid(void)
{
    ensure_loaded();
    return s_st == TIME_NETWORK_VALID || s_st == TIME_RTC_VALID ||
           s_st == TIME_GPS_VALID;
}

const char *svc_time_diag(char *buf, int n)
{
    ensure_loaded();
    if (!buf || n <= 0) return "";
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    int64_t since = (s_last_sync > 0) ? ((int64_t)t - s_last_sync) : -1;
    snprintf(buf, (size_t)n,
             "state=%s local=%lld(%04d-%02d-%02d %02d:%02d:%02dZ) last_sync=%lld 间隔=%llds "
             "本次成功=%u 终身=%u reason=%s",
             st_name(s_st), (long long)t,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
             (long long)s_last_sync, (long long)since,
             (unsigned)s_sync_count, (unsigned)s_sync_life, s_reason);
    return buf;
}
