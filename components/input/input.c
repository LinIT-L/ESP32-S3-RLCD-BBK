/**
 * @file input.c
 * @brief 按键输入处理 - 非阻塞状态机: 短按/长按
 */
#include "input.h"
/* 按键引脚 (Waveshare ESP32-S3-RLCD-4.2): BOOT=GPIO0(右), KEY=GPIO18(左/确认) */
#define BTN_GPIO_LEFT   GPIO_NUM_18
#define BTN_GPIO_RIGHT  GPIO_NUM_0
/* V1.0.68: 软关机键 GPIO1: 短按=确认, 长按0.5s=返回主菜单, 长按2s=软关机 */
#define BTN_GPIO_PWR    GPIO_NUM_1
#define POWER_HOLD_MS   2000
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "bt_manager.h"
#include "touch_panel.h"
#include "web_gamepad.h"
#include "virtual_keys.h"

#define TAG "INPUT"
#define DEBOUNCE_MS 30
#define LONG_PRESS_MS 500

/* ==== 触摸手势识别参数 ====
 * 位移阈值用原始像素(与分辨率无关); 区域判定用屏幕坐标(400x300). */
#define TOUCH_TAP_MAX_MS     300   /* 单击最长时长 */
#define TOUCH_SWIPE_MIN      40    /* 滑动最小位移(原始像素), 低于算点击 */
#define TOUCH_LONG_MS        3000  /* V1.0.68: 状态栏长按 3 秒 = 返回主菜单 */
#define TOUCH_LONG_PRESS_MS  2000  /* V1.0.9x: 游戏名/收藏栏长按 2s = 收藏/取消收藏; 应用管理长按弹窗也走此阈值 */
#define TOUCH_STILL_MAX      20    /* 长按判定时允许的抖动位移 */
#define TOUCH_STATUS_H       24    /* 状态栏高度: 长按此区域(按下点 y<24) = 返回主菜单 */
#define TOUCH_BACK_EDGE_Y    270   /* 底部上滑返回: 按下点屏幕 y >= 此值 */
#define TOUCH_BACK_EDGE_MINX 0     /* 底部上滑返回: 按下点屏幕 x 全宽下限 (V1.0.95: 100->0 全宽屏蔽) */
#define TOUCH_BACK_EDGE_MAXX 400   /* 底部上滑返回: 按下点屏幕 x 全宽上限 (V1.0.95: 300->400) */
#define TOUCH_BACK_SWIPE_DY  50    /* 底部上滑返回: 需往上滑动的屏幕距离 */
#define TOUCH_DOUBLE_SWIPE_MS 500  /* V1.0.9x: 0.5s 内连续两次底部上滑 → HOME 强制退出 */

/* V1.0.69: 本机是否带触摸屏 (运行时探测), 决定物理键语义分支.
 * 有触摸:  BOOT=返回上级/回主菜单, KEY=确认/收藏
 * 无触摸:  BOOT=下一步/返回上级,  KEY=确认/直达蓝牙映射 */
static bool s_has_touch = false;

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

static btn_ctx_t s_btns[3];

/* V1.0.41: 手柄导航键开关. 按键映射期间设为 false, 防止手柄按键产生 action
 * 干扰映射流程 (如映射"返回"键时 MENU_ACTION_BACK 会终止映射). */
static bool s_gamepad_nav_enabled = true;
void input_set_gamepad_nav_enabled(bool enabled) {
    s_gamepad_nav_enabled = enabled;
}

void input_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BTN_GPIO_LEFT) | (1ULL << BTN_GPIO_RIGHT) | (1ULL << BTN_GPIO_PWR),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t _ret = gpio_config(&io_conf);
    ESP_LOGI(TAG, "GPIO ret=%d K%d B%d P%d", _ret,
             gpio_get_level(GPIO_NUM_18), gpio_get_level(GPIO_NUM_0),
             gpio_get_level(GPIO_NUM_1));

    /* 触摸面板 (V1.0.65): 未检测到芯片时 read 恒返回 false, 零开销 */
    touch_panel_init();
    /* V1.0.69: 运行时分清两套机型, 物理键语义按有没有触摸屏分支. */
    s_has_touch = touch_panel_is_present();

    if (s_has_touch) {
        /* 有触摸机型: 导航/确认靠触摸手势, 两颗物理键只是触摸的等价物.
         *   BOOT: 短按=返回上级, 长按=回主菜单
         *   KEY : 短按=确认,     长按=收藏当前项 */
        s_btns[0] = (btn_ctx_t){ BTN_GPIO_RIGHT, ST_IDLE, 0, MENU_ACTION_BACK, MENU_ACTION_HOME, "BOOT" };
        s_btns[1] = (btn_ctx_t){ BTN_GPIO_LEFT,  ST_IDLE, 0, MENU_ACTION_CONFIRM, MENU_ACTION_KEY_FAV, "KEY" };
    } else {
        /* 无触摸机型: 只剩两颗物理键, 要让两键也能闭环操作到蓝牙映射界面.
         *   BOOT: 短按=下一步(右移/选中右移), 长按=返回上级
         *   KEY : 短按=确认(进入),             长按=直达蓝牙映射界面 */
        s_btns[0] = (btn_ctx_t){ BTN_GPIO_RIGHT, ST_IDLE, 0, MENU_ACTION_RIGHT, MENU_ACTION_BACK, "BOOT" };
        s_btns[1] = (btn_ctx_t){ BTN_GPIO_LEFT,  ST_IDLE, 0, MENU_ACTION_CONFIRM, MENU_ACTION_LONG_LEFT, "KEY" };
    }
    /* 开关键 (V1.0.69): 短按=锁屏(POWER_LOCK); 长按不再弹"返回菜单"中间层,
     * 长按 2s 的软关机由 input_power_should_sleep 独立轮询. */
    s_btns[2] = (btn_ctx_t){ BTN_GPIO_PWR, ST_IDLE, 0, MENU_ACTION_POWER_LOCK, MENU_ACTION_NONE, "PWR" };

    /* V1.0.68: 软关机键 GPIO1 同时配置 deep sleep 唤醒 (长按2s软关机后按下唤醒) */
    rtc_gpio_pullup_en(BTN_GPIO_PWR);
    esp_sleep_enable_ext1_wakeup_io(1ULL << BTN_GPIO_PWR, ESP_EXT1_WAKEUP_ANY_LOW);
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
            /* 长按已发射, 等待按键释放, 期间不产生任何动作.
             * V1.0.69: 开关键去掉 0.5s"返回菜单"中间层, 此处不再投递 POWER_RELEASE */
            if (!pressed) {
                b->state = ST_IDLE;
            }
            break;
    }
    return MENU_ACTION_NONE;
}

/* ==== 触摸手势状态机 (V1.0.65) ====
 * 单击 = 确认(CONFIRM); 长按状态栏 = 返回(HOME); 物理屏幕底部中间上滑 = 返回(BACK);
 * 不使用左右/上下滑动触发方向键(打开仅靠点击确定位置).
 * 每个手势只在"释放瞬间"投递一次 action (长按在按住超时瞬间投递),
 * 与物理键/手柄的边沿触发语义一致. */
typedef enum { TG_IDLE, TG_PRESS, TG_WAIT_RELEASE } touch_gest_t;
static touch_gest_t s_tg_state = TG_IDLE;
static int16_t  s_tg_x0, s_tg_y0;      /* 按下起点(原始坐标) */
static int16_t  s_tg_x,  s_tg_y;       /* 最近一次按下位置(原始坐标) */
static int      s_tg_sx0, s_tg_sy0;    /* 按下起点(屏幕坐标) */
static uint32_t s_tg_t0;               /* 按下时刻 */
static bool     s_tg_long_fired;       /* 长按是否已投递 */

/* 最近一次"点击(tap)"的屏幕坐标 (400x300), -1 表示无未消费的点击.
 * main.c 用 input_consume_tap 取走后清零, 用于"点哪进哪"的 hit-test. */
static int s_tap_x = -1, s_tap_y = -1;

/* V1.0.66: 当前触摸的实时屏幕坐标 (供主菜单跟手拖动读取) */
static bool s_touch_down = false;
static int  s_touch_sx = 0, s_touch_sy = 0;

/* V1.0.68: 每 tick 只读一次触摸芯片并缓存, 供 touch_gesture_poll 与
 * input_poll_touch 共用, 避免 GT911 读后清状态被两次读互相偷走事件. */
static tp_point_t s_touch_cached;
static bool       s_touch_cached_valid = false;
static uint32_t   s_touch_cache_tick = 0;
/* V1.0.68 fix: 连续读失败计数 — 触摸芯片挂死/总线卡住时触发自动恢复 */
static uint32_t   s_touch_fail_count = 0;
#define TOUCH_FAIL_RECOVER_N  12   /* 连续 12 次读失败 → 恢复触摸芯片 */

static void touch_jump_filter(tp_point_t *pt, bool pressed);   /* 前向声明 */

static void touch_read_once(tp_point_t *pt) {
    uint32_t tick = xTaskGetTickCount();
    if (s_touch_cached_valid && tick == s_touch_cache_tick) {
        *pt = s_touch_cached;   /* 本 tick 已读过, 直接回放缓存 */
        return;
    }
    s_touch_cached_valid = true;
    s_touch_cache_tick = tick;
    /* V1.0.xx: 无触摸机型自动识别: 未探测到触摸芯片 → 直接视为"未按压"跳过读取,
     * 避免每帧 touch_panel_read 恒返回 false 被误判为连续读失败狂刷日志/请求恢复. */
    if (!touch_panel_is_present()) {
        s_touch_cached.pressed = false;
        s_touch_fail_count = 0;
        *pt = s_touch_cached;
        return;
    }
    if (!touch_panel_read(&s_touch_cached)) {
        /* V1.0.68 fix: 读失败时**保留上一帧状态**(不伪造"松开").
         * 旧代码把失败当成"手指已松开", 飞线 I2C 一个误码就会打断进行中的
         * 手势 → 触摸跳变/误触/只能慢慢拖; 连续失败则是芯片挂死需重启.
         * 现在: 单次失败忽略(坐标暂不更新), 连续失败自动恢复芯片.
         *
         * V1.0.xx: 但如果连续失败 ≥3 次, 强制释放触摸状态, 防止状态残留
         * 导致 s_touch_down 永远为 true → 菜单无法左右滑动 → 需重启恢复. */
        s_touch_fail_count++;
        if (s_touch_fail_count >= 3) {
            s_touch_cached.pressed = false;
            ESP_LOGW("INPUT", "触摸连续 %d 次读失败, 强制释放触摸状态",
                     (int)s_touch_fail_count);
        }
        if (s_touch_fail_count >= TOUCH_FAIL_RECOVER_N) {
            s_touch_fail_count = 0;
            ESP_LOGW("INPUT", "触摸连续 %d 次读失败, 请求看门狗恢复",
                     (int)TOUCH_FAIL_RECOVER_N);
            touch_panel_request_recover();
        }
    } else {
        s_touch_fail_count = 0;
        touch_jump_filter(&s_touch_cached, s_touch_cached.pressed);
    }
    *pt = s_touch_cached;
}

static void touch_map_screen(int16_t rx, int16_t ry, int *sx, int *sy);
static menu_action_t touch_gesture_poll(void);

void input_poll_touch(void) {
    tp_point_t pt;
    touch_read_once(&pt);
    s_touch_down = pt.pressed;
    if (pt.pressed) touch_map_screen(pt.x, pt.y, &s_touch_sx, &s_touch_sy);
}

/* V1.0.95: 返回当前触摸按住时长(ms); 未按住返回 0.
 * 依赖 input_get_action / input_get_touch_action 的触摸轮询维护 s_touch_down/s_tg_t0.
 * 用于壁纸屏保"触摸需长按 1 秒才退出"的实时判定. */
uint32_t input_touch_hold_ms(void) {
    if (!s_touch_down) return 0;
    uint32_t t = now_ms();
    return ((int32_t)(t - s_tg_t0)) >= 0 ? (t - s_tg_t0) : 0;
}

menu_action_t input_get_touch_action(void) {
    return touch_gesture_poll();
}

/* === V1.0.68: 软关机键 (GPIO1) 长按 2 秒软关机 ===
 * 长按 2 秒达标瞬间即返回 true (无需松手), 调用方显示"正在关机"并进入 deep sleep;
 * 短按/0.5s 长按都不触发. s_power_prev 初始 true: 开机时若按键仍被按住, 不误判. */
static bool s_power_prev = true;
static uint32_t s_power_press_ms = 0;
static bool s_power_armed = false;
static bool s_power_press_valid = false;   /* 本次按住是否为新按下 (防开机误触) */

bool input_power_should_sleep(void) {
    bool pressed = (gpio_get_level(BTN_GPIO_PWR) == 0);
    uint32_t now = now_ms();

    /* 新按下: 记录起点, 重新武装 (松手后再次按下需重新计时) */
    if (pressed && !s_power_prev) {
        s_power_press_ms = now;
        s_power_armed = false;
        s_power_press_valid = true;
    }
    /* 松开: 复位武装, 防止下次按下沿用旧计时 */
    if (!pressed) {
        s_power_armed = false;
        s_power_press_valid = false;
    }
    /* V1.0.68: 长按 2s 达标瞬间立即触发软关机, 无需松手 */
    if (pressed && s_power_press_valid && !s_power_armed &&
        (now - s_power_press_ms) >= POWER_HOLD_MS) {
        s_power_armed = true;
        s_power_prev = pressed;
        return true;
    }
    s_power_prev = pressed;
    return false;
}

/* 把触摸面板原始坐标映射到 400x300 屏幕坐标 */
/* V1.0.68 fix: 触摸跳变滤波 — 单帧位移超过屏宽 55% 视为噪声, 保持上一帧坐标.
 * CST816 在受潮/干扰时偶尔返回跳变的坐标, 旧代码直接采用导致点击乱跳/误触. */
#define TOUCH_JUMP_PCT 550   /* 千分比: 屏宽 55% */
static int16_t s_flt_rx = -1, s_flt_ry = -1;

static void touch_map_screen(int16_t rx, int16_t ry, int *sx, int *sy) {
    int mx, my;
    touch_panel_get_resolution(&mx, &my);
    if (mx > 0 && my > 0) {
        *sx = (int)((int32_t)rx * ST7305_WIDTH / mx);
        *sy = (int)((int32_t)ry * ST7305_HEIGHT / my);
    } else {
        *sx = rx;
        *sy = ry;
    }
    if (*sx < 0) *sx = 0;
    if (*sx >= ST7305_WIDTH) *sx = ST7305_WIDTH - 1;
    if (*sy < 0) *sy = 0;
    if (*sy >= ST7305_HEIGHT) *sy = ST7305_HEIGHT - 1;
}

/* 在 raw 坐标上做跳变滤波 (按分辨率归一化), 返回滤波后的坐标 */
static void touch_jump_filter(tp_point_t *pt, bool pressed) {
    if (!pressed) {
        s_flt_rx = s_flt_ry = -1;
        return;
    }
    int res_x, res_y;
    touch_panel_get_resolution(&res_x, &res_y);
    if (s_flt_rx >= 0) {
        int jx = res_x ? (int)(((int32_t)pt->x - s_flt_rx) * 1000 / res_x) : 0;
        int jy = res_y ? (int)(((int32_t)pt->y - s_flt_ry) * 1000 / res_y) : 0;
        if (jx * jx + jy * jy > TOUCH_JUMP_PCT * TOUCH_JUMP_PCT) {
            /* 限频日志: 跳变风暴时最多 1 条/秒 */
            static uint32_t s_jump_log_ms = 0;
            uint32_t now_ms2 = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (now_ms2 - s_jump_log_ms >= 1000) {
                s_jump_log_ms = now_ms2;
                ESP_LOGW("INPUT", "触摸跳变被滤除: (%d,%d)->(%d,%d)",
                         (int)s_flt_rx, (int)s_flt_ry, (int)pt->x, (int)pt->y);
            }
            pt->x = s_flt_rx;   /* 保持上一帧, 不采用跳变点 */
            pt->y = s_flt_ry;
            return;
        }
    }
    s_flt_rx = pt->x;
    s_flt_ry = pt->y;
}

/* === V1.0.68: 屏幕旋转 (电子书竖屏时触摸跟随旋转) ===
 * 0=横屏(默认) 1=180° 2=左90°(竖屏) 3=右90°(竖屏) */
static int s_screen_rot = 0;

void input_set_screen_rotation(int rot) {
    s_screen_rot = rot;
}

/* 把 400x300 横屏物理坐标旋转到逻辑坐标 (跟随屏幕显示方向) */
static void rotate_screen_coord(int *x, int *y) {
    int ox = *x, oy = *y;
    switch (s_screen_rot) {
    case 1:
        *x = ST7305_WIDTH  - 1 - ox;
        *y = ST7305_HEIGHT - 1 - oy;
        break;
    case 2:
        *x = oy;
        *y = ST7305_WIDTH  - 1 - ox;
        break;
    case 3:
        *x = ST7305_HEIGHT - 1 - oy;
        *y = ox;
        break;
    default:
        break;
    }
}

/* V1.0.90: 判断逻辑坐标 (sx,sy) 是否落在"物理屏幕底部中间"区域.
 * 该区域在触摸上用作: 底部上滑=返回 + 拖动屏蔽带. 屏幕旋转时, 物理底边
 * 映射到不同的逻辑区域, 这里按当前旋转方向 s_screen_rot 逐态换算:
 *   rot0 横屏   : 底边 = 逻辑下侧 (y 大)
 *   rot1 180°   : 底边 = 逻辑上侧 (y 小)
 *   rot2 左转90°: 底边 = 逻辑右侧 (x 大, 竖屏宽=ST7305_HEIGHT)
 *   rot3 右转90°: 底边 = 逻辑左侧 (x 小)
 * thick 为厚度 (逻辑像素), 底边中心带 x/y 按另一个轴取中间区域. */
bool input_in_bottom_zone(int sx, int sy, int thick) {
    switch (s_screen_rot) {
    case 1:
        return sx >= TOUCH_BACK_EDGE_MINX && sx <= TOUCH_BACK_EDGE_MAXX &&
               sy >= 0 && sy < thick;
    case 2:
        return sx >= (ST7305_HEIGHT - thick) && sx < ST7305_HEIGHT &&
               sy >= TOUCH_BACK_EDGE_MINX && sy <= TOUCH_BACK_EDGE_MAXX;
    case 3:
        return sx >= 0 && sx < thick &&
               sy >= TOUCH_BACK_EDGE_MINX && sy <= TOUCH_BACK_EDGE_MAXX;
    default:
        return sx >= TOUCH_BACK_EDGE_MINX && sx <= TOUCH_BACK_EDGE_MAXX &&
               sy >= (ST7305_HEIGHT - thick);
    }
}

static menu_action_t touch_gesture_poll(void) {
    /* 每帧先清点击坐标: 只有本帧真正产生"点击"时才会重新写入,
     * 避免屏保唤醒等场景把上一帧的点击坐标残留到下一次 CONFIRM. */
    s_tap_x = s_tap_y = -1;
    tp_point_t pt;
    touch_read_once(&pt);
    /* 更新实时触摸屏幕坐标 (供主菜单跟手拖动读取) */
    s_touch_down = pt.pressed;
    if (pt.pressed) {
        touch_map_screen(pt.x, pt.y, &s_touch_sx, &s_touch_sy);
        rotate_screen_coord(&s_touch_sx, &s_touch_sy);   /* 旋转到逻辑坐标 */
    }
    uint32_t t = now_ms();
    menu_action_t out = MENU_ACTION_NONE;

    switch (s_tg_state) {
    case TG_IDLE:
        if (pt.pressed) {
            s_tg_state = TG_PRESS;
            s_tg_x0 = s_tg_x = pt.x;
            s_tg_y0 = s_tg_y = pt.y;
            touch_map_screen(pt.x, pt.y, &s_tg_sx0, &s_tg_sy0);
            rotate_screen_coord(&s_tg_sx0, &s_tg_sy0);    /* 旋转到逻辑坐标 */
            s_tg_t0 = t;
            s_tg_long_fired = false;
        }
        break;

    case TG_PRESS:
        if (pt.pressed) {
            s_tg_x = pt.x;
            s_tg_y = pt.y;
            /* V1.0.68: 长按状态栏任意位置 3 秒 -> 投递 HOME (返回主菜单, 任何界面生效) */
            if (!s_tg_long_fired && s_tg_sy0 < TOUCH_STATUS_H &&
                (int32_t)(t - s_tg_t0) >= TOUCH_LONG_MS) {
                int dx = s_tg_x - s_tg_x0, dy = s_tg_y - s_tg_y0;
                int adx = dx < 0 ? -dx : dx;
                int ady = dy < 0 ? -dy : dy;
                if (adx < TOUCH_STILL_MAX && ady < TOUCH_STILL_MAX) {
                    s_tg_long_fired = true;
                    s_tg_state = TG_WAIT_RELEASE;
                    ESP_LOGI(TAG, "长按状态栏 3s -> HOME");
                    out = MENU_ACTION_HOME;
                }
            }
            /* V1.0.xx: 触摸状态超时保护 — 按住超过 9 秒且无位移, 强制复位.
             * 防止触摸芯片读失败导致 s_touch_down 状态残留, 菜单无法滑动.
             * (5 秒被主菜单长按删除用, 故提到 9 秒避免误复位) */
            if ((int32_t)(t - s_tg_t0) >= 9000) {
                int dx = s_tg_x - s_tg_x0, dy = s_tg_y - s_tg_y0;
                int adx = dx < 0 ? -dx : dx;
                int ady = dy < 0 ? -dy : dy;
                if (adx < 10 && ady < 10) {
                    ESP_LOGW(TAG, "触摸状态超时 5s 复位 (可能芯片读失败导致状态残留)");
                    s_tg_state = TG_IDLE;
                    s_tg_long_fired = false;
                    s_touch_down = false;
                    /* 不设置 out, 直接复位, 不产生任何按键动作 */
                }
            }
            /* 其他区域长按不在此处触发: 改为松手时判定 (见释放分支),
             * 避免"按住再拖动"误触收藏 */
        } else {
            /* 释放 -> 判定手势 */
            int dx = s_tg_x - s_tg_x0, dy = s_tg_y - s_tg_y0;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            uint32_t dt = t - s_tg_t0;

            /* V1.0.68: 长按(≥800ms)且按住期间未移动 → LONG_PRESS (游戏列表收藏等).
             * 松手时判定: 拖动(移动超 20px)不触发, 避免拖动误收藏. */
            if (dt >= TOUCH_LONG_PRESS_MS && s_tg_sy0 >= TOUCH_STATUS_H &&
                adx < TOUCH_STILL_MAX && ady < TOUCH_STILL_MAX) {
                out = MENU_ACTION_LONG_PRESS;
                s_tap_x = s_tg_sx0; s_tap_y = s_tg_sy0;
                ESP_LOGI(TAG, "长按 %lums -> LONG_PRESS @ %d,%d",
                         (unsigned long)dt, s_tap_x, s_tap_y);
            } else if (adx < TOUCH_SWIPE_MIN && ady < TOUCH_SWIPE_MIN) {
                /* 位移小 = 点击 (tap 坐标用旋转后的逻辑坐标, 供阅读器分区命中) */
                out = MENU_ACTION_CONFIRM;
                s_tap_x = s_tg_sx0; s_tap_y = s_tg_sy0;
                ESP_LOGI(TAG, "触摸点击 (%lums) raw=%d,%d -> %d,%d",
                         (unsigned long)dt, s_tg_x0, s_tg_y0, s_tap_x, s_tap_y);
            } else if (input_in_bottom_zone(s_tg_sx0, s_tg_sy0, ST7305_HEIGHT - TOUCH_BACK_EDGE_Y) &&
                       dy <= -TOUCH_BACK_SWIPE_DY) {
                /* V1.0.68: 物理屏幕底部中间区域往上滑 -> 返回 (V1.0.90: 跟随屏幕旋转).
                 * V1.0.94: 1s 内连续两次上滑 -> HOME, 免确认强制退出到主菜单
                 * (游戏内/游戏列表或任意菜单, 上滑只会触发退出, 不会拖动页面). */
                static uint32_t s_last_sw_ms = 0;
                bool double_sw = s_last_sw_ms && (int32_t)(t - s_last_sw_ms) <= TOUCH_DOUBLE_SWIPE_MS;
                s_last_sw_ms = t;
                out = double_sw ? MENU_ACTION_HOME : MENU_ACTION_BACK;
                ESP_LOGI(TAG, "底部上滑 (%d,%d)->(%d,%d) -> %s",
                         s_tg_x0, s_tg_y0, s_tg_x, s_tg_y,
                         double_sw ? "HOME" : "BACK");
            }
            /* V1.0.8x: 已删除左右/上下滑动 -> 方向键. 打开仅靠点击确定位置. */
            s_tg_state = TG_IDLE;
        }
        break;

    case TG_WAIT_RELEASE:
        /* 长按已投递 BACK, 等手指抬起, 期间不再产生任何动作 */
        if (!pt.pressed) {
            s_tg_state = TG_IDLE;
        }
        break;
    }
    return out;
}

/* V1.0.68: 最近一次 input_get_action 返回的动作是否来自触摸 (底部上滑等) */
static bool s_last_action_touch = false;
bool input_touch_last_action(void) {
    return s_last_action_touch;
}

menu_action_t input_get_action(void) {
    /* V1.0.68: 记录最近一次动作来源 (触摸 vs 物理键/手柄), 供确认框区分
     * "底部上滑(BACK)"与"物理 BACK 键" (上滑再划一次=确认, 物理 BACK=取消) */
    s_last_action_touch = false;
    for (int i = 0; i < 3; i++) {
        menu_action_t a = tick(&s_btns[i]);
        if (a != MENU_ACTION_NONE) { s_tap_x = s_tap_y = -1; return a; }
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
                    s_tap_x = s_tap_y = -1;
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
                    s_tap_x = s_tap_y = -1;
                    return MENU_ACTION_LONG_LEFT;
                }
                /* 长按自动重复(仅方向键), 带 hold-cap: 超时停止, 防释放丢失导致一直滚 */
                if ((int32_t)(now - st[i].press_start_ms) < NAV_HOLD_CAP_MS
                    && (int32_t)(now - st[i].repeat_next_ms) >= 0) {
                    st[i].repeat_next_ms = now + NAV_REPEAT_RATE;
                    s_tap_x = s_tap_y = -1;
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
    /* V1.0.xx: 网页手柄 (WiFi AP, 手机浏览器) 导航接入.
     * 背景: 开启 WiFi 手柄时会 bt_manager_disable() 关蓝牙腾 DMA, 故上方蓝牙块不生效;
     * 此前 web_gamepad 只在 GB/GBC 引擎 (input_get_held_gb_joypad) 生效, 菜单导航完全无反应.
     * web_gamepad 掩码与 GB joypad 一致 (低电平有效):
     *   bit0=A bit1=B bit2=Select bit3=Start bit4=右 bit5=左 bit6=上 bit7=下.
     * 状态是"保持态"(按住常量), 这里用上升沿(某位从 1→0)触发一次 action, 与摇杆边沿语义一致. */
    if (s_gamepad_nav_enabled && web_gamepad_is_running()) {
        static uint8_t s_wp_prev = 0xFF;
        uint8_t wp_cur = web_gamepad_get_joypad_state();
        uint8_t pressed = (uint8_t)(s_wp_prev & ~wp_cur);   /* 本次新按下 (prev=1, cur=0) */
        s_wp_prev = wp_cur;
        if (pressed) {
            s_tap_x = s_tap_y = -1;
            if (pressed & (1 << 6)) return MENU_ACTION_UP;      /* 上 */
            if (pressed & (1 << 7)) return MENU_ACTION_DOWN;    /* 下 */
            if (pressed & (1 << 5)) return MENU_ACTION_LEFT;    /* 左 */
            if (pressed & (1 << 4)) return MENU_ACTION_RIGHT;   /* 右 */
            if (pressed & (1 << 0)) return MENU_ACTION_CONFIRM; /* A=确认 */
            if (pressed & (1 << 1)) return MENU_ACTION_BACK;    /* B=返回 */
            if (pressed & (1 << 3)) return MENU_ACTION_CONFIRM; /* Start=确认 */
        }
    }
    /* 触摸手势 (V1.0.65): 物理键/手柄无动作时才投递, 三者互不抢占 */
    menu_action_t ta = touch_gesture_poll();
    if (ta != MENU_ACTION_NONE) {
        s_last_action_touch = true;   /* V1.0.68: 标记来源为触摸 */
        return ta;
    }
    return MENU_ACTION_NONE;
}

bool input_is_held(int idx) {
    if (idx < 0 || idx > 1) return false;
    return (gpio_get_level(s_btns[idx].gpio) == 0);
}

/* V1.0.69: 本机是否带触摸屏 (两套硬件). 供 menu_system 区分物理 KEY 收藏与触摸收藏 */
bool input_has_touch(void) {
    return s_has_touch;
}

/* 消费最近一次"点击"的屏幕坐标 (V1.0.65). 返回 true 表示有未消费的点击,
 * 并把坐标写入 x 和 y (可为 NULL). 取走后自动清零, 保证一次点击只 hit-test 一次. */
bool input_consume_tap(int *x, int *y) {
    if (s_tap_x < 0) return false;
    if (x) *x = s_tap_x;
    if (y) *y = s_tap_y;
    s_tap_x = s_tap_y = -1;
    return true;
}

/* V1.0.66: 返回当前触摸的实时屏幕坐标 (供主菜单跟手拖动).
 * 返回 false 表示当前没有手指按下.
 * V1.0.94: 底部屏蔽带统一拦截 —— 凡是从物理屏幕底部中间区域开始的手势, 一旦发生
 * 拖动位移 (>40px) 就返回 false (视同无触摸), 这样依赖本函数的各拖动方 (应用管理、
 * 游戏引擎虚拟按键、游戏列表等) 都无法从屏蔽带拖动内容页. 原地/点击不受影响
 * (点击走 input_consume_tap, 与本节无关), 屏蔽带内的点击/确认照常生效. */
/* V1.0.99: 拖动屏蔽带的锁存状态提升到文件作用域, 供 input_get_touch_pos 与
 * input_touch_in_bottom_zone 共享. 目的: 二者必须**复用同一套锁存结果**, 绝不
 * 在本帧内对 input_get_touch_pos 二次调用 (multi-consumer 竞态会让 static 状态
 * 被两次消费而残留, 导致 s_o_armed/s_o_zone 破坏 → 所有拖动方都无法建立拖动).
 * 约定: 本帧内 input_touch_in_bottom_zone 必须先于/紧随 input_get_touch_pos 调用. */
static bool s_o_armed = true;   /* 本手势是否尚未记录起点 (进入抬起态复位) */
static bool s_o_zone  = false;  /* 手势起点是否落在底部屏蔽带 */
static int  s_o_sx = 0, s_o_sy = 0;
static bool s_o_prev_down = false; /* 上一帧 s_touch_down, 用于上升沿检测 */

bool input_get_touch_pos(int *x, int *y) {
    /* V1.0.xx: 检测 s_touch_down 上升沿 — 新触摸开始, 强制复位状态.
     * 修复场景: 从底部屏蔽带触摸返回后, 若状态机复位不完整导致 s_o_zone
     * 残留为 true, 后续所有滑动(位移≥40px)都被拦截, 菜单无法左右拖动.
     * 上升沿检测确保每次新触摸都重新计算起点, 不受上一轮残留影响. */
    if (s_touch_down && !s_o_prev_down) {
        s_o_armed = true;
        s_o_zone  = false;
    }
    s_o_prev_down = s_touch_down;

    if (s_touch_down) {
        if (s_o_armed) {
            s_o_armed = false;
            s_o_sx = s_touch_sx;
            s_o_sy = s_touch_sy;
            s_o_zone = input_in_bottom_zone(s_o_sx, s_o_sy,
                                            ST7305_HEIGHT - TOUCH_BACK_EDGE_Y);
        }
        if (s_o_zone) {
            int dx = s_touch_sx - s_o_sx, dy = s_touch_sy - s_o_sy;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;
            if (adx >= TOUCH_SWIPE_MIN || ady >= TOUCH_SWIPE_MIN)
                return false;   /* 屏蔽带内拖动 → 隐藏坐标, 禁止拖动 */
        }
        if (x) *x = s_touch_sx;
        if (y) *y = s_touch_sy;
        return true;
    }
    s_o_armed = true;
    s_o_zone  = false;
    return false;
}

/* V1.0.99: 只读查询"当前手指按下 且 本次手势起点落在底部屏蔽带" (不消费任何状态).
 * 供 main.c 的 touch_shield_blocks_drag 使用, 替代其内部二次调用 input_get_touch_pos,
 * 从而消除"同帧对 input_get_touch_pos 两次调用 → 锁存状态被两次消费而残留
 * → 所有拖动方无法建立拖动 (拖动全面失效, 重启即消)"的多消费者竞态 bug.
 * 前提: 本函数复用 input_get_touch_pos 已维护的 s_o_armed/s_o_zone 锁存结果,
 * 必须在同帧 input_get_touch_pos 调用之后 (main.c 拖动分支即此顺序) 使用. */
bool input_touch_in_bottom_zone(void) {
    return s_touch_down && s_o_zone;
}

/* V1.0.9x: 本次手势按下的屏幕起点 (逻辑坐标). 未按住返回 false. */
bool input_touch_start_pos(int *x, int *y) {
    if (!s_touch_down) return false;
    if (x) *x = s_tg_sx0;
    if (y) *y = s_tg_sy0;
    return true;
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
        /* V1.0.68: Select/Start 由 10 键按键映射提供 (F_SELECT/F_START);
         * 未映射时回退手柄物理 Select/Start 保证 NES/GB 开箱可用
         * (例如超级玛丽标题画面必须按 Start 才能开始游戏). */
        bool gb_sel = bt_manager_is_key_pressed(F_SELECT);
        bool gb_start = bt_manager_is_key_pressed(F_START);
        if (!gb_sel) gb_sel = bt_manager_is_phys_pressed(P_SELECT);
        if (!gb_start) gb_start = bt_manager_is_phys_pressed(P_START);
        if (gb_sel)   j &= ~(1 << 2); /* Select */
        if (gb_start) j &= ~(1 << 3); /* Start */
    }
    /* 设备物理键兼容: KEY(GPIO18)=A, BOOT(GPIO0)=B */
    if (input_is_held(0)) j &= ~(1 << 0);  /* KEY → A */
    if (input_is_held(1)) j &= ~(1 << 1);  /* BOOT → B */
    /* V1.0.67: 网页手柄 (WiFi AP, 手机浏览器) */
    if (web_gamepad_is_running()) {
        j &= web_gamepad_get_joypad_state();
    }
    /* V1.0.68: 游戏内屏幕虚拟按键 (游戏设置里开启) */
    if (virtual_keys_is_enabled()) {
        j &= virtual_keys_poll(s_touch_sx, s_touch_sy, s_touch_down);
    }
    return j;
}
