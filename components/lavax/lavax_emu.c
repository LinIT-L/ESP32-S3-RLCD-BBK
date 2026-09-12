/*
 * lavax_emu.c — LavaXVM 宿主 (ESP32 生命周期 / 内存 / 运行循环).
 *
 * 职责:
 *   - 在 PSRAM 分配 VM 内存 (MY_DATA_SIZE=1MB) 与屏平面缓冲 (~100KB) + 灰度缓冲
 *   - 把 st7305 句柄与灰度缓冲注入平台层 (lavax_platform_set_display)
 *   - 提供 lavax_emu_load(路径) 启动单个 .lav 程序, lavax_emu_run() 运行
 *   - 提供 lavax_emu_background_init / lavax_emu_unload 供 engine_manager 调用
 *
 * 设计: 不引入 retro-go, 直接在主菜单选择 .lav 后:
 *   init -> 分配内存 -> lavaxvm_init -> 加载程序字节码 -> lavaxvm_run 直至退出。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lavaxvm.h"
#include "lavax_emu.h"  /* lavax_exit_check_fn 等宿主 API */
#include "st7305.h"
#include "virtual_keys.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "type.h"   /* byte/word/a32 */
#include "base.h"   /* MY_DATA_SIZE, LCD_WIDTH/HEIGHT, SCREEN_BUFFER_SIZE */

#include "cheater.h"
#include "cheat_ui.h"
#include "cheat_float.h"
#include "board_rlcd.h"

#define TAG "LAVAX"

/* ---- 由 lavaxvm 暴露的内部符号 ---- */
extern char CurrentProgramPath[];      /* 大小见 I_MAX_PATH */
extern int  SetRoot(char *dir);
extern void lavReset(void);
extern void main_loop(void);
extern byte *pLAVA;
extern byte *TaskOpen(char *fname);
extern void TaskClose(byte *program);
extern void Color256Init(void);
extern void filesys_init(void);
/* 引擎运行缓冲: 由 lava.c 定义, 进入文曲星时再分配 (不常驻后台) */
extern int  *func_stack;   /* 调用栈, 动态加大防深递归溢出 */
extern int   func_top;     /* 调用栈顶 (进入时需清零) */
extern int  *circle_buf;   /* 圆绘制临时缓冲 */
extern byte *patbuf;       /* 图形字符临时缓冲 */
/* 由平台层导出 */
void lavax_platform_set_display(st7305_handle_t *lcd, uint8_t *gray_buf);
void lavax_platform_set_display_mode(int mode);
void lavax_platform_request_exit(void);
void lavax_platform_reset(void);
/* lcd.c 导出的屏平面缓冲指针, 宿主负责分配在 PSRAM */
extern byte *bmpdata_buf;
extern int  ScreenWidth, ScreenHeight;

/* 灰度缓冲大小 (平台层读取的前平面 256x192) */
#define GRAY_SIZE (256 * 192)

/* 内部状态 */
static void *s_vm_ram = NULL;      /* PSRAM: VM 数据内存 */
static void *s_local_ram = NULL;   /* 屏缓冲 (BmpData) */
static uint8_t *s_gray = NULL;     /* 灰度缩略缓冲 */
static st7305_handle_t *s_lcd = NULL;
static bool s_ready = false;
static bool s_running = false;

/* V1.0.9x: 宿主注入的"退出确认浮层"绘制回调 (文曲星两步退出确认) */
static void (*s_confirm_draw)(st7305_handle_t *lcd) = NULL;
void lavax_set_exit_confirm_ui(void (*draw)(st7305_handle_t *lcd)) { s_confirm_draw = draw; }
void lavax_set_display_mode(int mode) { lavax_platform_set_display_mode(mode); }
void lavax_draw_exit_confirm(st7305_handle_t *lcd)
{
    if (s_confirm_draw && lcd) s_confirm_draw(lcd);
}

bool lavax_emu_is_running(void)
{
    return s_running;
}

esp_err_t lavax_emu_background_init(st7305_handle_t *lcd)
{
    if (s_ready)
        return ESP_OK;
    s_lcd = lcd;

    /* 分配 PSRAM */
    s_vm_ram = heap_caps_malloc(lavaxvm_vm_ram_size() + lavaxvm_local_ram_size(),
                                MALLOC_CAP_SPIRAM);
    if (!s_vm_ram)
    {
        ESP_LOGE(TAG, "PSRAM 分配失败: VM %d + local %d bytes",
                 (int)lavaxvm_vm_ram_size(), (int)lavaxvm_local_ram_size());
        return ESP_ERR_NO_MEM;
    }
    memset(s_vm_ram, 0, lavaxvm_vm_ram_size() + lavaxvm_local_ram_size());

    /* 屏平面缓冲 (BmpData) 放 PSRAM */
    bmpdata_buf = heap_caps_malloc(SCREEN_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!bmpdata_buf)
    {
        ESP_LOGE(TAG, "PSRAM 屏缓冲分配失败 (%d bytes)", SCREEN_BUFFER_SIZE);
        heap_caps_free(s_vm_ram); s_vm_ram = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_gray = heap_caps_malloc(GRAY_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_gray)
    {
        ESP_LOGE(TAG, "PSRAM 灰度缓冲分配失败 (%d bytes)", GRAY_SIZE);
        heap_caps_free(s_vm_ram); s_vm_ram = NULL;
        heap_caps_free(bmpdata_buf); bmpdata_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 引擎运行缓冲: 进入文曲星才分配, 退出释放 (不常驻后台).
     * func_stack 加大到 16KB 防深递归溢出; 放内部 DRAM 保证热访问速度. */
    func_stack = heap_caps_malloc(4096 * sizeof(int), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    circle_buf = heap_caps_malloc(320 * sizeof(int),   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    patbuf     = heap_caps_malloc(0x400,              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!func_stack || !circle_buf || !patbuf)
    {
        ESP_LOGE(TAG, "LavaX 运行缓冲分配失败 (func_stack=%p circle=%p patbuf=%p)",
                 (void*)func_stack, (void*)circle_buf, (void*)patbuf);
        if (func_stack) { heap_caps_free(func_stack); func_stack = NULL; }
        if (circle_buf) { heap_caps_free(circle_buf); circle_buf = NULL; }
        if (patbuf)     { heap_caps_free(patbuf);     patbuf = NULL; }
        heap_caps_free(s_vm_ram); s_vm_ram = NULL;
        heap_caps_free(bmpdata_buf); bmpdata_buf = NULL;
        heap_caps_free(s_gray); s_gray = NULL;
        return ESP_ERR_NO_MEM;
    }
    func_top = 0;   /* 栈顶归零, 防上次退出残留 */

    /* 注入平台显示 */
    lavax_platform_set_display(s_lcd, s_gray);

    if (!lavaxvm_init(s_vm_ram, lavaxvm_vm_ram_size(),
                      (uint8_t *)s_vm_ram + lavaxvm_vm_ram_size(),
                      lavaxvm_local_ram_size()))
    {
        ESP_LOGE(TAG, "LavaXVM init failed: %s",
                 lavaxvm_last_error() ? lavaxvm_last_error() : "?");
        heap_caps_free(s_vm_ram); s_vm_ram = NULL;
        heap_caps_free(bmpdata_buf); bmpdata_buf = NULL;
        heap_caps_free(s_gray); s_gray = NULL;
        return ESP_ERR_INVALID_STATE;
    }

    s_ready = true;
    ESP_LOGI(TAG, "LavaX VM ready: VM=%d local=%d", 
             (int)lavaxvm_vm_ram_size(), (int)lavaxvm_local_ram_size());
    return ESP_OK;
}

/* 修改机 region (完整定义在文件后部修; 此处前向声明供 load_and_run 使用) */
static cheater_region_t lavax_cheat_region;

/* 加载一个 .lav 程序并运行直至退出 */
esp_err_t lavax_emu_load_and_run(const char *lav_path)
{
    if (!s_ready)
        return ESP_ERR_INVALID_STATE;

    filesys_init();
    Color256Init();

    /* 以游戏所在目录设为 LavaX 文件根, 使程序能读写同级的 LavaData/ 存档目录
     * (例如 暴龙机.app/LavaData/digivice.dat). lav_path 形如
     * /sdcard/lava/暴龙机/暴龙机.lav; 取目录部分, 去掉文件名.
     * 若该目录名是 Lava/lava(游戏放 <游戏>/Lava/x.lav, 数据在上级 <游戏>/LavaData),
     * 则再上移一级到游戏主目录. */
    {
        char root_dir[160];
        const char *slash = strrchr(lav_path, '/');
        if (slash && slash != lav_path) {
            size_t n = (size_t)(slash - lav_path);
            if (n >= sizeof(root_dir)) n = sizeof(root_dir) - 1;
            memcpy(root_dir, lav_path, n);
            root_dir[n] = '\0';
            /* Lava 子目录 → 上移到游戏主目录(数据在 ../LavaData) */
            const char *base = strrchr(root_dir, '/');
            base = base ? base + 1 : root_dir;
            if (strcmp(base, "Lava") == 0 || strcmp(base, "lava") == 0) {
                char *slash2 = strrchr(root_dir, '/');
                if (slash2) *slash2 = 0;
            }
            SetRoot(root_dir);
            ESP_LOGI(TAG, "文曲星文件根: %s", root_dir);
            /* V1.x 缺文件诊断: 原样打印根路径字节(含非UTF8时按hex), 确认根目录设置是否正确 */
            {
                char hex[512]; int h=0, i;
                for (i=0; root_dir[i] && h < (int)sizeof(hex)-4; i++) {
                    h += snprintf(hex+h, sizeof(hex)-h, "%02X ", (unsigned char)root_dir[i]);
                }
                printf("[LAVAX-ROOT] %s\n", hex);
            }
        }
    }

    pLAVA = TaskOpen((char *)lav_path);
    if (!pLAVA)
    {
        ESP_LOGE(TAG, "无法加载游戏: %s", lav_path);
        return ESP_ERR_NOT_FOUND;
    }

    /* 复位平台退出/组合状态, 开始新一轮运行 */
    lavax_platform_reset();
    lavReset();
    /* 启用屏幕虚拟按键 (文曲星用标准手柄布局: 方向键+A/B+SEL/STA, 与步步高一致) */
    virtual_keys_set_layout(VK_LAYOUT_STANDARD);
    virtual_keys_set_enabled(true);
    s_running = true;
    /* 独立修改机浮标: 登记本局游戏内存 region (LavaX VM RAM) */
    cheat_float_attach(&lavax_cheat_region, CHEAT_BITS_8);
    main_loop();
    s_running = false;
    cheat_float_detach();
    virtual_keys_set_enabled(false);

    TaskClose(pLAVA);
    pLAVA = NULL;
    return ESP_OK;
}

/*
 * 退出键检测 (由 platform.c 的 lavax_platform_poll 每帧调用).
 * 委托给 menu 层注入的 lavax_exit_check 回调, 使本组件不依赖 menu 头.
 * 回调由 menu 负责检测物理返回键/手柄 BACK/F_EXIT 等.
 */
static lavax_exit_check_fn s_exit_check = NULL;

void lavax_set_exit_check(lavax_exit_check_fn fn)
{
    s_exit_check = fn;
}

bool lavax_check_exit(void)
{
    if (s_exit_check)
        return s_exit_check();
    return false;
}

/* === 修改机(cheat)内存 region: 只覆盖本引擎游戏 RAM ===
 * s_vm_ram (PSRAM) 是 LavaX VM 数据内存 (byte 寻址), 游戏变量都在这里.
 * 只经 s_vm_ram peek/poke, 绝不触碰其它系统/软件内存. */
static uint8_t lavax_cheat_peek(uint16_t off)
{
    if (!s_vm_ram) return 0;
    uint32_t n = lavaxvm_vm_ram_size() + lavaxvm_local_ram_size();
    return (off < n) ? ((uint8_t *)s_vm_ram)[off] : 0;
}
static void lavax_cheat_poke(uint16_t off, uint8_t v)
{
    if (!s_vm_ram) return;
    uint32_t n = lavaxvm_vm_ram_size() + lavaxvm_local_ram_size();
    if (off < n) ((uint8_t *)s_vm_ram)[off] = v;
}
static cheater_region_t lavax_cheat_region = {
    .base = 0,
    .size = 0xFC00,     /* VM 数据区上限 (MY_DATA_SIZE); peek/poke 内部按实际大小钳制 */
    .peek = lavax_cheat_peek,
    .poke = lavax_cheat_poke,
};

void lavax_emu_cheat_poll(void)
{
    /* 修改机已独立到 cheat_float 覆盖层 (浮标绘制 + 热键/点按呼出), 此处不再处理. */
}

void lavax_emu_unload(void)
{
    if (s_vm_ram)  { heap_caps_free(s_vm_ram);  s_vm_ram  = NULL; }
    if (bmpdata_buf) { heap_caps_free(bmpdata_buf); bmpdata_buf = NULL; }
    if (s_gray)  { heap_caps_free(s_gray);     s_gray  = NULL; }
    /* 释放引擎运行缓冲 (func_stack/circle_buf/patbuf): 退出文曲星即归还内存 */
    if (func_stack) { heap_caps_free(func_stack); func_stack = NULL; }
    if (circle_buf) { heap_caps_free(circle_buf); circle_buf = NULL; }
    if (patbuf)     { heap_caps_free(patbuf);     patbuf = NULL; }
    func_top = 0;
    lavaxvm_shutdown();
    s_ready = false;
    s_running = false;
}