/* ============================================================================
 * espnow_peer.c — 设备发现 / 配对表 / 设备列表 (约束与安全边界见 include/espnow_peer.h)
 *
 * 上下文纪律 (最重要的两条):
 *   ① espnow_peer_on_frame() 在 **WiFi 任务**里跑: 只动内存 + 直接投递小帧,
 *      不写 NVS、不 malloc、不打印多于一条(限流)、不等待超过 10ms 的锁。
 *   ② NVS 落盘只在 espnow_peer_persist() 里做, 由 Lua 侧(非回调)触发。
 * ==========================================================================*/
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "espnow_peer.h"
#include "espnow_proto.h"
#include "svc_nvs.h"

static const char *TAG = "ESPNOW_PEER";

#define NVS_NS           "espnow"
#define NVS_KEY_PEERS    "peers"
#define PEERS_BLOB_VER   1

/* ---- NVS 里存的结构 (只存已配对项, 401 B) ---- */
typedef struct {
    uint8_t mac[ESPNOW_MAC_LEN];
    uint8_t enc;
    uint8_t name_len;
    char    name[ESPNOW_NAME_LEN];
    uint8_t lmk[ESPNOW_LMK_LEN];
} persist_item_t;

typedef struct {
    uint8_t        ver;
    uint8_t        count;
    persist_item_t items[ESPNOW_PEER_MAX];
} peers_blob_t;

static espnow_dev_t     s_dev[ESPNOW_PEER_MAX];
static char             s_local[ESPNOW_NAME_LEN] = "LinTOS";
static uint8_t          s_local_len = 6;
static uint32_t         s_disc_until_ms = 0;
static bool             s_dirty = false;
static bool             s_loaded = false;   /* 配对表是否已从 NVS 载入 */
static SemaphoreHandle_t s_lock = NULL;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void lock_take(void) { if (s_lock) xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)); }
static void lock_give(void) { if (s_lock) xSemaphoreGive(s_lock); }

/* ==================== 设备表 ==================== */
static espnow_dev_t *find_dev(const uint8_t mac[ESPNOW_MAC_LEN])
{
    for (int i = 0; i < ESPNOW_PEER_MAX; i++)
        if (s_dev[i].used && memcmp(s_dev[i].mac, mac, ESPNOW_MAC_LEN) == 0) return &s_dev[i];
    return NULL;
}

/* 找一个槽位: 空位 → 最久未见的非配对 → 最久未见的(含配对, 最后手段) */
static espnow_dev_t *alloc_dev(void)
{
    for (int i = 0; i < ESPNOW_PEER_MAX; i++)
        if (!s_dev[i].used) return &s_dev[i];

    espnow_dev_t *oldest_unpaired = NULL, *oldest = NULL;
    for (int i = 0; i < ESPNOW_PEER_MAX; i++) {
        if (!oldest || s_dev[i].last_seen_ms < oldest->last_seen_ms) oldest = &s_dev[i];
        if (!s_dev[i].paired &&
            (!oldest_unpaired || s_dev[i].last_seen_ms < oldest_unpaired->last_seen_ms))
            oldest_unpaired = &s_dev[i];
    }
    return oldest_unpaired ? oldest_unpaired : oldest;
}

static int encrypted_count(void)
{
    int n = 0;
    for (int i = 0; i < ESPNOW_PEER_MAX; i++)
        if (s_dev[i].used && s_dev[i].paired && s_dev[i].encrypted) n++;
    return n;
}

/* 记录/刷新一个设备。name 可为 NULL (只更新时间/RSSI)。调用方持锁。 */
static espnow_dev_t *dev_touch(const uint8_t mac[ESPNOW_MAC_LEN], int8_t rssi,
                               const char *name, uint8_t name_len)
{
    espnow_dev_t *d = find_dev(mac);
    if (!d) {
        d = alloc_dev();
        if (!d) return NULL;
        memset(d, 0, sizeof(*d));
        memcpy(d->mac, mac, ESPNOW_MAC_LEN);
        d->used = true;
    }
    d->rssi = rssi;
    d->last_seen_ms = now_ms() ? now_ms() : 1u;
    if (name && name_len > 0) {
        if (name_len > ESPNOW_NAME_LEN) name_len = ESPNOW_NAME_LEN;
        memcpy(d->name, name, name_len);
        d->name_len = name_len;
        d->name[ESPNOW_NAME_LEN - 1] = '\0';
    }
    return d;
}

/* ==================== NVS 持久化 ==================== */
void espnow_peer_persist(void)
{
    if (!s_dirty) return;
    s_dirty = false;

    peers_blob_t *b = calloc(1, sizeof(peers_blob_t));
    if (!b) { s_dirty = true; return; }
    b->ver = PEERS_BLOB_VER;

    lock_take();
    for (int i = 0; i < ESPNOW_PEER_MAX && b->count < ESPNOW_PEER_MAX; i++) {
        if (!s_dev[i].used || !s_dev[i].paired) continue;
        persist_item_t *it = &b->items[b->count++];
        memcpy(it->mac, s_dev[i].mac, ESPNOW_MAC_LEN);
        it->enc      = s_dev[i].encrypted ? 1 : 0;
        it->name_len = s_dev[i].name_len;
        memcpy(it->name, s_dev[i].name, ESPNOW_NAME_LEN);
    }
    lock_give();

    nvs_handle_t h;
    if (svc_nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        svc_nvs_set_blob(h, NVS_KEY_PEERS, b, sizeof(*b));
        svc_nvs_commit(h);
        svc_nvs_close(h);
        ESP_LOGI(TAG, "配对表已保存 (%u 台)", (unsigned)b->count);
    } else {
        ESP_LOGW(TAG, "配对表保存失败: 打不开 %s", NVS_NS);
        s_dirty = true;
    }
    free(b);
}

/* 从 NVS 载入: 恢复"已配对"标记 + 把 peer 重新挂回驱动 (否则重启后发不出去)。
 * LMK 也一并存 —— 明文存 NVS, 与 ESP-NOW 自身的 key 存储等级一致, 不放 secret 区。 */
static void persist_load(void)
{
    peers_blob_t *b = calloc(1, sizeof(peers_blob_t));
    if (!b) return;
    size_t len = sizeof(*b);
    nvs_handle_t h;
    if (svc_nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) { free(b); return; }
    esp_err_t e = svc_nvs_get_blob(h, NVS_KEY_PEERS, b, &len);
    svc_nvs_close(h);
    if (e != ESP_OK || len != sizeof(*b) || b->ver != PEERS_BLOB_VER ||
        b->count > ESPNOW_PEER_MAX) {
        free(b);
        return;
    }

    lock_take();
    for (int i = 0; i < b->count; i++) {
        espnow_dev_t *d = find_dev(b->items[i].mac);
        if (!d) { d = alloc_dev(); if (!d) continue; memset(d, 0, sizeof(*d)); d->used = true;
                  memcpy(d->mac, b->items[i].mac, ESPNOW_MAC_LEN); }
        d->paired    = true;
        d->encrypted = b->items[i].enc != 0;
        d->name_len  = b->items[i].name_len > ESPNOW_NAME_LEN ? ESPNOW_NAME_LEN
                                                             : b->items[i].name_len;
        memcpy(d->name, b->items[i].name, ESPNOW_NAME_LEN);
        d->name[ESPNOW_NAME_LEN - 1] = '\0';
    }
    lock_give();

    /* 驱动侧 peer 要重建: 只在 esp-now 已启动时做 (没启动就等 start 后再挂) */
    if (espnow_link_is_running()) {
        for (int i = 0; i < b->count; i++)
            espnow_link_add_peer(b->items[i].mac, b->items[i].enc ? b->items[i].lmk : NULL);
    }
    ESP_LOGI(TAG, "配对表已载入 (%u 台)", (unsigned)b->count);
    free(b);
}

void espnow_peer_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    /* ★ 用 s_loaded 而不是"只初始化一次": espnow_link_stop() → start() 会先把
     *   设备表清空, 若这里直接 return, 配对表就再也载不回来了(掉电保存形同虚设)。 */
    if (s_loaded) return;
    s_loaded = true;
    persist_load();
}

void espnow_peer_reset(void)
{
    /* ⚠ 只清运行时表, **不写 NVS**: 本函数会被 WiFi 事件回调调用(见 espnow_link.c),
     *   那里做 flash 操作风险高(cache 关闭期 PSRAM 不可访问)。落盘由
     *   espnow_link_stop()/espnow_peer_persist() 在正常任务上下文完成。 */
    lock_take();
    memset(s_dev, 0, sizeof(s_dev));
    lock_give();
    s_disc_until_ms = 0;
    /* 丢弃"未落盘"的变更: 宁可丢这一次配对, 也不能在表被清空后把 NVS 里的
     * 已有配对表覆盖成空表。 */
    s_dirty = false;
    s_loaded = false;     /* 下次 start 时重新从 NVS 载入 */
}

/* ==================== 本机名 ==================== */
void espnow_peer_set_local_name(const char *name)
{
    if (!name || !*name) return;
    size_t n = strlen(name);
    if (n >= ESPNOW_NAME_LEN) n = ESPNOW_NAME_LEN - 1;
    lock_take();
    memcpy(s_local, name, n);
    s_local[n] = '\0';
    s_local_len = (uint8_t)n;
    lock_give();
}

const char *espnow_peer_local_name(void) { return s_local; }

/* ==================== 发现 ==================== */
static void send_frame(const uint8_t mac[ESPNOW_MAC_LEN], uint8_t type,
                       const void *pl, int len)
{
    /* ⚠ 本函数会在 **WiFi 收包回调**里被调用(回 ANNOUNCE / BIND_RSP), 不能用
     *   250B 的整包缓冲去占 WiFi 任务栈。本层最长控制帧 = ANNOUNCE(1+最多15 字节
     *   名字) ⇒ 缓冲 12+1+16+4=33B 足够; 万一将来加长帧, 下面的长度检查会拦住。 */
    uint8_t buf[ESPNOW_HDR_LEN + 1 + ESPNOW_NAME_LEN + 4];
    if (len < 0 || len > (int)sizeof(buf) - ESPNOW_HDR_LEN) return;
    int n = espnow_proto_pack(buf, sizeof(buf), type,
                              espnow_proto_next_seq(type), pl, len);
    if (n > 0) espnow_link_send_frame(mac, buf, n);
}

/* 单播回 ANNOUNCE。★ 机制来自 IDF 官方示例: 收到广播后若 peer 不存在, 先建
 * 一个非加密 peer 才能单播回去 —— 这是"无需预先知道 MAC 就能互相发现"的关键。 */
static void announce_to(const uint8_t mac[ESPNOW_MAC_LEN])
{
    if (!esp_now_is_peer_exist(mac)) {
        if (espnow_link_add_peer(mac, NULL) != ESP_OK) return;
    }
    uint8_t pl[1 + ESPNOW_NAME_LEN];
    uint8_t nl;
    lock_take();
    nl = s_local_len;
    memcpy(pl + 1, s_local, nl);
    lock_give();
    pl[0] = nl;
    send_frame(mac, ESPNOW_FT_ANNOUNCE, pl, 1 + nl);
}

esp_err_t espnow_peer_discover(uint32_t window_ms)
{
    if (!espnow_link_is_running()) return ESP_ERR_INVALID_STATE;
    if (window_ms < 200)   window_ms = 200;
    if (window_ms > 20000) window_ms = 20000;

    s_disc_until_ms = now_ms() + window_ms;

    uint8_t flags = 0;
    esp_err_t e = espnow_link_broadcast_frame(ESPNOW_FT_DISCOVER, &flags, 1);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "发现广播失败: %s", esp_err_to_name(e));
        s_disc_until_ms = 0;
        return e;
    }
    ESP_LOGI(TAG, "开始发现 (%ums 窗口)", (unsigned)window_ms);
    return ESP_OK;
}

bool espnow_peer_discover_active(void)
{
    return s_disc_until_ms != 0 && now_ms() < s_disc_until_ms;
}

int espnow_peer_dev_count(void)
{
    int n = 0;
    lock_take();
    for (int i = 0; i < ESPNOW_PEER_MAX; i++) if (s_dev[i].used) n++;
    lock_give();
    return n;
}

const espnow_dev_t *espnow_peer_dev_get(int idx)
{
    /* 按"最近见到"排序对外? 不重排表, 调用方自己按 last_seen 排 (表很小) */
    int seen = 0;
    lock_take();
    for (int i = 0; i < ESPNOW_PEER_MAX; i++) {
        if (!s_dev[i].used) continue;
        if (seen++ == idx) { lock_give(); return &s_dev[i]; }
    }
    lock_give();
    return NULL;
}

int espnow_peer_paired_count(void)
{
    int n = 0;
    lock_take();
    for (int i = 0; i < ESPNOW_PEER_MAX; i++) if (s_dev[i].used && s_dev[i].paired) n++;
    lock_give();
    return n;
}

bool espnow_peer_is_paired(const uint8_t mac[ESPNOW_MAC_LEN])
{
    if (!mac) return false;
    lock_take();
    const espnow_dev_t *d = find_dev(mac);
    const bool paired = (d && d->paired);
    lock_give();
    return paired;
}

/* ==================== 配对 ==================== */
esp_err_t espnow_peer_pair(const uint8_t mac[ESPNOW_MAC_LEN], const uint8_t *key, int key_len)
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    if (!espnow_link_is_running()) return ESP_ERR_INVALID_STATE;
    if (key && key_len != ESPNOW_LMK_LEN) return ESP_ERR_INVALID_ARG;
    const bool enc = (key != NULL);

    if (enc) {
        /* 已经是加密 peer 就不占新额度 */
        lock_take();
        const espnow_dev_t *d = find_dev(mac);
        const bool already = (d && d->paired && d->encrypted);
        const bool full = (encrypted_count() >= ESPNOW_ENCRYPT_MAX);
        lock_give();
        if (!already && full) {
            ESP_LOGW(TAG, "加密 peer 已达 %d 上限 (驱动硬限制)", ESPNOW_ENCRYPT_MAX);
            return ESP_ERR_ESPNOW_NO_MEM;
        }
    }

    esp_err_t e = espnow_link_add_peer(mac, enc ? key : NULL);
    if (e != ESP_OK) return e;

    uint8_t pl[1 + ESPNOW_LMK_LEN];
    int n = 0;
    pl[n++] = enc ? ESPNOW_BIND_F_ENC : 0;
    if (enc) { memcpy(pl + n, key, ESPNOW_LMK_LEN); n += ESPNOW_LMK_LEN; }
    send_frame(mac, ESPNOW_FT_BIND_REQ, pl, n);

    lock_take();
    espnow_dev_t *d = dev_touch(mac, 0, NULL, 0);
    if (d) { d->paired = true; d->encrypted = enc; }
    lock_give();

    s_dirty = true;
    ESP_LOGI(TAG, "已配对 %02X:%02X:%02X:%02X:%02X:%02X (%s)",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], enc ? "加密" : "明文");
    return ESP_OK;
}

esp_err_t espnow_peer_unpair(const uint8_t mac[ESPNOW_MAC_LEN])
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    if (!espnow_link_is_running()) return ESP_ERR_INVALID_STATE;

    uint8_t flags = ESPNOW_BIND_F_UNBIND;
    send_frame(mac, ESPNOW_FT_BIND_REQ, &flags, 1);
    espnow_link_del_peer(mac);           /* 本机立刻断 */

    lock_take();
    espnow_dev_t *d = find_dev(mac);
    if (d) { d->paired = false; d->encrypted = false; }
    lock_give();

    s_dirty = true;
    ESP_LOGI(TAG, "已解除配对 %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

/* ==================== 收包回调入口 (WiFi 任务) ==================== */
void espnow_peer_on_frame(uint8_t type, uint16_t seq, const uint8_t *payload, int len,
                          const uint8_t src[ESPNOW_MAC_LEN], int8_t rssi)
{
    (void)seq;
    switch (type) {
    case ESPNOW_FT_DISCOVER:
        lock_take(); dev_touch(src, rssi, NULL, 0); lock_give();
        announce_to(src);
        break;

    case ESPNOW_FT_ANNOUNCE: {
        if (len < 2) return;
        uint8_t nl = payload[0];
        if ((int)nl + 1 > len || nl > ESPNOW_NAME_LEN) return;
        lock_take();
        dev_touch(src, rssi, (const char *)(payload + 1), nl);
        lock_give();
        break;
    }

    case ESPNOW_FT_BIND_REQ: {
        if (len < 1) return;
        const bool enc    = (payload[0] & ESPNOW_BIND_F_ENC) != 0;
        const bool unbind = (payload[0] & ESPNOW_BIND_F_UNBIND) != 0;
        uint8_t rsp = 0;                       /* 0 = 成功, 非 0 = 拒绝码 */

        /* ⚠ 结构说明: 每种失败都只给 rsp 赋值, 最后统一发 BIND_RSP ——
         *   对端在等应答, 任何分支都不能"静默退出"(否则对端以为配上了)。 */
        if (unbind) {
            espnow_link_del_peer(src);
            lock_take();
            espnow_dev_t *d = find_dev(src);
            if (d) { d->paired = false; d->encrypted = false; }
            lock_give();
        } else if (enc) {
            if (len < 1 + ESPNOW_LMK_LEN)                       rsp = 1;  /* 帧不完整 */
            else if (encrypted_count() >= ESPNOW_ENCRYPT_MAX)    rsp = 2;  /* 本机额度满 */
            else if (espnow_link_add_peer(src, payload + 1) != ESP_OK) rsp = 3;
            else {
                lock_take();
                espnow_dev_t *d = dev_touch(src, rssi, NULL, 0);
                if (d) { d->paired = true; d->encrypted = true; }
                lock_give();
            }
        } else {
            if (espnow_link_add_peer(src, NULL) != ESP_OK) rsp = 3;
            else {
                lock_take();
                espnow_dev_t *d = dev_touch(src, rssi, NULL, 0);
                if (d) { d->paired = true; d->encrypted = false; }
                lock_give();
            }
        }
        send_frame(src, ESPNOW_FT_BIND_RSP, &rsp, 1);
        if (rsp == 0) s_dirty = true;          /* 落盘交给非回调上下文 */
        break;
    }

    case ESPNOW_FT_BIND_RSP:
        lock_take(); dev_touch(src, rssi, NULL, 0); lock_give();
        break;

    default:
        break;
    }
}
