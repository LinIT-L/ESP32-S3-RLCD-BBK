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

/* ---------------- 板型 ---------------- */
typedef enum {
    BOARD_ID_XIAOZHI = 0,   /* 小智 ESP32-S3-AI-2E 模块 + 新载板 */
    BOARD_ID_WEIXUE  = 1,   /* 微雪官方 ESP32-S3-RLCD-4.2 原板 (无触摸) */
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
    int bat_adc_chan;                                    /* 电池 ADC 通道号 (ADC1) */
    int i2s_mclk, i2s_bclk, i2s_ws, i2s_dout, i2s_din;   /* 音频 I2S */
    int codec_sda, codec_scl, pa_pin;                    /* 编解码器 I2C + 功放使能 */
    /* SD/TF: 0 = SDMMC(1-bit, 小智) · 1 = SPI(微雪)。上层挂载后都是 /sdcard (FAT),
     * 应用代码不受影响, 只有驱动初始化这条路不同。 */
    int sd_bus;                                          /* 0=SDMMC 1=SPI */
    int sd_clk, sd_cmd, sd_d0;                           /* SDMMC: CLK/CMD/D0 */
    int sd_cs, sd_sck, sd_mosi, sd_miso;                 /* SPI: CS/SCK/MOSI/MISO (CS<0 = 常选中) */
} board_pins_t;

/* ---------------- 接口 ---------------- */
const board_pins_t *board_pins_get(void);              /* 当前生效的表 (未识别 = 默认板型) */
const board_pins_t *board_pins_of(board_id_t id);      /* 指定板型的表 (只读; 未知返回默认) */
bool board_pins_select(board_id_t id);                 /* 设当前板型, 返回是否发生变化 */

/* 开机识别板型 (I2C 器件指纹):
 *   ① 先做 NVS 缓存的**轻量校验** (缓存那块板的指纹还在不在) → 在就直接用
 *   ② 否则探"默认板型自己的 I2C" → 有自家器件即定板 (常规场景不碰另一组脚)
 *   ③ 都没有 → 探另一组 → 命中即定板
 *   ④ 全无应答 → NVS 上次值 → 再不行默认板型
 * ★ 必须在**任何外设初始化之前**调用; 只在 I2C 上扫描, 探完立即释放总线。 */
/* 板型识别已删除 (linit 2026-10-16): 不再扫描 I2C、不写 NVS, 用 BOARD_IS_WEIXUE 选板。
 * 下面两个声明保留为空壳以便老调用点编译。 */
board_id_t board_identify(void);

void board_pins_save(void);   /* 空实现(保留兼容) */

/* 深睡唤醒掩码: PWR + LEFT (⛔ 故意不含 RIGHT/GPIO0 —— strapping 脚电平保不住会误唤醒) */
uint64_t board_wake_mask(void);

/* ---------------- 兼容宏: 只作"默认板型的编译期缺省" ----------------
 * ⚠ 运行时一律用 board_pins_get()->xxx; 这些宏仅在没有表上下文的地方兜底。 */
/* ★★★ 2026-10-16 linit: **删除板型识别**, 回到最简单方式 —— 编译期选一块板。
 *   换板 = 改这一行 + 重编 (与 v1.3 一样直观, 不扫描 I2C、不写 NVS)。 */
/* 选板: 0=小智  1=微雪  2=自制(仿微雪, 暂同微雪) */
/* ★★★ 逐项外设开关(排查屏冲突用): 1=启用, 0=不启动。
 *   用法: 一次只把一个改回 1, 烧录后看屏还亮不亮 ⇒ 亮=它不冲突, 黑=它就是冲突源。
 *   ⛔ 排查完**全部改回 1**(即恢复正常全功能)。 */
#define LINTOS_USE_SD        1    /* ← 本轮测试项: SD 卡 */
#define LINTOS_USE_INPUT     1    /* ← 待测: 按键 */
#define LINTOS_USE_TOUCH     0    /* ← 微雪无触摸: 保持关闭 */    /* 触摸(微雪无触摸, 先关) */
#define LINTOS_USE_AUDIO     1    /* ← 待测: 音频 */
#define LINTOS_USE_BATTERY   1
#define LINTOS_USE_GPS       0    /* 微雪无 GPS */
#define LINTOS_LCD_ONLY      0    /* 兼容旧开关: 1=全部跳过 */

#define BOARD_IS_WEIXUE      1   /* ← 当前: 微雪 */
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
