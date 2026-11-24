/* vid_tune.h — 画面优化器 (引擎通用, 灰度类模拟器共用)
 *
 * 为什么单独一层:
 *   所有彩色→灰度的模拟器 (GB/GBC/NES/MD/SMS/GAM4980/ArduBoy…) 最后都归结成
 *   "4 档 shade → 2x2 黑点图案"。以前这套图案是写死的，用户只能选 3 个预设。
 *   这里把 **shade → 黑点数(0..4)** 变成可调曲线，并在游戏画面上挂一个浮标：
 *   点浮标 → 顶部弹出细滑条，实时看效果，保存到 NVS / 取消还原。
 *
 * 与仙剑的关系:
 *   仙剑走的是自己的 8bpp→1bpp 管线 (pal_port.c)，那边有 256 级亮度的完整曲线；
 *   这里是 4 级 shade 的简化版，**公式保持一致**（对比以 128 为支点、白点/黑点截断），
 *   这样两边的观感调法一致。
 *
 * 接线方式 (三步):
 *   1) 引擎启动时:  vid_tune_attach("gbc");      // 载入该引擎自己的档
 *   2) 引擎退出时:  vid_tune_detach();
 *   3) 每合成帧(画完游戏内容、flush 之前):
 *        vid_tune_overlay_tick(lcd);            // 画浮标/面板 + 吃触摸
 *   参数变化时通过回调让调用方重建抖动表:
 *        vid_tune_set_rebuild_cb(board_shim_apply_tune);
 */
#ifndef VID_TUNE_H
#define VID_TUNE_H

#include <stdbool.h>
#include <stdint.h>
#include "st7305.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 滑杆顺序固定, 面板按这个顺序并列 4 条 */
typedef enum {
    VT_GAIN = 0,    /* 亮度 60..140  (%) */
    VT_CON     ,    /* 对比 100..200 (%) */
    VT_WHITE   ,    /* 白点 0..255 (越大越容易出纯白) */
    VT_BLACK   ,    /* 黑点 0..255 (越大越容易压成纯黑) */
    VT_PARAM_N
} vt_param_t;

void vid_tune_init(void);                       /* 读 NVS + 建默认档 (幂等) */
bool vid_tune_enabled(void);                    /* 引擎优化里的"画面优化器"总开关 */
void vid_tune_set_enabled(bool on);             /* 写 NVS */
void vid_tune_attach(const char *engine_id);    /* 进游戏: 载入该引擎档 (NULL=通用档) */
void vid_tune_detach(void);
bool vid_tune_is_panel_open(void);
void vid_tune_open_panel(void);   /* 外部入口(快捷栏)打开调节面板 */
int  vid_tune_get_pct(int i);          /* 参数归一化值 0..100 */
void vid_tune_set_pct(int i, int pct); /* 0..100, 立即生效 */
void vid_tune_save(void);              /* 存 NVS */
void vid_tune_restore(void);           /* 还原到展开前 */
void vid_tune_begin_edit(void);        /* 展开前备份 */

/* 关键输出: 4 档 shade → 黑点数(0..4)。引擎侧据此重建抖动表 */
void vid_tune_get_dots(uint8_t dots[4]);

/* 参数变化回调 (引擎侧重建抖动表用) */
void vid_tune_set_rebuild_cb(void (*cb)(void));
void vid_tune_refresh(void);                    /* 手动触发一次重建回调 */

/* 每合成帧调用: 画浮标; 点浮标 → 打开顶部面板; 面板打开期间独占触摸.
 * 返回 true 表示本帧画面已被面板接管 (调用方可选择跳过自己的 flush)。 */
bool vid_tune_overlay_tick(st7305_handle_t *lcd);

#ifdef __cplusplus
}
#endif
#endif /* VID_TUNE_H */
