/*
 * vpet_emu.c — 暴龙机 (虚拟宠物) 引擎宿主
 *
 * 基于 vpet-emu-zepp 的 E0C6200 4-bit CPU 核心 (e0c6200_cpu.c) 运行第一代
 * 虚拟宠物 ROM (Tamagotchi P1/P2、Digimon 等). 移植自:
 *   third_party/vpet-emu-zepp/page/gt/emulator/index.page.js
 *   third_party/vpet-emu-zepp/utils/display.js   (packVram / VRAM 布局)
 *   third_party/vpet-emu-zepp/utils/rom.js       (ROM → big-endian 16 位字)
 *
 * 职责:
 *   - 从 TF 卡 /sdcard/vpet/ 读取 .bin ROM, 转为 16 位大端指令字喂给 CPU
 *   - 运行模拟任务 (task 方式): 自适应批次推进 CPU 时钟, 保持接近真实 1.6MHz
 *   - 把 CPU VRAM 渲染为 32x16 虚拟像素 → 1bpp 位图 → ST7305 屏幕
 *   - 音效经 CPU 内置 sound 状态机回调到 tone_player (非阻塞持续音)
 *   - 按键: GB joypad A/B/Start → K0.2/K0.1/K0.0
 *
 * 生命周期: background_init(进入菜单) / start+stop(游戏运行) / unload(回主菜单).
 */

#include "vpet_emu.h"
#include "e0c6200_cpu.h"
#include "tone_player.h"
#include "virtual_keys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"

#define TAG "VPET"

/* ============ 显示参数 ============
 * SCALE=7 (非原版8): 缩小到 224x112 并上移到 y=30, 避免虚拟按键
 * (UP y=167 起) 与 LCD 底部 (y=214 @SCALE=8) 重叠.
 * 图标区高度独立为 28px (24px图标+2px边距), 不再用 VPET_ICON_SLOT 做垂直间距. */
#define VPET_COLS   32          /* 虚拟像素列数 */
#define VPET_ROWS   16          /* 虚拟像素行数 */
#define VPET_SCALE  7           /* 每虚拟像素放大倍数 (7x7 → 224x112) */
#define VPET_DISP_W (VPET_COLS * VPET_SCALE)   /* 224 */
#define VPET_DISP_H (VPET_ROWS * VPET_SCALE)   /* 112 */
#define VPET_X_OFF  ((ST7305_WIDTH  - VPET_DISP_W) / 2)   /* 88 */
#define VPET_ICON_AREA_H 28                        /* 图标区高度 (24图标+4边距) */
#define VPET_Y_OFF  (VPET_ICON_AREA_H + 32)        /* 60: 整体下移30px, 避开顶部状态图标 */

/* 图标槽: LCD 上下各 4 个 (对齐 Zepp: nibble 16=顶部, nibble 137=底部) */
#define VPET_ICON_SLOT  (VPET_DISP_W / 4)   /* 56 (仅水平间距) */
#define VPET_ICON_TOP_Y (VPET_Y_OFF - VPET_ICON_AREA_H)   /* 2 */
#define VPET_ICON_BOT_Y (VPET_Y_OFF + VPET_DISP_H)        /* 142 */

/* packVram 的 VRAM 列偏移表 (display.js vramOffsets, 1:1) */
static const uint16_t s_vram_offsets[VPET_COLS] = {
    0 | (40 << 8), 1 | (41 << 8), 2 | (42 << 8), 3 | (43 << 8),
    4 | (44 << 8), 5 | (45 << 8), 6 | (46 << 8), 7 | (47 << 8),
    9 | (49 << 8), 10 | (50 << 8), 11 | (51 << 8), 12 | (52 << 8),
    13 | (53 << 8), 14 | (54 << 8), 15 | (55 << 8), 16 | (56 << 8),
    36 | (76 << 8), 35 | (75 << 8), 34 | (74 << 8), 33 | (73 << 8),
    32 | (72 << 8), 31 | (71 << 8), 30 | (70 << 8), 29 | (69 << 8),
    27 | (67 << 8), 26 | (66 << 8), 25 | (65 << 8), 24 | (64 << 8),
    23 | (63 << 8), 22 | (62 << 8), 21 | (61 << 8), 20 | (60 << 8),
};

/* 屏幕帧缓冲 (整屏 1bpp, 放 PSRAM 不占内部 SRAM) */
#define VPET_FRAME_BYTES (ST7305_WIDTH * ST7305_HEIGHT / 8)   /* 15000 */
EXT_RAM_BSS_ATTR static uint8_t s_frame[VPET_FRAME_BYTES];

/* ============ 运行状态 ============ */
static st7305_handle_t *s_lcd = NULL;
static TaskHandle_t    s_task = NULL;
static volatile bool   s_stop_requested = false;
static volatile bool   s_paused = false;
static volatile bool   s_pause_ack = false;
static volatile uint8_t s_joypad = 0xFF;   /* 全松开 */
static bool            s_ready = false;

/* V1.0.9x: 是否按内容旋转 180° — 按 ROM 尺寸判断机型, 不用文件名:
 *    16KB (8192 字, 数码宝贝/Digimon 家族) → 旋转 180°
 *    12KB (6144 字, Tamagotchi P1/P2)     → 不旋转
 * 由 vpet_emu_start 根据读到的 words 设置. */
static bool s_rotate = false;

/* ============ 显示模式 / 抗锯齿 (仅暴龙机, 不吃共享灰度) ============
 * s_disp_mode: 0=点对点, 1=放大(等比拉到最宽/置顶), 2=拉伸(强制全屏)
 * s_aa: EPX 抗锯齿 (把虚拟 32x16 格扩展为 64x32 平滑缩放) */
static int  s_disp_mode = 0;
static bool s_aa = false;
#define VPET_DISP_FIT   0
#define VPET_DISP_ZOOM  1   /* 放大 */
#define VPET_DISP_STRETCH 2 /* 拉伸 */

static void vpet_fill_black(int x, int y, int w, int h);   /* 前向声明 (定义在下方) */

/* EPX 2x (scale2x) 平滑: 从 32x16 packed 生成 64x32, 四角判断邻居 */
static void vpet_epx_expand(const uint16_t *packed, uint8_t e[64][32]) {
    const int W = VPET_COLS, H = VPET_ROWS;
    /* 先取按位转 bits[y][x] (x 列, y 行), 便于取邻居 */
    static uint8_t b[16][32];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            b[y][x] = (packed[x] & (1u << y)) ? 1 : 0;
    for (int x = 0; x < W; x++) {
        for (int y = 0; y < H; y++) {
            uint8_t P = b[y][x];
            uint8_t B = (y > 0)     ? b[y-1][x] : P;
            uint8_t D = (x > 0)     ? b[y][x-1] : P;
            uint8_t E = (x < W-1)   ? b[y][x+1] : P;
            uint8_t F = (y < H-1)   ? b[y+1][x] : P;
            uint8_t Hh = (x < W-1 && y < H-1) ? b[y+1][x+1] : P;
            uint8_t E0 = P, E1 = P, E2 = P, E3 = P;
            if (B != Hh && F != D) {
                E0 = (D == B) ? D : P;
                E1 = (B == E) ? B : P;
                E2 = (F == D) ? F : P;
                E3 = (Hh == E) ? Hh : P;
            }
            e[y*2  ][x*2  ] = E0;
            e[y*2  ][x*2+1] = E1;
            e[y*2+1][x*2  ] = E2;
            e[y*2+1][x*2+1] = E3;
        }
    }
}

/* 由网格坐标算屏上矩形并填黑. mode 决定几何:
 *  - FIT:     当前 224x112 @ (88,60), 每次源格 7x7
 *  - ZOOM:    等比拉到最宽 400 宽, 高=200, 置顶 y=30
 *  - STRETCH: 强制整屏 400x300 */
static void vpet_fill_cell(int gx, int gy, int GW, int GH) {
    int W = ST7305_WIDTH, H = ST7305_HEIGHT, X = 0, Y = 0;
    if (s_disp_mode == VPET_DISP_FIT) { W = VPET_DISP_W; H = VPET_DISP_H; X = VPET_X_OFF; Y = VPET_Y_OFF; }
    else if (s_disp_mode == VPET_DISP_ZOOM) { W = ST7305_WIDTH; H = (ST7305_WIDTH * VPET_ROWS) / VPET_COLS; X = 0; Y = 30; }
    else { W = ST7305_WIDTH; H = ST7305_HEIGHT; X = 0; Y = 0; }   /* 拉伸 */
    int x0 = X + (W * gx) / GW;
    int x1 = X + (W * (gx + 1)) / GW;
    int y0 = Y + (H * gy) / GH;
    int y1 = Y + (H * (gy + 1)) / GH;
    vpet_fill_black(x0, y0, x1 - x0, y1 - y0);
}

/* ROM 缓冲 (PSRAM): 加载后整个模拟期间常驻 */
static uint16_t       *s_rom = NULL;
static int             s_rom_words = 0;
#define VPET_ROM_MAX_BYTES (64 * 1024)

/* ============ VRAM → 屏幕 ============ */

/* packVram (display.js): 32 列, 每列 16 bit (bit0=顶行), 输出到 buf[32] */
static void vpet_pack_vram(const uint16_t *vram_words, uint16_t *buf) {
    for (int x = 0; x < VPET_COLS; x++) {
        uint16_t offset = s_vram_offsets[x];
        uint16_t word0 = vram_words[offset & 0xff];
        uint16_t word1 = vram_words[offset >> 8];
        uint8_t byte0 = (uint8_t)((word0 >> 4) | (word0 & 0xf));
        uint8_t byte1 = (uint8_t)((word1 >> 4) | (word1 & 0xf));
        buf[x] = (uint16_t)((byte1 << 8) | byte0);
    }
}

/* 在 s_frame 中画一个 (w*h) 的黑色填充矩形 (1bpp, MSB 在左) */
static void vpet_fill_black(int x, int y, int w, int h) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > ST7305_WIDTH)  w = ST7305_WIDTH - x;
    if (y + h > ST7305_HEIGHT) h = ST7305_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    const int row_bytes = (w + 7) >> 3;
    for (int r = 0; r < h; r++) {
        uint8_t *rowp = &s_frame[(size_t)(y + r) * (ST7305_WIDTH / 8) + (x >> 3)];
        for (int c = 0; c < row_bytes; c++) rowp[c] = 0xff;
        /* 首/尾字节需按实际 x 边界掩码, 避免越界画到旁边列 (用整字节覆盖即可,
         * 因为 x 对齐到 8 由调用方保证: LCD 与图标槽的 x 都 8 对齐). */
        if (x & 7) {
            rowp[0] &= (uint8_t)(0xff >> (x & 7));
        }
        if ((x + w) & 7) {
            rowp[row_bytes - 1] &= (uint8_t)(0xff << (8 - ((x + w) & 7)));
        }
    }
}

/* 渲染整帧: 清白 → LCD 显示区 → 上下图标 → 虚拟按键 → 刷屏 */
static void vpet_render(void) {
    if (!s_lcd) return;

    /* 关键: st7305_blit_1bit 只画黑点、白点跳过不改 dev->fb. 若不清屏,
     * 上一帧/主菜单壁纸会残留在面板上, 叠出新黑点 → 花屏.
     * 先整屏清白, 再用黑点重绘. */
    st7305_clear(s_lcd, ST7305_COLOR_WHITE);

    uint16_t packed[VPET_COLS];
    vpet_pack_vram(get_VRAM_words(), packed);

    memset(s_frame, 0, sizeof(s_frame));   /* 全 0 = 白 */

    /* LCD 区: 逐像素填充 (V1.0.9x 回退: 新显示模式/EPX 的整字节填格在非对齐起点会漏跨字节 → 花屏.
     * 回到验证过的按像素 OR 位填充, 保留按 ROM 尺寸旋转). */
    for (int c = 0; c < VPET_COLS; c++) {
        uint16_t word = packed[c];
        for (int r = 0; r < VPET_ROWS; r++) {
            if (!(word & (1u << r))) continue;    /* 该像素 off */
            const int px = s_rotate ? (VPET_X_OFF + (VPET_COLS - 1 - c) * VPET_SCALE)
                                    : (VPET_X_OFF + c * VPET_SCALE);
            const int py = s_rotate ? (VPET_Y_OFF + (VPET_ROWS - 1 - r) * VPET_SCALE)
                                    : (VPET_Y_OFF + r * VPET_SCALE);
            for (int dy = 0; dy < VPET_SCALE; dy++) {
                uint8_t *rowbase = &s_frame[(size_t)(py + dy) * (ST7305_WIDTH / 8)];
                for (int dx = 0; dx < VPET_SCALE; dx++) {
                    rowbase[(px + dx) >> 3] |= (uint8_t)(0x80 >> ((px + dx) & 7));
                }
            }
        }
    }

    /* LCD 外框 + 上下图标 */
    vpet_fill_black(VPET_X_OFF - 2, VPET_Y_OFF - 2, VPET_DISP_W + 4, 2);
    vpet_fill_black(VPET_X_OFF - 2, VPET_Y_OFF + VPET_DISP_H, VPET_DISP_W + 4, 2);
    vpet_fill_black(VPET_X_OFF - 2, VPET_Y_OFF - 2, 2, VPET_DISP_H + 4);
    vpet_fill_black(VPET_X_OFF + VPET_DISP_W, VPET_Y_OFF - 2, 2, VPET_DISP_H + 4);

    /* 上下图标: nibble 16 / 137 的 bit0..3, 亮则画 24x24 方块 */
    const uint8_t *vram = get_VRAM();
    for (int i = 0; i < 4; i++) {
        if ((vram[16] >> i) & 1) {
            vpet_fill_black(VPET_X_OFF + i * VPET_ICON_SLOT + (VPET_ICON_SLOT - 24) / 2,
                            VPET_ICON_TOP_Y + (VPET_ICON_AREA_H - 24) / 2, 24, 24);
        }
        if ((vram[137] >> i) & 1) {
            vpet_fill_black(VPET_X_OFF + i * VPET_ICON_SLOT + (VPET_ICON_SLOT - 24) / 2,
                            VPET_ICON_BOT_Y + (VPET_ICON_AREA_H - 24) / 2, 24, 24);
        }
    }

    /* 整屏送入 ST7305 fb: 位=1 → 黑 */
    st7305_blit_1bit(s_lcd, 0, 0, ST7305_WIDTH, ST7305_HEIGHT, s_frame);
    /* 屏幕虚拟按键 (底部分栏按键) 叠加 */
    virtual_keys_draw(s_lcd);
    st7305_flush(s_lcd);
}

/* ============ 按键 ============ */
static void vpet_k0_pin(int pin, bool pressed) {
    if (pressed) pin_set("K0", pin, 0);      /* 低电平 = 按下 */
    else         pin_release("K0", pin);
}

static void vpet_apply_joypad(uint8_t jp) {
    vpet_k0_pin(2, !(jp & 0x01));   /* GB A     → K0.2 (btnLeft) */
    vpet_k0_pin(1, !(jp & 0x02));   /* GB B     → K0.1 (btnCenter) */
    vpet_k0_pin(0, !(jp & 0x08));   /* GB Start → K0.0 (btnRight) */
}

/* ============ 音效 (CPU sound 状态机 → tone_player) ============ */
static void vpet_tone_handler(void *ctx, int freq, int enable) {
    (void)ctx;
    if (enable) tone_tone_on(freq);
    else        tone_stop();
}

/* ============ 模拟任务 ============ */
static void vpet_emu_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "vpet emu task started (rom_words=%d)", s_rom_words);

    esp_task_wdt_add(NULL);

    /* V1.0.9x: 墙钟节流 — 固定每个 20ms 迭代仿真固定周期数, 再睡满余量.
     * 旧实现贪心把批次撑到 CPU 满载并忙轮转, 导致模拟远快于实机(蛋跑太快).
     * 目标节奏对齐参考: clockHz=1.6MHz, TARGET_FRACTION=0.8 → 1.28M cycles/s.
     * 每 20ms 仿真 = 1.28M * 0.02 = 25600 cycles, 再睡到满 20ms, 严格保真. */
    const uint32_t CLOCK_INTERVAL_US = 20000;
    const uint32_t CYCLES_PER_TICK    = 25600;   /* = 1.6MHz * 0.8 * 20ms */
    uint32_t s_last_render_ms = 0;

    vpet_apply_joypad(s_joypad);

    while (1) {
        if (s_stop_requested) break;

        if (s_paused) {
            s_pause_ack = true;
            vTaskDelay(pdMS_TO_TICKS(16));
            continue;
        }

        vpet_apply_joypad(s_joypad);

        const int64_t tick_start = esp_timer_get_time();
        clockBatch(CYCLES_PER_TICK);

        /* 渲染 (~16 FPS, 对齐屏幕刷新节奏) */
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if ((uint32_t)(now_ms - s_last_render_ms) >= 62) {
            s_last_render_ms = now_ms;
            vpet_render();
        }

        /* 睡满本迭代 ~20ms 真实时间, 保证 1.28MHz 节奏并喂看门狗 */
        while ((int32_t)(esp_timer_get_time() - tick_start) < (int32_t)CLOCK_INTERVAL_US) {
            esp_task_wdt_reset();
            if (s_stop_requested) break;
            if (s_paused) { s_pause_ack = true; break; }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    ESP_LOGW(TAG, "vpet emu task exiting");
    tone_stop();
    vpet_k0_pin(2, false); vpet_k0_pin(1, false); vpet_k0_pin(0, false);
    esp_task_wdt_delete(NULL);
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ============ 公共接口 ============ */

esp_err_t vpet_emu_background_init(st7305_handle_t *lcd) {
    if (lcd) s_lcd = lcd;
    tone_player_init();
    if (!s_rom) {
        s_rom = heap_caps_malloc(VPET_ROM_MAX_BYTES / 2 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!s_rom) {
            ESP_LOGE(TAG, "PSRAM ROM 缓冲分配失败");
            return ESP_ERR_NO_MEM;
        }
    }
    s_ready = true;
    ESP_LOGI(TAG, "vpet emu ready (ROM buf %d words, LCD %dx%d)",
             (int)(VPET_ROM_MAX_BYTES / 2), VPET_DISP_W, VPET_DISP_H);
    return ESP_OK;
}

esp_err_t vpet_emu_start(const char *path) {
    if (!path) return ESP_ERR_INVALID_ARG;
    if (!s_ready) {
        esp_err_t r = vpet_emu_background_init(NULL);
        if (r != ESP_OK) return r;
    }
    if (!s_rom) return ESP_ERR_NO_MEM;

    /* 读取 ROM: 小端字节流 → 大端 16 位指令字 (rom.js 语义) */
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "ROM 打开失败: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > VPET_ROM_MAX_BYTES || (len & 1)) {
        ESP_LOGE(TAG, "ROM 尺寸非法: %ld bytes", len);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *raw = malloc((size_t)len);
    if (!raw) { fclose(f); return ESP_ERR_NO_MEM; }
    if (fread(raw, 1, (size_t)len, f) != (size_t)len) {
        ESP_LOGE(TAG, "ROM 读取失败: %s", path);
        free(raw); fclose(f);
        return ESP_FAIL;
    }
    fclose(f);

    const int words = (int)(len / 2);
    for (int i = 0; i < words; i++) {
        s_rom[i] = (uint16_t)((raw[i * 2] << 8) | raw[i * 2 + 1]);
    }
    free(raw);
    s_rom_words = words;

    /* V1.0.9x: 按 ROM 尺寸决定是否旋转 180°(不依赖文件名) */
    s_rotate = (words == 8192);   /* 16KB = 数码宝贝; 12KB = Tamagotchi P1/P2 */

    initCPU(s_rom, s_rom_words, 1600000, vpet_tone_handler, NULL);   /* clockHz=1.6MHz */

    s_stop_requested = false;
    s_paused = false;
    s_joypad = 0xFF;
    vpet_apply_joypad(s_joypad);

    /* 切换到暴龙机专属3键布局 (左A 右上B 右下C) */
    virtual_keys_set_layout(VK_LAYOUT_VPET);
    virtual_keys_set_enabled(true);

    if (!s_task) {
        BaseType_t r = xTaskCreatePinnedToCore(vpet_emu_task, "vpet_emu",
                                               8192, NULL, 3, &s_task, 1);
        if (r != pdPASS) {
            s_task = NULL;
            ESP_LOGE(TAG, "无法创建模拟任务");
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "暴龙机启动: %s (%d words)", path, words);
    return ESP_OK;
}

void vpet_emu_pause(void) {
    s_pause_ack = false;
    s_paused = true;
    for (uint16_t i = 0; i < 100 && s_task && !s_pause_ack; i++)
        vTaskDelay(pdMS_TO_TICKS(2));
    tone_stop();
}

void vpet_emu_resume(void) {
    s_paused = false;
    s_pause_ack = false;
}

void vpet_emu_set_joypad(uint8_t joypad) {
    s_joypad = joypad;
}

esp_err_t vpet_emu_stop(void) {
    if (!s_task) return ESP_OK;
    virtual_keys_set_enabled(false);
    virtual_keys_set_layout(VK_LAYOUT_STANDARD);
    s_stop_requested = true;
    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(2));
    s_stop_requested = false;
    return ESP_OK;
}

void vpet_emu_unload(void) {
    virtual_keys_set_enabled(false);
    virtual_keys_set_layout(VK_LAYOUT_STANDARD);
    if (s_task) {
        s_stop_requested = true;
        for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(2));
    }
    s_stop_requested = false;
    s_paused = false;

    if (s_rom) { heap_caps_free(s_rom); s_rom = NULL; }
    s_rom_words = 0;
    s_ready = false;
    ESP_LOGI(TAG, "vpet emu unloaded (memory released)");
}

/* V1.0.9x: 暴龙机独立显示设置 (不吃共享灰度):
 *  mode: 0=点对点, 1=放大(等比拉到最宽/置顶), 2=拉伸(强制全屏);
 *  aa:   EPX 抗锯齿. 供菜单在启动暴龙机前设置. */
void vpet_emu_set_display(int mode, bool aa) {
    s_disp_mode = (mode >= VPET_DISP_FIT && mode <= VPET_DISP_STRETCH) ? mode : VPET_DISP_FIT;
    s_aa = aa;
}
