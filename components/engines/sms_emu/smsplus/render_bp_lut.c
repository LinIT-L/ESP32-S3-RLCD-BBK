/**
 * render_bp_lut.c — SMS bitplane-to-pixel 查找表.
 *
 * 原表 256KB const uint32 bp_lut[0x10000] 已从固件 factory 迁出,
 * 改为二进制 bp_lut.bin 存于 appdata 分区固定槽位(见 merge_flash.sh),
 * 运行时 esp_partition_mmap 直接映射为只读指针 (数据仍留在 Flash,
 * 经片内 flash 缓存访问, 速度与 .rodata 一致, 不占内部 RAM).
 */
#include "esp_partition.h"
#include "esp_log.h"
#include <stdint.h>
#include <stddef.h>

typedef uint32_t uint32;

#define TAG "BPLUT"

#define BP_LUT_OFF  0x540000UL   /* appdata 分区内偏移(64KB 对齐, 与 merge_flash.sh 一致) */
#define BP_LUT_LEN  0x40000UL    /* 256KB */

/* 指向 appdata 中 mmap 出来的只读表首元素; render.c 以 bp_lut[i] 访问 */
const uint32 *bp_lut = NULL;

int bp_lut_init(void)
{
    if (bp_lut) return 0;

    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "appdata");
    if (!p) {
        ESP_LOGD(TAG, "找不到 appdata 分区");
        return -1;
    }

    const void *addr = NULL;
    static esp_partition_mmap_handle_t handle;
    esp_err_t e = esp_partition_mmap(p, BP_LUT_OFF, BP_LUT_LEN, ESP_PARTITION_MMAP_DATA,
                                     &addr, &handle);
    if (e != ESP_OK) {
        ESP_LOGD(TAG, "esp_partition_mmap 失败: %s (off=0x%x len=0x%x)",
                 esp_err_to_name(e), BP_LUT_OFF, BP_LUT_LEN);
        return -1;
    }

    bp_lut = (const uint32 *)addr;
    ESP_LOGI(TAG, "bp_lut mmap ok @%p (off=0x%x len=0x%x)", addr, BP_LUT_OFF, BP_LUT_LEN);
    return 0;
}
