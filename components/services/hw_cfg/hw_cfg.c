/* hw_cfg.c — 一块固件通吃三块板: 开机探测芯片指纹 ⇒ 自动定板型 + 自动启用 */
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
static uint8_t nvs_get_u8(const char *k, uint8_t def) {
    nvs_handle_t h; uint8_t v = def;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, k, &v); nvs_close(h); }
    return v;
}
static void nvs_set_u8(const char *k, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, k, v); nvs_commit(h); nvs_close(h); }
}

/* ---------- 单次 I2C 探测: 该总线上 addr 是否有应答 ---------- */
static bool probe_addr(int sda, int scl, uint8_t addr) {
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

/* ---------- 探测: 按命中概率 3 次, 命中即止 ---------- */
static int detect_board(void) {
    if (probe_addr(38, 47, 0x18)) { ESP_LOGW(TAG, "指纹命中: 38/47 有 ES8311(0x18)"); return HW_BOARD_XIAOZHI; }
    if (probe_addr(13, 14, 0x18)) { ESP_LOGW(TAG, "指纹命中: 13/14 有 ES8311(0x18)"); return HW_BOARD_WEIXUE;  }
    if (probe_addr(15,  7, 0x15)) { ESP_LOGW(TAG, "指纹命中: 15/7  有 CST816(0x15)"); return HW_BOARD_HOMEMADE; }
    ESP_LOGW(TAG, "三个指纹都没命中 ⇒ 用 NVS 兜底");
    return -1;
}

/* ---------- 逐项存在性检查(填 det_mask) ---------- */
static uint8_t scan_peripherals(int board) {
    uint8_t m = 0;
    /* 音频: 小智/微雪 有 ES8311; 自制是 NS4168(无 I2C) ⇒ 按板型给 */
    if (board != HW_BOARD_HOMEMADE) m |= (1u << HW_AUDIO);
    /* 触摸: 小智(5/13) 与 自制(15/7) 有; 微雪原版没有 */
    if (board == HW_BOARD_XIAOZHI)  { if (probe_addr(5, 13, 0x15)) m |= (1u << HW_TOUCH); }
    if (board == HW_BOARD_HOMEMADE) { if (probe_addr(15, 7, 0x15)) m |= (1u << HW_TOUCH); }
    /* SD / 电池 / 震动: 三块板都有(真正的"有无"由挂载/读值运行时判) */
    m |= (1u << HW_SD) | (1u << HW_BATTERY) | (1u << HW_VIB);
    /* GPS: 小智可能不焊 ⇒ 用串口是否收到字节判(粗略: 先按板型给, 运行时 standby 失败即关) */
    if (board == HW_BOARD_XIAOZHI) m |= (1u << HW_GPS);
    return m;
}

/* ---------- 对外 ---------- */
int hw_cfg_board_id(void) { return s_board; }
const char *hw_cfg_board_name(void) {
    return s_board == HW_BOARD_XIAOZHI ? "小智" : (s_board == HW_BOARD_WEIXUE ? "微雪" : "自制");
}
uint8_t hw_cfg_det_mask(void) { return s_det; }
uint8_t hw_cfg_en_mask(void)  { return s_en; }
bool hw_cfg_detected(hw_id_t id) { return (s_det >> id) & 1u; }
bool hw_cfg_enabled(hw_id_t id)  { return (s_en  >> id) & 1u; }

void hw_cfg_set_enabled(hw_id_t id, bool on) {
    if (on) s_en |=  (1u << id); else s_en &= ~(1u << id);
    nvs_set_u8("en_mask", s_en);
}

void hw_cfg_rescan(void) {
    int b = detect_board();
    if (b >= 0) { s_board = b; nvs_set_u8("board_id", (uint8_t)b); }
    s_det = scan_peripherals(s_board);
    uint8_t en = nvs_get_u8("en_mask", 0xFF);
    s_en = (en == 0xFF) ? s_det : (uint8_t)(en & s_det);   /* 手动关过的继续关, 但探测不到的一律不开 */
    nvs_set_u8("en_mask", s_en);
    ESP_LOGW(TAG, "HW: board=%s det=0x%02X en=0x%02X", hw_cfg_board_name(), s_det, s_en);
}

int hw_cfg_init(void) {
    int b = detect_board();
    if (b >= 0) { s_board = b; nvs_set_u8("board_id", (uint8_t)b); }
    else        { s_board = nvs_get_u8("board_id", HW_BOARD_XIAOZHI); }
    hw_cfg_board_apply();          /* ★把板型交给 board_pins (引脚随之确定) */
    s_det = scan_peripherals(s_board);
    uint8_t en = nvs_get_u8("en_mask", 0xFF);
    s_en = (en == 0xFF) ? s_det : (uint8_t)(en & s_det);
    ESP_LOGW(TAG, "HW: board=%s det=0x%02X en=0x%02X", hw_cfg_board_name(), s_det, s_en);
    return s_board;
}

void hw_cfg_board_apply(void) {
    board_pins_set_id(s_board == HW_BOARD_XIAOZHI ? BOARD_ID_XIAOZHI :
                      (s_board == HW_BOARD_WEIXUE ? BOARD_ID_WEIXUE : BOARD_ID_WEIXUE));
    ESP_LOGW(TAG, "已应用板型到 board_pins: %s", board_pins_get()->name);
}
