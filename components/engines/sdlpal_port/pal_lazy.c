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
static int   s_n;

void *pal_lazy_get(void **slot, size_t size)
{
    if (*slot) return *slot;
    void *p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(1, size, MALLOC_CAP_8BIT);      /* 兜底内部 RAM */
    if (!p) { ESP_LOGE(TAG, "分配失败 %u B (仙剑的该功能会异常)", (unsigned)size); return NULL; }
    *slot = p;
    for (int i = 0; i < s_n; i++) if (s_slots[i] == slot) return p;
    if (s_n < MAX_SLOTS) s_slots[s_n++] = slot;
    else ESP_LOGW(TAG, "slot 登记表满, 退出时可能漏释放");
    return p;
}

void pal_lazy_release_all(void)
{
    for (int i = 0; i < s_n; i++) {
        void **slot = (void **)s_slots[i];
        if (slot && *slot) { heap_caps_free(*slot); *slot = NULL; }
    }
    s_n = 0;
}
