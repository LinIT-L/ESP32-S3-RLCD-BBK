/* hw_cfg.c — 一块固件通吃三块板: 开机探测芯片指纹 ⇒ 自动定板型 + 自动启用硬件
 *
 * ★ 2026-10-18 定案: **每次开机都探测**（原来"首次探测后锁进 NVS 永不再探"，
 *   换板后固件仍按旧板型跑 ⇒ 触摸失灵 + 按键错位，等于没通吃）。
 *   探测命中 ⇒ 定板型并落盘；全不中 ⇒ 用上次板型兜底；再无 ⇒ 默认小智（不会变砖）。
 *
 * ⛔ 探测必须是**最早**的外设动作（在建屏/触摸/音频/SD 之前），且每组脚探完立刻释放总线。
 */
#include "hw_cfg.h"
#include "board_pins.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

#define TAG "HW_CFG"
#define NS  "os_hw"

static int     s_board = HW_BOARD_XIAOZHI;
static uint8_t s_det   = 0;
static uint8_t s_en    = 0;

/* ---------- NVS ---------- */
static uint8_t cfg_get_u8(const char *k, uint8_t def) {
    nvs_handle_t h; uint8_t v = def;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, k, &v); nvs_close(h); }
    return v;
}
static void cfg_set_u8(const char *k, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, k, v); nvs_commit(h); nvs_close(h); }
}

/* ---------- 单次 I2C 探测: 该总线上 addr 是否有应答 ---------- */
static bool probe_addr(int sda, int scl, uint8_t addr) {
    if (sda < 0 || scl < 0) return false;
    i2c_master_bus_config_t bc = {
        .i2c_port = -1, .sda_io_num = sda, .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return false;
    esp_err_t r = i2c_master_probe(bus, addr, 50);   /* 50ms 超时 */
    i2c_del_master_bus(bus);                          /* ★立刻释放, 防占脚 */
    return (r == ESP_OK);
}
/* ES8311 的地址由 CE 脚决定 ⇒ 0x18 / 0x19 都要试 */
static bool probe_es8311(int sda, int scl) {
    return probe_addr(sda, scl, 0x18) || probe_addr(sda, scl, 0x19);
}

/* ---------- 探测顺序: 命中即止（小智只探一次, 微雪两次, 标准扩展三次） ---------- */
static int detect_board(void) {
    if (probe_es8311(38, 47)) { ESP_LOGW(TAG, "指纹命中: 38/47 有 ES8311 ⇒ 小智一体版"); return HW_BOARD_XIAOZHI; }
    if (probe_es8311(13, 14)) { ESP_LOGW(TAG, "指纹命中: 13/14 有 ES8311 ⇒ 微雪原版");   return HW_BOARD_WEIXUE;  }
    if (probe_addr(15, 7, 0x15)) { ESP_LOGW(TAG, "指纹命中: 15/7 有 CST816 ⇒ 标准扩展版"); return HW_BOARD_STD; }
    ESP_LOGW(TAG, "三个指纹都没命中");
    return -1;
}

/* ---------- 逐项存在性检查(填 det_mask) ---------- */
static uint8_t scan_peripherals(int board) {
    uint8_t m = 0;
    /* 音频: 三块板都有声音通路（小智/微雪 = ES8311 解码 + I2S；
     *       标准扩展 = NS4168 纯功放 + I2S，无 I2C ⇒ 不能用 I2C 判断有无） */
    m |= (1u << HW_AUDIO);
    /* 触摸: 小智 5/13 · 标准扩展 15/7 · 微雪原板无 */
    if (board == HW_BOARD_XIAOZHI) { if (probe_addr(5, 13, 0x15)) m |= (1u << HW_TOUCH); }
    if (board == HW_BOARD_STD)     { if (probe_addr(15, 7, 0x15)) m |= (1u << HW_TOUCH); }
    /* SD / 电池 / 震动: 三块板都有(真正的"有无"由挂载/读值运行时判) */
    m |= (1u << HW_SD) | (1u << HW_BATTERY) | (1u << HW_VIB);
    /* GPS: ⛔ 2026-10-16 实测: 开机动 UART0 探测会导致整屏花屏 ⇒ 暂时禁用
     *   (改由 gps_init 运行时自行判定; 未焊 GPS 的板子在设置里手动关) */
    if (board == HW_BOARD_XIAOZHI) m |= (1u << HW_GPS);
    return m;
}

/* ---------- 对外 ---------- */
int hw_cfg_board_id(void) { return s_board; }
const char *hw_cfg_board_name(void) {
    switch (s_board) {
        case HW_BOARD_XIAOZHI: return "小智一体版";
        case HW_BOARD_WEIXUE:  return "微雪原版";
        case HW_BOARD_STD:     return "标准扩展版";
        default:               return "未知";
    }
}
uint8_t hw_cfg_det_mask(void) { return s_det; }
uint8_t hw_cfg_en_mask(void)  { return s_en; }
bool hw_cfg_detected(hw_id_t id) { return (s_det >> id) & 1u; }
bool hw_cfg_enabled(hw_id_t id)  { return (s_en  >> id) & 1u; }

void hw_cfg_set_enabled(hw_id_t id, bool on) {
    if (id == HW_TOUCH && !on) return;          /* ★核心项不允许关闭 */
    if (on) s_en |=  (1u << id); else s_en &= ~(1u << id);
    cfg_set_u8("en_mask", s_en);
}

/* 设置页「重新自检」: 唯一可以重探的入口（用户改线/补焊硬件后才需要） */
void hw_cfg_rescan(void) {
    uint8_t old = (uint8_t)s_board;
    int b = detect_board();
    if (b < 0) b = s_board;                       /* 探不到 ⇒ 保持当前板型 */
    if (b != old) {
        cfg_set_u8("en_mask", 0xFF);              /* 换了板: 旧掩码无意义, 重置 */
        ESP_LOGW(TAG, "重新自检: 板型 %u→%d", old, b);
    }
    s_board = b;
    cfg_set_u8("board_id",  (uint8_t)s_board);
    hw_cfg_board_apply();                         /* 把新引脚表下发给 board_pins */
    s_det = scan_peripherals(s_board);
    cfg_set_u8("det_mask", s_det);                /* ★ 记下来, 以后开机不再探 */
    uint8_t en = cfg_get_u8("en_mask", 0xFF);
    s_en = (en == 0xFF) ? s_det : (uint8_t)(en & s_det);
    if (s_det & (1u << HW_TOUCH)) s_en |= (1u << HW_TOUCH);
    cfg_set_u8("en_mask", s_en);
    ESP_LOGW(TAG, "重新自检: board=%s det=0x%02X en=0x%02X",
             hw_cfg_board_name(), s_det, s_en);
}

int hw_cfg_init(void) {
    /* ★★★ 配置版本校验: 固件里的 HWCFG_VER 与 NVS 存的不一致 ⇒ 旧存档一律作废。
     *   这样"刷固件"绝不会再让触摸/按键行为变样(旧 en_mask 会被重置为默认)。 */
    uint8_t nvs_ver = cfg_get_u8("cfg_ver", 0);
    if (nvs_ver != HWCFG_VER) {
        cfg_set_u8("en_mask", 0xFF);            /* 清掉可能误关核心项的旧掩码 */
        cfg_set_u8("cfg_ver", HWCFG_VER);
        ESP_LOGW(TAG, "配置版本 %u→%u ⇒ 已重置旧存档(en_mask)", nvs_ver, HWCFG_VER);
    }

    /* ★★★ 板型 + 硬件项**只探测一次**（linit 2026-10-18 定）。
     *   一块板刷一份固件，硬件不会自己变、也不会两块板互相切换 ⇒ 开机没必要反复扫 I2C。
     *   NVS 里有上次结果 ⇒ 直接用（**0 次 I2C 访问**，也不再占启动时间）；
     *   没有 ⇒ 探一次并把 board_id + det_mask 一起记下。
     *   需要重探：设置 → 网络与系统 → 硬件管理 → 重新自检（或擦 NVS）。 */
    uint8_t id_saved  = cfg_get_u8("board_id", 0xFF);
    uint8_t det_saved = cfg_get_u8("det_mask", 0xFF);
    bool have_id  = (id_saved  <= HW_BOARD_STD);
    bool have_det = ((det_saved & 0x3Fu) != 0);          /* 0/0xFF = 没存过 (det 恒含 SD|BAT|VIB) */

    if (have_id && have_det) {
        s_board = (int)id_saved;
        s_det   = det_saved;
        ESP_LOGW(TAG, "板型已记录: %s det=0x%02X ⇒ 本次不探测", hw_cfg_board_name(), s_det);
    } else {
        int b = detect_board();
        if (b < 0) {
            b = have_id ? (int)id_saved : HW_BOARD_XIAOZHI;   /* 探不到: 上次值 → 默认小智 */
            ESP_LOGW(TAG, "首次探测未命中 ⇒ 用兜底板型 %d（可到设置里重新自检）", b);
        }
        s_board = b;
        s_det   = scan_peripherals(b);
        cfg_set_u8("board_id", (uint8_t)s_board);         /* ★ 只此一次, 之后开机不再探 */
        cfg_set_u8("det_mask", s_det);
        ESP_LOGW(TAG, "首次探测完成并记录: %s det=0x%02X（以后不再探测）",
                 hw_cfg_board_name(), s_det);
    }

    hw_cfg_board_apply();                     /* ★把板型交给 board_pins (引脚随之确定) */

    uint8_t en = cfg_get_u8("en_mask", 0xFF);
    s_en = (en == 0xFF) ? s_det : (uint8_t)(en & s_det);
    /* ★★ 核心保护: 触摸是核心输入通道 ⇒ 探测到就必开, NVS/用户都关不掉 */
    if (s_det & (1u << HW_TOUCH)) s_en |= (1u << HW_TOUCH);
    cfg_set_u8("en_mask", s_en);
    ESP_LOGW(TAG, "HW: board=%s det=0x%02X en=0x%02X (触摸核心已保护)", hw_cfg_board_name(), s_det, s_en);
    return s_board;
}

void hw_cfg_board_apply(void) {
    /* hw_cfg 的板型枚举与 board_id_t 数值一致（0/1/2）⇒ 直接映射 */
    board_id_t id = (s_board == HW_BOARD_WEIXUE) ? BOARD_ID_WEIXUE
                  : (s_board == HW_BOARD_STD)    ? BOARD_ID_STD
                                                 : BOARD_ID_XIAOZHI;
    board_pins_set_id(id);
    ESP_LOGW(TAG, "已应用板型到 board_pins: %s", board_pins_get()->name);
}
