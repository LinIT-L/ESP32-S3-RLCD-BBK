/**
 * gps.c — u-blox MAX-M10S 驱动实现 (UART + UBX). 见 gps.h 的接线与设计约定.
 *
 * 关键协议依据 (2026-09-29 查证):
 *   - MAX-M10S (SPG 5.00/5.10) **不支持** UBX-CFG-RXM 的 powerSaveMode;
 *   - 软件待机只能用 **UBX-RXM-PMREQ**, 且 **必须置 force 标志**
 *     (MAX-M10S Integration Manual §2.7.3.2 "Software standby mode");
 *   - 唤醒源由 UBX-RXM-PMREQ 指定: UART RX / EXTINT / SPI CS;
 *   - 参考开源: SparkFun_u-blox_GNSS_Arduino_Library 的 powerOff()/powerOffWithInterrupt()
 *     (常量 VAL_RXM_PMREQ_WAKEUPSOURCE_UARTRX=0x08, EXTINT0=0x20 …)。
 *
 * UBX-RXM-PMREQ 长版 (16B payload, class 0x02 id 0x41):
 *   [0..3]  duration      = 0            不限时, 直到唤醒源触发
 *   [4..7]  flags         = 0x02 | 0x04  bit1 backup + bit2 force
 *   [8..11] wakeupSources = 0x08         UART RX
 *   [12..15] reserved     = 0
 */
#include "gps.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "board_pins.h"

static const char *TAG = "GPS";

#define UBX_SYNC1       0xB5
#define UBX_SYNC2       0x62
#define UBX_CLS_RXM     0x02
#define UBX_ID_PMREQ    0x41
#define UBX_CLS_MON     0x0A
#define UBX_ID_MONVER   0x04

#define GPS_RX_BUF      1024
#define GPS_TX_BUF      1024

static gps_config_t       s_cfg;
static bool               s_ready = false;
/* ★ 2026-10-26: gps_init 全程标记 (含 autobaud 阻塞读 UART 的 0~3.6s),
 *   期间禁止 gps_release() 删 UART 驱动——否则两个任务抢同一 ringbuf
 *   会撞 `assert failed: spinlock_acquire`。 */
static volatile bool      s_initing = false;
static gps_power_state_t  s_state = GPS_POWER_UNKNOWN;
/* ★ 2026-10-27: "确认已入睡"(实测 1.2s 无输出)。管家任务靠它判断要不要再补一次，
 *   避免每 30s 都去装 UART(装 UART 会重新配置 TX pad，那一下的电平跳变会把模块叫醒)。 */
static volatile bool s_sleep_verified = false;
/* ★★★ 2026-10-28: 本次上电是否**真的收到过有效帧**(NMEA 的 '$G' / UBX 的 B5 62)。
 *   为什么必须单独记: gps_boot_park() 面对"模块没插"和"模块正在睡"两种情况都是 got==0,
 *   返回值都是 ESP_OK —— 上层无法据此判断"这块板到底有没有 GPS"。
 *   有了它, 开机探测就能明确区分: 收到过帧=有模块; 一次没收到=可能没插。
 *   用途(linit 要求): 前 3 次开机各探一次, 探到就不必再探; 3 次都没探到就不再主动碰。 */
static volatile bool s_seen_frame = false;
bool gps_seen_valid_frame(void) { return s_seen_frame; }

/* ═══════════════════════════════════════════════════════════════════════════
 * 2026-10-28 睡眠状态机 + 诊断 (linit 任务书)
 *   目的: 把"发命令 / TX 完成 / UART 静默 / 真进低功耗"四件事分开记,
 *         禁止把"串口安静了"直接当成"已进入低功耗"。
 *   ⚠ 判据分级:
 *     · VERIFIED   = 命令已发 + TX 已确认完成 + 全程无接收活动 (仍是**软件推测**,
 *                    没有独立电流测量就不算硬件确认)
 *     · UNCERTAIN  = 只看到 UART 静默, 但没走完完整流程 (例如发送失败却恰好没数据)
 *     · FAILED     = 明确失败证据 (发送失败 / 超时后仍在收有效 NMEA)
 * ═══════════════════════════════════════════════════════════════════════════ */
/* 枚举定义在 gps.h (gps_sm_t), 这里只放运行时状态与工具函数 */

static volatile gps_sm_t s_sm = GPS_SM_IDLE;
static const char *gps_sm_name(gps_sm_t s)
{
    switch (s) {
        case GPS_SM_IDLE:       return "IDLE";
        case GPS_SM_ACTIVE:     return "ACTIVE";
        case GPS_SM_PREPARING:  return "PREPARING";
        case GPS_SM_WAIT_IDLE:  return "WAIT_IDLE";
        case GPS_SM_PMREQ_SENT: return "PMREQ_SENT";
        case GPS_SM_WAIT_SLEEP: return "WAIT_SLEEP";
        case GPS_SM_VERIFIED:   return "VERIFIED";
        case GPS_SM_UNCERTAIN:  return "UNCERTAIN";
        case GPS_SM_FAILED:     return "FAILED";
        default:                return "?";
    }
}
/* 状态切换 + 一行日志 (只在变化时打, 深睡流程里不会刷屏) */
static void gps_sm_set(gps_sm_t s, const char *why)
{
    if (s_sm == s) return;
    ESP_LOGW(TAG, "[SM] %s → %s%s%s", gps_sm_name(s_sm), gps_sm_name(s),
             why ? " : " : "", why ? why : "");
    s_sm = s;
}
gps_sm_t gps_sleep_state(void) { return s_sm; }

/* 睡眠诊断 (每次尝试覆盖写, 供 remcon/日志查证) */
static struct {
    uint32_t tx_ms;         /* PMREQ 发出时刻 (ms since boot) */
    uint32_t tx_done_ms;    /* TX 完成耗时 (0=未等到) */
    int      rx_before;     /* 发送前 UART 缓冲里未读字节 */
    int      rx_after;      /* 判定窗口内又收到的字节 */
    uint32_t verify_ms;     /* 睡眠判定耗时 */
    int      retries;       /* 本轮重试次数 */
    bool     psmoo_sent;    /* 是否下发过 PSMOO */
    bool     psmoo_ok;      /* PSMOO 是否观察到静默 */
} s_diag;

/* ==== 默认配置 ====
 * ★ 2026-10-05 小智模块板 (分支 board/xiaozhi-s3-core):
 *   TX=43 / RX=44 不变 —— 模块脚 36/37 正是 `RXD0(TXD0)` = **GPIO44/GPIO43**，都在 24 个可用脚内 ✓
 *   ⛔ **EXTINT 从 47 改成 -1(不接)**: GPIO47 在本模块上**没有引出** ——
 *      模块内部拿它当 `codec 的 I2C_SCL`，同时焊盘 24 是 `VSYS_IN`(电源开关进)。
 *      固件再去驱动 47 = 一边搅 codec 控制总线、一边扯电源通路 ⇒ 必须弃用。
 * ⇒ EXTINT 挪到 **GPIO8**(linit 2026-10-05 定)。选它的理由:
 *    8/9/16 是模块上仅剩的三个空闲脚, 其中 **16 = `XTAL_32K_N`**(和 15 = `XTAL_32K_P` 是一对)，
 *    **留给以后上 32.768kHz 晶振做 RTC 低功耗时钟**，所以不用它；
 *    8 是纯普通 IO(TOUCH8 / ADC1_CH7 / SUBSPICS1，没有第二身份)，拿来当输入最省事。
 *    (备选 GPIO9; 若以后要撤, 把下面改回 -1 即可。) */
static gps_config_t s_default_cfg = {
    .uart_num    = UART_NUM_1,   /* console 是 USB-Serial-JTAG, UART0/1 都空; 用 1 避开 ROM 默认口 */
    .pin_tx      = 43,           /* 模块脚 37 (TXD0) */
    .pin_rx      = 44,           /* 模块脚 36 (RXD0) */
    .pin_extint  = 18,           /* 由板型表覆盖, 见 default_cfg_bound() */
    /* ★ 2026-10-24: 模块确认 = **MAX-M10S-00B-01**, u-blox M10 出厂默认 **38400**。
     * 开机时 app_board 就把模块送进 `UBX-RXM-PMREQ(backup)`, 而 backup 模式
     * **不保留接收机配置** ⇒ 唤醒后模块回到出厂 38400。
     * 固件固定 38400 即与"睡醒即默认"天然对齐, 不需要唤醒后 UBX-CFG-PRT 重配。
     * 1Hz NMEA 整轮 ≈ 1.5KB, 38400 只需 ~320ms 传完, 余量充足。 */
    .baud        = 38400,
    .drive_extint = false,       /* 默认不驱动 EXTINT: 极性未确认, 悬空/误电平会自己叫醒模块 */
};

static bool s_default_bound = false;

/* ★ 2026-10-18: 引脚一律按**板型表运行时**下发（小智 43/44/18 · 微雪/标准扩展 43/44/47）。
 *   原来是 `BOARD_IS_WEIXUE ? 47 : 18` 的编译期常量 ⇒ 一个固件换板后 GPS 引脚不会变，
 *   在微雪板上等于把 EXTINT 接到小智的 18 号脚。 */
static const gps_config_t *default_cfg_bound(void) {
    if (!s_default_bound) {
        const board_pins_t *bp = board_pins_get();
        s_default_cfg.pin_tx     = bp->gps_tx;
        s_default_cfg.pin_rx     = bp->gps_rx;
        s_default_cfg.pin_extint = bp->gps_extint;
        s_default_bound = true;
    }
    return &s_default_cfg;
}
const gps_config_t *gps_default_config(void) { return default_cfg_bound(); }
bool gps_is_ready(void) { return s_ready; }
uint32_t gps_baud(void) { return s_cfg.baud; }
gps_power_state_t gps_power_state(void) { return s_state; }
bool gps_sleep_verified(void) { return s_sleep_verified; }
bool gps_is_busy(void) { return s_initing; }

/* ---- UBX 组帧 (含 8-bit Fletcher 校验) ---- */
static size_t ubx_build(uint8_t *out, uint8_t cls, uint8_t id, const uint8_t *pl, uint16_t n)
{
    size_t i = 0;
    out[i++] = UBX_SYNC1;
    out[i++] = UBX_SYNC2;
    out[i++] = cls;
    out[i++] = id;
    out[i++] = (uint8_t)(n & 0xFF);
    out[i++] = (uint8_t)(n >> 8);
    if (n && pl) { memcpy(&out[i], pl, n); i += n; }
    uint8_t a = 0, b = 0;
    for (size_t k = 2; k < i; k++) { a = (uint8_t)(a + out[k]); b = (uint8_t)(b + a); }
    out[i++] = a;
    out[i++] = b;
    return i;
}

esp_err_t gps_send_ubx(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    uint8_t frame[64];
    if ((size_t)len + 8 > sizeof(frame)) return ESP_ERR_INVALID_SIZE;
    size_t n = ubx_build(frame, cls, id, payload, len);
    int w = uart_write_bytes(s_cfg.uart_num, (const char *)frame, n);
    /* ★ 2026-10-25 linit: 不再用 uart_wait_tx_done 阻塞等 TX 完成 ——
     *   在"首次识别/重装 UART 驱动"的窗口里它会在临界区里等信号量,
     *   撞 `assert failed: spinlock_acquire (lock->count == 0)` ⇒ 开机循环重启。
     *   UBX 帧 ≤ 64B, 38400bps 下 ~17ms 发完, 用一个短延时替代即可。 */
    vTaskDelay(pdMS_TO_TICKS(20));
    return (w == (int)n) ? ESP_OK : ESP_FAIL;
}

int gps_read(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    if (!s_ready || !buf) return -1;
    int n = uart_read_bytes(s_cfg.uart_num, buf, max_len, pdMS_TO_TICKS(timeout_ms));
    return n;
}

/* ★ 2026-10-09: 自动波特率 ------------------------------------------------
 * 背景: 开机就把模块送进 UBX-RXM-PMREQ(backup), 而 backup 会丢接收机配置 ⇒ 唤醒后
 * 波特率回到 u-blox 出厂值(MAX-M10S = 38400), 也可能被厂商固化在别的值。
 * **靠固定值猜只会一直收乱码** (实测: 探测单次就读到 256 字节 ⇒ 线上速率远高于配置值, 却解析不出一句 NMEA)。
 * 做法: 挨个试常见波特率, 谁先看到 UBX 同步头(B5 62)或 NMEA 头($G) 就用谁。
 * ⚠ 必须在 gps_sleep() 之前完成 —— 否则 PMREQ 发不出去, 模块会一直连续跟踪(~10mA)。
 *
 * ★★★ 2026-10-20 修"乱识别": 原来认不出来时**返回配置值**, 调用方就打印
 *   "9600 bps 确认" —— 没焊模块也报成功, 接着还去发 PMREQ, 白跑 2 秒。
 *   现在认不出来一律返回 **0 = 没有任何 GPS 应答**(模块不在/没接/没供电)。 */
/* ★ 2026-10-24: 候选表按**可能性**排, 38400 提首位 —— MAX-M10S-00B-01 出厂就是 38400,
 *   开机/睡醒常态下第 1 档即命中(≈240ms), 不必再空跑 9600。
 *   后面几档只为"模块被别的主机改过速率"兜底。 */
/* ★ 2026-10-24: 数据手册 §5.3 注 32 —— **默认 GSV 每 5 秒才发一次**
 *   (避免 TX 缓冲溢出)。卫星页要的是每颗星的仰角/方位/信噪, 5 秒一更太慢。
 *   这里用 M10 原生的 UBX-CFG-VALSET(0x06/0x8A) 把
 *   `CFG_MSGOUT_NMEA_ID_GSV_UART1` 置 1 (=每次导航解, 1Hz), 只写 RAM 层
 *   (掉电即失, 与"睡进 backup 就回出厂设置"的现有策略一致, 每次 init 重发)。
 * ⚠ 只在 baud ≥ 38400 时提频: 9600 下整轮 NMEA 已接近链路极限(≈960B/s),
 *   再塞 4 组 GSV 会丢句。9600 保持默认 5 秒。
 * 失败(模块 NAK / 老固件)最多打一条日志, 不致命。 */
#define UBX_CLS_CFG     0x06
#define UBX_ID_VALSET   0x8A
#define CFG_MSGOUT_NMEA_ID_GSV_UART1  0x209100C1u
static void gps_boost_gsv(uint32_t baud)
{
    if (baud < 38400) return;
    uint8_t pl[9];
    pl[0] = 0x00;                       /* version */
    pl[1] = 0x01;                       /* layers: RAM */
    pl[2] = 0x00; pl[3] = 0x00;         /* transactionID / reserved */
    pl[4] = (uint8_t)(CFG_MSGOUT_NMEA_ID_GSV_UART1);
    pl[5] = (uint8_t)(CFG_MSGOUT_NMEA_ID_GSV_UART1 >> 8);
    pl[6] = (uint8_t)(CFG_MSGOUT_NMEA_ID_GSV_UART1 >> 16);
    pl[7] = (uint8_t)(CFG_MSGOUT_NMEA_ID_GSV_UART1 >> 24);
    pl[8] = 0x01;                       /* 每次导航解输出一次 */
    esp_err_t e = gps_send_ubx(UBX_CLS_CFG, UBX_ID_VALSET, pl, sizeof(pl));
    ESP_LOGI(TAG, "GSV 提频至 1Hz: %s (UBX-CFG-VALSET)", e == ESP_OK ? "已发送" : "发送失败");
}

static uint32_t gps_autobaud(void)
{
    static const uint32_t cand[] = { 38400, 9600, 115200, 57600, 230400, 921600 };
    uint8_t buf[128];
    /* ★★★ 2026-10-24 关键修复: **必须先敲 RX 把模块唤醒**。
     *   MAX-M10S 集成手册 §2.7.3.2: 进 software standby(UBX-RXM-PMREQ) 后
     *   VCC 内部关断、**接收机停止输出**, 只有 UART RX 上出现电平变化(任意字节)
     *   或 EXTINT 才会醒。u-blox 支持社区同样答复: "send it a byte like 0x00"。
     *   而本机每次开机 app_board 都会把模块送进 standby ⇒ 下次开机它一定是哑的;
     *   旧代码只"听"不发 ⇒ 6 档候选全超时, 永远报"未检测到 GPS 模块"。
     *   现在: 每档候选先发 64 个 0x00 当"敲门砖"(波特率错了也无所谓, 只要线上有跳变),
     *   模块醒后按自己的波特率开始吐 NMEA, 再靠下面的嗅探把它抓出来。 */
    static const uint8_t knock[64] = { 0 };
    for (size_t i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
        uart_set_baudrate(s_cfg.uart_num, cand[i]);
        uart_flush_input(s_cfg.uart_num);
        uart_write_bytes(s_cfg.uart_num, (const char *)knock, sizeof(knock));
        vTaskDelay(pdMS_TO_TICKS(20));   /* ★ 同上: 不用 uart_wait_tx_done, 见 gps_send_ubx 注释 */
        /* 醒来的模块要几十~几百 ms 才吐出第一句, 所以每档给 600ms 连续窗口
         * (旧代码 4×60ms 只覆盖 1Hz NMEA 的 24% 概率, 就算模块醒着也常常扑空) */
        for (int k = 0; k < 12; k++) {                /* 12 × 50ms ≈ 600ms / 候选 */
            int n = uart_read_bytes(s_cfg.uart_num, buf, sizeof(buf), pdMS_TO_TICKS(50));
            for (int j = 0; j + 1 < n; j++) {
                if (buf[j] == 0xB5 && buf[j + 1] == 0x62) return cand[i];               /* UBX */
                if (buf[j] == '$' && (buf[j + 1] == 'G' || buf[j + 1] == 'P')) return cand[i]; /* NMEA */
            }
        }
    }
    uart_set_baudrate(s_cfg.uart_num, s_cfg.baud);
    return 0;                                          /* 一个候选都没应答 ⇒ 模块不在/没电/线反了 */
}

/* ★★★ 2026-10-25 临时屏蔽 (linit 指定, 模拟器调试期间): 置 0 即恢复。
 *   屏蔽期间: 不装 UART、不发 UBX、不探测 —— 开机路径完全跳过 GPS。
 *   起因: 开机 "首次识别" 失败的收尾路径里 uart_wait_tx_done 撞 spinlock 断言(
 *         assert failed: spinlock_acquire), 导致 6~11 次重启, 卡住 SD/音频验证。 */
#define GPS_HW_DISABLED 0   /* 0 = 正常启用 (GPS 只在模拟器侧移除模型) */

esp_err_t gps_init(const gps_config_t *cfg)
{
#if GPS_HW_DISABLED
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;          /* 屏蔽期: 一律按"没有模块"处理 */
#endif
    if (s_initing) return ESP_ERR_INVALID_STATE;   /* 并发重入: 上一次还没结束 */
    s_initing = true;
    s_cfg = cfg ? *cfg : *default_cfg_bound();

    esp_err_t e = uart_driver_install(s_cfg.uart_num, GPS_RX_BUF, GPS_TX_BUF, 0, NULL, 0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "UART%d 驱动安装失败: %s", (int)s_cfg.uart_num, esp_err_to_name(e));
        s_initing = false;
        return e;
    }
    const uart_config_t uc = {
        .baud_rate  = (int)s_cfg.baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(s_cfg.uart_num, &uc));
    /* ★ 2026-10-27: 上一次 gps_sleep() 把 TX 钉成 GPIO 高并 hold 了 (防待机被噪声唤醒)。
     *   这里必须**先解除 hold**, 否则 pad 被冻住, UART 根本发不出去(GPS 从此失联)。 */
    if (s_cfg.pin_tx >= 0) gpio_hold_dis((gpio_num_t)s_cfg.pin_tx);
    uart_set_pin(s_cfg.uart_num, s_cfg.pin_tx, s_cfg.pin_rx,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    if (s_cfg.pin_extint >= 0) {
        gpio_config_t g = {
            .pin_bit_mask = (1ULL << s_cfg.pin_extint),
            .mode = s_cfg.drive_extint ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT,
            .pull_up_en = s_cfg.drive_extint ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&g);
        if (s_cfg.drive_extint) gpio_set_level(s_cfg.pin_extint, 0);   /* 不驱动时留低, 见 gps_wakeup */
    }


    /* 定出模块真实波特率 (见 gps_autobaud 注释)。这一步也是最好的"接线是否通"判据:
     * 打出来的实测值就是模块当前在用的速率, 认不出来说明线/供电有问题。 */
    uint32_t real = gps_autobaud();
    if (real == 0) {
        /* ★★★ 2026-10-20: 没有任何 UBX/NMEA 应答 ⇒ 模块没焊/没接/没供电。
         *   **必须干净退出**: 卸掉 UART(否则一路外设常驻 + 内部 RAM 被占),
         *   把 GPS 引脚还原, 不留下任何后台耗电。调用方据返回值判 gps_ok=0。 */
        ESP_LOGW(TAG, "未检测到 GPS 模块(6 档波特率均无 UBX/NMEA 应答) ⇒ 放弃初始化");
        if (s_cfg.pin_extint >= 0 && !s_cfg.drive_extint) gpio_reset_pin((gpio_num_t)s_cfg.pin_extint);
        uart_driver_delete(s_cfg.uart_num);
        s_ready = false;
        s_state = GPS_POWER_UNKNOWN;
        s_initing = false;
        return ESP_ERR_NOT_FOUND;
    }
    if (real != s_cfg.baud) {
        ESP_LOGD(TAG, "自动波特率: 配置 %u → 实测 %u bps (模块已改用它)",
                 (unsigned)s_cfg.baud, (unsigned)real);
        s_cfg.baud = real;
    } else {
        ESP_LOGI(TAG, "自动波特率: %u bps 确认", (unsigned)real);
    }

    /* ★★★ 2026-10-26 linit 修复 (开机循环重启):
     *   s_ready / ACTIVE 原来在 autobaud **之前** 就置位——而 autobaud 最长要阻塞 3.6s
     *   读 UART。这段窗口里主循环每秒的“GPS 无人使用 → 送回待机”兜底
     *   会看到 ACTIVE ⇒ gps_release() ⇒ uart_driver_delete(), 而本任务还在 uart_read_bytes,
     *   ringbuf 句柄已销毁 ⇒ `assert failed: spinlock_acquire` ⇒ 重启。
     *   现在: 真正探测成功后才置 ready/ACTIVE。 */
    s_ready = true;
    s_state = GPS_POWER_ACTIVE;
    gps_boost_gsv(s_cfg.baud);   /* 见 gps_boost_gsv: GSV 默认 5s 一次 → 提到 1Hz */

    ESP_LOGI(TAG, "初始化完成: TX=G%d RX=G%d EXTINT=%s%d @%u bps",
             s_cfg.pin_tx, s_cfg.pin_rx, s_cfg.drive_extint ? "驱动" : "监听",
             s_cfg.pin_extint, (unsigned)s_cfg.baud);
    s_initing = false;                    /* 探测成功: 解除并发保护 */
    return ESP_OK;
}

/* ★★★ 2026-10-27 linit 实测「关机还有 0.1W」修复 ★★★
 *   官方 Integration Manual 2.7.3.2: 软件待机时 **V_IO 保持供电、PIO 可被主机驱动**,
 *   唤醒源由 PMREQ 的 wakeupSources 指定(我们用 UART RX = bit3)。
 *   ⇒ 待机期间只要 GPS 的 RXD 上出现一个边沿, 它立刻醒, 醒了就再没人让它睡
 *     (整夜 10~25mA)。而 ESP 这边 `uart_driver_delete()` 会让 pad 短暂**浮空**,
 *     这点噪声就足够唤醒 —— 这是"PMREQ 发了却没睡"的真正原因。
 *   对策: PMREQ 发完**立刻**把 TX 引脚切成 GPIO 输出高并 hold, 再卸 UART。
 *   (此函数幂等, 可重复调用) */
void gps_tx_hold_high(void)
{
    if (s_cfg.pin_tx < 0) return;
    gpio_set_direction((gpio_num_t)s_cfg.pin_tx, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)s_cfg.pin_tx, 1);      /* UART 空闲电平 = 高 */
    /* ★ 2026-10-28: 小智一体版的 GPIO41 在板型表里**没有任何外部上拉**(见 docs/硬件与IO.md
     *   "上拉实测"表), 而它正好就是 GPS 的 RXD 那根线。开内部上拉(~45k)成本为零:
     *   ① pad 若因任何原因失去 hold(深睡/复位/驱动切换), 这根线仍有确定电平;
     *   ② gpio_hold_en 锁存的是"当前有效配置", 所以上拉会一起被锁进深睡。
     *   外部 10k 仍然更稳(内部上拉偏弱), 但软件这一层先把它兜住。 */
    gpio_pullup_en((gpio_num_t)s_cfg.pin_tx);
    gpio_hold_en((gpio_num_t)s_cfg.pin_tx);
}

/* 判断"真的睡了": 数 150ms 内还收到多少字节。
 *   ⚠ 不能用 UBX-ACK 判断 —— 官方 Integration Manual §2.7.3.2: 软件待机时
 *     **VCC 被内部关断**, 模块根本没法回 ACK。所以 ACK 缺失是正常的。
 *   可靠的判据是"它不再说话": 1Hz NMEA 每 200~500ms 就有一帧, 150ms 静默 + 前面
 *   已强制 TX 高电平(无唤醒边沿) ⇒ 基本可确认已入睡。 */
static int gps_quiet_bytes(uint32_t ms)
{
    uint8_t b[64];
    int total = 0;
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    while ((uint32_t)(esp_timer_get_time() / 1000) - t0 < ms) {
        int n = uart_read_bytes(s_cfg.uart_num, b, sizeof(b), pdMS_TO_TICKS(30));
        if (n > 0) total += n;
    }
    return total;
}

/* ★★★ 2026-10-27 【根因】UBX-RXM-PMREQ 有两套载荷布局, 原来把老偏移套在新长度上 ★★★
 *   ① protocol 23 及更早 (u-blox 8/9):
 *        8 字节 = [0..3] duration | [4..7] flags
 *   ② protocol 27 (u-blox M10 —— 我们的 MAX-M10S):
 *        16 字节 = [0] version=0 | [1..3] reserved1 | [4..7] duration
 *                  | [8..11] flags | [12..15] wakeupSources
 *   原代码: pl[4]=0x06(flags) + pl[8]=0x08(wakeup), 长度 16 —— 偏移是①的, 长度是②的。
 *   M10 按②解读 ⇒ duration=6ms、flags=0x08(既没 backup 也没 force)、
 *   wakeupSources=0(**没配唤醒源**) ⇒ "睡 6 毫秒就醒, 醒后再也睡不着"。
 *   现象完全对上: 日志偶报"已确认入睡", 但电流长期 10~25mA。
 *   标志位(两套布局同义): flags bit1=backup(0x02) | bit2=force(0x04);
 *                        wakeupSources bit3=uartrx(0x08)。
 *   ⇒ 现在按②正确构造, 并以①做兜底(两种都能发, 不匹配的一方会被模块丢弃, 无害)。 */
static uint16_t pmreq_build(uint8_t *pl, bool proto27, uint32_t duration_ms)
{
    memset(pl, 0, 16);
    if (proto27) {
        pl[0]  = 0;                                        /* version */
        pl[4]  = (uint8_t)(duration_ms & 0xFF);            /* duration, 0 = 无限期 */
        pl[5]  = (uint8_t)((duration_ms >> 8) & 0xFF);
        pl[6]  = (uint8_t)((duration_ms >> 16) & 0xFF);
        pl[7]  = (uint8_t)((duration_ms >> 24) & 0xFF);
        /* ★★★ 2026-10-28 定稿: **backup + force 两个标志位都必须置** ★★★
         *   官方 Integration Manual §2.7.3.2 只写了 "force flag must be set",
         *   但 u-blox 论坛的实测反馈是: 只置 force(0x04) 时模块**毫无反应、电流不降**,
         *   必须再加 backup(0x02) 才真正进睡眠, 到期或被 EXTINT 唤醒。
         *   (本板 V_BCKP 悬空, 按官方 §2.7.3.2 "V_BCKP should be left open if not used" 处理,
         *    置 backup 位不等于要求外部断电, 它让模块使用 backup 域进入低功耗。)
         *   ⇒ flags = 0x02 | 0x04 = 0x06。上次改成 0x04 是错的, 这里改回。 */
        pl[8]  = 0x02 | 0x04;                              /* backup + force */
        pl[12] = 0x08;                                     /* wakeupSources = UART RX */
        return 16;
    }
    pl[0] = (uint8_t)(duration_ms & 0xFF);                 /* duration */
    pl[1] = (uint8_t)((duration_ms >> 8) & 0xFF);
    pl[2] = (uint8_t)((duration_ms >> 16) & 0xFF);
    pl[3] = (uint8_t)((duration_ms >> 24) & 0xFF);
    pl[4] = 0x02 | 0x04;                                   /* backup + force (同 16B 分支) */
    return 8;
}

/* ==== PSMOO 参数 (2026-10-28) ====
 * ⚠ 必须定义在 gps_sleep_rounds() 之前 —— 它的失败分支要用这些值做显式策略判断。
 * MAXACQ 原为 60 秒: 本板天线频段不对(433/470MHz ≠ 1575.42), 永远搜不到星,
 * 每次都会跑满 MAXACQ 才关 ⇒ 压到 15 秒, 平均电流降一个档, "多久能睡下"也从 1 分钟变 15 秒。 */
#define GPS_PSMOO_PERIOD_S     3600    /* 周期: 每小时一次 */
#define GPS_PSMOO_ONTIME_S     2       /* 每次开机定位时长 */
#define GPS_PSMOO_MAXACQ_S     15      /* 搜星上限 15 秒, 超时强制关 */
/* PMREQ 失败后是否允许走 PSMOO 兜底 —— **显式开关**, 不埋在逻辑里。
 * 1 = 允许(默认: PMREQ 实测多不被受理, 不兜底就整夜 20~25mA);
 * 0 = 只发 PMREQ, 失败如实报 FAILED, 交上层/硬件断电。
 * ⚠ 语义不同: PMREQ 求的是**永久待机**, PSMOO 是**周期性省电**, 不能混称。 */
#define GPS_PSMOO_FALLBACK_EN  1

static bool gps_wait_steady(uint32_t quiet_ms, uint32_t max_ms);   /* 前向 */
static esp_err_t gps_sleep_rounds(int max_rounds);
static bool gps_enter_psmoo(void);                                 /* 前向: PMREQ 失效时的兜底 */

esp_err_t gps_sleep(void) { return gps_sleep_rounds(3); }

static esp_err_t gps_sleep_rounds(int max_rounds)
{
    /* ★★★ 2026-10-27 linit 修复: 原来这里直接 return ESP_ERR_INVALID_STATE,
     *   导致所有"还没 init 就要求送睡"的调用(关机路径 / 无人使用兜底)
     *   **全都变成空操作** —— 模块整夜按 10~25mA 跑。 */
    if (!s_ready) return gps_boot_park();

    /* ★★★ 2026-10-27 最终形态（依据 MAX-M10S Data Sheet Table 10 + Integration Manual §2.7.3.2）
     *   每轮: **发 1 帧**(不能连发, 见下) → TX 钉高 → 丢 350ms 尾巴 → 静默 2.6s + 600ms 复核。
     *   多轮只用于关机路径(gps_sleep=3 轮, 布局 16B/8B/16B), 开机停靠只用 1 轮(gps_sleep_rounds(1))。
     *   为什么不能连发: Data Sheet Table 10 —— 软件待机时 **RXD 仍是 "Input pull-up"**,
     *   而 PMREQ 指定的唤醒源正是 UART RX ⇒ 我们再发一帧, 第一个下降沿就把它叫醒了。 */
    gps_sm_set(GPS_SM_PREPARING, "开始送待机, 清空本轮诊断");
    memset(&s_diag, 0, sizeof(s_diag));

    uint8_t pl[16];
    esp_err_t last = ESP_OK;
    int last_still = 0;
    /* ★ 2026-10-28: 多轮改用「布局 × duration」组合表, 每轮只发一帧。
     *   论坛实测提示 duration 也要试非零值(定时唤醒), 所以第 2 轮用 60 秒。 */
    static const struct { bool proto27; uint32_t dur; } rounds_tab[3] = {
        { true,  0      },      /* protocol27 · duration=0   (无限期) */
        { true,  60000  },      /* protocol27 · duration=60s (定时唤醒) */
        { false, 0      },      /* protocol23 · 老布局兜底 */
    };

    for (int round = 0; round < max_rounds; round++) {
        const int ri = (round < 3) ? round : 2;
        bool proto27 = rounds_tab[ri].proto27;
        uint16_t n = pmreq_build(pl, proto27, rounds_tab[ri].dur);
        /* ★ 2026-10-28: 发之前必须等稳态(见 gps_wait_steady 注释) —— 这是"时好时坏"的真正原因 */
        gps_sm_set(GPS_SM_WAIT_IDLE, "等 1Hz 输出出现静默窗口");
        if (round == 0) gps_wait_steady(300, 3000);

        /* ★ 任务书要求: 清缓存**之前**先记账, 别把测量证据一起清掉。
         *   uart_get_buffered_data_len() 是驱动自己的"未读字节数", 拿不到就记 -1, 不虚构。 */
        {
            size_t pre = 0;
            s_diag.rx_before = (uart_get_buffered_data_len(s_cfg.uart_num, &pre) == ESP_OK)
                               ? (int)pre : -1;
            ESP_LOGI(TAG, "PMREQ 前: UART 未读 %d 字节(即将清理, 只为去掉在途残帧)",
                     s_diag.rx_before);
        }
        uart_flush_input(s_cfg.uart_num);
        /* ★★★ 2026-10-27 【关键】只发**一帧**, 不能连发。
         *   理由: PMREQ 配的唤醒源就是 UART RX —— 模块一入睡, 我们**再发一帧**时
         *   线上第一个下降沿就把它叫醒了。连发 3 帧等于"让它睡→叫醒→再睡→再叫醒",
         *   最终态取决于时序, 这就是"有时像睡了、有时一直醒"的来源。
         *   SparkFun 与官方示例都是单帧发完就等, 不再碰总线。 */
        s_diag.tx_ms    = (uint32_t)(esp_timer_get_time() / 1000);
        s_diag.retries  = round;
        last = gps_send_ubx(UBX_CLS_RXM, UBX_ID_PMREQ, pl, n);
        if (last != ESP_OK) {
            ESP_LOGE(TAG, "PMREQ(%s) 发送失败: %s ⇒ 本轮作废, **不改 TX 引脚状态**",
                     proto27 ? "protocol27/16B" : "protocol23/8B", esp_err_to_name(last));
            continue;
        }
        gps_sm_set(GPS_SM_PMREQ_SENT, "帧已写入 UART FIFO");

        /* ★★★ 任务书修改 1: 等 UART 真正发完, 再动 TX 引脚 ★★★
         *   原来 gps_send_ubx() 只把帧写进 FIFO 就返回, 20ms 后立刻 gps_tx_hold_high():
         *   若最后一个字节还在移位寄存器里, hold 引脚会把它截断 ⇒ 模块收到坏帧静默丢弃,
         *   现象正好是"命令发了却完全没反应"。
         *   (gps_send_ubx 内部刻意不调 uart_wait_tx_done: 它取信号量, 在"刚重装 UART 驱动"
         *    的窗口里会撞 spinlock_acquire 断言 —— 见该函数 2026-10-25 的注释。
         *    本调用点已确定不在初始化窗口, 所以这里用带超时的版本是安全的。)
         *   ⚠ uart_wait_tx_done 只证明 **UART 发送端发完了**;
         *     不证明模块解析了命令, 更不证明它进入低功耗。 */
        {
            uint32_t td0 = (uint32_t)(esp_timer_get_time() / 1000);
            esp_err_t te = uart_wait_tx_done(s_cfg.uart_num, pdMS_TO_TICKS(120));
            s_diag.tx_done_ms = (uint32_t)(esp_timer_get_time() / 1000) - td0;
            if (te != ESP_OK) {
                ESP_LOGE(TAG, "PMREQ 帧长 %uB, TX 完成确认失败(%s, 等了 %ums) ⇒ 本轮作废, 不 hold 引脚",
                         (unsigned)n, esp_err_to_name(te), (unsigned)s_diag.tx_done_ms);
                last = te;
                continue;
            }
            ESP_LOGI(TAG, "PMREQ 帧长 %uB, TX 已确认发完(耗时 %u ms)",
                     (unsigned)n, (unsigned)s_diag.tx_done_ms);
        }
        /* TX 必须在卸载 UART 之前钉高, 否则 pad 浮空会被当 RXD 起始位叫醒 */
        gps_tx_hold_high();
        gps_sm_set(GPS_SM_WAIT_SLEEP, "观察接收活动 3.2s");
        uint32_t vt0 = (uint32_t)(esp_timer_get_time() / 1000);
        /* ★★★ 判据必须 ≥2.5 秒。实测: NMEA 是 1Hz 突发输出, 两包之间天然有 ~700ms 空档,
         *   原来 700+500ms 会**误判成已睡**(前面那些"已确认入睡"全是假阳性)。
         *   2.6s 覆盖两个完整输出周期, 收不到字节才算真进 backup。 */
        /* ★ 先丢掉"在途尾巴": PMREQ 生效瞬间, UART RX ring/FIFO 里还剩半帧,
         *   不清掉会被算成"仍在输出"(实测: 单帧发完后仍读到 34 字节 = 纯尾巴)。 */
        gps_quiet_bytes(350);
        int still = gps_quiet_bytes(2600);
        if (still == 0) {
            still = gps_quiet_bytes(600);
            if (still == 0) {
                s_diag.rx_after  = 0;
                s_diag.verify_ms = (uint32_t)(esp_timer_get_time() / 1000) - vt0;
                s_state = GPS_POWER_SLEEP;
                s_sleep_verified = true;
                gps_sm_set(GPS_SM_VERIFIED, "TX 已确认完成 + 3.2s 无接收活动");
                /* ⚠ 任务书要求: 这里只能报"软件推测", 不能报"已确认低功耗" */
                ESP_LOGI(TAG, "睡眠判定=VERIFIED(软件推测) · 第%d轮 · %s · 帧长%uB · 判定%ums"
                              " · ⚠未做独立电流测量, 不等于硬件确认",
                         round + 1, proto27 ? "protocol27/16B" : "protocol23/8B",
                         (unsigned)n, (unsigned)s_diag.verify_ms);
                return ESP_OK;
            }
        }
        last_still = still;
        s_diag.rx_after = still;
        ESP_LOGW(TAG, "PMREQ(%s) 第%d轮 后仍在输出 %d 字节(判定 %ums) ⇒ 有限重试",
                 proto27 ? "protocol27/16B" : "protocol23/8B", round + 1, still,
                 (unsigned)((uint32_t)(esp_timer_get_time() / 1000) - vt0));
    }

    /* ---- 全部轮次失败 ---- */
    s_state = GPS_POWER_ACTIVE;
    s_sleep_verified = false;
    gps_sm_set(GPS_SM_FAILED, "PMREQ 未被受理(仍收到有效数据)");
    ESP_LOGE(TAG, "PMREQ 多轮无效: 发送前 %d 字节 / 最后仍收 %d 字节 / TX 完成 %ums",
             s_diag.rx_before, last_still, (unsigned)s_diag.tx_done_ms);

    /* ---- 任务书修改 4: 失败后走**显式**策略分支, 不再无条件 PSMOO ---- */
    if (!GPS_PSMOO_FALLBACK_EN) {
        ESP_LOGE(TAG, "PMREQ 失败且 PSMOO 兜底关闭 ⇒ 模块保持 ACTIVE(20~25mA), 交上层重试");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "PMREQ 失败 ⇒ 走 PSMOO 兜底(**周期省电, 不是永久待机**)");
    s_diag.psmoo_sent = true;
    s_diag.psmoo_ok   = gps_enter_psmoo();
    if (!s_diag.psmoo_ok) {
        gps_sm_set(GPS_SM_FAILED, "PSMOO 配置失败或未观察到省电");
        ESP_LOGE(TAG, "PSMOO 兜底失败 ⇒ **不能宣称已省电**, 模块仍在 ACTIVE");
        return ESP_FAIL;
    }
    /* PSMOO 成功只能报"已配置周期省电", 不能报"永久待机" */
    gps_sm_set(GPS_SM_UNCERTAIN, "PSMOO 周期模式(每小时醒一次), 非 standby");
    ESP_LOGW(TAG, "PSMOO 已配置: 周期 %us / on %us / maxacq %us ⇒ 平均电流取决于搜星占空比",
             (unsigned)GPS_PSMOO_PERIOD_S, (unsigned)GPS_PSMOO_ONTIME_S,
             (unsigned)GPS_PSMOO_MAXACQ_S);
    return ESP_OK;
}

/* ★★★ 2026-10-28 方案 A: 用 M10 原生 PSMOO 代替 PMREQ ★★★
 *   实测(gpsdiag, ROM SPG 5.10): PMREQ 完全被模块无视 —— ACK=0 NAK=0,
 *   3 秒内仍输出 1524 字节; 换 duration=60000、去掉 backup 位都一样。
 *   同一现象在 meshtastic #4061 复现(M10 + duration=0 + wakeupSources=uartrx 从不睡),
 *   他们最终改用硬件 EN 断电。
 *   ⇒ 这里改走 M10 自己的 CFG-PM 接口: OPERATEMODE=1(PSMOO) + 周期 1 小时,
 *     模块自己在 on/off 间循环, 平时不定位(off 态约 50µA)。
 *   ⚠ 文档(MAX-M10S IM §2.7.3.2 / EVK-M10 UG §8.5): OPERATEMODE 必须**最后**写。
 *   ⚠ 配置写 RAM+BBR; 本板 V_BCKP 悬空 ⇒ 掉电即失, 每次上电都要重写。 */
/* ⚠ 参数 (GPS_PSMOO_PERIOD_S / ONTIME_S / MAXACQ_S / FALLBACK_EN)
 *   已统一移到文件前部 —— gps_sleep_rounds() 的失败分支要就地判断策略。 */

static esp_err_t gps_set_psmoo(uint32_t period_s)
{
    uint8_t pl[40];
    size_t i = 0;
    pl[i++] = 0;                        /* version */
    pl[i++] = 0x03;                     /* layers: RAM(bit0) + BBR(bit1) */
    pl[i++] = 0; pl[i++] = 0;           /* transaction */
    pl[i++] = 0; pl[i++] = 0; pl[i++] = 0; pl[i++] = 0;   /* reserved */

    /* CFG-PM-POSUPDATEPERIOD = 0x40D00002 (U4) —— 定位周期 */
    pl[i++] = 0x02; pl[i++] = 0x00; pl[i++] = 0xD0; pl[i++] = 0x40;
    memcpy(&pl[i], &period_s, 4); i += 4;

    /* CFG-PM-ACQPERIOD = 0x40D00003 (U4) —— 搜星周期, 同值 */
    pl[i++] = 0x03; pl[i++] = 0x00; pl[i++] = 0xD0; pl[i++] = 0x40;
    memcpy(&pl[i], &period_s, 4); i += 4;

    /* CFG-PM-ONTIME = 0x30D00005 (U2) —— 每次开机电时长 */
    pl[i++] = 0x05; pl[i++] = 0x00; pl[i++] = 0xD0; pl[i++] = 0x30;
    pl[i++] = (uint8_t)(GPS_PSMOO_ONTIME_S & 0xFF);
    pl[i++] = (uint8_t)((GPS_PSMOO_ONTIME_S >> 8) & 0xFF);

    /* CFG-PM-MAXACQTIME = 0x20D00007 (U1) —— 最长搜星时间, 到点强制关 */
    pl[i++] = 0x07; pl[i++] = 0x00; pl[i++] = 0xD0; pl[i++] = 0x20;
    pl[i++] = (uint8_t)GPS_PSMOO_MAXACQ_S;

    /* CFG-PM-OPERATEMODE = 0x10D00001 (E1) = 1 (PSMOO) —— **必须最后写** */
    pl[i++] = 0x01; pl[i++] = 0x00; pl[i++] = 0xD0; pl[i++] = 0x10;
    pl[i++] = 1;

    return gps_send_ubx(UBX_CLS_CFG, UBX_ID_VALSET, pl, (uint16_t)i);
}

/* 返回 true = 已确认静默(进了 off 态) */
static bool gps_enter_psmoo(void)
{
    if (!s_ready) return false;
    gps_wait_steady(300, 3000);
    uart_flush_input(s_cfg.uart_num);
    esp_err_t e = gps_set_psmoo(GPS_PSMOO_PERIOD_S);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "PSMOO 配置发送失败: %s", esp_err_to_name(e));
        return false;
    }
    ESP_LOGW(TAG, "PSMOO 已下发(周期 %us / on %us / maxacq %us), 等它进 off 态...",
             (unsigned)GPS_PSMOO_PERIOD_S, (unsigned)GPS_PSMOO_ONTIME_S,
             (unsigned)GPS_PSMOO_MAXACQ_S);
    /* 首次定位最多 MAXACQ 秒, 给 12 秒窗口看是否先安静下来(搜星中会一直输出)。
     * ★ 2026-10-28: 窗口跟着 MAXACQ 走 —— MAXXACQ=15 秒, 这里等 12+3=15 秒,
     *   刚好覆盖"搜满 MAXACQ 后被强制关断"的那一刻, 避免明明关了却判失败。 */
    int still = gps_quiet_bytes(12000);
    if (still == 0) {
        still = gps_quiet_bytes(3000);
        if (still == 0) {
            s_state = GPS_POWER_SLEEP;
            s_sleep_verified = true;
            ESP_LOGI(TAG, "PSMOO 生效: 已静默(模块进 off 态)");
            return true;
        }
    }
    ESP_LOGW(TAG, "PSMOO 后仍在输出 %d 字节(可能在搜星, 最长 %us 后自动关)",
             still, (unsigned)GPS_PSMOO_MAXACQ_S);
    s_state = GPS_POWER_ACTIVE;
    s_sleep_verified = false;
    return false;
}

/* ★★★ 2026-10-24 linit 铁律落地: "没用的绝不常驻内部 RAM, 打开才加载、退出即释放"。
 *   原来 gps_sleep() 只发 PMREQ, UART 驱动(2KB 缓冲 + 驱动结构 ≈ 3KB 内部 RAM)、
 *   EXTINT 上拉、s_ready 全部留着 —— 应用退出了, 这些资源却还在。
 *   gps_teardown() 把 UART 驱动整个卸掉、引脚还原、状态清零。下次 gps_init() 重装。 */
static void gps_teardown(void)
{
    if (!s_ready) return;
    /* ★ 同上: 用短延时替代 uart_wait_tx_done (它在驱动重装窗口里会撞 spinlock 断言) */
    vTaskDelay(pdMS_TO_TICKS(20));
    uart_driver_delete(s_cfg.uart_num);
    gps_tx_hold_high();      /* ★ 删驱动后 pad 可能被还原 ⇒ 再钉一次高, 防浮空唤醒 */
    if (s_cfg.pin_extint >= 0) gpio_reset_pin((gpio_num_t)s_cfg.pin_extint);
    s_ready = false;
    s_state = GPS_POWER_UNKNOWN;
    ESP_LOGI(TAG, "已释放: UART%d 驱动卸载 + 引脚还原 (内部 RAM 归还)");
}

/* gps_release(): 送待机 + 彻底释放 (应用退出用)。幂等。 */
esp_err_t gps_release(void)
{
    if (s_initing) return ESP_ERR_INVALID_STATE;   /* 探测/初始化进行中: 绝不能删驱动 */
    if (!s_ready) return ESP_OK;
    gps_sleep();          /* UBX-RXM-PMREQ → 46µA */
    gps_teardown();
    return ESP_OK;
}

/* ★★★ 2026-10-24 开机停靠 (gps_boot_park):
 *   开机唯一目的 = 别让模块 10mA 空转, **不是**让 GPS 可用。
 *   跑完整 gps_init() 要: 装 UART + autobaud(命中 240ms / 全落空 3.6s) + GSV 提频 +
 *   gps_sleep(560ms) ⇒ 0.9~4.2s 白占后台。
 *   新逻辑 (先听再决定, 不盲发):
 *     ① 装 UART @38400, 静听 300ms
 *     ② 0 字节 ⇒ 模块本来就在 backup(≤46µA), **什么都不做**, 直接释放  ← 常态(热启动)
 *     ③ 收到 NMEA/UBX ⇒ 它还醒着(冷上电/上次没睡成) ⇒ 发 PMREQ 送睡
 *     ④ 收到但校验全错 ⇒ 波特率不对 ⇒ 退回完整 gps_init()+gps_sleep() 兜底
 *   常态耗时 ≈ 320ms, 且全程在后台任务, 不阻塞首帧。 */
/* ★★★ 300ms 太短: NMEA 1Hz 突发, 包间天然 ~700ms 空档, 静听 300ms 常撞空档
 *   ⇒ 误判"已在待机"⇒ 从此不再发 PMREQ。1.8s 覆盖近两个周期, 判定才稳。 */
#define GPS_PARK_LISTEN_MS  1800

/* ★★★ 2026-10-28: 等接收机进入"稳态"(1Hz 突发输出, 出现 ≥quiet_ms 的静默窗口)。
 *   为什么必须等: 我们装 UART 时重配引脚会把模块从待机吵醒, 它刚上电 1~2 秒在跑自检,
 *   这期间进来的 UBX **会被直接丢弃**。实测对照(同一帧、同一份代码):
 *     · 不等稳态, 紧跟着发 → 被忽略(模块继续输出 1984 字节)
 *     · 等到 300ms 静默再发 → **第 1 轮就成功**(静默 3.2s 验证)
 *   返回 true = 已进入稳态。 */
static bool gps_wait_steady(uint32_t quiet_ms, uint32_t max_ms)
{
    if (!s_ready) return false;
    uint8_t b[96];
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t quiet_from = t0;
    while ((uint32_t)(esp_timer_get_time() / 1000) - t0 < max_ms) {
        int n = uart_read_bytes(s_cfg.uart_num, b, sizeof(b), pdMS_TO_TICKS(50));
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (n > 0) { quiet_from = now; continue; }
        if (now - quiet_from >= quiet_ms) return true;
    }
    return false;
}

/* ★★★ 2026-10-27 linit「GPS 还是耗电」诊断: 逐档波特率试听。
 *   为什么需要: 开机停靠只在 38400 听 300ms, 若模块跑在别的波特率,
 *   我们**听不到就误判"它已在待机"**, 结果整夜不睡。
 *   本函数对 6 档逐一监听 300ms, 打印每档收到的字节数 ——
 *   有字节 ⇒ 接线/供电都 OK, 只是波特率不对; 全都 0 ⇒ 模块没供电/线没接/已睡。
 *   跑完不改变模块状态(不发送任何东西)。 */
/* ═══════════════════════════════════════════════════════════════════════════
 * GPS 接线 / 通信诊断 (安全版, 替代原来会崩的 gps_probe_bauds)
 *
 * 为什么重写: 旧版在开机停靠任务还没结束时就可能进来抢同一个 UART 驱动,
 *   两个任务对同一 ringbuf 做 install/delete ⇒ `assert failed: spinlock_acquire` ⇒ 重启。
 *   现在: 只要 s_ready / s_initing 被别人占着就**直接拒绝**, 不做任何事。
 *
 * 判据(逐条给结论, 不靠猜):
 *   ① 6 档波特率各听 400ms, 统计 总字节 / NMEA 帧头($G,$B) / UBX 同步头(B5 62)
 *   ② 在听到数据的那档发 UBX-MON-VER(只读轮询, 不改配置), 看**模块有没有回 UBX**
 *      → 回了: ESP TX→GPS RXD 这根线也是通的(回程完整)
 *      → 没回: 要么 ESP TX→GPS RXD 断/接反/虚焊, 要么是只会 NMEA 的山寨模块
 *   ③ 全档 0 字节: 模块已在待机(好事) / 没供电 / GPS TXD→ESP RX 断
 *      → 用电流区分: 若仍 ~10mA 却一个字节都没有 = 传输线断了
 * ═══════════════════════════════════════════════════════════════════════════ */
void gps_diag(void)
{
    extern bool gps_service_is_running(void);
    const gps_config_t *gc = default_cfg_bound();
    ESP_LOGW(TAG, "==== GPSDIAG BEGIN ====");
    ESP_LOGW(TAG, "GPSDIAG 引脚: ESP_TX(GPIO%d)→GPS.RXD / ESP_RX(GPIO%d)←GPS.TXD / EXTINT=%d",
             gc ? gc->pin_tx : -1, gc ? gc->pin_rx : -1, gc ? gc->pin_extint : -1);
    ESP_LOGW(TAG, "GPSDIAG 状态: ready=%d initing=%d 解析服务=%d 已确认入睡=%d",
             (int)s_ready, (int)s_initing, (int)gps_service_is_running(), (int)s_sleep_verified);

    if (s_initing) { ESP_LOGW(TAG, "GPSDIAG 拒绝: 正在初始化/停靠中, 等 2 秒再试"); return; }
    if (s_ready)   { ESP_LOGW(TAG, "GPSDIAG 拒绝: UART 已被占用(有任务在用 GPS), 先退出应用再诊断"); return; }
    if (gps_service_is_running()) { ESP_LOGW(TAG, "GPSDIAG 拒绝: 解析服务在跑, 先 app.gps.stop()"); return; }

    s_initing = true;                       /* 挡住 gps_release() 删驱动 */
    s_sleep_verified = false;               /* 装 UART 会重配 TX pad ⇒ "已睡"结论作废 */
    s_cfg = *gc;
    s_cfg.baud = 38400;

    esp_err_t e = uart_driver_install(s_cfg.uart_num, GPS_RX_BUF, GPS_TX_BUF, 0, NULL, 0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "GPSDIAG UART 安装失败: %s", esp_err_to_name(e));
        s_initing = false;
        return;
    }
    const uart_config_t uc = {
        .baud_rate = 38400, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(s_cfg.uart_num, &uc);
    if (s_cfg.pin_tx >= 0) gpio_hold_dis((gpio_num_t)s_cfg.pin_tx);
    uart_set_pin(s_cfg.uart_num, s_cfg.pin_tx, s_cfg.pin_rx,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    s_ready = true;                         /* gps_send_ubx 要用 */

    /* ★★★ 2026-10-27 诊断流程(第二版, 按实测时序重写) ★★★
     *   实测教训: 模块从 backup 唤醒要 **1.5~2 秒** 才开始吐 NMEA。
     *   上一版发完唤醒脉冲只等 600ms 就开始逐档扫(每档 400ms), 扫到 38400 时它还没醒,
     *   于是全档都是"0 字节/噪声", 被误判成接线断。而且按字节数选最优率会被
     *   过采样噪声带偏(230400 采 38400 的流 = 1353 字节乱码)。
     *   新版: ①发唤醒脉冲 ②死等 38400 上出现有效帧(最多 4s) ③就地发 MON-VER 验回程
     *        ④只有 38400 完全无输出时才去扫其它档(纯参考)。 */
    uint8_t buf[160];
    int got = 0, nmea = 0, ubx = 0;

    uart_set_baudrate(s_cfg.uart_num, 38400);
    uart_flush_input(s_cfg.uart_num);
    {
        static const uint8_t wake[8] = { 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF };
        uart_write_bytes(s_cfg.uart_num, (const char *)wake, sizeof(wake));
        vTaskDelay(pdMS_TO_TICKS(20));
        uart_write_bytes(s_cfg.uart_num, (const char *)wake, sizeof(wake));
        ESP_LOGW(TAG, "GPSDIAG 已发唤醒脉冲, 等模块启动(最多 4s)...");
    }

    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t first_byte_ms = 0;
    while ((uint32_t)(esp_timer_get_time() / 1000) - t0 < 4000) {
        int n = uart_read_bytes(s_cfg.uart_num, buf, sizeof(buf), pdMS_TO_TICKS(60));
        if (n <= 0) continue;
        if (!first_byte_ms) first_byte_ms = (uint32_t)(esp_timer_get_time() / 1000) - t0;
        got += n;
        for (int q = 0; q + 1 < n; q++) {
            if (buf[q] == '$' && (buf[q+1] == 'G' || buf[q+1] == 'P' ||
                                  buf[q+1] == 'B' || buf[q+1] == 'L')) nmea++;
            if (buf[q] == UBX_SYNC1 && buf[q+1] == UBX_SYNC2) ubx++;
        }
        if (nmea + ubx >= 3) break;          /* 已经确认有有效帧, 不用等满 4s */
    }
    ESP_LOGW(TAG, "GPSDIAG @38400: %d 字节 NMEA=%d UBX=%d (首个字节在 %ums)",
             got, nmea, ubx, (unsigned)first_byte_ms);

    int ubx_reply = 0;
    int pmreq_silent = -1;      /* PMREQ 实测末段静默(ms); -1 = 没测到（结论段据此分支） */
    uint8_t mv[320]; int mv_n = 0;
    if (nmea + ubx > 0) {
        /* 有有效帧 ⇒ RX 通了。现在验 TX: 发 UBX-MON-VER(只读轮询), 真 u-blox 会回。 */
        uart_flush_input(s_cfg.uart_num);
        gps_send_ubx(UBX_CLS_MON, UBX_ID_MONVER, NULL, 0);
        uint32_t t1 = (uint32_t)(esp_timer_get_time() / 1000);
        while ((uint32_t)(esp_timer_get_time() / 1000) - t1 < 1200) {
            int n = uart_read_bytes(s_cfg.uart_num, buf, sizeof(buf), pdMS_TO_TICKS(60));
            if (n <= 0) continue;
            for (int q = 0; q < n && mv_n < (int)sizeof(mv) - 1; q++) mv[mv_n++] = buf[q];
            for (int q = 0; q + 3 < n; q++)
                if (buf[q] == UBX_SYNC1 && buf[q+1] == UBX_SYNC2 &&
                    buf[q+2] == UBX_CLS_MON) ubx_reply++;
        }
        ESP_LOGW(TAG, "GPSDIAG MON-VER 回程: %d 帧", ubx_reply);
        /* ★ 把 MON-VER 里的固件版本串打出来 —— M10 的 SPG 5.00 与 5.10 对 PMREQ 支持不同
         *   (SparkFun 官方答复: 5.00 只支持长版; 5.10 长短都支持)。不知道版本就无法判断
         *   到底是"我们发错了"还是"这版固件根本不支持", 所以必须读出来。 */
        if (ubx_reply > 0) {
            /* 按 UBX 帧头解析: B5 62 0A 04 lenL lenH | swVersion(30B) | hwVersion(10B) ... */
            char vs[40] = {0};
            for (int q = 0; q + 7 < mv_n; q++) {
                if (mv[q] == UBX_SYNC1 && mv[q+1] == UBX_SYNC2 &&
                    mv[q+2] == UBX_CLS_MON && mv[q+3] == UBX_ID_MONVER) {
                    int pl = mv[q+4] | (mv[q+5] << 8);
                    if (pl >= 30 && q + 36 <= mv_n) {
                        int k = 0;
                        for (int t = 0; t < 30 && k < (int)sizeof(vs) - 1; t++) {
                            uint8_t c = mv[q + 6 + t];
                            vs[k++] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
                        }
                        while (k > 0 && vs[k-1] == ' ') vs[--k] = 0;
                    }
                    break;
                }
            }
            ESP_LOGW(TAG, "GPSDIAG 模块固件(swVersion): %s", vs[0] ? vs : "(未解析到)");
        }

        /* ③ PMREQ 实测: 用正确布局发一次, 看它是 ACK / NAK / 装死, 以及多久静音。
         *   UBX-ACK-ACK = 05 01 02 02 41;  UBX-ACK-NAK = 05 01 02 00 02 41
         *   (payload 是 [clsID][msgID]) ⇒ 能直接读出"模块是否受理了 PMREQ"。 */
        uint8_t pl[16];
        uint16_t pn = pmreq_build(pl, true, 0);
        uart_flush_input(s_cfg.uart_num);
        /* ★★★ 2026-10-28 关键: **只发一帧**。
         *   原来这里 120ms 后又发了一遍 —— 而 PMREQ 配的唤醒源正是 UART RX,
         *   第二帧的起始位(下降沿)就是一次唤醒事件 ⇒ "让它睡 → 立刻叫醒",
         *   所以永远测不到静默, ACK 也永远收不到(睡着的模块没法回 ACK)。 */
        gps_send_ubx(UBX_CLS_RXM, UBX_ID_PMREQ, pl, pn);

        int nack = 0, ack = 0, after = 0;
        uint32_t silent_ms = 0, silent_at = 0;
        uint32_t t3 = (uint32_t)(esp_timer_get_time() / 1000);
        while ((uint32_t)(esp_timer_get_time() / 1000) - t3 < 4000) {
            int n = uart_read_bytes(s_cfg.uart_num, buf, sizeof(buf), pdMS_TO_TICKS(60));
            if (n <= 0) { silent_ms += 60; if (silent_ms >= 600 && !silent_at) silent_at = 600; continue; }
            after += n;
            silent_ms = 0;
            for (int q = 0; q + 5 < n; q++) {
                if (buf[q] == UBX_SYNC1 && buf[q+1] == UBX_SYNC2 &&
                    buf[q+2] == 0x05 && (buf[q+3] == 0x01 || buf[q+3] == 0x00)) {
                    if (buf[q+4] == 0x02 && buf[q+5] == UBX_ID_PMREQ) {
                        if (buf[q+3] == 0x01) ack++; else nack++;
                    }
                }
            }
        }
        ESP_LOGW(TAG, "GPSDIAG PMREQ: ACK=%d NAK=%d, 之后又收到 %d 字节, 末段静默=%dms",
                 ack, nack, after, (int)silent_ms);
        pmreq_silent = (int)silent_ms;      /* ★ 结论段要用, 别让两段日志互相打脸 */

        /* ★ 单变量实验: duration 从 0 改成 60000ms。
         *   目的: 区分两种完全不同的失败原因 ——
         *     ① 模块**根本没受理**PMREQ  ⇒ 带 duration 也一样一直在输出
         *     ② 受理了但**立刻被唤醒**(V_IO 域/唤醒源问题) ⇒ duration=60s 时会安静 60 秒
         *   这一步不改别的任何东西, 只改 duration 一个变量。 */
        if (silent_ms < 600) {
            uint8_t pl2[16];
            uint16_t pn2 = pmreq_build(pl2, true, 60000);   /* duration = 60s */
            uart_flush_input(s_cfg.uart_num);
            gps_send_ubx(UBX_CLS_RXM, UBX_ID_PMREQ, pl2, pn2);
            vTaskDelay(pdMS_TO_TICKS(100));

            int g60 = 0; uint32_t t4 = (uint32_t)(esp_timer_get_time() / 1000);
            while ((uint32_t)(esp_timer_get_time() / 1000) - t4 < 3000) {
                int n = uart_read_bytes(s_cfg.uart_num, buf, sizeof(buf), pdMS_TO_TICKS(60));
                if (n > 0) g60 += n;
            }
            ESP_LOGW(TAG, "GPSDIAG PMREQ(duration=60000ms) 后 3 秒收到 %d 字节 ⇒ %s",
                     g60, g60 == 0 ? "**受理了**(能睡满 60 秒, 说明 duration 必须非 0)" :
                                     "**仍未受理**(同 duration=0 一样被无视)");
        }
        if (nack > 0)
            ESP_LOGW(TAG, "GPSDIAG ⇒ 模块**明确拒绝** PMREQ(NAK): 这套固件不支持该消息/该布局");
        else if (ack > 0 && silent_ms >= 600)
            ESP_LOGW(TAG, "GPSDIAG ⇒ 模块**受理** PMREQ 并已静音: 待机可用");
        else if (ack > 0)
            ESP_LOGW(TAG, "GPSDIAG ⇒ 模块回 ACK 但**仍在输出**: 受理了却没进 backup(可能缺 V_BCKP)");
        else if (silent_ms >= 600)
            ESP_LOGW(TAG, "GPSDIAG ⇒ 无应答但静音了: 多半已进待机(backup 模式本来就不回 ACK)");
        else
            ESP_LOGW(TAG, "GPSDIAG ⇒ 无应答且一直在输出: PMREQ 被丢弃(不支持 backup / V_BCKP 未接)");
    } else {
        /* 38400 没东西: 扫其它档只为参考(说明是不是被改过速率) */
        static const uint32_t rates[] = { 9600, 19200, 57600, 115200, 230400 };
        for (int i = 0; i < (int)(sizeof(rates)/sizeof(rates[0])); i++) {
            uart_set_baudrate(s_cfg.uart_num, rates[i]);
            uart_flush_input(s_cfg.uart_num);
            int g2 = 0, f2 = 0;
            uint32_t t2 = (uint32_t)(esp_timer_get_time() / 1000);
            while ((uint32_t)(esp_timer_get_time() / 1000) - t2 < 350) {
                int n = uart_read_bytes(s_cfg.uart_num, buf, sizeof(buf), pdMS_TO_TICKS(50));
                if (n <= 0) continue;
                g2 += n;
                for (int q = 0; q + 1 < n; q++)
                    if (buf[q] == '$' && (buf[q+1] == 'G' || buf[q+1] == 'P')) f2++;
            }
            ESP_LOGW(TAG, "GPSDIAG %6lu bps: %d 字节 有效帧=%d",
                     (unsigned long)rates[i], g2, f2);
            if (f2 > 0) { ESP_LOGW(TAG, "GPSDIAG 该档有有效 NMEA ⇒ 模块速率被改成 %lu",
                                   (unsigned long)rates[i]); break; }
        }
        uart_set_baudrate(s_cfg.uart_num, 38400);
    }

    /* ---- 结论 ---- */
    if (got == 0) {
        ESP_LOGW(TAG, "GPSDIAG 结论=【唤醒后 38400 完全无输出】");
        ESP_LOGW(TAG, "          ① GPS.TXD→ESP RX(GPIO%d) 断线/接反/虚焊  ← 最可能", s_cfg.pin_rx);
        ESP_LOGW(TAG, "          ② 模块没供电(量 GPS 的 VCC 脚应为 3.3V)");
        ESP_LOGW(TAG, "          ③ 模块已损坏");
    } else if (nmea + ubx == 0) {
        ESP_LOGW(TAG, "GPSDIAG 结论=【收到 %d 字节但没有一帧有效】", got);
        ESP_LOGW(TAG, "          ① 电平/接触不良(收到乱码说明 RX 有线, 但采样不对)");
        ESP_LOGW(TAG, "          ② 波特率不是 38400(看上面其它档有没有有效帧)");
    } else if (ubx_reply > 0) {
        ESP_LOGW(TAG, "GPSDIAG 结论=【收发双向正常】收得到 NMEA, MON-VER 也有回 ⇒ **接线没问题**。");
        /* ★ 2026-10-28: 结论必须跟着 PMREQ 实测结果走。
         *   原来这里无条件写"不肯待机 ⇒ 需要硬件断电" —— 后来实测 PMREQ(backup+force)
         *   单帧发送后静默 3.9s、开机停靠 verified=1, 那条文案是错的, 会把排查带偏。 */
        if (pmreq_silent >= 600) {
            ESP_LOGW(TAG, "          ★ PMREQ 后静默 %dms ⇒ **软件待机可用**"
                          "(backup 模式不回 ACK 是正常的, 别拿 ACK 判断)", pmreq_silent);
        } else if (pmreq_silent >= 0) {
            ESP_LOGW(TAG, "          ✗ PMREQ 后仍在输出(末段只静默 %dms) ⇒ 模块不受理"
                          " ⇒ 唯一可靠方案是给 GPS 的 VCC 串 MOS 做硬件断电", pmreq_silent);
        } else {
            ESP_LOGW(TAG, "          (PMREQ 没测到结果, 无法判断待机能力)");
        }
    } else {
        ESP_LOGW(TAG, "GPSDIAG 结论=【收得到 NMEA, 但 MON-VER 无回应】");
        ESP_LOGW(TAG, "          ① ESP TX(GPIO%d)→GPS.RXD 这根线断/接反/虚焊  ← 最可能", s_cfg.pin_tx);
        ESP_LOGW(TAG, "          ② 模块是只支持 NMEA 的山寨货(不认 UBX)");
        ESP_LOGW(TAG, "          ⇒ 两种情况 PMREQ 都发不进去, 硬件断电是唯一可靠方案");
    }

    /* 收尾: **诊断完立刻送回待机** —— 诊断本身会把它叫醒, 不能就这么留在 10~25mA */
    gps_teardown();
    s_initing = false;
    gps_boot_park();
    ESP_LOGW(TAG, "==== GPSDIAG END (已尝试送回待机, verified=%d) ====", (int)s_sleep_verified);
}

/* 兼容旧命令名 */
void gps_probe_bauds(void) { gps_diag(); }

/* ★★★ 2026-10-28 【真互斥】实测关机路径与管家任务会**同时**进 park ★★★
 *   证据(关机日志):
 *     W (1731) park: uart install ret=ESP_OK ... 开始静听 1800ms   ← 上下文 A
 *     E (1744) uart: UART driver already installed                ← 上下文 B 又装一次
 *     W (1745) PWR: GPS 停靠失败(ESP_FAIL) ⇒ 第 1 次重试           ← A 只跑了 14ms 就失败
 *   原因: s_initing 只是个"进入时置位、退出时还原"的软标志, **不阻塞后来者**。
 *   两者共用同一个 s_cfg/s_ready, B 的 gps_teardown() 会把 A 的 UART 卸掉,
 *   A 的 uart_read_bytes 立刻失败 ⇒ got=0 循环空转, 或提前退出。
 *   这里用原子标志做**真正的串行化**: 后来者直接 BUSY 返回, 由上层按自己的节奏重试。
 *   注意: 只保护"停靠"这一条路径; 应用侧读写仍靠 s_initing + 上层时序(见下方说明)。 */
static esp_err_t gps_park_inner(void);

esp_err_t gps_boot_park(void)
{
#if GPS_HW_DISABLED
    return ESP_ERR_NOT_SUPPORTED;
#endif
    static volatile int busy = 0;
    if (__atomic_test_and_set(&busy, __ATOMIC_ACQ_REL)) {
        ESP_LOGW(TAG, "park 让路: 另一个停靠正在执行 ⇒ 返回 BUSY");
        /* ★ 2026-10-29: 原来这里返回 ESP_ERR_INVALID_STATE, 与"模块没就绪(!s_ready)"
         *   共用同一个错误码 ⇒ 调用方(power_mgr)无法区分"在忙(值得等)"和
         *   "没插/没初始化(等也没用)", 只能瞎重试。
         *   改成 ESP_ERR_NOT_FINISHED: 语义准确("另一个操作还没跑完"), 且调用方可以据此等待。
         *   已核: 全仓调用者(app_board / remcon / power_mgr)都只判断 ret==ESP_OK,
         *   没有任何人依赖这个具体错误码 ⇒ 改码安全。 */
        return ESP_ERR_NOT_FINISHED;
    }
    uint32_t t_enter = (uint32_t)(esp_timer_get_time() / 1000);
    esp_err_t r = gps_park_inner();
    ESP_LOGW(TAG, "park 退出: %s · 耗时 %ums · verified=%d",
             esp_err_to_name(r),
             (unsigned)((uint32_t)(esp_timer_get_time() / 1000) - t_enter),
             (int)gps_sleep_verified());
    __atomic_clear(&busy, __ATOMIC_RELEASE);
    return r;
}

static esp_err_t gps_park_inner(void)
{
    /* ★★★ 2026-10-27: 停靠期间必须把 s_initing 置上。
     *   否则 main 主循环的"1 秒兜底"会看到 state==ACTIVE 又去 gps_release(),
     *   两个任务同时对同一 UART 发 PMREQ / 删驱动 ⇒ 日志标签串味(实测出现过),
     *   严重时撞 spinlock 断言。 */
    /* ★★★ 任务书修改 2: 应用正在用 GPS 时**让路**, 绝不抢 UART ★★★
     *   gps_service_is_running() 是应用层(Lua app.gps)维护的"当前有人在读 UART"信号。
     *   ⚠ 它只是一个布尔快照, **不能替代互斥锁** —— 真正的串行化靠两层:
     *     ① s_initing (驱动级, 覆盖 gps_init/gps_release/gps_boot_park 的整段生命周期)
     *     ② 上层调用时序 (guard task 在 !gps_service_is_running 且 !verified 时才重试)
     *   这里只做"尽力避让": 服务在跑就立刻返回 BUSY, 由 guard task 按 30s 节奏再来,
     *   不在持锁状态里等待(任务书要求: 不能持锁阻塞)。 */
    {
        extern bool gps_service_is_running(void);
        if (gps_service_is_running()) {
            ESP_LOGW(TAG, "停靠让路: GPS 解析服务正在使用 UART (返回 BUSY)");
            gps_sm_set(GPS_SM_FAILED, "BUSY: 应用占用 UART");
            return ESP_ERR_INVALID_STATE;
        }
    }
    bool was_initing = s_initing;
    s_initing = true;
    s_cfg = *default_cfg_bound();
    s_cfg.baud = 38400;                  /* 出厂值; 睡醒必回它, 见 gps.c 头注释 */

    esp_err_t e = uart_driver_install(s_cfg.uart_num, GPS_RX_BUF, GPS_TX_BUF, 0, NULL, 0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { s_initing = was_initing; return e; }
    /* ★ 2026-10-28 诊断: 实测出现过"直接返回 ESP_FAIL 但一行分支日志都没有"的情况,
     *   加这两行就能判断到底卡在装驱动 还是 卡在静听。 */
    ESP_LOGW(TAG, "park: uart install ret=%s (INVALID_STATE=驱动已在, 容忍), 开始静听 %dms",
             esp_err_to_name(e), (int)GPS_PARK_LISTEN_MS);
    const uart_config_t uc = {
        .baud_rate = (int)s_cfg.baud, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(s_cfg.uart_num, &uc);
    if (s_cfg.pin_tx >= 0) gpio_hold_dis((gpio_num_t)s_cfg.pin_tx);   /* ★ 解 hold 才能发 UBX */
    uart_set_pin(s_cfg.uart_num, s_cfg.pin_tx, s_cfg.pin_rx,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    s_ready = true;
    s_state = GPS_POWER_ACTIVE;
    s_sleep_verified = false;      /* ★ 上电/装驱动 = 模块被叫醒了 */

    uint8_t buf[128];
    int got = 0, sentences = 0;
    uart_flush_input(s_cfg.uart_num);
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    while ((uint32_t)(esp_timer_get_time() / 1000) - t0 < GPS_PARK_LISTEN_MS) {
        int n = uart_read_bytes(s_cfg.uart_num, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n <= 0) continue;
        got += n;
        for (int i = 0; i + 1 < n; i++)
            if ((buf[i] == '$' && buf[i + 1] == 'G') ||
                (buf[i] == 0xB5 && buf[i + 1] == 0x62)) sentences++;
    }

    /* ★★★ 2026-10-28 关键: **等接收机完全起来**再发 PMREQ ★★★
     *   实测证据: 同一帧 PMREQ、同一份代码 ——
     *     开机第 1 次: 被忽略(模块继续输出 1125 字节)
     *     37 秒后重试: 成功(静默 3.2s 验证)
     *   原因: 我们装 UART 时的引脚重配会把模块从待机吵醒, 而它刚上电的 1~2 秒在跑
     *   自检/启动, 这段时间进来的 UBX 会被直接丢掉。
     *   判据: 启动完成后的接收机是 **1Hz 突发**输出 ⇒ 出现 ≥300ms 静默就说明它已进稳态。
     *   最多等 3 秒, 不无限拖。
     */
    if (got > 0 && sentences > 0) {
        s_seen_frame = true;        /* ★ 2026-10-28: 记下"确认收到过有效帧" = 这块板有 GPS */
        bool steady = gps_wait_steady(300, 3000);
        ESP_LOGI(TAG, "开机停靠: %s ⇒ 现在发 PMREQ", steady ? "已进入稳态" : "等稳态超时(照发)");
    }

    if (got == 0) {
        /* ★ 2026-10-27: 静默 = 已在待机。必须同时置 verified, 否则管家会以为"没睡成",
         *   每 30s 再装一次 UART —— 而装 UART 会重配 TX pad, 那一下电平跳变恰好会把
         *   模块叫醒, 变成"越管越醒"的反效果。 */
        s_state = GPS_POWER_SLEEP;
        s_sleep_verified = true;
        ESP_LOGI(TAG, "开机停靠: 模块无输出(got=0, 静听 %dms) ⇒ 本来就在待机(≤46µA), 不动它",
                 (int)GPS_PARK_LISTEN_MS);
    } else if (sentences == 0) {
        /* 有字节但全是乱码 ⇒ 波特率被改过, 用完整初始化兜底(会 autobaud 找对再送睡) */
        ESP_LOGW(TAG, "开机停靠: 收到 %d 字节但无有效帧头 ⇒ 走完整初始化兜底", got);
        gps_teardown();
        if (gps_init(NULL) == ESP_OK) gps_sleep();
        gps_teardown();
        return ESP_OK;
    } else {
        ESP_LOGI(TAG, "开机停靠: 模块仍是醒的(%d 字节) ⇒ 先试 PMREQ", got);
        gps_sleep_rounds(1);        /* 停靠只要快: 1 轮 ≈3.5s; 关机路径才用 3 轮 */
        if (!s_sleep_verified) {
            /* ★ 2026-10-28: 本板 PMREQ 被模块完全无视(gpsdiag 实测 ACK=0 NAK=0)
             *   ⇒ 兜底改走 M10 原生 PSMOO, 让模块自己 on/off 循环。 */
            ESP_LOGW(TAG, "开机停靠: PMREQ 无效 ⇒ 改用 PSMOO(CFG-PM)");
            gps_enter_psmoo();
        }
    }
    gps_teardown();
    if (s_sleep_verified) s_state = GPS_POWER_SLEEP;   /* teardown 会清成 UNKNOWN, 这里按实测还原 */
    s_initing = was_initing;
    return s_sleep_verified ? ESP_OK : ESP_FAIL;
}

esp_err_t gps_wakeup(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    s_sleep_verified = false;      /* ★ 唤醒指令 → 之前的"已睡"结论作废 */

    /* (1) UART RX 唤醒源: 送一个无害的 UBX-MON-VER 轮询 (待机时收到字节即醒) */
    esp_err_t e = gps_send_ubx(UBX_CLS_MON, UBX_ID_MONVER, NULL, 0);

    /* (2) 若 EXTINT 配成输出: 补一个"低→高"脉冲, 覆盖极高/极低两种有效极性 */
    if (s_cfg.pin_extint >= 0 && s_cfg.drive_extint) {
        gpio_set_level(s_cfg.pin_extint, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(s_cfg.pin_extint, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (e == ESP_OK) {
        s_state = GPS_POWER_ACTIVE;
        ESP_LOGI(TAG, "已唤醒 (UBX-MON-VER%s)", s_cfg.drive_extint ? " + EXTINT 脉冲" : "");
    }
    return e;
}
