#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <sys/time.h>
#include "libretro.h"

// ESP32 移植: 大数组放 PSRAM (sys 结构体约 6MB)
// 需开启 CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#include "esp_attr.h"
#define PSRAM_BSS EXT_RAM_BSS_ATTR

#define _DATA1          0x00
#define _DATA2          0x01
#define _DATA3          0x02
#define _DATA4          0x03
#define _ISR            0x04
#define _TISR           0x05
#define _BK_SEL         0x0c
#define _BK_ADRL        0x0d
#define _BK_ADRH        0x0e
#define _IRCNT          0x1b
#define __oper1         0x20
#define __oper2         0x23
#define __addr_reg      0x26
#define _SYSCON         0x200
#define _INCR           0x207
#define _ADDR1L         0x208
#define _ADDR1M         0x209
#define _ADDR1H         0x20a
#define _ADDR2L         0x20b
#define _ADDR2M         0x20c
#define _ADDR2H         0x20d
#define _ADDR3L         0x20e
#define _ADDR3M         0x20f
#define _ADDR3H         0x210
#define _ADDR4L         0x211
#define _ADDR4M         0x212
#define _ADDR4H         0x213
#define _PB             0x21b
#define _STCON          0x226
#define _ST1LD          0x227
#define _ST2LD          0x228
#define _ST3LD          0x229
#define _ST4LD          0x22a
#define _MTCT           0x22b
#define _STCTCON        0x22e
#define _CTLD           0x22f
#define _ALMMIN         0x230
#define _ALMHR          0x231
#define _ALMDAYL        0x232
#define _ALMDAYH        0x233
#define _RTCSEC         0x234
#define _RTCMIN         0x235
#define _RTCHR          0x236
#define _RTCDAYL        0x237
#define _RTCDAYH        0x238
#define _IER            0x23a
#define _TIER           0x23b
#define _AUDCON         0x23f
#define _KEYCODE        0x24e
#define _MACCTL         0x260
#define _KeyBuffTop     0x2003
#define _KeyBuffBottom  0x2004
#define _KeyBuffer      0x2008

#define LCD_WIDTH 159
#define LCD_HEIGHT 96

static void fallback_log(enum retro_log_level level, const char *fmt, ...);
static retro_log_printf_t       log_cb = fallback_log;
static retro_environment_t      environ_cb;
static retro_video_refresh_t    video_cb;
static retro_input_poll_t       input_poll_cb;
static retro_input_state_t      input_state_cb;
static retro_audio_sample_t     audio_cb;

/* ===== BBK 定时器方波音效 (新增) =====
 * 真实硬件 (W65C02S) 通过定时器输出翻转驱动扬声器产生方波/蜂鸣音.
 * 原模拟器从未实现音频 (audio_cb 注册但从未调用, ram_write 还把 _PB 强置 0).
 * 这里在 sys_timer 中统计各定时器溢出次数并翻转方波电平, 每帧 (60fps)
 * 按实际溢出率生成 44100Hz 方波样本馈入 audio_cb.
 * ST1 被固件用作按键扫描 (见 sys_isr 0x2018 计数), 故音频只取 ST2/ST3/ST4. */
#define BBK_AUDIO_SR          44100
#define BBK_SAMPLES_PER_FRAME (BBK_AUDIO_SR / 60)   /* 735 */
#define BBK_SPK_AMP           12000                  /* 方波幅度, 避免全幅刺耳 */
static volatile int s_spk_overflow[4];   /* 本帧各定时器溢出次数 */
static volatile int s_spk_level[4];      /* 本帧各定时器方波电平 */
static int          s_spk_phase[4];      /* 相位累加 (跨帧保持) */

/* V1.0.44: fa/fb 从 .dram1.data 移到 PSRAM, 释放约 30KB 内部 DRAM.
 * fa 是抗闪烁累加器 (int8), fb 是 RGB565 帧缓冲, 都不需要 DMA, PSRAM 即可.
 * 电子词典帧率低 (~60fps), PSRAM 带宽足够. */
EXT_RAM_BSS_ATTR static int8_t  fa[(LCD_WIDTH + 1) * LCD_HEIGHT];
EXT_RAM_BSS_ATTR static uint16_t fb[(LCD_WIDTH + 1) * LCD_HEIGHT];


static void sys_isr(void);
static bool sys_halt_p(void);
static void mem_bs(uint8_t sel);
static uint8_t IRAM_ATTR mem_read(uint16_t addr);
static uint8_t IRAM_ATTR mem_readx(uint16_t addr);
static uint16_t mem_read16(uint16_t addr);
static uint16_t mem_readx16(uint16_t addr);
static uint16_t mem_read16_wrapped(uint16_t addr);
static void IRAM_ATTR mem_write(uint16_t addr, uint8_t val);
static void bbk_audio_flush_frame(void);

#define READ8(addr)       mem_read(addr)
#define READX8(addr)      mem_readx(addr)
#define READ16(addr)      mem_read16(addr)
#define READX16(addr)     mem_readx16(addr)
#define READ16W(addr)     mem_read16_wrapped(addr)
#define WRITE8(addr, val) mem_write(addr, val)
#define BRK_HOOK                                      \
    {                                                 \
        executed = cycles;                            \
        pc = _MACCTL;                                 \
        environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL); \
    }
#include "s6502.c"

/* sys.ram 32KB: 引擎热数据. V1.0.6x+ 改放 PSRAM:
 * 原放内部 SRAM 需 32KB 连续块, 但主菜单系统占掉 ~88KB 内部后最大连续块常 <32KB,
 * 内部分配失败 -> 游戏 5% 崩溃. PSRAM 一定分配成功, 电子词典帧率低影响可忽略.
 * (若后续主菜单内部占用压缩出 32KB 连续块, 可再改回 INTERNAL 提升性能) */
static uint8_t *sys_ram = NULL;

/* PSRAM 大数组 (共 6MB) - 进电子词典时动态分配, 回主菜单时释放.
 * 由 retro_mem_alloc()/retro_mem_free() (见 retro_init/retro_deinit) 管理. */
#define SYS_FLASH_SIZE 0x200000
static uint8_t *sys_flash = NULL;
static uint8_t *sys_rom_8  = NULL;
static uint8_t *sys_rom_e  = NULL;

/* 内部 SRAM 热数据 (~6KB) - 频繁访问，放内部 SRAM 加速 */
static s6502_t      s_cpu;
static uint8_t     *s_mem_r[0x100];
static uint8_t    (*s_mem_ir[0x100])(uint16_t);
static void       (*s_mem_iw[0x100])(uint16_t, uint8_t);
static uint8_t     *s_ram = NULL;   /* sys_init 分配成功后指向 sys_ram */
static uint8_t      s_flash_cmd;
static uint8_t      s_flash_cycles;
static uint8_t      s_bk_sel;
static uint16_t     s_bk_tab[16];
static uint16_t     s_bk_sys_d;

/* flash 懒初始化: 游戏主循环中每帧填充一部分, 完成后置 true */
static bool g_flash_inited = true;
static size_t g_flash_init_offset = 0;

/* 每帧初始化 128KB, 16 帧 (约 0.3 秒) 完成 2MB */
bool gam4980_flash_init_step(void) {
    if (g_flash_inited) return true;
    const size_t chunk = 0x20000;  /* 128KB */
    uint32_t *p = (uint32_t *)(sys_flash + g_flash_init_offset);
    int words = chunk / 4;
    for (int i = 0; i < words; i++)
        p[i] = 0xFFFFFFFF;
    g_flash_init_offset += chunk;
    if (g_flash_init_offset >= 0x200000) {
        g_flash_inited = true;
        ESP_LOGE("INIT", "flash 懒初始化完成 (2MB)");
    }
    return g_flash_inited;
}
static struct {
    float cpu_rate;
    float timer_rate;
    uint16_t lcd_bg;
    uint16_t lcd_fg;
    uint8_t lcd_ghosting;
    long key_pressed_input_min_interval; //按下按键的最小输入间隔(ms)
} vars = { 1.0, 1.0, 0xd6da, 0x0000, 20, 0x0000 };

static void s6502_push(uint8_t val)
{
    mem_write(0x100 | s_cpu.sp--, val);
}

static bool IRAM_ATTR sys_halt_p(void)
{
    return s_ram[_SYSCON] & 0x08;
}

static inline uint32_t PA(uint16_t addr)
{
    uint8_t bank = addr >> 12;
    return (s_bk_tab[bank] << 12) | (addr & 0x0fff);
}

static uint8_t flash_read(uint32_t addr)
{
    static uint8_t flash_info[0x35] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x51, 0x52, 0x59, 0x01, 0x07, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x27, 0x36, 0x00, 0x00, 0x04,
        0x00, 0x04, 0x06, 0x01, 0x00, 0x01, 0x01, 0x15,
        0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0x01, 0x10,
        0x00, 0x1f, 0x00, 0x00, 0x01,
    };
    if (s_flash_cmd == 0 || s_flash_cmd == 1) {
        // Rotate last 32KiB to the front for save.
        addr = (addr + 0x8000) % 0x200000;
        return sys_flash[addr];
    } else {
        // Software ID or CFI
        return flash_info[addr];
    }
}

static void flash_write(uint32_t addr, uint8_t val)
{
    switch (s_flash_cycles) {
    case 0:
        // 1st Bus Write Cycle
        if (addr == 0x5555 && val == 0xaa)
            s_flash_cycles += 1;
        else if (val == 0xf0)
            // Software ID Exit / CFI Exit
            s_flash_cmd = 0;
        break;
    case 1:
    case 4:
        // 2nd Bus Write Cycle / 5th Bus Write Cycle
        if (addr == 0x2aaa && val == 0x55)
            s_flash_cycles += 1;
        break;
    case 2:
        // 3rd Bus Write Cycle
        if (addr != 0x5555)
            return;
        switch (val) {
        case 0xa0:
            // Byte-Program
            s_flash_cmd = 1;
            s_flash_cycles += 1;
            break;
        case 0x80:
            s_flash_cycles += 1;
            break;
        case 0x90:
            // Software ID Entry
            s_flash_cmd = 2;
            s_flash_cycles = 0;
            break;
        case 0x98:
            // CFI Query Entry
            s_flash_cmd = 3;
            s_flash_cycles = 0;
            break;
        case 0xf0:
            // Software ID Exit / CFI Exit
            s_flash_cmd = 0;
            s_flash_cycles = 0;
            break;
        }
        break;
    case 3:
        // 4th Bus Write Cycle
        if (s_flash_cmd == 1) {
            s_flash_cmd = 0;
            s_flash_cycles = 0;
            // Rotate last 32KiB to the front for save.
            addr = (addr + 0x8000) % 0x200000;
            sys_flash[addr] = val;
        } else if ((addr == 0x5555) && (val == 0xaa)) {
            s_flash_cycles += 1;
        }
        break;
    case 5:
        // 6th Bus Write Cycle
        switch (val) {
        case 0x10:
            // Chip-Erase
            if (addr == 0x5555)
                memset(sys_flash, 0xff, 0x200000);
            break;
        case 0x30:
            // Sector-Erase
            addr = (addr + 0x8000) % 0x200000;
            memset(sys_flash + (addr & 0x1ff000), 0xff, 0x1000);
            break;
        case 0x50:
            // Block-Erase
            addr = ((addr & 0x1f0000) + 0x8000) % 0x200000;
            memset(sys_flash + addr, 0xff, 0x8000);
            addr = (addr + 0x8000) % 0x200000;
            memset(sys_flash + addr, 0xff, 0x8000);
            break;
        }
        s_flash_cmd = 0;
        s_flash_cycles = 0;
        break;
    }

    // Read CFI/ID info via 'sys.mem_ir'.
    if (s_flash_cmd == 2 || s_flash_cmd == 3) {
        for (int i = 0; i < 0x100; i += 1) {
            if (s_mem_r[i] >= sys_flash && s_mem_r[i] < sys_flash + 0x200000) {
                s_mem_r[i] = 0;
            }
        }
    }
}

static uint8_t invalid_read(uint16_t addr)
{
    return 0x00;
}

static void invalid_write(uint16_t addr, uint8_t val)
{
}

static uint8_t ram_read(uint16_t addr)
{
    return s_ram[addr];
}

static void ram_write(uint16_t addr, uint8_t val)
{
    s_ram[addr] = val;

    // XXX: Disable ROM (0x400000-0x7fffff) channels and audio.
    if (addr == _PB)
        s_ram[addr] = 0;

    // Never return 0 for AutoPowerOffCount to prevent poweroff.
    if (addr == 0x2028)
        s_ram[addr] = 0xff;
}

static uint8_t direct_read(uint16_t addr)
{
    int _L = _ADDR1L + addr * 3;
    int _M = _L + 1;
    int _H = _M + 1;
    uint32_t paddr = s_ram[_L] | s_ram[_M] << 8 | s_ram[_H] << 16;
    if (s_ram[_INCR] & (1 << addr)) {
        s_ram[_L] += 1;
        if (s_ram[_L] == 0) {
            s_ram[_M] += 1;
            if (s_ram[_M] == 0) {
                s_ram[_H] += 1;
            }
        }
    }
    if (paddr < 0x8000)
        return ram_read(paddr & 0x7fff);
    else if (paddr >= 0x200000 && paddr < 0x400000)
        return flash_read(paddr - 0x200000);
    else if (paddr >= 0x800000 && paddr < 0xa00000)
        return sys_rom_8[paddr - 0x800000];
    else if (paddr >= 0xe00000 && paddr < 0x1000000)
        return sys_rom_e[paddr - 0xe00000];
    else
        return 0x00;
}

static void direct_write(uint16_t addr, uint8_t val)
{
    int _L = _ADDR1L + addr * 3;
    int _M = _L + 1;
    int _H = _M + 1;
    uint32_t paddr = s_ram[_L] | s_ram[_M] << 8 | s_ram[_H] << 16;
    if (s_ram[_INCR] & (1 << addr)) {
        s_ram[_L] += 1;
        if (s_ram[_L] == 0) {
            s_ram[_M] += 1;
            if (s_ram[_M] == 0) {
                s_ram[_H] += 1;
            }
        }
    }
    if (paddr < 0x8000)
        ram_write(paddr & 0x7fff, val);
    else if (paddr >= 0x200000 && paddr < 0x400000)
        flash_write(paddr - 0x200000, val);
}

static uint8_t page0_read(uint16_t addr)
{
    switch (addr) {
    case _DATA1:
    case _DATA2:
    case _DATA3:
    case _DATA4:
        return direct_read(addr);
    case _BK_SEL:
        return s_bk_sel;
    case _BK_ADRL:
        return s_bk_tab[s_bk_sel] & 0xff;
    case _BK_ADRH:
        return s_bk_tab[s_bk_sel] >> 8;
    }
    return s_ram[addr];
}

static void page0_write(uint16_t addr, uint8_t val)
{
    switch (addr) {
    case _DATA1:
    case _DATA2:
    case _DATA3:
    case _DATA4:
        direct_write(addr, val);
        return;
    case _ISR:
        s_ram[_ISR] &= val;
        return;
    case _TISR:
        s_ram[_TISR] &= val;
        return;
    case _BK_SEL:
        s_bk_sel = val & 0x0f;
        return;
    case _BK_ADRL:
        s_bk_tab[s_bk_sel] &= 0xff00;
        s_bk_tab[s_bk_sel] |= val;
        mem_bs(s_bk_sel);
        return;
    case _BK_ADRH:
        s_bk_tab[s_bk_sel] &= 0x00ff;
        s_bk_tab[s_bk_sel] |= (val & 0x0f) << 8;
        mem_bs(s_bk_sel);
        return;
    }
    s_ram[addr] = val;
}

static void mem_init()
{
    for (int i = 0; i < 0x100; i += 1) {
        s_mem_r[i] = 0;
        s_mem_ir[i] = invalid_read;
        s_mem_iw[i] = invalid_write;
    }
    for (int i = 1; i < 16; i += 1) {
        s_mem_r[i] = s_ram + i * 0x100;
        s_mem_ir[i] = ram_read;
        s_mem_iw[i] = ram_write;
    }
    s_mem_ir[0x00] = page0_read;
    s_mem_iw[0x00] = page0_write;
    s_mem_r[0x03] = sys_rom_e + 0x1fff00;
    s_mem_iw[0x03] = invalid_write;
    /* ESP32 补丁: PSRAM 上电是 0, 但 OS 启动码期望 ram[0x3e4]/[0x3e5]
     * 已经是 bank 0xd 的 address (4988: 0x0e88). 真实机器用 NVRAM 保留,
     * 我们直接预设:
     *   1) bk_tab[0xd] = 0x0e88, mem_bs 把 0xd0-0xdf 指向 rom_e
     *   2) ram[0x3e4]=0x88, ram[0x3e5]=0x0e, OS 启动码读这些不会把 bank 又改错
     * 同时预设 bank 0x9-0xc 指向 rom_8 (字库), 因为 OS 启动码也会 mem_bs 它们 */
    s_ram[0x3e4] = 0x88;  /* 4988 bank 0xd low  */
    s_ram[0x3e5] = 0x0e;  /* 4988 bank 0xd high */
    /* bank 0x9-0xc 指向 rom_8 (字库), 高 4 位 0x8 表示 rom_8 区 (0x800000+)
     * bk_tab = (high<<8)|low, PA = bk_tab<<12, 范围 0x800000-0x9fffff → rom_8 */
    s_bk_tab[0x9] = 0x080e;
    s_bk_tab[0xa] = 0x081e;
    s_bk_tab[0xb] = 0x082e;
    s_bk_tab[0xc] = 0x083e;
    mem_bs(0x9); mem_bs(0xa); mem_bs(0xb); mem_bs(0xc);
    s_bk_tab[0xd] = 0x0e88;  /* rom_e 区 (0xe00000+) */
    mem_bs(0xd);
}

static uint8_t flash_vread(uint16_t addr)
{
    return flash_read(PA(addr) - 0x200000);
}

static void flash_vwrite(uint16_t addr, uint8_t val)
{
    return flash_write(PA(addr) - 0x200000, val);
}

static uint8_t rom_8_vread(uint16_t addr)
{
    return sys_rom_8[PA(addr) - 0x800000];
}

static uint8_t rom_e_vread(uint16_t addr)
{
    return sys_rom_e[PA(addr) - 0xe00000];
}

static uint8_t ram_vread(uint16_t addr)
{
    return ram_read(PA(addr));
}

static void ram_vwrite(uint16_t addr, uint8_t val)
{
    ram_write(PA(addr), val);
}

static void mem_bs(uint8_t sel)
{
    uint32_t paddr = PA(sel * 0x1000);
    if (sel == 0)
        return;
    if (paddr < 0x8000) {
        for (int i = 0; i < 16; i += 1) {
            s_mem_r[sel * 16 + i] = s_ram + paddr + i * 0x100;
            s_mem_ir[sel * 16 + i] = ram_vread;
            s_mem_iw[sel * 16 + i] = ram_vwrite;
        }
    } else if (paddr >= 0x200000 && paddr < 0x400000) {
        for (int i = 0; i < 16; i += 1) {
            uint32_t faddr = (paddr - 0x200000 + 0x8000) % 0x200000;
            s_mem_r[sel * 16 + i] = sys_flash + faddr + i * 0x100;
            s_mem_ir[sel * 16 + i] = flash_vread;
            s_mem_iw[sel * 16 + i] = flash_vwrite;
        }
    } else if (paddr >= 0x800000 && paddr < 0xa00000) {
        for (int i = 0; i < 16; i += 1) {
            s_mem_r[sel * 16 + i] = sys_rom_8 + (paddr - 0x800000) + i * 0x100;
            s_mem_ir[sel * 16 + i] = rom_8_vread;
            s_mem_iw[sel * 16 + i] = invalid_write;
        }
    } else if (paddr >= 0xe00000 && paddr < 0x1000000) {
        for (int i = 0; i < 16; i += 1) {
            s_mem_r[sel * 16 + i] = sys_rom_e + (paddr - 0xe00000) + i * 0x100;
            s_mem_ir[sel * 16 + i] = rom_e_vread;
            s_mem_iw[sel * 16 + i] = invalid_write;
        }
    } else {
        for (int i = 0; i < 16; i += 1) {
            s_mem_r[sel * 16 + i] = 0;
            s_mem_ir[sel * 16 + i] = invalid_read;
            s_mem_iw[sel * 16 + i] = invalid_write;
        }
    }
}

static uint8_t IRAM_ATTR mem_readx(uint16_t addr)
{
    uint8_t page = addr >> 8;
    if (s_mem_r[page])
        return s_mem_r[page][addr & 0xff];
    return s_mem_ir[page](addr);
}

static uint8_t IRAM_ATTR mem_read(uint16_t addr)
{
    uint8_t page = addr >> 8;
    if (s_mem_r[page])
        return s_mem_r[page][addr & 0xff];
    else
        return s_mem_ir[page](addr);
}

static uint16_t mem_read16(uint16_t addr)
{
    return mem_read(addr) | (mem_read(addr + 1) << 8);
}

static uint16_t mem_readx16(uint16_t addr)
{
    return mem_readx(addr) | (mem_readx(addr + 1) << 8);
}

static uint16_t mem_read16_wrapped(uint16_t addr)
{
    return mem_read(addr) | (mem_read((addr + 1) & 0xff) << 8);
}

static void IRAM_ATTR mem_write(uint16_t addr, uint8_t val)
{
    uint8_t page = addr >> 8;
    if (s_mem_iw[page])
        s_mem_iw[page](addr, val);
}

enum _key {
    KEY_ON_OFF     = 0x00,      /* 开关 */
    KEY_HOME_MENU  = 0x01,      /* 目录 */
    KEY_EC_SJ      = 0x02,      /* 双解 */
    KEY_EC_SW      = 0x03,      /* 十万 (4988: 现代) */
    KEY_CE         = 0x04,      /* 汉英 */
    KEY_DLG        = 0x05,      /* 对话 */
    KEY_DOWNLOAD   = 0x06,      /* 下载 */
    KEY_SPK        = 0x07,      /* 发音 */
    KEY_1          = 0x08,
    KEY_2          = 0x09,
    KEY_3          = 0x0a,
    KEY_4          = 0x0b,
    KEY_5          = 0x0c,
    KEY_6          = 0x0d,
    KEY_7          = 0x0e,
    KEY_8          = 0x0f,
    KEY_9          = 0x30,
    KEY_0          = 0x31,
    KEY_Q          = 0x10,
    KEY_W          = 0x11,
    KEY_E          = 0x12,
    KEY_R          = 0x13,
    KEY_T          = 0x14,
    KEY_Y          = 0x15,
    KEY_U          = 0x16,
    KEY_I          = 0x17,
    KEY_O          = 0x32,
    KEY_P          = 0x33,
    KEY_SPACE      = 0x36,      /* 空格 */
    KEY_A          = 0x18,
    KEY_S          = 0x19,
    KEY_D          = 0x1a,
    KEY_F          = 0x1b,
    KEY_G          = 0x1c,
    KEY_H          = 0x1d,
    KEY_J          = 0x1e,
    KEY_K          = 0x1f,
    KEY_L          = 0x34,
    KEY_INPUT      = 0x20,      /* 输入法 */
    KEY_CAPS       = KEY_INPUT,
    KEY_Z          = 0x21,
    KEY_X          = 0x22,
    KEY_C          = 0x23,
    KEY_V          = 0x24,
    KEY_B          = 0x25,
    KEY_N          = 0x26,
    KEY_M          = 0x27,
    KEY_ZY         = 0x28,      /* 中英 */
    KEY_SHIFT      = KEY_ZY,
    KEY_HELP       = 0x29,      /* 帮助 */
    KEY_SEARCH     = 0x2a,      /* 查找 */
    KEY_INSERT     = 0x2b,      /* 插入 */
    KEY_MODIFY     = 0x2c,      /* 修改 */
    KEY_DEL        = 0x2d,      /* 删除 */
    KEY_SHIFT_4988 = 0x2d,
    KEY_EXIT       = 0x2e,      /* 跳出 */
    KEY_ENTER      = 0x2f,      /* 输入 */
    KEY_UP         = 0x35,
    KEY_DOWN       = 0x38,
    KEY_LEFT       = 0x37,
    KEY_RIGHT      = 0x39,
    KEY_PGUP       = 0x3a,
    KEY_PGDN       = 0x3b,
};

static uint8_t _joyk[20] = {
    [RETRO_DEVICE_ID_JOYPAD_B]      = KEY_EXIT,     /* 返回键 = 跳出/退出: 进/出游戏内菜单 (BBK 跳出键); 与长按BOOT/第7键硬退出并存 */
    [RETRO_DEVICE_ID_JOYPAD_Y]      = KEY_HELP,
    [RETRO_DEVICE_ID_JOYPAD_SELECT] = KEY_INSERT,
    [RETRO_DEVICE_ID_JOYPAD_START]  = KEY_SEARCH,
    [RETRO_DEVICE_ID_JOYPAD_UP]     = KEY_UP,
    [RETRO_DEVICE_ID_JOYPAD_DOWN]   = KEY_DOWN,
    [RETRO_DEVICE_ID_JOYPAD_LEFT]   = KEY_LEFT,
    [RETRO_DEVICE_ID_JOYPAD_RIGHT]  = KEY_RIGHT,
    [RETRO_DEVICE_ID_JOYPAD_A]      = KEY_ENTER,    /* KEY 键 = 确认/回车 */
    [RETRO_DEVICE_ID_JOYPAD_X]      = KEY_R,
    [RETRO_DEVICE_ID_JOYPAD_L]      = KEY_PGUP,
    [RETRO_DEVICE_ID_JOYPAD_R]      = KEY_PGDN,
    [RETRO_DEVICE_ID_JOYPAD_L2]     = KEY_MODIFY,
    [RETRO_DEVICE_ID_JOYPAD_R2]     = KEY_DEL,
    [RETRO_DEVICE_ID_JOYPAD_L3]     = KEY_A,
    [RETRO_DEVICE_ID_JOYPAD_R3]     = KEY_Z,
    /* V1.0.46: 补充按键 (手柄任意键 → BBK 功能1-4), 由补充按键映射流程分配物理键.
     * 复用 RetroPad 扩展 ID 16..19: joypad_state 中映射到 bt_manager_is_sup_pressed(0..3). */
    [16] = KEY_F,        /* 功能1 (F) */
    [17] = KEY_G,        /* 功能2 (G) */
    [18] = KEY_SHIFT,    /* 功能3 (Shift) */
    [19] = KEY_SPACE,    /* 功能4 (空格) */
};

static uint8_t _kbdk[RETROK_LAST] = {
    [RETROK_F1]        = KEY_ON_OFF,
    [RETROK_F2]        = KEY_HOME_MENU,
    [RETROK_F3]        = KEY_EC_SJ,
    [RETROK_F4]        = KEY_EC_SW,
    [RETROK_F5]        = KEY_CE,
    [RETROK_F6]        = KEY_DLG,
    [RETROK_F7]        = KEY_DOWNLOAD,
    [RETROK_F8]        = KEY_SPK,
    [RETROK_F9]        = KEY_HELP,
    [RETROK_F10]       = KEY_SEARCH,
    [RETROK_F11]       = KEY_INSERT,
    [RETROK_F12]       = KEY_MODIFY,
    [RETROK_1]         = KEY_1,
    [RETROK_2]         = KEY_2,
    [RETROK_3]         = KEY_3,
    [RETROK_4]         = KEY_4,
    [RETROK_5]         = KEY_5,
    [RETROK_6]         = KEY_6,
    [RETROK_7]         = KEY_7,
    [RETROK_8]         = KEY_8,
    [RETROK_9]         = KEY_9,
    [RETROK_0]         = KEY_0,
    [RETROK_q]         = KEY_Q,
    [RETROK_w]         = KEY_W,
    [RETROK_e]         = KEY_E,
    [RETROK_r]         = KEY_R,
    [RETROK_t]         = KEY_T,
    [RETROK_y]         = KEY_Y,
    [RETROK_u]         = KEY_U,
    [RETROK_i]         = KEY_I,
    [RETROK_o]         = KEY_O,
    [RETROK_p]         = KEY_P,
    [RETROK_SPACE]     = KEY_SPACE,
    [RETROK_a]         = KEY_A,
    [RETROK_s]         = KEY_S,
    [RETROK_d]         = KEY_D,
    [RETROK_f]         = KEY_F,
    [RETROK_g]         = KEY_G,
    [RETROK_h]         = KEY_H,
    [RETROK_j]         = KEY_J,
    [RETROK_k]         = KEY_K,
    [RETROK_l]         = KEY_L,
    [RETROK_CAPSLOCK]  = KEY_INPUT,
    [RETROK_z]         = KEY_Z,
    [RETROK_x]         = KEY_X,
    [RETROK_c]         = KEY_C,
    [RETROK_v]         = KEY_V,
    [RETROK_b]         = KEY_B,
    [RETROK_n]         = KEY_N,
    [RETROK_m]         = KEY_M,
    [RETROK_LSHIFT]    = KEY_SHIFT,
    [RETROK_BACKSPACE] = KEY_DEL,
    [RETROK_DELETE]    = KEY_DEL,
    [RETROK_ESCAPE]    = KEY_EXIT,
    [RETROK_RETURN]    = KEY_ENTER,
    [RETROK_UP]        = KEY_UP,
    [RETROK_DOWN]      = KEY_DOWN,
    [RETROK_LEFT]      = KEY_LEFT,
    [RETROK_RIGHT]     = KEY_RIGHT,
    [RETROK_PAGEUP]    = KEY_PGUP,
    [RETROK_PAGEDOWN]  = KEY_PGDN,
};

static void error_msg(const char *msg)
{
    struct retro_message_ext m = {
        .msg = msg,
        .duration = 3000,
        .priority = 5,
        .level = RETRO_LOG_ERROR,
        .target = RETRO_MESSAGE_TARGET_ALL,
        .type = RETRO_MESSAGE_TYPE_NOTIFICATION_ALT,
        .progress = -1,
    };
    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &m);
}

static void sys_keydown(uint8_t key)
{
    if (key == 0)
        return;

    //控制连续输入的频率
    static long last_input_time = 0;
    static uint8_t last_input_key = 0;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    long current_time = (tv.tv_sec * 1000 + tv.tv_usec / 1000);

    if (key == last_input_key
        && current_time - last_input_time < vars.key_pressed_input_min_interval)
    {
        return;
    }

    last_input_key = key;
    last_input_time = current_time;

    s_ram[_SYSCON] &= 0xf7;
    s_ram[_KEYCODE] = key | 0x80;
    s_ram[_ISR] |= 0x80;
    if (s_ram[_IER] & 0x80) {
        s_ram[_KeyBuffTop] = 0x0;
        s_ram[_KeyBuffBottom] = 0xf;
        s_ram[_KeyBuffer + 0x0f] = key & 0x3f;
        s_ram[_KEYCODE] = 0x00;
    }
}

static void keyboard_cb(bool down, unsigned keycode,
                        uint32_t character, uint16_t key_modifiers)
{
    if (!down)
        return;
    sys_keydown(_kbdk[keycode]);
}

/* OS 启动进度回调: 显示启动进度条 */
static void (*g_boot_progress_cb)(int percent, const char *msg) = NULL;

void gam4980_set_boot_progress_cb(void (*cb)(int percent, const char *msg)) {
    g_boot_progress_cb = cb;
}

/* 动态分配引擎用内存 (6MB ROM 放 PSRAM + 32KB sys_ram 放内部 SRAM).
 * 进电子词典时调用; 任一失败则全部释放并返回 false. */
static bool retro_mem_alloc(void)
{
    if (sys_flash && sys_rom_8 && sys_rom_e && sys_ram)
        return true;   /* 已分配 */

    /* sys_ram 32KB: 引擎热数据. 放 PSRAM (内部 32KB 连续块被主菜单占碎, 分配常失败). */
    if (!sys_ram)
        sys_ram = (uint8_t *)heap_caps_malloc(0x8000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    sys_flash = (uint8_t *)heap_caps_malloc(SYS_FLASH_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    sys_rom_8 = (uint8_t *)heap_caps_malloc(SYS_FLASH_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    sys_rom_e = (uint8_t *)heap_caps_malloc(SYS_FLASH_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (sys_ram && sys_flash && sys_rom_8 && sys_rom_e)
        return true;

    /* 任一失败: 释放已成功的部分, 避免泄漏 */
    if (sys_ram)     { heap_caps_free(sys_ram);     sys_ram     = NULL; }
    if (sys_flash)   { heap_caps_free(sys_flash);   sys_flash   = NULL; }
    if (sys_rom_8)   { heap_caps_free(sys_rom_8);   sys_rom_8   = NULL; }
    if (sys_rom_e)   { heap_caps_free(sys_rom_e);   sys_rom_e   = NULL; }
    ESP_LOGE("INIT", "内存分配失败: sys_ram=%p sys_flash=%p sys_rom_8=%p sys_rom_e=%p (内部空闲=%u PSRAM空闲=%u)",
             (void*)sys_ram, (void*)sys_flash, (void*)sys_rom_8, (void*)sys_rom_e,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return false;
}

static void retro_mem_free(void)
{
    if (sys_ram)     { heap_caps_free(sys_ram);     sys_ram     = NULL; }
    if (sys_flash)   { heap_caps_free(sys_flash);   sys_flash   = NULL; }
    if (sys_rom_8)   { heap_caps_free(sys_rom_8);   sys_rom_8   = NULL; }
    if (sys_rom_e)   { heap_caps_free(sys_rom_e);   sys_rom_e   = NULL; }
}

static void sys_init(const char *romdir)
{
    /* 动态分配 6MB PSRAM + 32KB 内部 sys_ram; 失败则无法启动引擎 */
    if (!retro_mem_alloc()) {
        error_msg("GAM4980: mem alloc failed");
        environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
        return;
    }
    /* 分配成功后再把 ram 指针指向内部 SRAM */
    s_ram = sys_ram;

    static struct retro_input_descriptor inputs[] = {
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "EXIT" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "HELP" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "INSERT" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "SEARCH" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "UP" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "DOWN" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "LEFT" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "RIGHT" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "ENTER" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "R" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "PGUP" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "PGDN" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "MODIFY" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "DEL" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "A" },
        { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Z" },
        { 0, 0, 0, 0, NULL },
    };

    /* === 直接从 flash system 分区读取 ROM, 跳过 SD 卡 (速度快 10-20x) ===
     * subtype 0x40 (0x90 在 IDF5.5 被 tee_ota 占用, 与 partitions.csv/system_rom.c 一致) */
    const esp_partition_t *sys_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "system");
    
    if (sys_part && sys_part->size >= 4 * 1024 * 1024) {
        /* flash 读取: 8.BIN 在偏移 0, E.BIN 在偏移 2MB, 各 2MB */
        ESP_LOGE("INIT", "从 flash 分区读取 ROM (addr=0x%lx, size=%luKB)",
                 (unsigned long)sys_part->address, (unsigned long)sys_part->size / 1024);
        
        const size_t rom_size = 0x200000;  /* 2MB */
        const size_t chunk = 0x10000;      /* 64KB */
        uint8_t *tmp_buf = malloc(chunk);
        if (!tmp_buf) {
            error_msg("GAM4980: malloc failed for ROM read");
            environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
            return;
        }

        /* 读取 8.BIN: flash 偏移 0, 进度 0-40% */
        size_t total_read = 0;
        while (total_read < rom_size) {
            size_t to_read = (rom_size - total_read < chunk) ? (rom_size - total_read) : chunk;
            if (esp_partition_read(sys_part, total_read, tmp_buf, to_read) != ESP_OK) break;
            memcpy(sys_rom_8 + total_read, tmp_buf, to_read);
            total_read += to_read;
            int pct = total_read * 40 / rom_size;
            if (g_boot_progress_cb) g_boot_progress_cb(pct, "Loading 8.BIN");
            taskYIELD();
        }
        ESP_LOGE("INIT", "8.BIN 从 flash 读取 %u/0x200000, rom_8[0]=0x%02x",
                 (unsigned)total_read, sys_rom_8[0]);

        /* 读取 E.BIN: flash 偏移 2MB, 进度 40-80% */
        total_read = 0;
        while (total_read < rom_size) {
            size_t to_read = (rom_size - total_read < chunk) ? (rom_size - total_read) : chunk;
            if (esp_partition_read(sys_part, rom_size + total_read, tmp_buf, to_read) != ESP_OK) break;
            memcpy(sys_rom_e + total_read, tmp_buf, to_read);
            total_read += to_read;
            int pct = 40 + total_read * 40 / rom_size;
            if (g_boot_progress_cb) g_boot_progress_cb(pct, "Loading E.BIN");
            taskYIELD();
        }
        ESP_LOGE("INIT", "E.BIN 从 flash 读取 %u/0x200000, rom_e[0]=0x%02x",
                 (unsigned)total_read, sys_rom_e[0]);

        free(tmp_buf);
    } else {
        /* flash system 分区未找到 - 必须烧录 system.bin 到 0x900000 */
        ESP_LOGE("INIT", "flash system 分区未找到! 请烧录 system.bin 到 0x900000");
        error_msg("GAM4980: Flash system partition not found");
        environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
        return;
    }

    /* 验证入口点: PC=0x350 在 page 0x03, 映射到 rom_e[0x1fff00+0x50] */
    ESP_LOGE("INIT", "入口点 rom_e[0x1fff50..5f]: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
        sys_rom_e[0x1fff50], sys_rom_e[0x1fff51], sys_rom_e[0x1fff52], sys_rom_e[0x1fff53],
        sys_rom_e[0x1fff54], sys_rom_e[0x1fff55], sys_rom_e[0x1fff56], sys_rom_e[0x1fff57],
        sys_rom_e[0x1fff58], sys_rom_e[0x1fff59], sys_rom_e[0x1fff5a], sys_rom_e[0x1fff5b],
        sys_rom_e[0x1fff5c], sys_rom_e[0x1fff5d], sys_rom_e[0x1fff5e], sys_rom_e[0x1fff5f]);

    memset(s_ram, 0x00, 0x8000);
    if (g_boot_progress_cb) g_boot_progress_cb(82, "Init memory");
    taskYIELD();

    /* flash 填充 0xFF: 用 32 位写入替代 memset (-fno-builtin-memset 导致逐字节极慢) */
    {
        uint32_t *p = (uint32_t *)sys_flash;
        int words = 0x200000 / 4;  /* 512K 个 32 位字 */
        for (int i = 0; i < words; i++)
            p[i] = 0xFFFFFFFF;
    }
    if (g_boot_progress_cb) g_boot_progress_cb(85, "Init flash");
    taskYIELD();

    s_flash_cmd = 0;
    s_flash_cycles = 0;
    s_ram[_INCR] = 0x0f;

    mem_init();
    if (g_boot_progress_cb) g_boot_progress_cb(88, "Init CPU");
    taskYIELD();
    s_cpu.pc = 0x350;
    s_cpu.ac = 0;
    s_cpu.ix = 0;
    s_cpu.iy = 0;
    s_cpu.sp = 0xff;
    s_cpu.status = 0x04;
    ESP_LOGE("INIT", "CPU 初始: pc=0x%04x sp=0x%02x, 开始 OS 启动循环", s_cpu.pc, s_cpu.sp);

    // Run initialize instructions
    // XXX: SysStart set _MTCT to 0xfe just before 'main'.
    int _iters = 0;
    int _last_percent = 88;
    int64_t _boot_start = esp_timer_get_time();
    int64_t _last_lcd_update = _boot_start;
    while (s_ram[_MTCT] != 0xfe) {
        /* 大批量执行: 0x100000 周期/次, 减少 99% 的循环开销 */
        s6502_exec(&s_cpu, 0x100000);
        _iters++;
        int64_t now = esp_timer_get_time();
        /* 进度更新最多每 500ms 一次, 避免频繁刷 LCD (每次 15ms) 拖慢启动 */
        if (now - _last_lcd_update > 500000) {
            _last_lcd_update = now;
            int64_t elapsed_ms = (now - _boot_start) / 1000;
            int percent = 88 + (int)(elapsed_ms * 12 / 5000);
            if (percent > 99) percent = 99;
            if (percent != _last_percent) {
                _last_percent = percent;
                if (g_boot_progress_cb)
                    g_boot_progress_cb(percent, "Booting OS");
            }
        }
        taskYIELD();
        if (_iters > 500000) {
            ESP_LOGE("INIT", "OS 启动超时! pc=0x%04x MTCT=0x%02x", s_cpu.pc, s_ram[_MTCT]);
            if (g_boot_progress_cb)
                g_boot_progress_cb(100, "启动超时");
            break;
        }
    }
    s_bk_sys_d = s_bk_tab[0xd];
    ESP_LOGE("INIT", "OS 启动完成! bk_sys_d=0x%04x iters=%d", s_bk_sys_d, _iters);

    /* flash 初始化标记: PSRAM BSS 上电全 0, 需填充为 0xFF (flash 擦除态) */
    g_flash_inited = false;
    ESP_LOGE("INIT", "flash 将在游戏主循环中懒初始化");

    if (s_bk_sys_d == 0x0e88) { /* 4988 */
        _joyk[RETRO_DEVICE_ID_JOYPAD_Y] = KEY_Z;
        _joyk[RETRO_DEVICE_ID_JOYPAD_SELECT] = KEY_SHIFT_4988;
        _joyk[RETRO_DEVICE_ID_JOYPAD_START] = KEY_ZY;
        _joyk[RETRO_DEVICE_ID_JOYPAD_L2] = KEY_SPACE;
        _joyk[RETRO_DEVICE_ID_JOYPAD_R2] = KEY_X;
        _joyk[RETRO_DEVICE_ID_JOYPAD_R3] = KEY_S;
        inputs[RETRO_DEVICE_ID_JOYPAD_Y].description = "Z";
        inputs[RETRO_DEVICE_ID_JOYPAD_SELECT].description = "SHIFT";
        inputs[RETRO_DEVICE_ID_JOYPAD_START].description = "ZY";
        inputs[RETRO_DEVICE_ID_JOYPAD_L2].description = "SPACE";
        inputs[RETRO_DEVICE_ID_JOYPAD_R2].description = "X";
        inputs[RETRO_DEVICE_ID_JOYPAD_R3].description = "S";
    }
    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, &inputs);
}

static void sys_load(const uint8_t *gam, size_t size)
{
    if (g_boot_progress_cb) g_boot_progress_cb(15, "Restoring state...");

    if (g_boot_progress_cb) g_boot_progress_cb(25, "Loading game...");

    uint16_t start = gam[0x40] | (gam[0x41] << 8);
    uint32_t data = gam[0x42] | gam[0x43] << 8 | gam[0x44] << 16 | gam[0x45] << 24;
    uint8_t sys_hdr[16] = {
        0xc0, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x10, 0x00, 0x2f,
    };
    uint8_t gam_hdr[16] = {
        0xd0, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00,
        size & 0xff, (size >> 8) & 0xff, (size >> 16) * 0xff,
        0x3d,
    };

    uint8_t *flash = sys_flash + 0x8000;
    memcpy(gam_hdr + 2, gam + 6, 0x0a);
    memcpy(flash, sys_hdr, 16);
    memcpy(flash+16, gam_hdr, 16);

    if (g_boot_progress_cb) g_boot_progress_cb(40, "Copying data...");

    /* V1.0.52: 若调用方已把游戏数据直接读入目标区 (gam == flash+0xd000,
     * 避免额外分配 1MB 缓冲导致 PSRAM 耗尽), 这里用 memmove 兼容自拷贝. */
    if (gam != flash + 0xd000) {
        memcpy(flash+0xd000, gam, size);
    }
    memset(flash+0x1000, 0x01, 0x100);
    for (int i = 0; i < 0x0c; i += 1) {
        flash[0x1000 + i] = 0x04;
    }

    if (g_boot_progress_cb) g_boot_progress_cb(60, "Configuring...");

    if (s_bk_sys_d == 0x0ea8) {
        memset(flash+0x7000, 0x01, 0x100);
        flash[0x70f8] = 0x02;
        flash[0x70f9] = 0x02;
        flash[0x70fa] = 0x02;
        flash[0x70fb] = 0x02;
        flash[0x70fc] = 0x02;
        flash[0x70fd] = 0x02;
        flash[0x70fe] = 0x03;
        flash[0x70ff] = 0x02;
    } else if (s_bk_sys_d == 0x0e88) {
        memset(flash+0x8000, 0x01, 0x100);
        flash[0x80f8] = 0x02;
        flash[0x80f9] = 0x02;
        flash[0x80fa] = 0x02;
        flash[0x80fb] = 0x02;
        flash[0x80fc] = 0x02;
        flash[0x80fd] = 0x02;
        flash[0x80fe] = 0x03;
        flash[0x80ff] = 0x02;
    } else {
        return;
    }

    if (g_boot_progress_cb) g_boot_progress_cb(80, "Setting banks...");

    s_bk_tab[0x5] = 0x20d;
    s_bk_tab[0x6] = s_bk_tab[0x05] + 1;
    s_bk_tab[0x7] = s_bk_tab[0x05] + 2;
    s_bk_tab[0x8] = s_bk_tab[0x05] + 3;
    s_bk_tab[0x9] = 0x20d + (data >> 12);
    s_bk_tab[0xa] = s_bk_tab[0x09] + 1;
    s_bk_tab[0xb] = s_bk_tab[0x09] + 2;
    s_bk_tab[0xc] = s_bk_tab[0x09] + 3;
    for (int i = 0x05; i <= 0x0c; i += 1)
        mem_bs(i);
    mem_write(0x2029, 0x0d);
    mem_write(0x202a, 0x02);

    s6502_push(0x02);
    s6502_push(0x60);
    s_cpu.pc = start;

    if (g_boot_progress_cb) g_boot_progress_cb(95, "Starting...");
}

static void IRAM_ATTR sys_timer(uint32_t n)
{
    static uint32_t t[5] = { 0 };

    for (int i = 0; i < 4; i += 1) {
        if (s_ram[_STCON] & (1 << i)) {
            t[i] += n;
            if (t[i] >= 0x100) {
                t[i] = s_ram[_ST1LD + i];
                /* BBK 音频: 定时器溢出翻转方波电平 (ST1 除外, 它用于按键扫描) */
                if (i >= 1 && i <= 3) {
                    s_spk_overflow[i]++;
                    s_spk_level[i] ^= 1;
                }
                if (s_ram[_TIER] & (1 << i)) {
                    s_ram[_TISR] |= (1 << i);
                    s_ram[_SYSCON] &= 0xf7;
                }
            }
        }
    }

    if (s_ram[_STCTCON] & 0x10) {
        t[4] += n;
        if (t[4] >= 0x1000) {
            t[4] = s_ram[_CTLD];
            if (s_ram[_IER] & 0x02) {
                s_ram[_ISR] |= 0x02;
                s_ram[_SYSCON] &= 0xf7;
            }
        }
    }
 }

static void sys_rtc()
{
    if ((s_ram[_STCTCON] & 0x40) == 0x00)
        return;

    if (s_ram[_RTCSEC]++ == 59) {
        s_ram[_RTCSEC] = 0;
        if (s_ram[_RTCMIN]++ == 59) {
            s_ram[_RTCMIN] = 0;
            if (s_ram[_RTCHR]++ == 23) {
                s_ram[_RTCHR] = 0;
                if (s_ram[_RTCDAYL]++ == 0xff) {
                    if (s_ram[_RTCDAYH]++ == 1) {
                        s_ram[_RTCDAYH] = 0;
                    }
                }
            }
        }
    }
    if ((s_ram[_STCTCON] & 0x20) == 0x00)
        return;
    if ((s_ram[_RTCMIN] == s_ram[_ALMMIN]) &&
        (s_ram[_RTCHR] == s_ram[_ALMHR]) &&
        (s_ram[_RTCDAYL] == s_ram[_ALMDAYL]) &&
        (s_ram[_RTCDAYH] == s_ram[_ALMDAYH])) {
        s_ram[_ISR] |= 0x01;
    }
}


static void IRAM_ATTR sys_isr()
{
    uint8_t idx = 0;
    if (s_cpu.status & 0x04)
        return;
    if ((s_ram[_ISR] & 0x80) && (s_ram[_IER] & 0x80)) {
        idx = 0x02; // PI
        s_ram[_ISR] &= 0x7f;
        // Handled by 'sys_keydown'.
        return;
    } else if ((s_ram[_ISR] & 0x01) && (s_ram[_IER] & 0x01)) {
        idx = 0x13; // ALM
    } else if ((s_ram[_ISR] & 0x02) && (s_ram[_IER] & 0x02)) {
        idx = 0x12; // CT
    } else if ((s_ram[_TISR] & 0x20) && (s_ram[_TIER] & 0x20)) {
        idx = 0x11; // MT
    } else if ((s_ram[_TISR] & 0x80) && (s_ram[_TIER] & 0x80)) {
        idx = 0x10; // GTH
    } else if ((s_ram[_TISR] & 0x40) && (s_ram[_TIER] & 0x40)) {
        idx = 0x0f; // GTL
    }  else if ((s_ram[_TISR] & 0x01) && (s_ram[_TIER] & 0x01)) {
        idx = 0x03; // ST1
        s_ram[_TISR] &= 0xfe;
        s_ram[0x2018] += 1;
        if (s_ram[0x2018] >= s_ram[0x2019]) {
            s_ram[0x201e] |= 0x01;
            s_ram[0x2018] = 0;
        }
        return;
    } else if ((s_ram[_TISR] & 0x02) && (s_ram[_TIER] & 0x02)) {
        idx = 0x04; // ST2
    } else if ((s_ram[_TISR] & 0x04) && (s_ram[_TIER] & 0x04)) {
        idx = 0x05; // ST3
    } else if ((s_ram[_TISR] & 0x08) && (s_ram[_TIER] & 0x08)) {
        idx = 0x06; // ST4
    } else {
        return;
    }

    s6502_push(s_cpu.pc >> 8);
    s6502_push(s_cpu.pc & 0xff);
    s6502_push(s_cpu.status);
    s_cpu.status |= 0x04;
    s_cpu.pc = 0x0300 + idx * 4;
}

static void IRAM_ATTR sys_step()
{
    static int32_t cycles = 0;
    static uint32_t ticked = 0;
    uint32_t tstep = 400 * vars.cpu_rate / vars.timer_rate;
    cycles += vars.cpu_rate * 4000000 / 60;
    while (ticked + tstep < cycles) {
        if (sys_halt_p()) {
            ticked += tstep;
            sys_timer(1);
        } else {
            uint32_t p = ticked / tstep;
            sys_isr();
            ticked += s6502_exec(&s_cpu, 0x100);
            uint32_t q = ticked / tstep;
            sys_timer(q - p);
        }
    }
    cycles -= ticked;
    ticked %= tstep;
}

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
    (void)level;
    va_list va;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
}

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

static void frame_cb(retro_usec_t usec)
{
    static uint32_t ms = 0;
    ms += usec / 1000;
    if (ms > 1000) {
        ms -= 1000;
        sys_rtc();
    }
}

void retro_set_environment(retro_environment_t cb)
{
    /* const: 选项表只读, 移入 Flash 不占内部 DRAM (~6KB) */
    static const struct retro_core_option_definition opts[] = {
        {
            .key = "gam4980_lcd_color",
            .desc = "LCD color theme",
            .values = {{"grey"}, {"green"}, {"blue"}, {"yellow"}, {"random"}, {NULL}},
            .default_value = "random",
        },
        {
            .key = "gam4980_lcd_ghosting",
            .desc = "LCD ghosting frames",
            .values = {{"0"},{"5"},{"10"},{"15"},{"20"},{"25"},{"30"},{"35"},{"40"}},
            .default_value = "15",
        },
        {
            .key = "gam4980_cpu_rate",
            .desc = "CPU clock rate",
            .values = {{"0.25"},{"0.50"},{"0.75"},{"1.00"},{"1.50"},{"2.00"},{"3.00"},{"4.00"},{"8.00"},{NULL}},
            .default_value = "1.00",
        },
        {
            .key = "gam4980_timer_rate",
            .desc = "Timer clock rate",
            .values = {{"0.25"},{"0.50"},{"0.75"},{"1.00"},{"1.50"},{"2.00"},{"3.00"},{"4.00"},{"8.00"},{NULL}},
            .default_value = "1.00",
        },
        {
            .key = "gam4980_key_pressed_input_min_interval",
            .desc = "Key pressed input min interval(ms)",
            .values = {{"0"},{"50"},{"100"},{"150"},{"200"},{"250"},{"300"},{"400"},{"500"},{NULL}},
            .default_value = "0",
        },
        { NULL, NULL, NULL, {{0}}, NULL },
    };

    static struct retro_log_callback log;
    static struct retro_keyboard_callback kbd = {
        .callback = keyboard_cb,
    };
    static struct retro_frame_time_callback frame = {
        .callback = frame_cb,
        .reference = 1000000 / 60,
    };
    static bool yes = true;
    environ_cb = cb;
    if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
        log_cb = log.log;
    environ_cb(RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK, &frame);
    environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kbd);
    environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &yes);

    unsigned opts_ver = 0;
    environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &opts_ver);
    if (opts_ver >= 1) {
        environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, &opts);
    }
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
    video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
    audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
}

void retro_set_input_poll(retro_input_poll_t cb)
{
    input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
    input_state_cb = cb;
}

void retro_get_system_info(struct retro_system_info *info)
{
    info->need_fullpath = false;
    info->valid_extensions = "gam";
    info->library_version = "0.2";
    info->library_name = "gam4980";
    info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
//交换LCD宽高屏幕大小
#if SWAP_LCD_WIDTH_HEIGHT
    
    info->geometry.base_width = LCD_HEIGHT;
    info->geometry.base_height = LCD_WIDTH;
    info->geometry.max_width = LCD_HEIGHT;
    info->geometry.max_height = LCD_WIDTH;

#else
    
    info->geometry.base_width = LCD_WIDTH;
    info->geometry.base_height = LCD_HEIGHT;
    info->geometry.max_width = LCD_WIDTH;
    info->geometry.max_height = LCD_HEIGHT;

#endif // SWAP_LCD_WIDTH_HEIGHT

    info->geometry.aspect_ratio = 0.0;
    info->timing.fps = 60.0;
    info->timing.sample_rate = 44100;

    static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_RGB565;
    environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pixfmt);
}

static bool lcd_color_ok()
{
    uint8_t bg_r = (vars.lcd_bg >> 11) & 0x1f;
    uint8_t bg_g = (vars.lcd_bg >>  6) & 0x1f;
    uint8_t bg_b = (vars.lcd_bg >>  0) & 0x1f;
    uint8_t fg_r = (vars.lcd_fg >> 11) & 0x1f;
    uint8_t fg_g = (vars.lcd_fg >>  6) & 0x1f;
    uint8_t fg_b = (vars.lcd_fg >>  0) & 0x1f;

    if (bg_r < fg_r + 18 ||
        bg_g < fg_g + 18 ||
        bg_b < fg_b + 18)
        return false;

    return true;
}

static void apply_variables()
{
    struct retro_variable var = {0};

    var.key = "gam4980_lcd_color";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) {
        if (strcmp(var.value, "grey") == 0) {
            vars.lcd_bg = 0xd6da;
            vars.lcd_fg = 0x0000;
        } else if (strcmp(var.value, "green") == 0) {
            vars.lcd_bg = 0x96e1;
            vars.lcd_fg = 0x0882;
        } else if (strcmp(var.value, "blue") == 0) {
            vars.lcd_bg = 0x3edd;
            vars.lcd_fg = 0x09a8;
        } else if (strcmp(var.value, "yellow") == 0) {
            vars.lcd_bg = 0xf72c;
            vars.lcd_fg = 0x2920;
        } else if (strcmp(var.value, "random") == 0) {
            do {
                vars.lcd_bg = rand() % 0xffff;
                vars.lcd_fg = rand() % 0xffff;
            } while (!lcd_color_ok());
        }
    }
    var.key = "gam4980_lcd_ghosting";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
        vars.lcd_ghosting = atoi(var.value);

    var.key = "gam4980_cpu_rate";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
        vars.cpu_rate = atof(var.value);

    var.key = "gam4980_timer_rate";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
        vars.timer_rate = atof(var.value);

    var.key = "gam4980_key_pressed_input_min_interval";
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
        vars.key_pressed_input_min_interval = atof(var.value);
}

void retro_init(void)
{
    char *systemdir;
    char romdir[512];
    environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &systemdir);
    snprintf(romdir, 512, "%s/gam4980", systemdir);
    sys_init(romdir);
    apply_variables();

    // Support RetroArch cheats.
    struct retro_memory_descriptor rmdesc = {
        .flags = RETRO_MEMDESC_SYSTEM_RAM,
        .start = 0,
        .len   = 0x8000,
        .ptr   = s_ram,
    };
    struct retro_memory_map rmmap = {
        .descriptors = &rmdesc,
        .num_descriptors = 1,
    };
    environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &rmmap);
}

bool retro_load_game(const struct retro_game_info *game)
{
    if (game == NULL)
        return true;
    if (game->data == NULL)
        return false;
    if (game->size > 0x1e0000) {
        // Game too large! (>1920K)
        return false;
    }
    sys_load(game->data, game->size);
    return true;
}

/* V1.0.52: 返回游戏数据在 PSRAM 中的目标区指针 (sys_flash+0x8000+0xd000).
 * 供 gam4980_emu_load 直接把文件流式读入该区, 避免额外分配 1MB 缓冲. */
uint8_t *gam4980_retro_rom_target(void)
{
    return sys_flash + 0x8000 + 0xd000;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
}

void retro_deinit(void)
{
    /* 释放引擎动态分配的 6MB PSRAM + 32KB 内部 sys_ram.
     * 重进电子词典时由 retro_init → sys_init → retro_mem_alloc 重新分配并重读 ROM. */
    retro_mem_free();
    s_ram = NULL;   /* sys_ram 已释放, 指针置空 */
}

void retro_reset(void)
{
}

static inline void pp8(int y, int x, uint8_t p8)
{
    // Draw 8 pixels.
    for (int i = 0; i < 8; i += 1) {
        int z = y * (LCD_WIDTH + 1) + x * 8 + i;
        bool p = p8 & (1 << (7 - i));
        fb[z] = p ? vars.lcd_fg : vars.lcd_bg;

        if (vars.lcd_ghosting > 0) {
            // LCD ghosting effect.
            fa[z] += p ? 1 : -1;
            if (fa[z] < 0)
                fa[z] = 0;
            if (fa[z] > vars.lcd_ghosting - 1)
                fa[z] = vars.lcd_ghosting - 1;
        }
    }
}

static void blend_frame(void)
{
    uint8_t bg_r = (vars.lcd_bg >> 11) & 0x1f;
    uint8_t bg_g = (vars.lcd_bg >>  6) & 0x1f;
    uint8_t bg_b = (vars.lcd_bg >>  0) & 0x1f;
    uint8_t fg_r = (vars.lcd_fg >> 11) & 0x1f;
    uint8_t fg_g = (vars.lcd_fg >>  6) & 0x1f;
    uint8_t fg_b = (vars.lcd_fg >>  0) & 0x1f;

    for (int i = 0; i < LCD_HEIGHT; i += 1) {
        for (int j = 0; j < LCD_WIDTH; j += 1) {
            int z = i * (LCD_WIDTH + 1) + j;
            float a = (float)fa[z] / vars.lcd_ghosting;
            uint8_t mix_r = 0x1f & (uint8_t)((1 - a) * bg_r + a * fg_r);
            uint8_t mix_g = 0x1f & (uint8_t)((1 - a) * bg_g + a * fg_g);
            uint8_t mix_b = 0x1f & (uint8_t)((1 - a) * bg_b + a * fg_b);
            fb[z] = mix_r << 11 | mix_g << 6 | mix_b;
        }
    }
}


void IRAM_ATTR retro_run(void)
{
    bool vupdated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &vupdated) && vupdated)
        apply_variables();

    input_poll_cb();

    // Handle joypad.
    static int pressed = -1;
    static int repeat = 0;
    for (int i = 0; i < 20; i += 1) {
        if (pressed == i) {
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, i) == 0) {
                pressed = -1;
                repeat = 0;
            } else {
                repeat += 1;
                if (repeat > 20) {
                    repeat -= 5;
                    sys_keydown(_joyk[i]);
                }
            }
        }
        if (pressed == -1 && input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, i)) {
            pressed = i;
            sys_keydown(_joyk[i]);
            break;
        }
    }

    sys_step();

    // Draw the screen.
    uint8_t *v = s_ram + 0x400;
    s_ram[0x400] = s_ram[0x1000];

    for (int j = 65; j >= -30; j -= 1) {
        for (int i = 1; i < 20; i += 1) {
            pp8(j >= 0 ? j : (j * -1 + 65), i, *v++);
        }
        v += 13;
    }
    v = s_ram + 0x413;
    for (int j = 64; j >= -30; j -= 1) {
        pp8(j >= 0 ? j : (j * -1 + 65), 0, *v++);
        v += 31;
    }
    pp8(65, 0, s_ram[0x0ff3]);

    if (vars.lcd_ghosting > 0)
        blend_frame();
    video_cb(fb, LCD_WIDTH, LCD_HEIGHT, 2 * (LCD_WIDTH + 1));

    bbk_audio_flush_frame();
}

/* 每帧生成定时器方波音效 (60fps, 44100Hz). 取最活跃音频定时器 (溢出次数最多),
 * 按实际溢出率还原方波频率, 输出到 audio_cb. */
static void bbk_audio_flush_frame(void)
{
    int i;
    if (!audio_cb) {
        for (i = 0; i < 4; i++) s_spk_overflow[i] = 0;
        return;
    }

    /* 选择最活跃的音频定时器 */
    int best = -1;
    int best_ovf = 0;
    for (i = 1; i < 4; i++) {
        if (s_spk_overflow[i] >= 2 && s_spk_overflow[i] > best_ovf) {
            best_ovf = s_spk_overflow[i];
            best = i;
        }
    }

    if (best < 0) {
        /* 无音频: 输出静音 */
        for (i = 0; i < BBK_SAMPLES_PER_FRAME; i++)
            audio_cb(0, 0);
        for (i = 0; i < 4; i++) s_spk_overflow[i] = 0;
        return;
    }

    /* 方波频率: overflow/2 周期每帧, 帧率 60 */
    int cycles_f = best_ovf / 2;
    if (cycles_f < 1) cycles_f = 1;
    int period = BBK_SAMPLES_PER_FRAME / cycles_f;
    if (period < 2) period = 2;
    int half = period / 2;
    int phase = s_spk_phase[best];
    for (i = 0; i < BBK_SAMPLES_PER_FRAME; i++) {
        int16_t v = (phase < half) ? BBK_SPK_AMP : -BBK_SPK_AMP;
        audio_cb(v, v);
        phase++;
        if (phase >= period) phase = 0;
    }
    s_spk_phase[best] = phase;

    for (i = 0; i < 4; i++) s_spk_overflow[i] = 0;
}

struct __attribute__((packed)) sys_state {
    uint8_t ram[0x8000];
    s6502_t cpu;
    uint8_t bk_sel;
    uint16_t bk_tab[16];
    uint8_t flash_cmd;
    uint8_t flash_cycles;
};

size_t retro_serialize_size(void)
{
    return sizeof(struct sys_state);
}

bool retro_serialize(void *data, size_t size)
{
    struct sys_state state;
    memcpy(&state.ram, s_ram, 0x8000);
    state.cpu = s_cpu;
    state.bk_sel = s_bk_sel;
    for (int i = 0; i < 16; ++i)
        state.bk_tab[i] = s_bk_tab[i];
    state.flash_cmd = s_flash_cmd;
    state.flash_cycles = s_flash_cycles;
    memcpy(data, &state, size);
    return true;
}

bool retro_unserialize(const void *data, size_t size)
{
    struct sys_state state;
    memcpy(&state, data, size);
    memcpy(s_ram, &state.ram, 0x8000);
    s_cpu = state.cpu;
    s_bk_sel = state.bk_sel;
    for (int i = 0; i < 16; ++i)
        s_bk_tab[i] = state.bk_tab[i];
    s_flash_cmd = state.flash_cmd;
    s_flash_cycles = state.flash_cycles;
    for (int i = 0; i < 16; ++i)
        mem_bs(i);
    return true;
}

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code) {}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
    return false;
}

void retro_unload_game(void)
{
}

unsigned retro_get_region(void)
{
    return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id)
{
    switch (id) {
    case RETRO_MEMORY_SAVE_RAM:
        return sys_flash;
    case RETRO_MEMORY_SYSTEM_RAM:
        return s_ram;
    default:
        return NULL;
    }
}

size_t retro_get_memory_size(unsigned id)
{
    switch (id) {
    case RETRO_MEMORY_SAVE_RAM:
        // Saved: $000000-$00bfff, $1f8000-$1fffff
        return 0x14000;
    case RETRO_MEMORY_SYSTEM_RAM:
        return 0x8000;
    default:
        return 0;
    }
}

/* === 存档读写 (供 gam4980_emu.c 调用, 实现 SD 卡持久化) ===
 * 真正的存档区只有 sys_flash[0x0000..0x7FFF] 这 32KB:
 *   - 对应游戏视角 0x1F8000-0x1FFFFF (即 "last 32 KiB for save file", 经
 *     flash_read/flash_write 中的 (addr+0x8000)%0x200000 旋转映射到 sys_flash[0..0x7FFF])
 *   - sys_load 仅写入 sys_flash[0x8000+] 区域加载 .gam 数据, 0x0-0x7FFF 不会被覆盖
 *   - 大多数字节为 0xFF (flash 擦除态), 实际有效数据通常 < 4KB
 *
 * 历史问题 (V1.0.7 及之前): 曾尝试按 libretro 标准保存 0x14000 (80KB) 3 段拼接, 但段 2/段 3
 *   覆盖了游戏数据区, 加载时把 .gam 写入的游戏内容覆盖掉, 导致重启后游戏卡死/进度丢失.
 *   修复: 仅保存真正的 32KB 存档区, 不再触碰 sys_flash[0x8000+] 游戏数据区.
 *
 * size 参数必须 == 0x8000, 否则静默返回 (防止 caller 写错大小). */
#define GAM4980_SAVE_SIZE 0x8000
void gam4980_flash_read_save(uint8_t *buf, size_t size) {
    if (!buf || size != GAM4980_SAVE_SIZE) return;
    /* 仅读取 sys_flash[0..0x7FFF] 真正的存档区 (32KB) */
    memcpy(buf, sys_flash, 0x8000);
}
void gam4980_flash_write_save(const uint8_t *buf, size_t size) {
    if (!buf || size != GAM4980_SAVE_SIZE) return;
    /* 仅写入 sys_flash[0..0x7FFF] 真正的存档区 (32KB) */
    memcpy(sys_flash, buf, 0x8000);
}
