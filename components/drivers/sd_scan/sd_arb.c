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

#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sd_arb";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile sd_arb_state_t s_state = SD_ARB_OFF;
static volatile int s_inflight = 0;

/* ═══════════════════════════════════════════════════════════════════════════
 *  浅睡保护（2026-11-25）
 *
 * 挂一把 ESP_PM_NO_LIGHT_SLEEP, 在 sd_arb_enter() ~ sd_arb_leave() 之间一直压着。
 *
 * 为什么需要 —— IDF 6.0.3 的实际行为（已核对源码, 不要凭旧版本经验改）:
 *   esp_driver_sdmmc/src/sd_trans_sdmmc.c:488/588 确实在**每次事务**前后
 *   拿了 ESP_PM_APB_FREQ_MAX（sd_host_sdmmc.c:105 创建, 名字 "sdmmc"）。
 *   而 esp_pm/pm_impl.c 的 get_lowest_allowed_mode() 一旦看到 BIT(PM_MODE_APB_MAX)
 *   就直接返回 PM_MODE_APB_MAX ⇒ **单次事务期间不会进浅睡**。
 *   也就是说"事务中途睡着"这条不成立, 上一轮审计的这一条结论需要更正。
 *
 *   剩下的真实空档在**事务之间**: 一次逻辑访问（遍历目录、连续读一个文件）
 *   由很多次事务组成, 每次事务结束就释放锁、回到 LIGHT_SLEEP。
 *   空闲 8ms(CONFIG_FREERTOS_HZ=1000 + IDLE_TIME_BEFORE_SLEEP=8) 就能睡进去,
 *   醒来要重配 SDMMC 时钟、CONFIG_PM_SLP_SPIRAM_HALFSLEEP_ENABLED 还会让
 *   PSRAM 走半睡再等 150us —— 对搬运缓冲(在 PSRAM)是实打实的额外风险。
 *
 * 所以这里不重造事务级保护, 只把**整段逻辑访问**盖住:
 *   enter 时 acquire / leave 时 release, 与 inflight 严格 1:1。
 *   esp_pm_lock 内部是计数(count), 嵌套 enter/leave 天然正确(pm_locks.c:108/138)。
 *
 * 边界:
 *   · 不是"永久关浅睡": 没有在途访问时锁计数为 0, 该睡照睡。
 *   · 不改变频率: NO_LIGHT_SLEEP 只映射到 PM_MODE_APB_MIN, 不会把 CPU 顶到 240MHz。
 *   · 只能在任务上下文创建/获取 —— 我们的接口本来也只在任务里用。
 * ═══════════════════════════════════════════════════════════════════════════ */
static esp_pm_lock_handle_t s_pm_lock = NULL;
static volatile int s_pm_state = 0;   /* 0=未创建 2=可用 3=不可用(只报一次); 单调, 不回退 */

/* 建锁, 只建一次。
 *
 * 调用点选在 sd_arb_set_state(SD_ARB_LOCAL) —— 也就是"放开闸门"的那一刻, 不是 enter 里。
 * 理由: 状态还是 OFF/RESTORE 时任何人都进不来(sd_arb_enter 要求 LOCAL), 此刻建锁
 *      既没有并发、也不会出现"某次 enter 时锁还不存在、它的 leave 却把别人的计数减掉"
 *      这种配额失衡。放在 enter 里惰性创建则存在那个窗口(另一个任务正在建锁)。
 * ⛔ 不能放进临界区: esp_pm_lock_create 会 calloc + 取 _lock, 不是 ISR 安全的。 */
static void pm_lock_ensure(void)
{
    if (s_pm_state != 0) return;

    esp_pm_lock_handle_t h = NULL;
    esp_err_t err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "sd_arb", &h);
    if (err == ESP_OK && h) {
        s_pm_lock  = h;
        s_pm_state = 2;
        ESP_LOGI(TAG, "浅睡保护就绪: 本机访问 SD 期间禁止自动浅睡");
    } else {
        /* 没开 CONFIG_PM_ENABLE / 内存不足: 不致命, 退回只依赖驱动的事务级锁。 */
        s_pm_state = 3;
        ESP_LOGW(TAG, "浅睡保护不可用(%s) —— 仅依赖 sdmmc 驱动的事务级锁",
                 esp_err_to_name(err));
    }
}

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

    /* ⛔ 必须在临界区**之后**拿 PM 锁: esp_pm_lock_acquire 内部会
     *    portENTER_CRITICAL_SAFE(&handle->spinlock), 在同一个临界区里嵌套会撞锁。
     *    这里"先放行、再拿锁"的空档是安全的: 此刻调用方还没开始任何 SD 事务,
     *    就算真睡 8ms 也不存在"事务中途断时钟"。 */
    if (ok) {
        /* 锁已在 set_state(LOCAL) 建好(或已判定不可用), 这里只剩计数 → 严格 1:1。 */
        if (s_pm_lock) esp_pm_lock_acquire(s_pm_lock);   /* 计数式, 与 leave 1:1 */
    }
    return ok;
}

void sd_arb_leave(void)
{
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (s_inflight > 0) {
        s_inflight--;
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);

    /* 只有真的减了一次计数才释放 —— 未 enter 成功却调 leave 是调用方的 bug,
     * 不能让它把别人的配额一起减掉。锁的可用性在 set_state(LOCAL) 就定了, 不会中途出现,
     * 所以"enter 没 acquire、leave 却 release"这种失衡不存在。 */
    if (ok && s_pm_lock) esp_pm_lock_release(s_pm_lock);
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
    /* ★ 2026-11-25: 在放开闸门之前把浅睡保护准备好。
     *   此刻 s_state 还不是 LOCAL, sd_arb_enter() 全部会失败 ⇒ 没有并发,
     *   也不会出现"enter 时锁不存在 / leave 时却存在"的配额失衡。 */
    if (st == SD_ARB_LOCAL) pm_lock_ensure();

    portENTER_CRITICAL(&s_mux);
    s_state = st;
    portEXIT_CRITICAL(&s_mux);
}
