#include <stdlib.h>
#include <string.h>
#include "type.h"
#include "base.h"
#include "gmode16.h"
#include "../include/lavaxvm.h"

int ScreenWidth = LCD_WIDTH;
int ScreenHeight = LCD_HEIGHT;
int voff;
int vcnt;
byte *BmpData;
byte GRAY;
static byte zoom = 1;
static int display_source_width;
static int display_source_height;
static int display_canvas_width;
static int display_canvas_height;
static int display_scale = 1;
static int display_mask_enabled;
static byte palette[256 * 3];
static byte saved_palette[256 * 3];
/* 屏平面缓冲 (98KB), 由宿主 lavax_emu.c 分配在 PSRAM, 避免撑爆内部 RAM */
byte *bmpdata_buf = NULL;
extern word graph_mode;
extern word bgcolor;
extern word fgcolor;

void SetZoom(byte value) { zoom = value; (void)zoom; }

static int config_number(const char *value, int minimum, int maximum)
{
    char *end;
    long number = strtol(value, &end, 10);

    if (!value[0] || *end || number < minimum || number > maximum)
        return -1;
    return (int)number;
}

void ResetDisplayConfig(void)
{
    display_source_width = 0;
    display_source_height = 0;
    display_canvas_width = 0;
    display_canvas_height = 0;
    display_scale = 1;
    display_mask_enabled = 0;
}

int SetDisplayConfig(char *name, char *value)
{
    int number;

    if (strcmp(name, "SOURCE_WIDTH") == 0)
    {
        number = config_number(value, 0, LCD_WIDTH);
        if (number >= 0)
            display_source_width = number;
        return 1;
    }
    if (strcmp(name, "SOURCE_HEIGHT") == 0)
    {
        number = config_number(value, 0, LCD_HEIGHT);
        if (number >= 0)
            display_source_height = number;
        return 1;
    }
    if (strcmp(name, "CANVAS_WIDTH") == 0)
    {
        number = config_number(value, 0, LCD_WIDTH);
        if (number >= 0)
            display_canvas_width = number;
        return 1;
    }
    if (strcmp(name, "CANVAS_HEIGHT") == 0)
    {
        number = config_number(value, 0, LCD_HEIGHT);
        if (number >= 0)
            display_canvas_height = number;
        return 1;
    }
    if (strcmp(name, "DISPLAY_SCALE") == 0)
    {
        number = config_number(value, 1, 8);
        if (number >= 0)
            display_scale = number;
        return 1;
    }
    if (strcmp(name, "MASK_ENABLE") == 0)
    {
        number = config_number(value, 0, 1);
        if (number >= 0)
            display_mask_enabled = number;
        return 1;
    }
    return 0;
}

void SaveDisplayConfig(struct TASK *target)
{
    target->display_source_width = display_source_width;
    target->display_source_height = display_source_height;
    target->display_canvas_width = display_canvas_width;
    target->display_canvas_height = display_canvas_height;
    target->display_scale = display_scale;
    target->display_mask_enabled = display_mask_enabled;
}

void LoadDisplayConfig(const struct TASK *source)
{
    display_source_width = source->display_source_width;
    display_source_height = source->display_source_height;
    display_canvas_width = source->display_canvas_width;
    display_canvas_height = source->display_canvas_height;
    display_scale = source->display_scale;
    display_mask_enabled = source->display_mask_enabled;
}

int GetDisplaySourceWidth(int header_width)
{
    return display_source_width ? display_source_width : header_width;
}

int GetDisplaySourceHeight(int header_height)
{
    return display_source_height ? display_source_height : header_height;
}

void SetPalette(void)
{
    int i;

    for (i = 0; i < 256; i++)
    {
        byte value;

        if (graph_mode == 1)
            value = i == 1 ? 0 : 255;
        else if (graph_mode == 8)
            value = (byte)i;
        else
            value = (byte)((255 * (15 - (i & 15))) / 15);

        palette[i * 3] = palette[i * 3 + 1] = palette[i * 3 + 2] = value;
    }
}

void lav_setpalette(byte from, int num, byte *addr)
{
    int i;
    for (i = 0; i < num && from + i < 256; i++)
    {
        palette[(from + i) * 3] = addr[i * 4 + 2];
        palette[(from + i) * 3 + 1] = addr[i * 4 + 1];
        palette[(from + i) * 3 + 2] = addr[i * 4];
    }
}

void ClearNdsScreen(void)
{
    /* ★ 2026-10-31 修复: 原来写 `graph_mode == 8 ? 0 : 255`, 但 LavaX 的
     * 索引语义是「0 = 背景/白, 数值越大越暗」(与原作者 SetSBuffer 的
     * memset(BmpData,0,...) 一致)。graph_mode==4 时 15 才是黑, 清 255 会被
     * present_to_gray 映射成 idx 15 → **整屏黑**; 而 1 色模式清 255 与清 0
     * 结果都是白(idx!=1), 8 色模式原本就走 0 —— 所以统一清 0 后:
     *   1 色: 不变(白)   4 色: 由黑修正为白   8 色: 不变
     * 仅仅修掉 4 色模式下清屏反色这一支, 不影响另外两种模式。 */
    if (bmpdata_buf)
        memset(bmpdata_buf, 0, SCREEN_BUFFER_SIZE);
}

void DmaRefresh(void)
{
    int canvas_width = display_canvas_width ? display_canvas_width : ScreenWidth;
    int canvas_height = display_canvas_height ? display_canvas_height : ScreenHeight;

    lavax_platform_present_indexed(BmpData, ScreenWidth, ScreenHeight, LCD_WIDTH,
                                    palette, graph_mode, canvas_width, canvas_height,
                                    display_scale, display_mask_enabled);
}

void SetWindow(void)
{
    ScreenWidth = ScreenWidth > LCD_WIDTH ? LCD_WIDTH : ScreenWidth;
    ScreenHeight = ScreenHeight > LCD_HEIGHT ? LCD_HEIGHT : ScreenHeight;
    voff = 0;
    vcnt = LCD_WIDTH * ScreenHeight;
}

void Color256Init(void)
{
    BmpData = bmpdata_buf;
    graph_mode = 8;
    bgcolor = 0;
    fgcolor = 255;
    SetWindow();
    SetPalette();
    ClearNdsScreen();
}

void Save_Palette(void) { memcpy(saved_palette, palette, sizeof(palette)); }
void Load_Palette(void) { memcpy(palette, saved_palette, sizeof(palette)); }
