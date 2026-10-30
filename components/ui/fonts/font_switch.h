#ifndef FONT_SWITCH_H
#define FONT_SWITCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 系统 UI 字体切换 —— **按字号档位独立选择**（2026-10-30 改）
 *
 * ⚠ 只作用于**系统 UI 字库**（font_zh16 / font_zh，ZH1FNT01 格式，appdata mmap）。
 *   与阅读器的 LBF 字体是两套机制，互不影响 —— 阅读器字体仍在书内菜单里选。
 *
 * 两个档位完全独立：
 *   16px 档：默认槽 FONT_SLOT_16  ↔  候选槽 FONT_SLOT_ALT16
 *   24px 档：默认槽 FONT_SLOT_ZH  ↔  候选槽 FONT_SLOT_ALT24
 * 切 16px 不动 24px，反之亦然。切换 = 把 font_zh16 / font_zh 的内部 mmap
 * 重绑到目标槽，UI 绘制代码零改动。
 *
 * 槽内若是空的（0xFF）或格式不对，rebind 校验失败 → 自动回退该档默认槽并返回 false。
 * 持久化由应用层负责（page_settings / main），本层不碰 NVS，避免 ui→services 反向依赖。
 */

/* 16px 档候选 */
typedef enum {
    F16_DEFAULT = 0,   /* 文泉驿点阵宋 16（固件默认，槽 FONT_SLOT_16） */
    F16_ALT     = 1,   /* 长坂点宋 16（对比，槽 FONT_SLOT_ALT16） */
    F16_COUNT   = 2,
} ui_font16_t;

/* 24px 档候选 */
typedef enum {
    F24_DEFAULT = 0,   /* HZK24S 宋体（固件默认，槽 FONT_SLOT_ZH） */
    F24_ALT     = 1,   /* 方正像素 24（内部测试，槽 FONT_SLOT_ALT24） */
    F24_COUNT   = 2,
} ui_font24_t;

/* 显示名（UTF-8，可直接进列表）。越界返回默认名。 */
const char *font16_name(int i);
const char *font24_name(int i);

/* 应用某档字体（**不落盘**）。失败自动回退该档默认槽并返回 false。 */
bool font16_apply(int i);
bool font24_apply(int i);

/* 当前生效的档位（-1 表示尚未应用过任何非默认项 = 默认槽） */
int font16_get(void);
int font24_get(void);

/* 该档位是否有可用字库（试绑一次再绑回，不改变持久化状态） */
bool font16_available(int i);
bool font24_available(int i);

#ifdef __cplusplus
}
#endif

#endif
