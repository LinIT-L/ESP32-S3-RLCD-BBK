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

esp_err_t gps_sleep(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    uint8_t pl[16];
    memset(pl, 0, sizeof(pl));
    /* duration = 0 */
    pl[4] = 0x02 | 0x04;   /* backup + force (M10 必须 force) */
    pl[8] = 0x08;          /* wakeupSources: UART RX */

    /* 模块可能还在启动/输出 NMEA → 等一会再连发 3 次.
     * 每次发送会先唤醒"已睡下"的模块, 但紧接着的 PMREQ 立刻送回待机, 最终态必为待机. */
    vTaskDelay(pdMS_TO_TICKS(200));
    uart_flush_input(s_cfg.uart_num);
    esp_err_t last = ESP_OK;
    for (int k = 0; k < 3; k++) {
        last = gps_send_ubx(UBX_CLS_RXM, UBX_ID_PMREQ, pl, sizeof(pl));
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    if (last == ESP_OK) {
        s_state = GPS_POWER_SLEEP;
        ESP_LOGI(TAG, "已送 UBX-RXM-PMREQ ×3 → 软件待机 (backup+force, 唤醒源=UART RX; 待机电流 46µA)");
    } else {
        ESP_LOGD(TAG, "UBX-RXM-PMREQ 发送失败: %s", esp_err_to_name(last));
    }
    return last;
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
#define GPS_PARK_LISTEN_MS  300
esp_err_t gps_boot_park(void)
{
#if GPS_HW_DISABLED
    return ESP_ERR_NOT_SUPPORTED;
#endif
    s_cfg = *default_cfg_bound();
    s_cfg.baud = 38400;                  /* 出厂值; 睡醒必回它, 见 gps.c 头注释 */

    esp_err_t e = uart_driver_install(s_cfg.uart_num, GPS_RX_BUF, GPS_TX_BUF, 0, NULL, 0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    const uart_config_t uc = {
        .baud_rate = (int)s_cfg.baud, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(s_cfg.uart_num, &uc);
    uart_set_pin(s_cfg.uart_num, s_cfg.pin_tx, s_cfg.pin_rx,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    s_ready = true;
    s_state = GPS_POWER_ACTIVE;

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

    if (got == 0) {
        ESP_LOGI(TAG, "开机停靠: 模块无输出 ⇒ 本来就在待机(≤46µA), 不动它");
    } else if (sentences == 0) {
        /* 有字节但全是乱码 ⇒ 波特率被改过, 用完整初始化兜底(会 autobaud 找对再送睡) */
        ESP_LOGW(TAG, "开机停靠: 收到 %d 字节但无有效帧头 ⇒ 走完整初始化兜底", got);
        gps_teardown();
        if (gps_init(NULL) == ESP_OK) gps_sleep();
        gps_teardown();
        return ESP_OK;
    } else {
        ESP_LOGI(TAG, "开机停靠: 模块仍是醒的(%d 字节) ⇒ 送 PMREQ 待机", got);
        gps_sleep();
    }
    gps_teardown();
    return ESP_OK;
}

esp_err_t gps_wakeup(void)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

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
