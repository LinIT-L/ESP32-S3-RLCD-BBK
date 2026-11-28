/* board_pins.h — LinTOS **载板引脚唯一真相源** (运行时板型表)
 *
 * ★ 2026-10-13: 按键引脚原来散落三处(user_config.h / input.c / sleep.c), 改板漏改就"不生效"。
 * ★ 2026-10-16: LCD / 触摸 / 电池 / 音频 / 按键 全部并入本文件, 并改成**运行时按板型取表** ——
 *   开机自己识别板型 ⇒ **换板不用重刷固件**。
 *   起因事故: 微雪板 GPIO5 = LCD RS, 而触摸 I2C(默认 SDA=5) 无条件初始化把它配成开漏+上拉
 *   ⇒ RS 恒 3.3V、"设置不刷新"。教训: 引脚必须整套按板型切, 且识别要早于一切外设初始化。
 *
 * 用法: 任何地方 `const board_pins_t *bp = board_pins_get();` 然后取字段, 不要写死引脚号。
 * ⛔ 本文件只放常量/声明, 不放驱动逻辑。
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#include <stdint.h>
#include <stdbool.h>
#include "soc/gpio_num.h"
#include "driver/gpio.h"

/* ---------------- 板型（三档，官方命名） ---------------- */
typedef enum {
    BOARD_ID_XIAOZHI = 0,   /* ① 小智一体版: ESP32-S3-AI-2E 模块（自带充电/降压/功放/解码/麦克风） */
    BOARD_ID_WEIXUE  = 1,   /* ② 微雪原版: ESP32-S3-RLCD-4.2（无触摸） */
    BOARD_ID_STD     = 2,   /* ③ 标准扩展版: 官方标准模块 + 按需扩展（触摸/充电/功放自行加） */
    BOARD_ID_UNKNOWN = -1,
} board_id_t;

/* 一块板的全部引脚。**裸 GPIO 号** (-1 = 本板无此功能), 便于运行时切换与打印。
 * 引脚之外的差异 (如 SD 走 SDMMC 还是 SPI) 不在表里, 由各自驱动按 id 分支。 */
typedef struct {
    board_id_t id;
    const char *name;
    int lcd_dc, lcd_cs, lcd_sck, lcd_mosi, lcd_rst;      /* LCD (RS=DC) */
    int lcd_freq_mhz;                                    /* LCD SPI 时钟 (走线长/小板要降) */
    int tp_sda, tp_scl, tp_int, tp_rst;                  /* 触摸 I2C (-1 = 无触摸) */
    int btn_pwr, btn_right, btn_left;                    /* 三个按键 */
    /* LTC2954 电源控制脚 (-1 = 本板无此芯片)。★ 2026-11-28 四版本改造:
     * 从 ltc2954.h 的硬编码宏挪进板型表 —— 小智关机版 S1_4: KILL=44/INT=1,
     * 老版 S1_1/微雪/标准无 2954 ⇒ -1。随板型/0x68 判据动态生效。 */
    int pwr_kill, pwr_int;
    int bat_adc_chan;                                    /* 电池 ADC 通道号 (ADC1) */
    /* 充电中检测 (-1 = 未接): 开漏低有效, **低 = 正在充电**。
     * ★ 2026-11-21 定案: 只有这一根线, 小智板接在 GPIO44 (模组 29 脚 CHRG)。
     *   · 新板(PCB1_3): CHRG → GPIO44, 有效。
     *   · 旧小智板: 这脚**原本就没接 CHRG**(linit 确认), GPIO44 空闲 ⇒
     *     内部上拉读到高 ⇒ 报"未充电", 不误报, 也不需要任何板型判断。
     *   "已充满"没有硬信号(DONE 未接), 由 CHRG 释放 + VBAT≥4150mV 推出,
     *   见 board_battery.c 的 charge_state()。
     * ⛔ GPIO2 / GPIO43 是扩展脚, 不得占用 ⇒ 不做 VBUS 分压。 */
    int chrg;
    /* ★ 2026-11-28: CHRG 极性。0=模组 CHRG 开漏低有效(充电中拉低);
     *   1=VBUS 分压高有效(100k/100k 从 +5V 到 GND, 插电时 GPIO 读高)。
     *   S1_4 关机版用后者(GPIO42)。 */
    bool chrg_active_high;
    int i2s_mclk, i2s_bclk, i2s_ws, i2s_dout, i2s_din;   /* 音频 I2S */
    int codec_sda, codec_scl, pa_pin;                    /* 编解码器 I2C + 功放使能 */
    int vib;                                             /* 震动马达 PWM (-1 = 无) */
    int gps_tx, gps_rx, gps_extint;                       /* GPS (可选; -1 = 未接) */
    /* SD/TF: 0 = SDMMC(1-bit, 小智) · 1 = SPI(微雪)。上层挂载后都是 /sdcard (FAT),
     * 应用代码不受影响, 只有驱动初始化这条路不同。 */
    int sd_bus;                                          /* 0=SDMMC 1=SPI */
    int sd_clk, sd_cmd, sd_d0;                           /* SDMMC: CLK/CMD/D0 */
    int sd_cs, sd_sck, sd_mosi, sd_miso;                 /* SPI: CS/SCK/MOSI/MISO (CS<0 = 常选中) */
} board_pins_t;

/* ---------------- 接口 ---------------- */
const board_pins_t *board_pins_get(void);
void board_pins_set_id(board_id_t id);   /* ★运行时选板(hw_cfg 探测后调) */              /* 当前生效的表 (未识别 = 默认板型) */
const board_pins_t *board_pins_of(board_id_t id);      /* 指定板型的表 (只读; 未知返回默认) */
bool board_pins_select(board_id_t id);                 /* 设当前板型, 返回是否发生变化 */

/* 板型识别**不在本组件**：由 `services/hw_cfg` 开机探 I2C 指纹后调 `board_pins_set_id()`。
 * 本组件只负责「三张引脚表 + 运行时当前表」，不含任何探测/NVS 逻辑（避免两处各存一份板型）。 */

/* 深睡唤醒掩码: PWR + LEFT (⛔ 故意不含 RIGHT/GPIO0 —— strapping 脚电平保不住会误唤醒) */
uint64_t board_wake_mask(void);

/* ---------------- 兼容宏: 只作"默认板型的编译期缺省" ----------------
 * ⚠ 运行时一律用 board_pins_get()->xxx; 这些宏仅在没有表上下文的地方兜底。
 * ⛔ 任何"按板型分支"的逻辑都不许写成 `BOARD_IS_WEIXUE ? A : B` —— 那是编译期常量,
 *   运行时换板不会变（震动/触摸/GPS 都踩过）。要在运行期判，就取 `board_pins_get()`。 */
/* ★★★ 逐项外设开关(排查屏冲突用): 1=启用, 0=不启动。
 *   用法: 一次只把一个改回 1, 烧录后看屏还亮不亮 ⇒ 亮=它不冲突, 黑=它就是冲突源。
 *   ⛔ 排查完**全部改回 1**(即恢复正常全功能)。 */
#define LINTOS_USE_SD     1    /* ← 本轮测试项: SD 卡 */
#define LINTOS_USE_INPUT     1    /* ← 本轮: 只加按键 */    /* ← 待测: 按键 */
#define LINTOS_USE_TOUCH     1    /* ← 微雪无触摸: 保持关闭 */    /* 触摸(微雪无触摸, 先关) */
#define LINTOS_USE_AUDIO     1    /* ← 本轮: 加音频 */    /* ← 待测: 音频 */
#define LINTOS_USE_BATTERY     1    /* ← 本轮: 加电池 */
#define LINTOS_USE_GPS     1    /* 微雪无 GPS */
#define LINTOS_LCD_ONLY      0    /* 兼容旧开关: 1=全部跳过 */

/* ★★★ 2026-10-18 linit 定案: **一个固件通吃三块板** —— 本开关仅作"探测全失败时的兜底默认"
 *   运行时探测链: hw_cfg_init() → I2C 指纹(38/47@0x18 小智 · 13/14@0x18 微雪 · 15/7@0x15 标准扩展)
 *                → 失败读 NVS "board_id"(上次成功的) → 再失败才用本默认值
 *   探测结果会 cfg_set_u8("board_id") 固化 ⇒ **换板不用重编固件** ✓          */
#define BOARD_IS_WEIXUE      0   /* 0=小智(默认) 1=微雪 */
#define BOARD_DEFAULT_ID     (BOARD_IS_WEIXUE ? BOARD_ID_WEIXUE : BOARD_ID_XIAOZHI)
#define BOARD_BTN_PWR        GPIO_NUM_1
#define BOARD_BTN_RIGHT      GPIO_NUM_0
#define BOARD_BTN_LEFT       GPIO_NUM_4
#define BOARD_BTN_A          BOARD_BTN_LEFT
#define BOARD_BTN_B          BOARD_BTN_PWR
#define BOARD_BTN_UP         GPIO_NUM_NC
#define BOARD_BTN_DOWN       GPIO_NUM_NC
#define BOARD_LCD_DC         GPIO_NUM_7
#define BOARD_LCD_CS         GPIO_NUM_11
#define BOARD_LCD_SCK        GPIO_NUM_9
#define BOARD_LCD_MOSI       GPIO_NUM_10
#define BOARD_LCD_RST        GPIO_NUM_NC
#define BOARD_TP_SDA         5
#define BOARD_TP_SCL         13
#define BOARD_TP_INT         12
#define BOARD_TP_RST         6
#define BOARD_WAKE_MASK      ((1ULL << BOARD_BTN_PWR) | (1ULL << BOARD_BTN_LEFT))

/* ⛔ 引脚冲突自检 (编译期): 同一块板内, LCD 与触摸/音频/SD 之间不得重脚 */
#define BP_CK(cond, msg) _Static_assert(cond, msg)
#endif /* BOARD_PINS_H */
