/* V1.0.68: 游戏内屏幕虚拟按键 (触摸 -> joypad)
 * 布局仿 WiFi 网页手柄: 左侧十字方向键 + 右侧 A/B 斜排 + 底部 Select/Start.
 * 1bit 反射式 LCD 无真正透明, 用白色填充 + 黑色描边表示按键, 按下整键反色.
 *
 * 本模块不直接读触摸芯片 (触摸由 input.c 单点轮询, 避免 GT911 状态寄存器被
 * 两次读取互相偷走事件), 只消费输入层缓存的屏幕坐标 virtual_keys_poll().
 */
#include "virtual_keys.h"

#include <string.h>

/* joypad 位定义 (低电平有效) */
#define VK_BIT_A      0
#define VK_BIT_B      1
#define VK_BIT_SELECT 2
#define VK_BIT_START  3
#define VK_BIT_RIGHT  4
#define VK_BIT_LEFT   5
#define VK_BIT_UP     6
#define VK_BIT_DOWN   7

/* 摇杆 (中间圆圈): 按住中心圆圈后按偏移方向判定 上下左右 */
#define VK_STICK_CX      67
#define VK_STICK_CY      231
#define VK_STICK_R       20    /* 中心圆圈半径 */
#define VK_STICK_THRESH  8     /* 判定方向的最小偏移 */

static bool s_enabled = false;
static volatile uint8_t s_joypad = 0xFF;   /* 跨任务 (模拟任务 poll, 刷新任务 draw) */
static bool s_stick_active = false;        /* 摇杆激活后拖出圆圈仍持续, 直到松手 */

/* 按键几何: 命中框 (x0,y0)-(x1,y1), 圆心/文字由绘制函数按常量计算 */
typedef struct {
    int     x0, y0, x1, y1;
    uint8_t bit;
} vk_hit_t;

/* 方向键 (半径 20, 左移5 上移5) + A/B (半径 24, 下移10) + Select/Start 药丸 (贴屏幕下缘) */
static const vk_hit_t s_hits[] = {
    {  47, 167,  87, 207, VK_BIT_UP    },   /* 上 (中心 67,187) */
    {   3, 211,  43, 251, VK_BIT_LEFT  },   /* 左 (中心 23,231) */
    {  91, 211, 131, 251, VK_BIT_RIGHT },   /* 右 (中心 111,231) */
    {  47, 255,  87, 294, VK_BIT_DOWN  },   /* 下 (中心 67,275) */
    { 336, 182, 384, 230, VK_BIT_A     },   /* A (中心 360,206) */
    { 288, 230, 336, 278, VK_BIT_B     },   /* B (中心 312,254) */
    { 140, 278, 196, 298, VK_BIT_SELECT},   /* SELECT 贴下缘 */
    { 204, 278, 260, 298, VK_BIT_START },   /* START 贴下缘 */
};
#define VK_HIT_COUNT (int)(sizeof(s_hits) / sizeof(s_hits[0]))

void virtual_keys_set_enabled(bool en)
{
    s_enabled = en;
    if (!en) {
        s_joypad = 0xFF;
        s_stick_active = false;
    }
}

bool virtual_keys_is_enabled(void)
{
    return s_enabled;
}

uint8_t virtual_keys_poll(int touch_x, int touch_y, bool touch_down)
{
    if (!s_enabled) {
        s_joypad = 0xFF;
        s_stick_active = false;
        return s_joypad;
    }
    uint8_t j = 0xFF;
    if (touch_down) {
        /* 摇杆: 中心圆圈内按下即激活, 激活后拖出圆圈范围仍持续判定方向, 直到松手 */
        int dx = touch_x - VK_STICK_CX;
        int dy = touch_y - VK_STICK_CY;
        if (s_stick_active ||
            (dx * dx + dy * dy <= VK_STICK_R * VK_STICK_R)) {
            s_stick_active = true;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            if (adx >= VK_STICK_THRESH || ady >= VK_STICK_THRESH) {
                if (adx > ady) {
                    if (dx < 0) j &= (uint8_t)~(1u << VK_BIT_LEFT);
                    else        j &= (uint8_t)~(1u << VK_BIT_RIGHT);
                } else {
                    if (dy < 0) j &= (uint8_t)~(1u << VK_BIT_UP);
                    else        j &= (uint8_t)~(1u << VK_BIT_DOWN);
                }
            }
        }
        /* 常规按键命中 (摇杆激活时跳过 4 个方向键, 避免与摇杆重复) */
        for (int i = 0; i < VK_HIT_COUNT; i++) {
            const vk_hit_t *h = &s_hits[i];
            if (s_stick_active &&
                (h->bit == VK_BIT_UP || h->bit == VK_BIT_DOWN ||
                 h->bit == VK_BIT_LEFT || h->bit == VK_BIT_RIGHT)) {
                continue;
            }
            if (touch_x >= h->x0 && touch_x <= h->x1 &&
                touch_y >= h->y0 && touch_y <= h->y1) {
                j &= (uint8_t)~(1u << h->bit);
            }
        }
    } else {
        s_stick_active = false;
    }
    s_joypad = j;
    return j;
}

uint8_t virtual_keys_get_joypad(void)
{
    return s_joypad;
}

/* ==================== 绘制 ==================== */

static void vk_fill_circle(st7305_handle_t *l, int cx, int cy, int r,
                           st7305_color_t c)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r)
                st7305_draw_pixel(l, cx + x, cy + y, c);
}

/* 1 像素宽圆线 (Bresenham 中点圆) */
static void vk_draw_circle_outline(st7305_handle_t *l, int cx, int cy, int r,
                                   st7305_color_t c)
{
    int x = 0, y = r, d = 3 - 2 * r;
    while (y >= x) {
        st7305_draw_pixel(l, cx + x, cy + y, c);
        st7305_draw_pixel(l, cx + y, cy + x, c);
        st7305_draw_pixel(l, cx - x, cy + y, c);
        st7305_draw_pixel(l, cx - y, cy + x, c);
        st7305_draw_pixel(l, cx + x, cy - y, c);
        st7305_draw_pixel(l, cx + y, cy - x, c);
        st7305_draw_pixel(l, cx - x, cy - y, c);
        st7305_draw_pixel(l, cx - y, cy - x, c);
        x++;
        if (d < 0) d += 4 * x + 6;
        else { d += 4 * (x - y) + 10; y--; }
    }
}

/* 点 (px,py) 是否在三角形 (x0,y0)(x1,y1)(x2,y2) 内 */
static bool vk_pt_in_tri(int px, int py,
                         int x0, int y0, int x1, int y1, int x2, int y2)
{
    int d1 = (px - x2) * (y0 - y2) - (x0 - x2) * (py - y2);
    int d2 = (px - x0) * (y1 - y0) - (x1 - x0) * (py - y0);
    int d3 = (px - x1) * (y2 - y1) - (x2 - x1) * (py - y1);
    bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

/* 方向箭头三角形: dir 0=上 1=下 2=左 3=右, half=半宽 */
static void vk_draw_arrow(st7305_handle_t *l, int cx, int cy, int half, int dir,
                          st7305_color_t c)
{
    int x0 = cx, y0 = cy, x1 = cx, y1 = cy, x2 = cx, y2 = cy;
    switch (dir) {
    case 0: x0 = cx - half; y0 = cy + half; x1 = cx + half; y1 = cy + half; x2 = cx; y2 = cy - half; break;
    case 1: x0 = cx - half; y0 = cy - half; x1 = cx + half; y1 = cy - half; x2 = cx; y2 = cy + half; break;
    case 2: x0 = cx + half; y0 = cy - half; x1 = cx + half; y1 = cy + half; x2 = cx - half; y2 = cy; break;
    default: x0 = cx - half; y0 = cy - half; x1 = cx - half; y1 = cy + half; x2 = cx + half; y2 = cy; break;
    }
    int minx = cx - half, maxx = cx + half, miny = cy - half, maxy = cy + half;
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++)
            if (vk_pt_in_tri(x, y, x0, y0, x1, y1, x2, y2))
                st7305_draw_pixel(l, x, y, c);
}

void virtual_keys_draw(st7305_handle_t *lcd)
{
    if (!s_enabled || !lcd || !lcd->fb) return;
    uint8_t j = s_joypad;

    bool up_p    = !(j & (1u << VK_BIT_UP));
    bool down_p  = !(j & (1u << VK_BIT_DOWN));
    bool left_p  = !(j & (1u << VK_BIT_LEFT));
    bool right_p = !(j & (1u << VK_BIT_RIGHT));
    bool a_p     = !(j & (1u << VK_BIT_A));
    bool b_p     = !(j & (1u << VK_BIT_B));

    /* 方向键: 不画圆框, 只画箭头; 按下时用实心圆作反白背景 */
    if (up_p)    vk_fill_circle(lcd, 67, 187, 18, ST7305_COLOR_BLACK);
    if (left_p)  vk_fill_circle(lcd, 23, 231, 18, ST7305_COLOR_BLACK);
    if (right_p) vk_fill_circle(lcd, 111, 231, 18, ST7305_COLOR_BLACK);
    if (down_p)  vk_fill_circle(lcd, 67, 275, 18, ST7305_COLOR_BLACK);
    vk_draw_arrow(lcd, 67, 187, 8, 0, up_p    ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK);
    vk_draw_arrow(lcd, 23, 231, 8, 2, left_p  ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK);
    vk_draw_arrow(lcd, 111, 231, 8, 3, right_p ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK);
    vk_draw_arrow(lcd, 67, 275, 8, 1, down_p  ? ST7305_COLOR_WHITE : ST7305_COLOR_BLACK);

    /* 摇杆: 中心 1px 圆线 + 旋钮 */
    vk_draw_circle_outline(lcd, VK_STICK_CX, VK_STICK_CY, VK_STICK_R, ST7305_COLOR_BLACK);
    {
        int kx = VK_STICK_CX, ky = VK_STICK_CY;
        if (up_p)    ky -= 8;
        if (down_p)  ky += 8;
        if (left_p)  kx -= 8;
        if (right_p) kx += 8;
        vk_fill_circle(lcd, kx, ky, 6, ST7305_COLOR_BLACK);
    }

    /* A/B: 1px 圆线 + 字母 (按下整圆填充) */
    vk_draw_circle_outline(lcd, 360, 206, 24, ST7305_COLOR_BLACK);
    vk_draw_circle_outline(lcd, 312, 254, 24, ST7305_COLOR_BLACK);
    if (a_p) vk_fill_circle(lcd, 360, 206, 24, ST7305_COLOR_BLACK);
    if (b_p) vk_fill_circle(lcd, 312, 254, 24, ST7305_COLOR_BLACK);
    if (!a_p) st7305_draw_text(lcd, 354, 199, "A");
    if (!b_p) st7305_draw_text(lcd, 306, 247, "B");

    /* Select/Start: 只画文字, 不画方框 */
    st7305_draw_text(lcd, 150, 281, "SEL");
    st7305_draw_text(lcd, 217, 281, "STA");
}
