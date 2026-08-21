/**
 * @file gb_emu.c
 * @brief GB 引擎兼容层 — 转发到 esp-box-emu gnuboy 统一核心 (components/gbc_emu)
 *
 * 原 peanut-gb 核心已移除 (V1.0.53), 本文件只保留 gb_emu.h 公共接口,
 * 所有实现委托给 gbc_emu (esp-box-emu gnuboy). 菜单代码零改动.
 * ROM 缓冲由菜单 (gb_emu_free_rom) 负责释放, 因此 gb_emu_stop 会等待
 * 模拟任务完全退出后再返回, 避免任务仍引用 ROM 时被释放.
 */
#include "gb_emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "gbc_emu.h"

static const char *TAG = "gb_emu";
static gb_emu_progress_cb_t s_progress_cb = NULL;

void gb_emu_set_progress_cb(gb_emu_progress_cb_t cb)
{
    s_progress_cb = cb;
}

/* 设置电池存档目录 (转发 gbc_emu): 菜单用它区分 GB(/sdcard/dict/GB) 与 GBC(/sdcard/dict/GBC).
 * 必须在 gb_emu_load_rom 之前调用. */
void gb_emu_set_save_dir(const char *dir)
{
    gbc_emu_set_save_dir(dir);
}

esp_err_t gb_emu_load_rom(const char *path, gb_emu_rom_t *rom)
{
    if (!path || !rom) return ESP_ERR_INVALID_ARG;
    memset(rom, 0, sizeof(*rom));

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "打开失败: %s", path);
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0x150 || sz > 8 * 1024 * 1024) {
        ESP_LOGE(TAG, "ROM 大小异常: %ld", sz);
        fclose(f);
        return ESP_FAIL;
    }

    uint8_t *data = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        data = malloc((size_t)sz);
    }
    if (!data) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t rd = fread(data, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        heap_caps_free(data);
        ESP_LOGE(TAG, "读取不完整: %u/%ld", (unsigned)rd, sz);
        return ESP_FAIL;
    }

    rom->data = data;
    rom->size = (size_t)sz;

    /* 电池存档路径 (GB 兼容层: 菜单只把 path 传给 load_rom, 在此挂钩) */
    gbc_emu_set_save_path(path);

    /* 解析头部信息 (供日志/调试) */
    memcpy(rom->header.title, data + 0x134, 16);
    rom->header.title[16] = '\0';
    rom->header.cartridge_type = data[0x147];
    rom->header.rom_size_code   = data[0x148];
    rom->header.ram_size_code   = data[0x149];
    rom->header.cgb_flag        = data[0x143];
    rom->header.sgb_flag        = data[0x146];
    rom->header.destination_code = data[0x14A];
    rom->header.mask_rom_version = data[0x14C];
    rom->header.header_checksum  = data[0x14D];
    rom->header.expected_rom_size = (size_t)(32 * 1024) << rom->header.rom_size_code;
    uint8_t sum = 0;
    for (int i = 0x134; i <= 0x14C; i++) sum = (uint8_t)(sum - data[i] - 1);
    rom->header.computed_header_checksum = sum;
    rom->header.header_checksum_ok = (sum == data[0x14D]);

    if (s_progress_cb) s_progress_cb(100);
    return ESP_OK;
}

void gb_emu_free_rom(gb_emu_rom_t *rom)
{
    if (!rom) return;
    if (rom->data) {
        heap_caps_free(rom->data);
        rom->data = NULL;
    }
    rom->size = 0;
}

void gb_emu_log_rom_info(const gb_emu_rom_t *rom)
{
    if (!rom) return;
    ESP_LOGI(TAG, "GB ROM: \"%s\" type=0x%02X rom_size_code=0x%02X (%uKB) ram=0x%02X "
             "cgb=0x%02X sgb=0x%02X checksum=%s",
             rom->header.title, rom->header.cartridge_type, rom->header.rom_size_code,
             (unsigned)(rom->header.expected_rom_size / 1024), rom->header.ram_size_code,
             rom->header.cgb_flag, rom->header.sgb_flag,
             rom->header.header_checksum_ok ? "ok" : "BAD");
}

esp_err_t gb_emu_background_init(void)
{
    return gbc_emu_background_init();
}

void gb_emu_unload(void)
{
    gbc_emu_unload();
}

esp_err_t gb_emu_start(const gb_emu_rom_t *rom)
{
    if (!rom || !rom->data) return ESP_ERR_INVALID_ARG;
    return gbc_emu_start_data(rom->data, rom->size, false);   /* owned=false: 菜单负责释放 */
}

esp_err_t gb_emu_stop(void)
{
    esp_err_t r = gbc_emu_stop();
    gbc_emu_wait_stopped();   /* 确保任务退出后再让菜单释放 ROM 缓冲 */
    return r;
}

void gb_emu_set_joypad(uint8_t joypad)
{
    gbc_emu_set_joypad(joypad);
}

void gb_emu_set_volume(uint8_t volume)
{
    gbc_emu_set_volume(volume);
}

uint8_t gb_emu_get_volume(void)
{
    return gbc_emu_get_volume();
}

void gb_emu_pause(void)
{
    gbc_emu_pause();
}

void gb_emu_resume(void)
{
    gbc_emu_resume();
}

void gb_emu_set_fullscreen(int mode)
{
    gbc_emu_set_fullscreen(mode);
}
