/* pal_port.h — SDLPal 到 LinTOS 的输出端口
 *
 * 职责：把 SDL 的 8bpp + 256 色调色板画面转成 ST7305 的 1bpp 单色帧并上屏。
 * 为什么单独一层：游戏逻辑只管往 SDL Surface 写像素，转换/抖动/缩放/叠加
 *       全部在这里做，改显示策略不碰 SDLPal 源码。
 *
 * 虚拟按键不在这一层画 —— 直接用系统现成的 `components/ui/virtual_keys`
 * （GBC/NES 那套：左方向 + 右 A/B + 底 SEL/STA），present 末尾叠加。
 */
#ifndef PAL_PORT_H
#define PAL_PORT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 绑定当前活动画面（SDL_SetVideoMode 时调用） */
void pal_port_set_source(const uint8_t *pixels, int w, int h, const uint8_t *palette);

/* 调色板变化（SDL_SetPalette 时调用）。palette 为 256*(r,g,b,unused) */
void pal_port_set_palette(const uint8_t *palette);

/* 标脏：本帧画面有更新，等下一次 present 上屏 */
void pal_port_mark_dirty(void);

/* 真正上屏（页面 poll 里调用；无脏帧直接返回）。
 * 末尾会叠加系统虚拟按键（virtual_keys 已 enabled 时）再一次 flush。 */
void pal_port_present(void *lcd_handle);

/* 载入进度条: 首帧是否已经真正上屏 (false = 还在载入, 别画虚拟键盘) */
bool     pal_port_first_frame_done(void);
/* 载入阶段开关: on = 只采样不送屏(屏幕留给载入页); off = 恢复正常送屏 */
void     pal_port_pause_output(bool on);
/* 画面是否已经有真内容(连续 3 帧暗部 >= 6%): 白屏淡入阶段为 false */
bool     pal_port_scene_ready(void);
int      pal_port_scene_dark_pct(void);

/* 载入阶段(真实事件, 不含时间估算):
 *   0=启动 1=视频就绪 2=建屏 3=音频就绪 4=首帧已上屏 5=首场景已有内容 */
void     pal_port_note_stage(int st);
int      pal_port_load_stage(void);
uint32_t pal_port_frame_count(void);

/* 缩放：0 = 全屏铺满（默认）；其他值为百分比，超出屏幕自动等比缩回 */
void pal_port_set_zoom_pct(int pct);
int  pal_port_get_zoom_pct(void);

/* ---- 自适应灰度（分位拉伸）----
 * 默认**关**。开着会让"亮场景更白、暗场景更黑"(动态对比度)，1bpp 上很容易整片发白。 */
void pal_port_set_auto_level(bool on);
bool pal_port_get_auto_level(void);

/* ---- gamma 曲线（主旋钮）----
 * x100 = 30..100 即 gamma=0.30..1.00。越小越白(中暗部抬得越高)，黑位/高光不动。
 * 这是"哪些区域算白"的策略，不是单纯调亮度。默认 55。 */
void pal_port_set_gamma(int x100);
int  pal_port_get_gamma(void);

/* ---- S 曲线对比度 ----
 * x100 = 100..250，100=不扩张。以 128 为支点线性拉开明暗：黑的更黑、白的更白，
 * 中间调只留过渡。默认 160。现场 `remcon palcon N`。 */
void pal_port_set_contrast(int x100);
int  pal_port_get_contrast(void);

/* ---- 白点 ----
 * 曲线值 ≥ pt 一律当纯白（0 = 关闭）。只把"接近白"的区域推成纯白，不动黑位。
 * 默认 176。现场 `remcon palwhite N`。 */
void pal_port_set_white_point(int pt);
int  pal_port_get_white_point(void);

/* ---- 黑点 ----
 * 曲线值 ≤ pt 一律当纯黑（0 = 关闭）。默认 0。现场 `remcon palblack N`。 */
void pal_port_set_black_point(int pt);
int  pal_port_get_black_point(void);

/* ---- 亮度增益（线性微调）----
 * 100 = 不动；现场 `remcon palbr N`。一般先用 palgam 调策略，再拿这个补一点。 */
void pal_port_set_brightness(int pct);
int  pal_port_get_brightness(void);

/* 诊断: 把当前画面的亮度分位/各档像素占比打到串口 (调曲线用, 不靠猜) */
void pal_port_dump_stats(void);

/* ---- 仙剑单独限帧 ----
 * 全工程别处不限速；只有这一路限。做法是超时**跳过**(不阻塞调用方)，
 * 跳过时保留脏标记，等窗口到了再画最新一帧。fps<=0 表示不限。默认 20。 */
void pal_port_set_max_fps(int fps);
int  pal_port_get_max_fps(void);

#ifdef __cplusplus
}
#endif
#endif /* PAL_PORT_H */
