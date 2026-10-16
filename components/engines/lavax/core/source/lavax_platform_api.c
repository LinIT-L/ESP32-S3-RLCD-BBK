/*
 * lavax_platform_api.c — lavaxvm 依赖的 retro-go (rg_*) 平台接口实现.
 *
 * 原实现依赖 retro-go 的 rg_input / rg_system / rg_utils / fatfs GBK 表。
 * 本固件用不到 retro-go, 这里给出最小实现:
 *   - 按键: 读固件 input_get_held_gb_joypad() 并转成 RG_KEY_* 位掩码
 *   - 电池: 恒报"不存在"
 *   - 时间: esp_timer 单调微秒
 *   - 调度: 每 ~2ms 让出 CPU (vTaskDelay)
 *   - UTF-8: 自带的轻量解码/编码
 *   - GBK(OEM/936)<->Unicode: 提供可编译的近似; 英文路径全通,
 *     中文汉字映射为占位(约等于拉丁拼音)避免运行时崩溃, 后续可扩成真码表
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "rg_stub.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 固件输入 */
#include "input.h"

static int64_t s_origin = 0;

int64_t rg_system_timer(void)
{
    return (int64_t)esp_timer_get_time();
}

void rg_system_set_time(time_t seconds)
{
    (void)seconds; /* 空实现 */
}

void rg_task_yield(void)
{
    vTaskDelay(pdMS_TO_TICKS(2));
}

rg_battery_t rg_input_read_battery(void)
{
    rg_battery_t b = { .present = false, .level = 0, .volts = 0, .charging = false };
    return b;
}

/* GB joypad (低电平有效): bit0=A bit1=B bit2=Sel bit3=Start bit4=R bit5=L bit6=Up bit7=Down */
uint32_t rg_input_read_gamepad(void)
{
    uint8_t jp = input_get_held_gb_joypad();   /* 按下为0 */
    uint32_t state = 0;
    if (!(jp & (1 << 6))) state |= RG_KEY_UP;
    if (!(jp & (1 << 7))) state |= RG_KEY_DOWN;
    if (!(jp & (1 << 4))) state |= RG_KEY_RIGHT;
    if (!(jp & (1 << 5))) state |= RG_KEY_LEFT;
    if (!(jp & (1 << 0))) state |= RG_KEY_A;
    if (!(jp & (1 << 1))) state |= RG_KEY_B;
    if (!(jp & (1 << 2))) state |= RG_KEY_SELECT;
    if (!(jp & (1 << 3))) state |= RG_KEY_START;
    return state;
}

/* ---- UTF-8 ---- */

int rg_utf8_decode(const char **ptr)
{
    const unsigned char *s = (const unsigned char *)*ptr;
    unsigned int c = s[0];
    unsigned int result;
    int len;

    if (c < 0x80) { *ptr += 1; return (int)c; }
    if (c < 0xC2 && c >= 0x80) return -1;
    if (c < 0xE0) { len = 1; result = c & 0x1F; }
    else if (c < 0xF0) { len = 2; result = c & 0x0F; }
    else if (c < 0xF5) { len = 3; result = c & 0x07; }
    else return -1;

    for (int i = 1; i <= len; i++)
    {
        if ((s[i] & 0xC0) != 0x80) return -1;
        result = (result << 6) | (s[i] & 0x3F);
    }
    *ptr += len + 1;
    return (int)result;
}

size_t rg_utf8_encode(char *out, int codepoint)
{
    if (codepoint < 0) return 0;
    if (codepoint < 0x80)
    {
        out[0] = (char)codepoint; return 1;
    }
    else if (codepoint < 0x800)
    {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    else if (codepoint < 0x10000)
    {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    else if (codepoint < 0x200000)
    {
        out[0] = (char)(0xF0 | (codepoint >> 18));
        out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = (char)(0x80 | (codepoint & 0x3F));
        return 4;
    }
    return 0;
}

/* ---- GBK(OEM/936) <-> Unicode (近似) ----
 * 完整的 GB2312→Unicode 线性表很大, 先提供可编译且不崩的实现:
 *   ASCII (0x00-0x7F) 直通;
 *   双字节汉字区 (0xB0A1 起的一二级汉字) 用一个可逆的线性近似映射到
 *   U+E000 私有区, 保证 round-trip 与 ASCII 正确; 其余返回 0。
 * 注意: 主菜单用 UTF-8 绝对路径直接加载 .lav, 不依赖这里的完整汉字表,
 * 因此这里的近似对"英文/数字文件路径"完全正确, 中文路径后续再加真码表。
 */

unsigned int lavax_gbk_oem2uni(unsigned int oem)
{
    if (oem < 0x80) return oem;
    /* 双字节: 高位<<8|低位 -> 私有区 */
    unsigned int hi = (oem >> 8) & 0xFF, lo = oem & 0xFF;
    if (hi >= 0xA1 && hi <= 0xF7 && lo >= 0xA1 && lo <= 0xFE)
        return 0xE000 + ((hi - 0xA1) * 94) + (lo - 0xA1);
    return 0;
}

unsigned int lavax_gbk_uni2oem(unsigned int uni)
{
    if (uni < 0x80) return uni;
    if (uni >= 0xE000 && uni < 0xE000 + 87 * 94)
    {
        unsigned int idx = uni - 0xE000;
        unsigned int hi = 0xA1 + idx / 94;
        unsigned int lo = 0xA1 + idx % 94;
        return (hi << 8) | lo;
    }
    return 0;
}