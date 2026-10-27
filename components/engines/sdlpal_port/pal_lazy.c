/* ★ 2026-10-25 linit: 仙剑的大缓冲改成"跑仙剑时才分配"。
 *   原来 PAL_LARGE(=EXT_RAM_BSS_ATTR static) 让 ending/uigame/text/battle/resampler 的
 *   缓冲常年占着 PSRAM 约 660KB, 可这些代码只在仙剑里跑 —— 不合"没用的绝不常驻"。
 *   首次调用分配(清零, 优先 PSRAM), 之后复用; 仙剑退出 pal_lazy_release_all() 一次性归还。 */
#include "pal_lazy.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define TAG "PAL_LAZY"
#define MAX_SLOTS 40

static void *s_slots[MAX_SLOTS];
static size_t s_sizes[MAX_SLOTS];      /* ★ 记录每块的实际大小, 供"尺寸变大"时重分配 */
static int   s_n;

void *pal_lazy_get(void **slot, size_t size)
{
    /* ★ 2026-10-27 加固: 同一个 slot 在不同调用点可能请求不同大小
     *   (实测 text.c 的 palette 要 1024B 却只给 256B, uigame.c 的 bufImage 要 64000B 却先给了 2048B)
     *   —— 先分配小的、后面大的写进来就是堆越界。这里按"尺寸只增不减"处理。 */
    if (*slot) {
        for (int i = 0; i < s_n; i++) {
            if (s_slots[i] != slot) continue;
            if (s_sizes[i] >= size) return *slot;
            ESP_LOGW(TAG, "slot 需要更大缓冲: %u → %u B, 重新分配",
                     (unsigned)s_sizes[i], (unsigned)size);
            heap_caps_free(*slot);
            *slot = NULL;
            s_sizes[i] = 0;
            break;
        }
        if (*slot) return *slot;
    }
    void *p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(1, size, MALLOC_CAP_8BIT);      /* 兜底内部 RAM */
    if (!p) { ESP_LOGE(TAG, "分配失败 %u B (仙剑的该功能会异常)", (unsigned)size); return NULL; }
    *slot = p;
    for (int i = 0; i < s_n; i++) if (s_slots[i] == slot) { s_sizes[i] = size; return p; }
    if (s_n < MAX_SLOTS) { s_slots[s_n] = slot; s_sizes[s_n] = size; s_n++; }
    else ESP_LOGW(TAG, "slot 登记表满, 退出时可能漏释放");
    return p;
}

void pal_lazy_release_all(void)
{
    for (int i = 0; i < s_n; i++) {
        void **slot = (void **)s_slots[i];
        if (slot && *slot) { heap_caps_free(*slot); *slot = NULL; }
        s_sizes[i] = 0;
    }
    s_n = 0;
}
