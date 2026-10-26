#ifndef ST7305_H
#define ST7305_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST7305_WIDTH          400
#define ST7305_HEIGHT         300

#define ST7305_COLOR_BLACK    0
#define ST7305_COLOR_WHITE    1

typedef uint8_t st7305_color_t;

typedef struct {
    spi_device_handle_t spi;
    gpio_num_t          dc_gpio;
    gpio_num_t          rst_gpio;
    spi_host_device_t   spi_host;
    int                 spi_mosi;
    int                 spi_sclk;
    int                 spi_cs;
    int                 spi_freq;
    uint8_t            *fb;
    /* 竖屏绘制目标 (书库旋转左/右时由调用方启用): 非 NULL 时所有绘制写到 300×400
     * 逻辑缓冲, st7305_flush 自动旋转回物理 400×300 帧缓冲再送屏. 阅读器用自己的
     * pfb/portrait_rotate, 不启用此目标. */
    uint8_t            *pt_pfb;
    int                 pt_rot;
    /* 横屏 180° 翻转工作缓冲 (rot==1 时用, 80×37 竖排暂存) . */
    uint8_t            *r_flip1;
} st7305_handle_t;

typedef struct {
    gpio_num_t        dc_gpio;
    gpio_num_t        rst_gpio;
    spi_host_device_t spi_host;
    int               spi_mosi;
    int               spi_sclk;
    int               spi_cs;
    int               spi_freq;
} st7305_config_t;

st7305_config_t st7305_default_config(void);

esp_err_t st7305_init(st7305_handle_t *out, const st7305_config_t *config);

/* 浅睡唤醒后恢复显示: 重发初始化序列 + 清屏 + 整屏强刷, 消除"唤醒瞬间画面撕裂"。
 * 注意: 内部不打日志(唤醒瞬间 USB-Serial-JTAG 未重新枚举, vprintf 会 abort)。 */
esp_err_t st7305_resume(st7305_handle_t *dev);

void st7305_deinit(st7305_handle_t *dev);

/* 重建面板互斥量: 有任务持锁被强杀后调用, 否则后续送屏永远超时(表现为卡死) */
void st7305_lock_recover(void);

void st7305_clear(st7305_handle_t *dev, st7305_color_t color);

void st7305_draw_pixel(st7305_handle_t *dev, int x, int y, st7305_color_t color);

void st7305_set_clip_y(st7305_handle_t *dev, int y0, int y1);   /* ★y裁剪带(弹窗滚动用) */
void st7305_reset_clip_y(st7305_handle_t *dev);

/* 竖屏绘制目标开关: pfb=300×400 逻辑缓冲 (调用方分配), rot=2(左)/3(右).
 * 启用后所有绘制写入 pfb, st7305_flush 自动旋转回物理帧缓冲再送屏; pfb=NULL 关闭. */
void st7305_set_portrait(st7305_handle_t *dev, uint8_t *pfb, int rot);
/* 统一旋转: rot=0 原样 / 1 横屏180°(用 flip 缓冲翻转) / 2,3 竖屏(用 buf).
 * 书架与阅读器共用, 触摸由 input_set_screen_rotation(同值) 同步. */
void st7305_set_rotation(st7305_handle_t *dev, int rot, uint8_t *buf);

void st7305_draw_bitmap_1bit(st7305_handle_t *dev,
                              int x, int y, int w, int h,
                              const uint8_t *bitmap);

void st7305_draw_text(st7305_handle_t *dev, int x, int y, const char *text);

/* 带 1px 白描边的文字: 先对每个字形做一次 1px 二值膨胀画白 (描边), 再画黑色实体.
 * 用于虚拟按键等需要叠在黑色游戏画面上仍清晰可辨的场景 (V1.0.69). */
void st7305_draw_text_outlined(st7305_handle_t *dev, int x, int y, const char *text);

/* 深睡前把屏送进 SLPIN（Display OFF + Sleep In） */
esp_err_t st7305_sleep_in(st7305_handle_t *dev);

esp_err_t st7305_flush(st7305_handle_t *dev);

/* 从指定缓冲发送整帧 (与 st7305_flush 相同, 但数据源可独立于 dev->fb).
 * 用于双缓冲/快照刷新: 模拟任务持续写 fb, 刷新任务把快照拷出后异步送 SPI. */
esp_err_t st7305_flush_from(st7305_handle_t *dev, const uint8_t *fb);

/* === 屏保统一刷屏门禁 (V1.5.x) ===
 * 屏保激活时置 flush_blocked=true: 所有 st7305_flush/flush_from/flush_rotated
 * 全部变为 no-op → 任何程序/引擎(含无暂停接口的步步高/文曲星)都无法再往屏上写,
 * 视觉上"全部暂停/冻结". 壁纸独占渲染用 st7305_flush_override 强刷.
 * 由独立屏保监视模块在进入/退出屏保时设置. */
void       st7305_set_flush_blocked(bool block);
esp_err_t  st7305_flush_override(st7305_handle_t *dev);

/* 软件旋转整帧输出 (电子书阅读器用, 主菜单始终保持横屏)
 * rot: 0=上(横屏原样), 1=下(180°), 2=左(90°逆时针, 竖屏), 3=右(90°顺时针, 竖屏)
 * work_fb: 调用方提供的 15KB 备用缓冲 (PSRAM), 旋转时用于存放中间帧
 * 注意: 每次 flush 都会把面板扫描方向设回对应方向, 菜单的普通 flush 也会恢复横屏 */
esp_err_t st7305_flush_rotated(st7305_handle_t *dev, uint8_t rot, uint8_t *work_fb);

/* === 高性能 API === */

/* 1bpp bitmap 直接批量写入 ST7305 fb, 跳过 draw_pixel 调用.
 * bitmap 每字节 8 个水平像素 (MSB 在左), 内存中按行存储 (bytes_per_row = (w+7)/8).
 * 坐标 (x,y) 和尺寸 (w,h) 都是 1bpp bitmap 自身的; 整张图被绘制到 ST7305 屏幕.
 * ⚠️ 不做边界检查, 调用方需保证 x+w <= 400 且 y+h <= 300.
 * 比 st7305_draw_bitmap_1bit 快 5-10x (批量 PSRAM 写, 无函数调用开销). */
void st7305_blit_1bit(st7305_handle_t *dev,
                       int x, int y, int w, int h,
                       const uint8_t *bitmap);

/* 与 st7305_blit_1bit 相同, 但 bitmap 位=1 的像素写入"白" (置位 fb),
 * 位=0 保持原样. 用于白字黑底场景 (如 AVR 模拟器游戏画面). */
void st7305_blit_1bit_white(st7305_handle_t *dev,
                            int x, int y, int w, int h,
                            const uint8_t *bitmap);

/* 设置 ST7305 局部窗口 (CASET + RASET 一次), 之后 flush() 只刷这个矩形.
 * 用于只更新变化区域, 大幅减少 SPI 数据量.
 * ⚠️ 必须在 flush() 之前调用, 之后窗口仍保留 (不会自动恢复全屏). */
void st7305_set_window(st7305_handle_t *dev, int x, int y, int w, int h);

/* 恢复全屏窗口 */
void st7305_set_full_window(st7305_handle_t *dev);

/* 硬件对比度调整 (ST7305 0xC1 寄存器 VOP)
 * contrast: 0-63, 默认 0x41(65) 但 6-bit 范围内有效, 值越大像素越深
 * 实际范围限制在 0x20-0x3F 防止过深/过浅
 */
esp_err_t st7305_set_contrast(st7305_handle_t *dev, uint8_t contrast);

/* 反色显示开关 (0x21 = 反色, 0x20 = 正常) */
esp_err_t st7305_set_inversion(st7305_handle_t *dev, bool on);

/* 进入/退出低功耗模式 (0xD1 0x01 = 低功耗, 0xD1 0x00 = 高功耗) */
esp_err_t st7305_set_low_power(st7305_handle_t *dev, bool on);

/* 运行时设置帧率控制 (0xB2) — 测试不同刷新率对细纹/棋盘闪烁的影响 */
esp_err_t st7305_set_frame_rate(st7305_handle_t *dev, uint8_t v);
/* 读回当前 B2 值（驱动侧记录的真实值, 不是猜的） */
uint8_t    st7305_get_frame_rate(void);

/* 面板自刷新档位: lpm=true → 0x39 (≈1Hz 极省电) / false → 0x38 (≈32Hz) */
esp_err_t st7305_set_lpm(st7305_handle_t *dev, bool lpm);
/* 强制自刷新档位(排除性实验): -1=自动, 0=强制 HPM(0x38), 1=强制 LPM(0x39) */
void    st7305_set_lpm_force(int mode);
int     st7305_get_lpm_force(void);
/* 每帧调一次: 有绘制(drew=true)回 HPM; 连续 500ms 没画自动掉 LPM */
void st7305_idle_tick(st7305_handle_t *dev, bool drew, uint32_t now_ms);
/* 让局刷基线作废 (旋转变化/整帧重画后) */
void st7305_partial_invalidate_pub(void);
/* 局刷(只送变化的 12 行带)开关, **默认关** —— 整帧更稳, 面板 16Hz 下省字节无收益 */
/* 作废"已送帧"指纹: 面板内容被旁路刷过(屏保壁纸)之后必须调, 否则下次重绘会被
 * "内容没变"判据跳过, 屏幕停在旧画面上。 */
void st7305_invalidate_frame(void);

void st7305_set_partial(bool on);
bool st7305_get_partial(void);

/* ---- 深睡关屏方式 (2026-10-24, 实测哪一步会抹掉画面) ----
 * 0x28 Display OFF / 0x10 Sleep In 都可能让 MIP 屏"清屏"。
 * mode 0 = 不发命令(保持显示, 面板继续自刷新 ≈50~100µA)
 * mode 1 = 只发 0x10 Sleep In
 * mode 2 = 0x28 + 0x10 (默认, 最省电)
 * 默认值在 st7305.c 里, 用 `remcon slptest N` 现场试。 */
void st7305_set_sleep_mode(int mode);
int  st7305_get_sleep_mode(void);
/* 从 Sleep In / Display OFF 唤醒面板 (0x11 + 0x29) */
esp_err_t st7305_wake_out(st7305_handle_t *dev);

#ifdef __cplusplus
}
#endif

#endif
