/* st7305_rotate.h — 底层统一旋转模块 (ST7305 1bpp 压缩屏)
 *
 * 目的: 把"旋转"彻底下沉到驱动底层, 书架与阅读器共用同一套逻辑, 消除两处各自
 * 实现的旋转(方向不一致/白缝的来源). 应用层只按一个坐标系统一绘制, 旋转在此模块
 * 内完成并经由 st7305_flush_from 送屏.
 *
 * 物理现状(为什么必须在这层做):
 *   ST7305 帧存 = 25 列单元 × 200 行 × 3 字节 = 15000 字节; 每字节塞 2列×4行
 *   (MicroPython/u8g2 同样结论). 因此:
 *     - 90°/270°(竖屏): 硬件 MADCTL MV + 竖屏重排 (干净, 走硬件).
 *     - 180°(下): 硬件 MY 会把"每字节4行"行块整体倒序 -> 交界处白缝. 必须内存翻转.
 *   本模块两种都处理, 上层无感.
 *
 * 坐标契约:
 *   - rot=0/1(横屏):  逻辑面 400×300, 画进 st7305 -> 应用用 st7305_draw_pixel 即可.
 *   - rot=2/3(竖屏, 300×400): 应用把 300×400 逻辑内容画到调用方提供的竖屏缓冲,
 *     再调 st7305_rotate_flush_portrait 送屏.
 */
#ifndef ST7305_ROTATE_H
#define ST7305_ROTATE_H

#include <stdint.h>
#include "esp_err.h"
#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST7305_PT_W 300   /* 竖屏逻辑宽 */
#define ST7305_PT_H 400   /* 竖屏逻辑高 */

/* rot==2/3 (竖屏) 时, 应用把 300×400 顶层竖屏缓冲(15000B)旋转到横屏 fb 后送屏.
 * 旋转方向与硬件 MADCTL_PORTRAIT 一致, 保证上下左右全统一. */
esp_err_t st7305_rotate_portrait_flush(st7305_handle_t *dev,
                                       const uint8_t *pt_buf,   /* 300×400 竖屏缓冲 */
                                       int rot,                 /* 2=左 3=右 */
                                       uint8_t *land_work);     /* 15000B 横屏工作缓冲 */

/* rot==1 (下, 横屏180°): 内存翻转横屏 fb -> 工作缓冲后送屏.
 * 输入内容在 dev->fb(400×300), 结果写入 land_work(15000B), 送横屏 MADCTL. */
esp_err_t st7305_rotate_flip180_flush(st7305_handle_t *dev,
                                      uint8_t *flip_work);     /* 15000B 工作缓冲 */

#ifdef __cplusplus
}
#endif

#endif /* ST7305_ROTATE_H */