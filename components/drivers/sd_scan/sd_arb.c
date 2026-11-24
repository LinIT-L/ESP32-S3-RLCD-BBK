/**
 * @file sd_arb.c
 * @brief TF/SD 卡访问仲裁实现 —— 见 sd_arb.h 的设计说明。
 *
 * 实现刻意做得极小:
 *   · 一把 portMUX 临界区保护 (state, inflight), 不建互斥量 ⇒ 没有生命周期问题,
 *     panic / 关机流程里也能安全读写;
 *   · inflight 是"在途访问计数", 交卡时 `sd_arb_wait_idle()` 等它归零;
 *   · 不做句柄枚举/强制关闭 —— 关别人的 FILE* 会导致"重复 fclose"这种更凶的 UAF,
 *     句柄生命周期由各调用方自己用 enter/leave 括起来。
 */
#include "sd_arb.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile sd_arb_state_t s_state = SD_ARB_OFF;
static volatile int s_inflight = 0;

void sd_arb_init(void)
{
    portENTER_CRITICAL(&s_mux);
    s_state = SD_ARB_OFF;
    s_inflight = 0;
    portEXIT_CRITICAL(&s_mux);
}

sd_arb_state_t sd_arb_state(void) { return s_state; }

const char *sd_arb_state_str(void)
{
    switch (s_state) {
        case SD_ARB_LOCAL:   return "LOCAL(本机)";
        case SD_ARB_HANDOFF: return "HANDOFF(交卡中)";
        case SD_ARB_USB:     return "USB(电脑接管)";
        case SD_ARB_RESTORE: return "RESTORE(收卡中)";
        case SD_ARB_OFF:
        default:             return "OFF(未挂载)";
    }
}

bool sd_arb_available(void) { return s_state == SD_ARB_LOCAL; }

bool sd_arb_enter(void)
{
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (s_state == SD_ARB_LOCAL) {
        s_inflight++;
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

void sd_arb_leave(void)
{
    portENTER_CRITICAL(&s_mux);
    if (s_inflight > 0) s_inflight--;
    portEXIT_CRITICAL(&s_mux);
}

int sd_arb_inflight(void) { return s_inflight; }

bool sd_arb_wait_idle(uint32_t timeout_ms)
{
    TickType_t t0 = xTaskGetTickCount();
    for (;;) {
        if (s_inflight == 0) return true;
        if (timeout_ms == 0) return false;
        if ((uint32_t)(xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(timeout_ms)) return false;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void sd_arb_set_state(sd_arb_state_t st)
{
    portENTER_CRITICAL(&s_mux);
    s_state = st;
    portEXIT_CRITICAL(&s_mux);
}
