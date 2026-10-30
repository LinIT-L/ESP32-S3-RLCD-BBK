/**
 * ui_scroll.h — 全项目统一「跟手滚动」控制器（纯逻辑，不画内容）
 *
 * ★★★ 2026-10-24 linit 定案: **全项目列表/文本滚动只允许用这一套**。
 *   物理参数照抄弹窗列表 (os_dialog.c, 已被认可为"最流畅那档")。
 *
 * 支持 3 种滚动模式 × 4 个开关，页面用 ui_scroll_config() 一次配好：
 *
 *   UI_SCROLL_LIST   行吸附   —— 设置页/文件列表/应用列表（停稳对齐整行）
 *   UI_SCROLL_PIXEL  像素自由 —— 阅读器正文/终端输出/大图浏览
 *   UI_SCROLL_PAGE   整页翻   —— 电子书/漫画（停稳对齐整页，过半页才翻）
 *
 *   UI_SCROLL_F_NO_FLING   关掉惯性（只有跟手，松手立刻停）
 *   UI_SCROLL_F_NO_BOUNCE  关掉越界回弹（硬边界，拉不动）
 *   UI_SCROLL_F_HORIZ      横向滚动（主轴改用 x）
 *   UI_SCROLL_F_LOOP       循环滚动（到头接尾，做轮盘/环形菜单）
 *
 * 用法（页面 poll 里）:
 *   static ui_scroll_t sc;
 *   ui_scroll_config(&sc, UI_SCROLL_LIST, 0);          // ① 配模式（一次）
 *   ui_scroll_init(&sc, 行高, 可视高, 内容高);          // ② 进页面 / 内容变化
 *   bool down = input_get_touch_pos(&tx, &ty);
 *   if (ui_scroll_touch_xy(&sc, tx, ty, down)) ctx->needs_redraw = true;
 *   if (ui_scroll_poll(&sc))                   ctx->needs_redraw = true;   // ★ 每帧
 *   if (ui_scroll_dragging(&sc)) { 跳过点击/长按处理 }
 *   if (ui_scroll_busy(&sc))     { 跳过底层页面重绘 (手感才跟得住) }
 *   int y0 = LIST_Y - ui_scroll_offset(&sc);
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_SCROLL_LIST  = 0,   /* 行吸附（默认）: 停稳对齐 row_px */
    UI_SCROLL_PIXEL = 1,   /* 像素自由: 停在任意位置 */
    UI_SCROLL_PAGE  = 2,   /* 整页翻: 停稳对齐 view_px */
} ui_scroll_mode_t;

#define UI_SCROLL_F_NO_FLING   0x01u   /* 无惯性 */
#define UI_SCROLL_F_NO_BOUNCE  0x02u   /* 无越界回弹（硬边界） */
#define UI_SCROLL_F_HORIZ      0x04u   /* 横向（主轴 = x） */
#define UI_SCROLL_F_LOOP       0x08u   /* 循环（到头接尾） */
/* 吸附瞬跳（不缓动）。凡把 off 反算成"行下标 + 行内像素"两段来做渲染/命中的调用方
 * （典型: 弹窗列表 os_dialog）**必须**开它 —— 整除反算遇到缓动中间态会让行下标
 * 在两行之间反复跳，表现为"拖动一直闪、松手又卡回去"。2026-10-28 */
#define UI_SCROLL_F_SNAP_INSTANT 0x10u

typedef struct {
    int      row_px;      /* 行高（LIST 模式的吸附步长） */
    int      view_px;     /* 可视长度 */
    int      content_px;  /* 内容总长度 */
    int      off;         /* 偏移（0..max），恒在合法范围内 */
    int      sub;         /* 越界余量: <0 过头, >0 过尾；渲染用 off+sub */
    int      snap_to;     /* 吸附目标（仅 snapping=true 时有效） */
    bool     snapping;    /* 正在缓动到 snap_to（2026-10-28: 吸附改成缓动） */
    float    fling;       /* 松手后的速度 px/帧（带符号） */
    int      anchor;      /* 拖动起点主轴坐标 */
    int      base_off;    /* 拖动起点时的偏移 */
    int      last_axis;   /* 上一帧主轴坐标（算瞬时速度） */
    bool     dragging;    /* 已判定为拖动（≥12px） */
    bool     pressed;     /* 手指按住中（不能拿 anchor==0 判，可能正好在 0 按下） */
    uint32_t last_touch_ms;   /* 上次收到触摸的时刻: 超时兜底用, 防页面切走时 pressed 卡住 */
    uint8_t  mode;        /* ui_scroll_mode_t */
    uint8_t  flags;       /* UI_SCROLL_F_* 位或 */
} ui_scroll_t;

/* ① 配置滚动模式（在 ui_scroll_init 之前调；之后调也立即生效） */
void ui_scroll_config(ui_scroll_t *s, ui_scroll_mode_t mode, uint8_t flags);

/* ② 初始化/内容变化时重设（off 会被夹紧；sub/fling 清零） */
void ui_scroll_init(ui_scroll_t *s, int row_px, int view_px, int content_px);

/* 只改内容长度（新增/删除条目后调用，保留当前位置） */
void ui_scroll_set_content(ui_scroll_t *s, int content_px);

/* 回到头部并把速度清零（切分类/换目录时调） */
void ui_scroll_to_top(ui_scroll_t *s);

/* 页面切走/内容重建时清状态（拖动中、有惯性时都清干净，避免带进下一页） */
void ui_scroll_cancel(ui_scroll_t *s);

/* 把 [item_y, item_y+item_h) 纳入可视区（按键上下选中时用）。
 * 已经可见就不动 —— 不会因为"选中项往下走一格"就整屏跳。 */
bool ui_scroll_ensure_visible(ui_scroll_t *s, int item_y, int item_h);

/* ③ 每帧喂触摸（x/y 都传，横向模式自动取 x）。返回 true = 偏移变了需要重绘。 */
bool ui_scroll_touch_xy(ui_scroll_t *s, int x, int y, bool down);

/* 竖向便捷版（等价 touch_xy(s, 0, y, down)） */
bool ui_scroll_touch(ui_scroll_t *s, int y, bool down);

/* ④ 每帧推进惯性/回弹/吸附。返回 true = 画面有变化需重绘。 */
bool ui_scroll_poll(ui_scroll_t *s);

/* 是否正在拖动（页面据此屏蔽点击/长按/行点选） */
bool ui_scroll_dragging(const ui_scroll_t *s);

/* 是否还在动（拖动 / 惯性 / 回弹未收敛）。为真时页面应尽早 return，别重排整页。 */
bool ui_scroll_busy(const ui_scroll_t *s);

/* 最大偏移 */
int  ui_scroll_max(const ui_scroll_t *s);

/* 渲染偏移 = off + sub（内容起点 = 列表顶 - 本值） */
int  ui_scroll_offset(const ui_scroll_t *s);

/* 当前首个可见项下标（按吸附步长算） */
int  ui_scroll_first_row(const ui_scroll_t *s);

/* 统一滚动条（点阵渐隐，符合视觉铁律：不画整条实线）。
 * 竖向: a0=x(条中心), a1=轨道上端, a2=轨道下端
 * 横向: a0=y(条中心), a1=轨道左端, a2=轨道右端（按 HORIZ 自动切换）
 * 内容不足一屏时自动不画。 */
void ui_scroll_drawbar(const ui_scroll_t *s, void *lcd, int a0, int a1, int a2);

#ifdef __cplusplus
}
#endif
