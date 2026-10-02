/* ============================================================================
 * espsdr.c — ESP32-S3 原始 I/Q 采集 (移植自 ESPARGOS/esp-sdr, 去掉传输层)
 * ----------------------------------------------------------------------------
 * 原始实现: esp-sdr/main/targets/esp32s3/receiver.c
 *   这里保留: SRAM 保留区 / PHY 准备 / 调谐 / 增益 / 突发采集(寄存器序列)
 *   这里去掉: UART+USB 传输、命令解析、IQ 打包、CRC、速率探测、滤波探针
 *
 * 采集序列 (逐行对应官方实现, 别改顺序):
 *   1. 缓冲填哨兵 0xa5a0055a, 用于判断"DMA 是否真写满了"
 *   2. REG_WRITE(0x60033d90, ...)  配 pack/agc 字段
 *   3. 把 0x600c101c 的 MAC_DUMP_USAGE 置 4 → **这块 SRAM 归 modem**
 *   4. REG_WRITE(0x60033d5c, ctrl) 写控制字
 *   5. 再写一次 ctrl | (1<<19)     ← 触发
 *   6. 轮询 bit18 = 完成 (超时 20ms)
 *   7. 关控制字, 把 SRAM 归还 CPU
 * ==========================================================================*/
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc.h"          /* REG_READ / REG_WRITE */
#include "heap_memory_layout.h"  /* SOC_RESERVE_MEMORY_REGION (把 64KB 从堆里挖掉) */

#include "espsdr.h"

#define TAG "ESPSDR"

/* ---------------- ① 保留采集区给 modem 写 IQ ----------------
 * 官方注释: "Vendor S3 adctrig uses this 64 KiB aperture with MAC_DUMP_USAGE=4.
 *            Keep both its DRAM and IRAM aliases out of the heap and static sections."
 * 硬件把这块 aperture 钉在 0x3fcd0000 (整窗口 64KB), 但**调制解调器只写前 n 个字**
 * (n = 控制字低位的样本数) —— 所以只保留"实际会用到的部分", 其余留给内部堆。
 * 背景 (2026-10-01 实测): 白留满 64KB 时, 蓝牙初始化后内部空闲只剩 56KB / 最大连续块
 * 31KB, esp_wifi_init 直接 ESP_ERR_NO_MEM → WiFi 完全打不开。
 * ⚠ 当前是**第一阶段**(ESPSDR_APERTURE_BYTES = 64KB, 满窗口), 为的是让下面的"尾区守卫"
 *   拿到"modem 严格只写 n 个字"的实测证据; 拿到后改成 20KB 窗口 (样本上限 4096, 保留 4KB
 *   守卫尾区) → 内部堆一次多出 44KB。越写由 espsdr_capture() 的尾区守卫逐帧把关。 */
#define APERTURE_BASE   0x3fcd0000u
#define APERTURE_END    (APERTURE_BASE + ESPSDR_APERTURE_BYTES)
#define APERTURE_WORDS  (ESPSDR_APERTURE_BYTES / 4u)
SOC_RESERVE_MEMORY_REGION(APERTURE_BASE, APERTURE_END, s3_rf_dump);

/* 编译器兜底: 上层要的样本数不能超过保留窗口 */
_Static_assert(ESPSDR_MAX_SAMPLES <= APERTURE_WORDS,
               "ESPSDR_MAX_SAMPLES 超过 ESPSDR_APERTURE_BYTES 容量 —— 调大窗口或调小样本数");

#define IQ_WORDS       APERTURE_WORDS                     /* 采集区容量 (字) */
#define IQ_BUFFER      ((uint32_t *)APERTURE_BASE)        /* 采集落点 (modem 直写) */
#define SRAM_OWNER_REG 0x600c101cu                        /* MAC_DUMP_USAGE 所在 */
#define IQ_CTRL_REG    0x60033d5cu                        /* 采集控制/状态 */
#define IQ_PACK_REG    0x60033d90u                        /* pack/agc 配置 */
#define SENTINEL       0xa5a0055au                        /* 未写入标记 */

/* ---------------- PHY / ROM 符号 (与官方一致: 都是 ROM 里的, 直接 extern) ---------------- */
extern void stop_tx_tone(unsigned);
extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_rx_on(unsigned);
extern void rom_pbus_xpd_tx_off(void);
extern void rom_set_rxclk_en(unsigned);
extern void set_chanfreq(unsigned, unsigned);
extern void set_rf_freq_offset(unsigned, unsigned, int);
/* ⚠ 增益符号在 IDF 版本间改过名, 这里用 IDF 5.5 能直接链到的那个:
 *   官方 esp-sdr 用的是 librftest.a 里的 `force_rx_gain(manual,code,0)`,
 *   但 IDF **默认不把 librftest.a 挂进链接** → 链不过;
 *   libphy.a 里有功能等价的 `phy_force_rx_gain(manual, code)` (官方 S31 路径用的就是它)。
 *   若哪天发现增益行为不对, 可改回 librftest 版并在 CMakeLists 里显式加上该 .a。 */
extern void phy_force_rx_gain(unsigned manual, unsigned code);
extern unsigned rom_chip_i2c_readReg(unsigned, unsigned, unsigned);
extern void rom_chip_i2c_writeReg(unsigned, unsigned, unsigned, unsigned);

#define phy_stop_tx_tone    stop_tx_tone
#define phy_pbus_workmode   rom_pbus_workmode
#define phy_pbus_xpd_rx_on  rom_pbus_xpd_rx_on
#define phy_pbus_xpd_tx_off rom_pbus_xpd_tx_off
#define phy_set_rxclk_en    rom_set_rxclk_en

/* 手动增益表上限所在寄存器 (官方 burst_gain.h: 非 C5/C6/C61 都是这个) */
#define BURST_GAIN_REG 0x6001c02cu

/* ---------------- 状态 ---------------- */
static bool     s_ready;          /* PHY 已按 SDR 方式准备好 */
static unsigned s_freq_mhz = 2412;
static int      s_gain_code = ESPSDR_GAIN_AUTO;   /* -1 = 硬件 AGC */
static uint32_t s_last_us;        /* 最近一次采集耗时 */
static bool     s_have_owner;     /* 是否已把 SRAM 交给 modem (异常退出时兜底还原) */
static uint32_t s_owner_saved;

/* 越写守卫 (2026-10-01):
 *   modem 只应写采集区的前 n 个字。每次都把**整个保留窗口**填哨兵, 抓完逐字复核
 *   [n, APERTURE_WORDS) 是否仍是哨兵 —— 是则证明"窗口可以只有这么大";
 *   一旦发现被改, 立刻报错并让这一帧失败 (不把可能被砸坏的堆数据交给上层)。
 *   s_overrun_hi: 越写到的最高字下标 (-1 = 从未越写), 供 UI/远程控制台读取。 */
static int      s_overrun_hi = -1;
static bool     s_probe_logged;   /* 本次 enter 后是否已打过一次"尾区干净"的结论 */

/* ============================================================================
 * 调谐 / PHY 准备
 * ==========================================================================*/
static bool is_wifi_channel(unsigned mhz)
{
    return (mhz >= 2412 && mhz <= 2472 && ((mhz - 2412) % 5) == 0) || mhz == 2484;
}

static void s3_tune(unsigned mhz)
{
    /* WiFi 信道走 set_chanfreq; 其它频率用 PLL 直设 (40MHz 晶振, 直接给 MHz) */
    set_chanfreq(is_wifi_channel(mhz) ? mhz : 2412u, 0);
    if (!is_wifi_channel(mhz)) set_rf_freq_offset(0, mhz, 0);
}

int espsdr_gain_max(void)
{
    unsigned maximum = (REG_READ(BURST_GAIN_REG) >> 8) & 127u;
    /* 官方: S3/S2 只暴露 ≤82 的已校准档, 超过说明表没填, 视为不可用 */
    return maximum <= 82u ? (int)maximum : 0;
}

static void gain_apply(void)
{
    bool manual = (s_gain_code != ESPSDR_GAIN_AUTO);
    int  code   = manual ? s_gain_code : 0;
    int  mx     = espsdr_gain_max();
    if (manual && code > mx) code = mx;
    /* 官方 burst_gain.h 的 force_rx_gain(manual, code, 0) 等价调用 */
    phy_force_rx_gain(manual ? 1u : 0u, (unsigned)code);
}

/* 把 PHY 拉成"能抓 IQ"的状态 (官方 prepare_rx 的 rx_prep=0 分支) */
static bool prepare_rx(void)
{
    if (s_ready) return true;

    /* ⚠ 本固件**不允许 SDR 自己 init WiFi** —— 实测 (2026-10-01) 这条必失败:
     *     wifi:malloc buffer fail → esp_wifi_init: ESP_ERR_NO_MEM (内部空闲=48499 最大块=28672)
     *   WiFi 要 ~50KB **DMA 可用的内部 RAM**, 而 SDR 的 64KB 采集区
     *   (SOC_RESERVE_MEMORY_REGION) 已经把它从堆里挖走了 → 重新 init 不可能。
     * 所以约定: **上层先用 wifi_manager_enable() 把 WiFi 起来** (开机就是 init 好的),
     *           这里只做"接管": 断开 STA → NULL 模式 → 混杂模式;
     *           **退出时只切回 STA, 绝不 deinit** (deinit 掉就再也 init 不回来)。
     * 若 WiFi 没起, 直接报错并提示上层 (不再徒劳重试, 避免刷一堆 wifi 驱动报错)。 */
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 未初始化: 请由上层先 wifi_manager_enable() —— "
                      "本固件因 SDR 的 64KB 采集区, 无法自行 init WiFi (会 NO_MEM)");
        return false;
    }
    esp_wifi_disconnect();          /* 先断开, 免得 STA 还连着 AP 抢射频 */
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_err_t e = esp_wifi_start();
    if (e != ESP_OK && e != ESP_ERR_WIFI_CONN) ESP_LOGW(TAG, "esp_wifi_start: %s", esp_err_to_name(e));
    esp_wifi_set_ps(WIFI_PS_NONE);
    e = esp_wifi_set_promiscuous(true);
    if (e != ESP_OK) ESP_LOGW(TAG, "set_promiscuous: %s", esp_err_to_name(e));
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    /* PHY 上电序列 (顺序照抄官方, 不要调换) */
    s3_tune(s_freq_mhz);
    phy_stop_tx_tone(1);
    phy_pbus_workmode();
    phy_pbus_xpd_tx_off();
    phy_pbus_xpd_rx_on(1);
    phy_set_rxclk_en(1);
    gain_apply();

    s_ready = true;
    ESP_LOGI(TAG, "PHY 就绪: %u MHz, 增益=%s (上限 %d)", s_freq_mhz,
             s_gain_code == ESPSDR_GAIN_AUTO ? "自动" : "手动", espsdr_gain_max());
    return true;
}

/* ============================================================================
 * 对外接口
 * ==========================================================================*/
esp_err_t espsdr_enter(void)
{
    if (!prepare_rx()) return ESP_FAIL;
    return ESP_OK;
}

void espsdr_exit(void)
{
    if (s_have_owner) {           /* 兜底: 万一上次采集异常退出, 把 SRAM 还给 CPU */
        REG_WRITE(SRAM_OWNER_REG, s_owner_saved);
        s_have_owner = false;
    }
    REG_WRITE(IQ_CTRL_REG, 0);
    esp_wifi_set_promiscuous(false);
    /* 交回: 切回 STA 模式并保持 start —— ESP-IDF 会自动重连已保存的 AP ✓
     * ⚠ 这里**只 stop/deinit 我们不再碰**: 一旦 esp_wifi_deinit(), 本固件就再也 init 不回来 (NO_MEM)。 */
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
    s_ready = false;
    ESP_LOGI(TAG, "已退出 SDR 模式 (射频交回 STA, 自动重连)");
}

bool espsdr_ready(void) { return s_ready; }

void espsdr_tune(unsigned mhz)
{
    s_freq_mhz = mhz;
    if (s_ready) {
        s3_tune(mhz);
        gain_apply();      /* 官方: 重调谐后要重新应用增益 */
    }
}

void espsdr_set_gain(int code)
{
    s_gain_code = code;
    if (s_ready) gain_apply();
}

int espsdr_gain_get(void) { return s_gain_code; }

unsigned espsdr_rate_msps(unsigned divider)
{
    /* 官方: bit15 把 80 折半到 40; bit16 选 16MSa/s 硬件通路 */
    return divider == ESPSDR_DIV_40M ? 40u : divider == ESPSDR_DIV_16M ? 16u : 80u;
}

int espsdr_capture(unsigned n, unsigned divider)
{
    if (!s_ready) return -1;
    if (n == 0 || n > ESPSDR_MAX_SAMPLES || n > IQ_WORDS) return -1;

    /* 1. 哨兵: 前 n 个字用于判"写满", 其后是越写守卫区 (整窗口都填) */
    for (unsigned j = 0; j < APERTURE_WORDS; j++) IQ_BUFFER[j] = SENTINEL;

    /* 2. pack/agc 字段 (官方默认 rx_pack=0, rx_agc=0) */
    REG_WRITE(IQ_PACK_REG, 0u | (1u << 6) | (2u << 12) | (3u << 18) | (0u << 24));

    /* 3. 这块 SRAM 交给 modem */
    s_owner_saved = REG_READ(SRAM_OWNER_REG);
    REG_WRITE(SRAM_OWNER_REG, (s_owner_saved & ~15u) | 4u);
    s_have_owner = true;

    /* 4. 控制字: bit31=启动, bit15/16=速率, 低位=样本数 */
    uint32_t rate_bits = (divider == ESPSDR_DIV_40M) ? (1u << 15)
                       : (divider == ESPSDR_DIV_16M) ? (1u << 16) : 0u;
    uint32_t ctrl = 0x80000000u | rate_bits | n;

    REG_WRITE(IQ_CTRL_REG, 0);
    REG_WRITE(IQ_CTRL_REG, ctrl);
    /* 5. 触发 (bit19) */
    REG_WRITE(IQ_CTRL_REG, ctrl | (1u << 19));
    REG_WRITE(IQ_CTRL_REG, ctrl);

    /* 6. 等完成 (bit18), 超时 20ms */
    int64_t start = esp_timer_get_time();
    while (!(REG_READ(IQ_CTRL_REG) & (1u << 18)) && (esp_timer_get_time() - start) < 20000) { }
    bool done = (REG_READ(IQ_CTRL_REG) & (1u << 18)) != 0;
    s_last_us = (uint32_t)(esp_timer_get_time() - start);

    /* 7. 收尾: 关控制字 + SRAM 归还 CPU */
    REG_WRITE(IQ_CTRL_REG, 0);
    REG_WRITE(SRAM_OWNER_REG, s_owner_saved);
    s_have_owner = false;

    if (!done) { ESP_LOGW(TAG, "采集超时 (%u µs)", (unsigned)s_last_us); return -1; }
    if (IQ_BUFFER[0] == SENTINEL || IQ_BUFFER[n - 1] == SENTINEL) {
        ESP_LOGW(TAG, "缓冲未被写入 (modem 没动) —— 检查 PHY 是否就绪");
        return -1;
    }

    /* 8. 越写守卫: n 之后的字必须仍是哨兵 */
    if (n < APERTURE_WORDS) {
        unsigned bad = 0;
        for (unsigned j = n; j < APERTURE_WORDS; j++) {
            if (IQ_BUFFER[j] != SENTINEL) { bad = j; break; }
        }
        if (bad) {
            unsigned hi = bad;                       /* 只在异常时扫一遍, 找出真实写到的最高字 */
            for (unsigned j = APERTURE_WORDS; j-- > bad; ) {
                if (IQ_BUFFER[j] != SENTINEL) { hi = j; break; }
            }
            s_overrun_hi = (int)hi;
            ESP_LOGE(TAG, "⚠ modem 越写采集区: 首个越写字 #%u, 最高 #%u "
                          "(本次 n=%u, 窗口 %u 字) —— 采集区不能再缩小!",
                     bad, hi, n, APERTURE_WORDS);
            return -1;
        }
        if (!s_probe_logged) {                       /* 只打一次, 作为"能否缩小窗口"的证据 */
            s_probe_logged = true;
            ESP_LOGI(TAG, "越写守卫通过: 窗口 %u 字 (%u KB), 本次用 %u 字, "
                          "其后 %u 字未被写入",
                     APERTURE_WORDS, (unsigned)(ESPSDR_APERTURE_BYTES / 1024u),
                     n, APERTURE_WORDS - n);
        }
    }
    return (int)n;
}

const uint32_t *espsdr_raw(void) { return IQ_BUFFER; }

void espsdr_iq(int i, float *out_i, float *out_q)
{
    uint32_t w = IQ_BUFFER[i];
    int iv = (int)((w >> 10) & 0x3FFu);          /* bit19-10 = I */
    int qv = (int)(w & 0x3FFu);                  /* bit9-0  = Q */
    if (iv & 0x200) iv -= 0x400;                 /* 10bit 有符号扩展 */
    if (qv & 0x200) qv -= 0x400;
    if (out_i) *out_i = (float)iv / 512.0f;      /* 归一化到 [-1,1) */
    if (out_q) *out_q = (float)qv / 512.0f;
}

uint32_t espsdr_last_elapsed_us(void) { return s_last_us; }

unsigned espsdr_aperture_words(void) { return APERTURE_WORDS; }

int espsdr_overrun_word(void) { return s_overrun_hi; }
