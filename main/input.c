/**
 * @file input.c
 * @brief 按键输入处理 - 非阻塞状态机: 短按/长按
 */
#include "input.h"
#include "user_config.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "bt_manager.h"

#define TAG "INPUT"
#define DEBOUNCE_MS 30
#define LONG_PRESS_MS 500

static uint32_t now_ms(void) {
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

typedef enum { ST_IDLE, ST_PRESSED, ST_LONG_FIRED } btn_state_t;
typedef struct {
    gpio_num_t gpio;
    btn_state_t state;
    uint32_t press_start;
    menu_action_t short_action, long_action;
    const char *name;
} btn_ctx_t;

static btn_ctx_t s_btns[2];

/* V1.0.41: 手柄导航键开关. 按键映射期间设为 false, 防止手柄按键产生 action
 * 干扰映射流程 (如映射"返回"键时 MENU_ACTION_BACK 会终止映射). */
static bool s_gamepad_nav_enabled = true;
void input_set_gamepad_nav_enabled(bool enabled) {
    s_gamepad_nav_enabled = enabled;
}

void input_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BTN_GPIO_LEFT) | (1ULL << BTN_GPIO_RIGHT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t _ret = gpio_config(&io_conf);
    ESP_LOGI(TAG, "GPIO ret=%d K%d B%d", _ret,
             gpio_get_level(GPIO_NUM_18), gpio_get_level(GPIO_NUM_0));
    s_btns[0] = (btn_ctx_t){ BTN_GPIO_RIGHT, ST_IDLE, 0, MENU_ACTION_RIGHT, MENU_ACTION_BACK, "BOOT" };
    /* KEY 物理键 (LEFT_GPIO):
     *   短按 = 确认 (CONFIRM)
     *   长按 = 自动搜索蓝牙设备 (LONG_LEFT) -- V1.0.39: 全局快捷键 */
    s_btns[1] = (btn_ctx_t){ BTN_GPIO_LEFT,  ST_IDLE, 0, MENU_ACTION_CONFIRM, MENU_ACTION_LONG_LEFT, "KEY" };
}

static menu_action_t tick(btn_ctx_t *b) {
    bool pressed = (gpio_get_level(b->gpio) == 0);
    uint32_t now = now_ms();
    switch (b->state) {
        case ST_IDLE:
            if (pressed) { b->state = ST_PRESSED; b->press_start = now; }
            break;
        case ST_PRESSED:
            if (pressed) {
                if (now - b->press_start > LONG_PRESS_MS) {
                    /* V1.0.40: 长按发射后进入 ST_LONG_FIRED, 等按键释放才回 IDLE,
                     * 避免释放时再触发短按 (旧 bug: 长按后释放误触发 short_action) */
                    b->state = ST_LONG_FIRED;
                    return b->long_action;
                }
            } else {
                uint32_t dur = now - b->press_start;
                if (dur < DEBOUNCE_MS) { b->state = ST_IDLE; break; }
                b->state = ST_IDLE;
                return b->short_action;
            }
            break;
        case ST_LONG_FIRED:
            /* 长按已发射, 等待按键释放, 期间不产生任何动作 */
            if (!pressed) b->state = ST_IDLE;
            break;
    }
    return MENU_ACTION_NONE;
}

menu_action_t input_get_action(void) {
    for (int i = 0; i < 2; i++) {
        menu_action_t a = tick(&s_btns[i]);
        if (a != MENU_ACTION_NONE) return a;
    }
    /* 手柄导航: 每键独立防抖边沿 + 带hold-cap的长按重复 + 卡键守卫.
     * 历史 bug:
     *  - 旧 250ms 全局冷却 -> "连按很慢"; 按下瞬间单帧错码在冷却前先误触发 -> "按下变上".
     *  - 长按自动重复会把"卡住的键"(键码残留/释放报告丢失)放大成"菜单一直往上滚"(关手柄才停).
     * 现方案:
     *  1) 每键独立防抖(NAV_DEBOUNCE_MS): 单帧错码被过滤, 修复"按下变上"; 连按灵敏(无冷却).
     *  2) released_seen 守卫: 连接伊始就"按下"(从未松开过)的键视为卡住/坏映射, 直接忽略,
     *     彻底防住"一直往上". 正常键(空闲过)照常响应.
     *  3) hold-cap 重复: 方向键按住 NAV_REPEAT_DELAY 后每 NAV_REPEAT_RATE 重复, 但单次按住
     *     超过 NAV_HOLD_CAP_MS 即停止(释放丢失也不会无限滚, 需松开重按). A/B/HOME 仅边沿. */
    if (s_gamepad_nav_enabled && bt_manager_is_connected()) {
        /* V1.0.39: 摇杆强制 4 方向键, 纯边沿触发, 移动一次只触发一次.
         * 回正 = 松手; 无论压力多少, 只要方向保持就算按住状态(不重复). */
        static const struct { func_t key; menu_action_t act; bool rpt; } nav[] = {
            { F_UP,    MENU_ACTION_UP,     false },
            { F_DOWN,  MENU_ACTION_DOWN,   false },
            { F_LEFT,  MENU_ACTION_LEFT,   false },
            { F_RIGHT, MENU_ACTION_RIGHT,  false },
            { F_CONFIRM, MENU_ACTION_CONFIRM, false },
            { F_BACK,  MENU_ACTION_BACK,    false },
            { F_EXIT,  MENU_ACTION_HOME,    false },
        };
#define NAV_DEBOUNCE_MS    30    /* 防抖: 连续稳定 30ms 才算按下/松开, 过滤单帧错码 */
#define NAV_REPEAT_DELAY   450   /* 首次按下后 450ms 开始自动重复 */
#define NAV_REPEAT_RATE    110   /* 重复速率: 每 110ms 一次 */
#define NAV_HOLD_CAP_MS    1500  /* 单次按住最长重复 1.5s, 超时停止(防释放丢失导致一直滚) */
#define NAV_LONG_LEFT_MS   500   /* V1.0.39: LEFT 长按 500ms 发射一次 MENU_ACTION_LONG_LEFT (手柄配置快捷键) */
        typedef struct {
            bool prev_raw;          /* 上一帧原始电平(变化时重置防抖计时) */
            bool debounced;         /* 防抖后的按下态 */
            bool released_seen;     /* 是否见过该键松开(松开过才算正常键, 否则视为卡住) */
            bool long_emitted;      /* V1.0.39: 长按事件是否已发射 (防止 hold 期间重复发射) */
            uint32_t raw_change_ms; /* 原始电平最后变化的时刻 */
            uint32_t press_start_ms;/* 本次按下时刻(hold-cap 计时) */
            uint32_t repeat_next_ms;/* 下一次自动重复时刻 */
        } nav_key_state_t;
        static nav_key_state_t st[sizeof(nav) / sizeof(nav[0])] = {{0}};
        uint32_t now = now_ms();
        for (size_t i = 0; i < sizeof(nav) / sizeof(nav[0]); i++) {
            bool raw = bt_manager_is_key_pressed(nav[i].key);
            if (!raw) st[i].released_seen = true;   /* 见过松开 -> 标记为正常键 */
            /* 原始电平变化 -> 记录时刻; 防抖期内保持旧防抖态, 过滤短抖动 */
            if (raw != st[i].prev_raw) {
                st[i].prev_raw = raw;
                st[i].raw_change_ms = now;
            }
            bool new_db;
            if ((int32_t)(now - st[i].raw_change_ms) >= NAV_DEBOUNCE_MS)
                new_db = raw;               /* 已稳定 -> 跟随原始电平 */
            else
                new_db = st[i].debounced;   /* 防抖期内 -> 保持 */
            /* 上升沿: 首次按下触发一次, 并排定重复 */
            if (new_db && !st[i].debounced) {
                st[i].debounced = true;
                st[i].press_start_ms = now;
                st[i].repeat_next_ms = now + NAV_REPEAT_DELAY;
                st[i].long_emitted = false;  /* V1.0.39: 新一次按下, 长按事件可重新发射 */
                if (st[i].released_seen) {
                    ESP_LOGI(TAG, "nav按下 key=%d act=%d", (int)nav[i].key, (int)nav[i].act);
                    return nav[i].act;
                }
                /* 从未松开过就"按下" = 连接伊始即卡住(坏映射/键码残留), 忽略, 防止菜单 runaway */
                ESP_LOGW(TAG, "nav键%d 未见过松开即按下, 疑似卡住, 忽略", (int)nav[i].key);
            } else if (nav[i].rpt && new_db && st[i].debounced && st[i].released_seen) {
                /* V1.0.39: LEFT 长按 500ms 发射 MENU_ACTION_LONG_LEFT (仅 1 次, 长按期间不再发射).
                 * 优先级最高, 在长按重复之前处理, 避免被正常 repeat 抢走动作. */
                if (nav[i].key == F_LEFT && !st[i].long_emitted
                    && (int32_t)(now - st[i].press_start_ms) >= NAV_LONG_LEFT_MS) {
                    st[i].long_emitted = true;
                    ESP_LOGI(TAG, "nav长按LEFT %dms 发射 LONG_LEFT", (int)(now - st[i].press_start_ms));
                    return MENU_ACTION_LONG_LEFT;
                }
                /* 长按自动重复(仅方向键), 带 hold-cap: 超时停止, 防释放丢失导致一直滚 */
                if ((int32_t)(now - st[i].press_start_ms) < NAV_HOLD_CAP_MS
                    && (int32_t)(now - st[i].repeat_next_ms) >= 0) {
                    st[i].repeat_next_ms = now + NAV_REPEAT_RATE;
                    return nav[i].act;
                }
            }
            /* 下降沿: 复位, 准备下一次按下 */
            if (!new_db && st[i].debounced) {
                st[i].debounced = false;
                st[i].long_emitted = false;
            }
        }
#undef NAV_DEBOUNCE_MS
#undef NAV_REPEAT_DELAY
#undef NAV_REPEAT_RATE
#undef NAV_HOLD_CAP_MS
#undef NAV_LONG_LEFT_MS
    }
    return MENU_ACTION_NONE;
}

bool input_is_held(int idx) {
    if (idx < 0 || idx > 1) return false;
    return (gpio_get_level(s_btns[idx].gpio) == 0);
}

/* GB/GBC joypad 掩码 (低电平有效):
 *   bit0=A  bit1=B  bit2=Select  bit3=Start
 *   bit4=右 bit5=左 bit6=上      bit7=下
 * 游戏操作只用原生 上下左右 + 确定(A) + 返回(B);
 * Select/Start 只由 GB 辅助映射(额外映射)提供, 不占"退出到菜单(F_EXIT)"和"多功能(F_FAV)",
 * 这样"返回菜单"是独立特殊键, 不参与游戏操作, Start 也能正常当游戏键用.
 * 设备物理键: KEY(GPIO18)=A, BOOT(GPIO0)=B, 让 GB/GBC 无手柄也可玩. */
uint8_t input_get_held_gb_joypad(void) {
    uint8_t j = 0xFF;
    if (bt_manager_is_connected()) {
        if (bt_manager_is_key_pressed(F_UP))     j &= ~(1 << 6);
        if (bt_manager_is_key_pressed(F_DOWN))   j &= ~(1 << 7);
        if (bt_manager_is_key_pressed(F_LEFT))   j &= ~(1 << 5);
        if (bt_manager_is_key_pressed(F_RIGHT))  j &= ~(1 << 4);
        if (bt_manager_is_key_pressed(F_CONFIRM)) j &= ~(1 << 0); /* A */
        if (bt_manager_is_key_pressed(F_BACK))    j &= ~(1 << 1); /* B */
        /* Select/Start 优先用 GB 辅助映射; 若用户从未映射辅助键, 直接使用
         * 手柄物理 Select/Start (Q36 d[6] bit2/bit3), 保证 NES/GB 开箱可用
         * (例如超级玛丽标题画面必须按 Start 才能开始游戏). */
        bool gb_sel = bt_manager_is_gb_pressed(GB_SELECT);
        bool gb_start = bt_manager_is_gb_pressed(GB_START);
        if (!gb_sel && bt_manager_get_gb_map(GB_SELECT) >= PHYS_MAX)
            gb_sel = bt_manager_is_phys_pressed(P_SELECT);
        if (!gb_start && bt_manager_get_gb_map(GB_START) >= PHYS_MAX)
            gb_start = bt_manager_is_phys_pressed(P_START);
        if (gb_sel)   j &= ~(1 << 2); /* Select */
        if (gb_start) j &= ~(1 << 3); /* Start */
    }
    /* 设备物理键兼容: KEY(GPIO18)=A, BOOT(GPIO0)=B */
    if (input_is_held(0)) j &= ~(1 << 0);  /* KEY → A */
    if (input_is_held(1)) j &= ~(1 << 1);  /* BOOT → B */
    return j;
}
