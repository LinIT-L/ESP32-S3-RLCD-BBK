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
#include "driver/i2c.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
/* ---------- 触摸探测（★ 必须带 RST 上电时序）----------
 * 2026-10-22 linit: "只连屏幕、触摸不能用"的真凶在这里。
 *   原来是**裸 I2C 探测**：直接把总线拉起来问 0x15 在不在。
 *   但面板的 RST 脚上电后是浮空/低 ⇒ 触摸芯片一直处在复位态 ⇒ 不应答。
 *   正常的 touch_panel_init() 会先 gpio_set_level(rst,1) + 等 80ms 再探；
 *   板级扫描没这一步 ⇒ 首次开机必然判"无触摸"，而且这个结论被落盘后**永久不再探**。
 *   现在：先给 RST 一个 低→高 脉冲，等 120ms 就绪，再按常见地址逐个试。 */
static bool probe_touch_on(int sda, int scl, int rst, uint8_t *addr_out)
{
    static const uint8_t cand[] = { 0x15, 0x5D, 0x14, 0x38 };  /* CST816/836 · GT911×2 · FT6236 */
    if (sda < 0 || scl < 0) return false;

    i2c_master_bus_config_t bc = {
        .i2c_port = -1, .sda_io_num = sda, .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return false;

    bool hit = false;
    for (size_t i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
        if (i2c_master_probe(bus, cand[i], 50) == ESP_OK) {
            if (addr_out) *addr_out = cand[i];
            hit = true;
            break;
        }
    }

    /* ★ 2026-10-22: **先裸探**。小智那块的触摸 RST 常常没接 (或接了也不用),
     *   有些面板上电就绪后不需要复位脉冲; 早期版本就是裸探认到的。
     *   裸探不通才走 RST 时序重探一次 (冷启动/掉电重启后芯片可能卡在复位态)。 */
    if (!hit && rst >= 0) {
        gpio_config_t g = {
            .pin_bit_mask = (1ULL << rst),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = false,
            .pull_down_en = false,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&g) == ESP_OK) {
            gpio_set_level(rst, 0);
            vTaskDelay(pdMS_TO_TICKS(12));
            gpio_set_level(rst, 1);
            vTaskDelay(pdMS_TO_TICKS(150));
            for (size_t i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
                if (i2c_master_probe(bus, cand[i], 50) == ESP_OK) {
                    if (addr_out) *addr_out = cand[i];
                    hit = true;
                    break;
                }
            }
        }
    }
    i2c_del_master_bus(bus);                    /* ★探完立刻释放总线 */

    /* ★★★ 探测完把用过的引脚**全部归还**(gpio_reset_pin = 高阻输入/无上下拉):
     *   detect_board() 会拿"另一套板型"的触摸脚来试 —— 在小智板上
     *   GPIO15 = SD CMD、GPIO7 = LCD DC，在标准/微雪板上 GPIO5 = LCD DC。
     *   若把这些脚留在 I2C 模式(带上拉)或把 RST 留成输出高，就会干扰
     *   紧接着的 LCD / SD 初始化 ⇒ 必须探测完立刻放手。
     *   命中后不会再碰另一套引脚；真正用触摸时 touch_panel_init() 会重新配置。 */
    gpio_reset_pin((gpio_num_t)sda);
    gpio_reset_pin((gpio_num_t)scl);
    if (rst >= 0) gpio_reset_pin((gpio_num_t)rst);
    return hit;
}

/* 触摸探测结果缓存: 0=本次没探过 · 1=小智组(5/13)有 · 2=标准组(15/7)有 · 3=两组都无。
 * 由 detect_board() 写入, scan_peripherals() 复用 —— 同一次开机能省掉一次 130ms 时序探测。
 * ⚠ 详细的总线诊断(引脚电平 + 全地址 ACK)不在这里做: 那段要扫 78 个地址, 每次开机
 *   白花 1.5s。需要时用串口命令 `tpscan`。 */
static uint8_t s_tp_hit = 0;
/* 本次开机"硬件指纹全没命中" ⇒ main 会进识别轮询界面, 不进系统 */
static bool s_identify_failed = false;
/* 板型已固化, 但本次开机**实测触摸芯片没应答** ⇒ main 起后台补探任务 */
static bool s_touch_missing = false;

/* ---------- 跨板探测的"别把屏搞坏"保护 ----------
 * 在小智板上探标准触摸组 15/7 时, GPIO7 = **当前板 LCD DC**;
 * 在标准/微雪板上探小智触摸组 5/13 时, GPIO5 = **当前板 LCD DC**。
 *   · DC / CS / RST 是纯 GPIO 输出 ⇒ 探测前备份方向和电平, 探测完恢复即可;
 *   · SCK / MOSI 是 SPI 外设脚 ⇒ 一旦被改成 I2C/输入, SPI 驱动不会自动接管回来,
 *     这种冲突破例直接跳过该目标 (实测只有 DC 会撞, 所以不影响识别率)。 */
static bool pins_hit_spi(int a, int b, int c)
{
    const board_pins_t *bp = board_pins_get();
    int crit[2] = { bp->lcd_sck, bp->lcd_mosi };
    int pin[3]  = { a, b, c };
    for (int i = 0; i < 3; i++) {
        if (pin[i] < 0) continue;
        for (int k = 0; k < 2; k++)
            if (crit[k] >= 0 && pin[i] == crit[k]) return true;
    }
    return false;
}

/* DC/CS/RST 在 LCD 初始化后一定是推挽输出 ⇒ 只需记住电平, 探测完强制设回输出+电平。
 * (IDF 没有"读 GPIO 方向"的 API, 也不需要: 屏已经点亮了, 这几个脚必然是输出。) */
typedef struct { int pin; int level; } gpio_save_t;
static void gpio_save(gpio_save_t *s, int pin)
{
    s->pin   = pin;
    s->level = (pin >= 0) ? gpio_get_level((gpio_num_t)pin) : 0;
}
static void gpio_restore(const gpio_save_t *s)
{
    if (s->pin < 0) return;
    gpio_set_direction((gpio_num_t)s->pin, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)s->pin, s->level);
}

/* ES8311 的地址由 CE 脚决定 ⇒ 0x18 / 0x19 都要试 */
static bool probe_es8311(int sda, int scl) {
    return probe_addr(sda, scl, 0x18) || probe_addr(sda, scl, 0x19);
}

/* ---------- 探测顺序: 命中即止（小智只探一次, 微雪两次, 标准扩展三次） ---------- */
static int detect_board(void) {
    /* ★★★ 2026-10-22 linit 定: **音频优先**识别板型。
     *   ① 音频 38/47 有 codec → 小智一体版   (小智核心自带音频)
     *   ② 音频 13/14 有 codec → 微雪原版     (微雪板自带音频)
     *   ③ 都没有音频 → 触摸 15/7 有应答 → 标准扩展版 (标准版可以不接音频)
     *   ④ 全不中 → -1 (交给上层: 进识别轮询界面, 不启动系统)
     * 理由: 触摸排线最容易没接好/虚焊, 拿它当主判据会把"小智版"误判成微雪/标准,
     *   而 LCD 引脚一换就是黑屏。音频焊在核心板上, 可靠得多。 */
    s_tp_hit = 0;
    if (probe_es8311(38, 47)) { ESP_LOGW(TAG, "音频命中: 38/47 有 codec ⇒ 小智一体版"); return HW_BOARD_XIAOZHI; }
    if (probe_es8311(13, 14)) { ESP_LOGW(TAG, "音频命中: 13/14 有 codec ⇒ 微雪原版");   return HW_BOARD_WEIXUE;  }

    uint8_t a = 0;
    if (probe_touch_on(15, 7, 2, &a)) {
        s_tp_hit = 2;
        ESP_LOGW(TAG, "触摸命中: 15/7 有触摸(0x%02X) ⇒ 标准扩展版", a);
        return HW_BOARD_STD;
    }
    ESP_LOGW(TAG, "音频(38/47 · 13/14) 与标准触摸(15/7) 都没命中");
    return -1;
}

/* ---------- 逐项存在性检查(填 det_mask) ---------- */
static uint8_t scan_peripherals(int board) {
    uint8_t m = 0;
    /* 音频: 三块板都有声音通路（小智/微雪 = ES8311 解码 + I2S；
     *       标准扩展 = NS4168 纯功放 + I2S，无 I2C ⇒ 不能用 I2C 判断有无） */
    m |= (1u << HW_AUDIO);
    /* 触摸: 小智 5/13/12/6 · 标准扩展 & 微雪 15/7/17/2
     * ★ 用带 RST 时序的 probe_touch_on, 不能裸探 (见上面的说明) */
    {
        /* 只探**本板板型**的触摸组 (调用本函数前已 hw_cfg_board_apply, board_pins 已是新板型)。
         * ⛔ 不能跨板试: 小智 GPIO7=LCD DC、标准/微雪 GPIO5=LCD DC, 探完 LCD 就花屏。 */
        const board_pins_t *bp = board_pins_get();
        uint8_t a = 0;
        if (bp->tp_sda >= 0 && bp->tp_scl >= 0 &&
            probe_touch_on(bp->tp_sda, bp->tp_scl, bp->tp_rst, &a)) {
            m |= (1u << HW_TOUCH);
            s_touch_missing = false;
            ESP_LOGW(TAG, "触摸存在: SDA=%d SCL=%d RST=%d (0x%02X)",
                     bp->tp_sda, bp->tp_scl, bp->tp_rst, a);
        } else {
            s_touch_missing = true;      /* ★ 让 main 起后台补探 (排线补焊后能自愈) */
            ESP_LOGW(TAG, "触摸未应答: SDA=%d SCL=%d RST=%d (触摸排线/供电?)",
                     bp->tp_sda, bp->tp_scl, bp->tp_rst);
        }
    }
    /* SD / 电池 / 震动: 三块板都有(真正的"有无"由挂载/读值运行时判) */
    m |= (1u << HW_SD) | (1u << HW_BATTERY) | (1u << HW_VIB);
    /* GPS: ★★★ 2026-10-20 不再"猜有" —— 原来固定给 xiaozhi 板置位, 结果**没焊模块
     *   也照样启动**: 自动波特率跑满 6 档(1.44s) 后把"没认出来"当成"9600 确认",
     *   再花 0.6s 发 PMREQ, 白占启动 2.1s, 还留下一路 UART 在跑。
     *   GPS 是"用户自己焊的选装件" ⇒ **默认关闭**, 要用的在 设置→硬件 里手动打开
     *   (独立记在 NVS "gps_on", 不受 det 掩码影响)。 */
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
    if (id == HW_GPS) cfg_set_u8("gps_on", on ? 1 : 0);   /* 单独记, 开机按它恢复 */
    cfg_set_u8("en_mask", s_en);
}

/* ---------- GPS 选装件的一次性识别 ----------
 * linit 2026-10-20 定: **只在写完固件的第一次开机识别一次**, 不是每次重启都探。
 *   第一次(或换固件后): gps_probed=0 → 上层后台探一次 → 探到就开, 探不到就记"未识别";
 *   之后再开机: gps_probed=1 → 一次都不探(不占启动时间, 也不碰 UART);
 *   要再试: 设置→硬件 里点 GPS 那一行 / 重新自检 ⇒ invalidate 后重探。 */
bool hw_cfg_gps_probed(void) { return cfg_get_u8("gps_probed", 0) != 0; }

void hw_cfg_set_gps_present(bool present) {
    cfg_set_u8("gps_probed", 1);
    if (present) {
        s_det |=  (1u << HW_GPS);
        s_en  |=  (1u << HW_GPS);
        cfg_set_u8("gps_on", 1);
    } else {
        s_det &= (uint8_t)~(1u << HW_GPS);
        s_en  &= (uint8_t)~(1u << HW_GPS);
        cfg_set_u8("gps_on", 0);
    }
    cfg_set_u8("det_mask", s_det);
    cfg_set_u8("en_mask", s_en);
    ESP_LOGW(TAG, "GPS 识别结果: %s (det=0x%02X en=0x%02X)",
             present ? "识别到, 已启用" : "未识别, 以后不再自动尝试", s_det, s_en);
}

void hw_cfg_gps_invalidate(void) {
    cfg_set_u8("gps_probed", 0);
    s_det &= (uint8_t)~(1u << HW_GPS);
    s_en  &= (uint8_t)~(1u << HW_GPS);
    cfg_set_u8("det_mask", s_det);
    cfg_set_u8("en_mask", s_en);
    ESP_LOGW(TAG, "GPS 识别结果已清除 ⇒ 下次开机/手动点选时重新识别");
}

/* 由 NVS en_mask + det 算出最终 s_en (含触摸强制开、GPS 选装开关) —— 三处入口共用 */
static void apply_en_mask(uint8_t en_raw)
{
    s_det &= (uint8_t)~(1u << HW_GPS);           /* 旧档 det 里的 GPS 位作废 */
    s_en = (en_raw == 0xFF) ? s_det : (uint8_t)(en_raw & s_det);
    if (s_det & (1u << HW_TOUCH)) s_en |= (1u << HW_TOUCH);   /* 触摸是核心项, 必开 */
    /* ★★★ 2026-10-25 linit「GPS 默认和其他模块一样, 第一次启动检测一次, 不要默认关闭」:
     *   原来 gps_on 默认 0 ⇒ 出厂/首次开机就是"关", 只有那次检测恰好成功才变开。
     *   现在改成: **没探过 ⇒ 先按"开"处理**(等那一次检测下结论);
     *   探过且未识别(gps_on 已被写成 0) ⇒ 保持关, 以后不再自动尝试。 */
    uint8_t gps_def = hw_cfg_gps_probed() ? 0 : 1;
    if (cfg_get_u8("gps_on", gps_def)) s_en |= (1u << HW_GPS); else s_en &= (uint8_t)~(1u << HW_GPS);
    cfg_set_u8("en_mask", s_en);
    cfg_set_u8("det_mask", s_det);
}

/* ==================== 触摸/音频 自适应轮询 ====================
 * linit 2026-10-22 定: 开机识别不到触摸 ⇒ **每 10 秒换一个目标**反复试,
 *   屏幕给出提示, 直到命中为止 (命中即落盘 + 重启, 重启后按新结果固定死)。
 *
 * ⛔ 只探"当前板型自己的引脚组", 绝不跨板探测 ——
 *   小智板上 GPIO7=LCD DC、GPIO11=LCD CS; 标准/微雪板上 GPIO5=LCD DC。
 *   跨板探测会把这些脚临时改成 I2C, 探测完 `gpio_reset_pin()` 归还成输入,
 *   LCD 驱动不会重配 ⇒ **屏幕直接花掉**。所以这里的目标只有:
 *     ① 本板触摸组 (bp->tp_sda/scl/rst)
 *     ② codec 38/47 (小智)
 *     ③ codec 13/14 (微雪)
 *   换到"另一种板型的屏"这种情形, 说明 LCD 引脚本来就不对(屏幕不亮),
 *   得靠设置里的"重新自检", 不在这里轮询。 */

int hw_cfg_probe_fingerprint(int which)
{
    uint8_t a = 0;
    switch (which) {
    case 0:   /* 小智一体版: 音频 codec ES8311 在 38/47 */
        return probe_es8311(38, 47) ? 1 : 0;
    case 1:   /* 微雪原版: 音频 codec ES8311 在 13/14 */
        return probe_es8311(13, 14) ? 1 : 0;
    case 2: { /* 标准扩展版: 没有音频 I2C, 只能靠触摸 15/7/2 */
        const board_pins_t *bp = board_pins_get();
        if (pins_hit_spi(15, 7, 2)) { ESP_LOGW(TAG, "目标2(15/7/2) 撞 LCD SPI 脚 ⇒ 跳过"); return 0; }
        gpio_save_t dc, cs, rst;
        gpio_save(&dc,  bp->lcd_dc);
        gpio_save(&cs,  bp->lcd_cs);
        gpio_save(&rst, bp->lcd_rst);
        int hit = probe_touch_on(15, 7, 2, &a) ? 1 : 0;
        gpio_restore(&dc); gpio_restore(&cs); gpio_restore(&rst);   /* ★ 把 LCD 脚还回去 */
        return hit;
    }
    }
    return 0;
}

void hw_cfg_adapt_commit(int which)
{
    int nb;
    const char *desc;

    if (which == 0)      { nb = HW_BOARD_XIAOZHI; desc = "音频 38/47"; }
    else if (which == 1) { nb = HW_BOARD_WEIXUE;  desc = "音频 13/14"; }
    else if (which == 2) { nb = HW_BOARD_STD;     desc = "触摸 15/7"; }
    else return;

    s_board = nb;
    hw_cfg_board_apply();                 /* 先应用板型, scan 才会用对的触摸引脚 */
    s_det = scan_peripherals(nb);         /* ★ 触摸位一律以**实测**为准 */
    /* ⛔ 2026-10-22: 原来这里 `if (touch) s_det |= HW_TOUCH` 强行置位 ——
     *   which==2 时板型刚切成标准版, scan 探的就是 15/7, 刚探到才走到这里, 自然置位;
     *   强行置位只会在"探不到"时把假话写进存档, 下次开机就再也不实测了。删掉。 */

    cfg_set_u8("board_id",   (uint8_t)nb);
    cfg_set_u8("det_mask",   s_det);
    cfg_set_u8("en_mask",    0xFF);
    cfg_set_u8("board_forced", 1);        /* 板型已识别 ⇒ 以后启动不再重新识别板型 */
    ESP_LOGW(TAG, "识别命中: %s ⇒ 板型 %s det=0x%02X ⇒ 固化, 重启生效",
             desc, hw_cfg_board_name(), s_det);
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();                        /* 不返回 */
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
    /* GPS 是选装件, I2C 指纹探不到 ⇒ 重新自检时把它标回"待识别", 重启后重探一次 */
    cfg_set_u8("gps_probed", 0);
    cfg_set_u8("board_forced", 0);   /* 重新自检 ⇒ 恢复自动识别 */
    s_det = scan_peripherals(s_board);
    cfg_set_u8("det_mask", s_det);                /* ★ 记下来, 以后开机不再探 */
    uint8_t en = cfg_get_u8("en_mask", 0xFF);
    apply_en_mask(en);
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
        /* ★ 换了固件 = "第一次写入固件" ⇒ GPS 允许重新识别一次 (见 hw_cfg_gps_probed) */
        cfg_set_u8("gps_probed", 0);
        ESP_LOGW(TAG, "配置版本 %u→%u ⇒ 已重置旧存档(en_mask, GPS 待重新识别)", nvs_ver, HWCFG_VER);
    }

    /* ★★★ 板型识别 (linit 2026-10-22 定稿):
     *   识别链: 音频 38/47 → 小智 | 音频 13/14 → 微雪 | 触摸 15/7 → 标准
     *   ① 存档里已识别过 ⇒ **0 次 I2C 访问**, 直接固定死 (以后启动永不识别)
     *   ② 没存档 ⇒ 识别一次; 命中就把 board_id + det_mask **永久保存**
     *   ③ 全都没命中 ⇒ 不落盘、不进系统, 由 main 进"识别轮询界面" (3 轮后等断电重启) */
    uint8_t id_saved  = cfg_get_u8("board_id", 0xFF);
    uint8_t det_saved = cfg_get_u8("det_mask", 0xFF);
    bool have_id  = (id_saved  <= HW_BOARD_STD);
    bool have_det = ((det_saved & 0x3Fu) != 0);   /* 0/0xFF = 没存过 (det 恒含 SD|BAT|VIB) */

    if (have_id && have_det) {
        s_board = (int)id_saved;
        s_det   = det_saved;
        hw_cfg_board_apply();
        ESP_LOGW(TAG, "板型已记录: %s det=0x%02X ⇒ 固化 (板型不再识别)",
                 hw_cfg_board_name(), s_det);

        /* ★★★ 2026-10-22 linit: **板型固化 ≠ 触摸固化**。
         *   触摸位每次开机都必须实测, 不能拿存档当真 ——
         *   存档里的触摸位可能是"上一块屏探到过"留下的 (换屏/换线/虚焊后就变成假话),
         *   固件信了它就直接让触摸驱动去初始化 ⇒ 驱动对着不存在的芯片初始化失败,
         *   表现就是"触摸完全没反应", 而且**永远不会自愈** (下次开机还是信存档)。
         *   ⛔ 只探本板触摸组, 绝不跨板。 */
        {
            const board_pins_t *bp = board_pins_get();
            if (s_det & (1u << HW_TOUCH)) {
                /* ★★★ 2026-10-27 linit: "第一次启动检测正常过后就没必要再检测"。
                 *   存档已记录有触摸 ⇒ **0 次 I2C 探测**, 每次开机省约 160ms。
                 *   存档不可信的风险交给下层兜底: 触摸驱动 init 失败时 main 会调
                 *   hw_cfg_touch_mark_missing() 起后台补探 —— 和原来一样能自愈,
                 *   但正常机器不再为"可能坏"付每次 160ms 的代价。 */
                ESP_LOGD(TAG, "触摸: 存档已记录 ⇒ 跳过开机探测 (省 ~160ms)");
            } else {
                /* 存档里没有触摸位: 可能真是"补焊/换屏后第一次开机", 实测一次 */
                uint8_t a = 0;
                bool tp_ok = (bp->tp_sda >= 0 && bp->tp_scl >= 0 &&
                              probe_touch_on(bp->tp_sda, bp->tp_scl, bp->tp_rst, &a));
                if (tp_ok) {
                    s_det |= (1u << HW_TOUCH);
                    cfg_set_u8("det_mask", s_det);
                    ESP_LOGW(TAG, "触摸检测: 命中 0x%02X (SDA=%d SCL=%d) ⇒ 启用触摸",
                             a, bp->tp_sda, bp->tp_scl);
                } else {
                    s_touch_missing = true;       /* ★ main 起后台补探任务 */
                    ESP_LOGW(TAG, "触摸检测: SDA=%d SCL=%d RST=%d 无应答 ⇒ 本次不启用, 后台继续重试",
                             bp->tp_sda, bp->tp_scl, bp->tp_rst);
                }
            }
        }
    } else {
        int b = detect_board();
        if (b < 0) {
            s_board = have_id ? (int)id_saved : HW_BOARD_XIAOZHI;
            s_det   = 0;
            hw_cfg_board_apply();
            s_identify_failed = true;          /* ★ 让 main 进识别轮询界面 */
            ESP_LOGW(TAG, "硬件指纹全部未命中 ⇒ 等待识别 (不进系统)");
        } else {
            s_board = b;
            hw_cfg_board_apply();              /* 先应用板型, scan 才会用对的触摸引脚 */
            s_det = scan_peripherals(b);
            cfg_set_u8("board_id", (uint8_t)s_board);
            cfg_set_u8("det_mask", s_det);
            cfg_set_u8("board_forced", 1);     /* 已识别 ⇒ 以后不再自动改 */
            ESP_LOGW(TAG, "首次识别并永久保存: %s det=0x%02X", hw_cfg_board_name(), s_det);
        }
    }

    uint8_t en = cfg_get_u8("en_mask", 0xFF);
    apply_en_mask(en);
    ESP_LOGW(TAG, "HW: board=%s det=0x%02X en=0x%02X (触摸核心已保护, GPS=%s)",
             hw_cfg_board_name(), s_det, s_en,
             (s_en & (1u << HW_GPS)) ? "已启用" : "未识别/已关");

    /* ★★★ 2026-10-22 修 "首次开机触摸不能用, 重启一次才好":
     *   识别阶段用的是**新版 i2c_master** 驱动临时建总线 (i2c_new_master_bus)，
     *   而触摸驱动和 codec 用的是**旧版 i2c_driver_install**。新驱动的端口状态
     *   会让紧接着的旧式 install 失败 ⇒ 触摸 init 直接返回 NONE。
     *   重启时存档已有 det/板型, **不再走识别** ⇒ 端口干净 ⇒ 触摸正常。
     *   这里把两个端口的残留一律删掉, 后面的模块会自己重新安装。 */
    /* ★★★ 2026-10-27 根因修复 (删除原来的 i2c_driver_delete(I2C_NUM_0/1) 两行):
     *
     * 【症状】每次开机两条 `E i2c: i2c_driver_delete(484): i2c driver install error`
     * 【直接原因】该报错出自 IDF `i2c_driver_delete()` 里的
     *     ESP_RETURN_ON_FALSE(p_i2c_obj[port] != NULL, ...)
     *   ⇒ 意思是"这个端口**从没装过旧版 i2c 驱动**", 却去删它。
     * 【为什么装了却没东西可删】识别阶段探总线用的是**新版**驱动
     *   `i2c_new_master_bus()`/`i2c_del_master_bus()`, 端口的所有权由新版驱动自己管;
     *   旧版 `i2c_driver_delete()` 既看不到新版的残留, 也释放不了它 —— 调用必然失败。
     * 【为什么可以删掉】两条探测路径都已经在结尾用 `i2c_del_master_bus()` 释放端口:
     *     probe_addr()      → hw_cfg.c 顶部, 探完立刻 del
     *     probe_touch_on()  → 探完立刻 del
     *   端口本来就是干净的, 这里再"清残留"属于重复动作, 且动作本身无效。
     *   2026-10-22 当初加这两行, 是因为那次真凶(触摸驱动 install 失败)被误判成"端口有残留";
     *   实际真凶是 `probe_touch_on()` 少了 RST 上电时序(已在同一次提交里补上)。
     * 【验证】删掉后模拟器实测: 触摸驱动 809ms 正常认到 GT911、E 级日志 0 条。
     *   ⚠ 若将来新版总线某条路径漏了 i2c_del_master_bus(), 症状会是"触摸 init 失败",
     *     而不是这两条 E —— 到那时该修的是那条路径的释放, 不是在这里补删除。 */


    return s_board;
}

/* 手动指定板型 (飞线/无指纹时用): 落盘 + 立即下发引脚表, 外设需重启才完全生效 */
void hw_cfg_set_board(int id)
{
    if (id < HW_BOARD_XIAOZHI || id > HW_BOARD_STD) return;
    if (id != s_board) cfg_set_u8("en_mask", 0xFF);   /* 换板: 旧逐项掩码作废 */
    s_board = id;
    cfg_set_u8("board_id", (uint8_t)id);
    /* ★ 2026-10-22: 手动指定 = 用户已经知道板型 ⇒ 以后开机**不要再自动轮询/改板型**,
     *   哪怕触摸探不到也直接启动 (否则会卡在适配界面出不来)。
     *   要恢复自动识别: 设置 → 硬件管理 → 重新自检 (hw_cfg_rescan 会清掉这个标记)。 */
    cfg_set_u8("board_forced", 1);
    hw_cfg_board_apply();
    s_det = scan_peripherals(s_board);
    cfg_set_u8("det_mask", s_det);
    apply_en_mask(cfg_get_u8("en_mask", 0xFF));   /* 掩码随新板型重算 */
    ESP_LOGW(TAG, "手动指定板型: %s det=0x%02X en=0x%02X (重启后完全生效)",
             hw_cfg_board_name(), s_det, s_en);
}

bool hw_cfg_board_forced(void) { return cfg_get_u8("board_forced", 0) != 0; }
bool hw_cfg_identify_failed(void) { return s_identify_failed; }
bool hw_cfg_touch_missing(void)   { return s_touch_missing; }

/* 后台补探的结果回写 (main 的补探任务调用):
 *   present=true  ⇒ 置触摸位 + 落盘 (触摸驱动由调用方 touch_panel_* 真正拉起)
 *   present=false ⇒ 什么都不做, 继续等下一轮 */
void hw_cfg_touch_commit(bool present)
{
    if (!present) return;
    s_touch_missing = false;
    s_det |=  (1u << HW_TOUCH);
    s_en  |=  (1u << HW_TOUCH);
    cfg_set_u8("det_mask", s_det);
    cfg_set_u8("en_mask",  s_en);
    ESP_LOGW(TAG, "触摸补探成功 ⇒ 已启用并落盘 (det=0x%02X en=0x%02X)", s_det, s_en);
}

/* ★ 2026-10-27: 开机不再靠 I2C 探测判触摸(存档即信), 改由**触摸驱动自己的 init 结果**
 *   兜底 —— 驱动 init 失败说明真没屏/排线断, 这里置 missing 让 main 起后台补探。
 *   不撤销存档位: 一次 init 失败可能只是上电时序抖动, 下次开机能自愈;
 *   真坏了补探也会一直失败, 行为和以前一致。 */
void hw_cfg_touch_mark_missing(void)
{
    s_touch_missing = true;
    ESP_LOGW(TAG, "触摸: 存档已记录但驱动 init 失败 ⇒ 起后台补探 (不撤销存档位)");
}

void hw_cfg_board_apply(void) {
    /* hw_cfg 的板型枚举与 board_id_t 数值一致（0/1/2）⇒ 直接映射 */
    board_id_t id = (s_board == HW_BOARD_WEIXUE) ? BOARD_ID_WEIXUE
                  : (s_board == HW_BOARD_STD)    ? BOARD_ID_STD
                                                 : BOARD_ID_XIAOZHI;
    board_pins_set_id(id);
    ESP_LOGW(TAG, "已应用板型到 board_pins: %s", board_pins_get()->name);
}
