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
#define PWR_SHORT_MS    500    /* V1.5.x: 开关键短按判定阈值 — 按住<0.5s 松手=锁屏(短按);
                                * 达到 2s(POWER_HOLD_MS) 由 input_power_should_sleep 弹关机确认 */
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_pm.h"          /* V1.5.x: 游戏锁频 (PM 降频下锁 240MHz 防卡顿) */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "bt_manager.h"
#include "touch_panel.h"
#include "web_gamepad.h"
#include "virtual_keys.h"
#include "vibrator.h"   /* V1.2.x: 游戏震动 (joypad 边沿 / 退出键) */
#include "tone_player.h" /* 引擎按键点击音 (引擎优化"按键声音"开关) */

#define TAG "INPUT"
#define DEBOUNCE_MS 30
#define LONG_PRESS_MS 500

/* 引擎优化开关 (page_engine 通过 input_set_engine_* 写入, 默认开):
 *  - 引擎按键点击音 s_eng_sound
 *  - 引擎游戏操作震动 s_eng_vib (仅游戏模式防线, 不碰 UI/按键模拟震动) */
static bool s_eng_sound = true;
static bool s_eng_vib   = true;
void input_set_engine_sound(bool on) { s_eng_sound = on; }
void input_set_engine_vib(bool on)   { s_eng_vib   = on; }

/* ==== 触摸手势识别参数 ====
 * 位移阈值用原始像素(与分辨率无关); 区域判定用屏幕坐标(400x300). */
#define TOUCH_TAP_MAX_MS     300   /* 单击最长时长 */
#define TOUCH_SWIPE_MIN      40    /* 滑动最小位移(原始像素), 低于算点击 */
#define TOUCH_LONG_MS        3000  /* V1.0.68: 状态栏长按 3 秒 = 返回主菜单 */
#define TOUCH_LONG_PRESS_MS  2000  /* V1.0.9x: 游戏名/收藏栏长按 2s = 收藏/取消收藏; 应用管理长按弹窗也走此阈值 */
#define TOUCH_HOLD_VIB_MS    500   /* V1.4.x: 按住超过此毫秒松手 → 补一次"弹起"震动 (按键模拟) */
#define TOUCH_STILL_MAX      20    /* 长按判定时允许的抖动位移 */
#define TOUCH_STATUS_H       24    /* 状态栏高度: 长按此区域(按下点 y<24) = 返回主菜单 */
#define TOUCH_BACK_EDGE_Y    285   /* 底部上滑返回: 按下点屏幕 y >= 此值.
                                     * V1.0.xx: 30px(10%屏高) 收窄到 15px(5%屏高),
                                     * 对齐 iOS(~4%)/Android(~5%) 底部触发条比例.
                                     * 低分辨率(300px)下收窄后, 底部控件(如 MP3 按钮 y274-298)
                                     * 绝大部分移出触发区, 减少与上滑返回的争抢. */
#define TOUCH_BACK_EDGE_MINX 125   /* 底部上滑返回: 按下点屏幕 x 下限 (V1.0.9x: 收缩到中间约150px, 左右两侧不触发, 避免摇杆从底部左右上滑误触发) */
#define TOUCH_BACK_EDGE_MAXX 275   /* 底部上滑返回: 按下点屏幕 x 上限 (屏宽400, 中间150px => 125..275) */
#define TOUCH_BACK_SWIPE_DY  60    /* 底部上滑返回: 需往上滑动超过60px才触发 */
#define TOUCH_DOUBLE_SWIPE_MS 1000  /* V1.0.9x: 1s 内连续两次底部上滑 → 第2次=确认退出 (返回+确认) */

/* V1.0.69: 本机是否带触摸屏 (运行时探测), 决定物理键语义分支.
 * 有触摸:  BOOT=返回上级/回主菜单, KEY=确认/收藏
 * 无触摸:  BOOT=下一步/返回上级,  KEY=确认/直达蓝牙映射 */
static bool s_has_touch = false;

/* V1.4.x: 上下文屏蔽按键模拟震动 (按下/松手). 由 os_core 每帧驱动:
 * 主菜单/软件管家页 = true (拖动图标/管理列表不咔哒), 弹窗/二级页/程序 = false. */
static bool s_key_sim_ctx_block = false;
void input_set_key_sim_ctx_block(bool block) { s_key_sim_ctx_block = block; }

/* V1.1.0: 是否允许底部上滑=返回 (白板等全屏绘图页屏蔽, 避免误退) */
static bool s_back_swipe = true;

/* 阅读器横滑翻页开关: 仅阅读器打开时启用, 横向滑动 -> LEFT/RIGHT (翻上一页/下一页).
 * 其余界面保持"横滑=主菜单拖动/其它", 不产生方向键动作. */
static bool s_swipe_turn = false;
void input_set_swipe_turn(bool enable) { s_swipe_turn = enable; }

static uint32_t now_ms(void) {
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

typedef enum { ST_IDLE, ST_DEBOUNCE, ST_PRESSED, ST_LONG_FIRED } btn_state_t;
typedef struct {
    gpio_num_t gpio;
    btn_state_t state;
    uint32_t press_start;
    uint32_t long_ms;          /* 本键长按阈值 (ms), 0=用全局 LONG_PRESS_MS */
    menu_action_t short_action, long_action;
    bool rel_short;            /* V1.5.x: 短按动作改为"释放时判定" — 按住时长<阈值(0.5s)
                                * 松手才投 short_action; 长按时不提前投, 避免与长按动作打架.
                                * 目前仅开关键(PWR)用: 短按(<0.5s)松手=锁屏; 长按2s=关机弹窗. */
    const char *name;
} btn_ctx_t;

static btn_ctx_t s_btns[3];

/* V1.0.41: 手柄导航键开关. 按键映射期间设为 false, 防止手柄按键产生 action
 * 干扰映射流程 (如映射"返回"键时 MENU_ACTION_BACK 会终止映射). */
static bool s_gamepad_nav_enabled = true;
void input_set_gamepad_nav_enabled(bool enabled) {
    s_gamepad_nav_enabled = enabled;
}

/* V1.2.x: 游戏模式标志. 引擎运行循环进入游戏置 true, 退出置 false.
 * 详情见 input.h. 区分"nav 关闭"的两种场景: 游戏中(允许返回/退出键) 与 按键映射中
 * (返回键应被当作被映射的键捕获, 不能误触发退出). */
static bool s_gamepad_gamemode = false;
/* V1.5.x: 游戏 CPU 锁频句柄 — PM 自动降频下, 进入游戏锁 240MHz 防引擎掉帧 */
static esp_pm_lock_handle_t s_pm_lock = NULL;
/* V1.3.x: 游戏模式变化回调 (暗黑UI模式等按游戏态切换反色用, 游戏阻塞主循环需即时通知) */
static input_gamemode_cb_t s_gamemode_cb = NULL;
void input_set_gamepad_gamemode(bool enabled) {
    if (s_gamepad_gamemode == enabled) return;
    /* V1.5.x: 锁频跟随游戏态 — 进游戏锁 240MHz, 退出释放让 PM 降频省电.
     * PM 未开启时 esp_pm_lock_create 返回不支持, s_pm_lock 保持 NULL 自动跳过. */
    if (enabled) {
        if (!s_pm_lock) esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "bbk_game", &s_pm_lock);
        if (s_pm_lock) esp_pm_lock_acquire(s_pm_lock);
    } else {
        if (s_pm_lock) esp_pm_lock_release(s_pm_lock);
    }
    s_gamepad_gamemode = enabled;
    if (s_gamemode_cb) s_gamemode_cb(enabled);
}
void input_set_gamemode_cb(input_gamemode_cb_t cb) { s_gamemode_cb = cb; }

/* V1.2.x: 查询游戏模式 (供震动子系统区分游戏/UI). */
bool input_is_game_mode(void) { return s_gamepad_gamemode; }

/* V1.5.x: 统一"最近输入"时钟 — 桌面主循环与阻塞游戏循环共用同一把尺.
 * s_last_any_ms 由任意输入(手柄/物理键/触摸/joypad)刷新, 供休眠判定使用.
 * 这样休眠无论在哪触发口径都一致, 不随各页面各自计时而漂移. */
static uint32_t s_last_any_ms = 0;
uint32_t input_last_any_ms(void) { return s_last_any_ms; }

/* V1.5.x: 休眠(屏保)禁触屏 — 屏保激活时置 true, 触摸入口全部短路:
 * 不刷新 s_last_any_ms、不产生 tap/坐标/手势动作, 杜绝"休眠中碰屏误唤醒". */
static volatile bool s_touch_blocked = false;
void input_set_touch_blocked(bool block) { s_touch_blocked = block; }
bool input_touch_blocked(void) { return s_touch_blocked; }

/* 输入方案: 统一"选择式 UI"(弹窗/列表/键盘) 的交互策略.
 * 优先规则: 只要有触屏 → TOUCH(触屏优先, 即使同时连着手柄);
 *          无触屏 → GAMEPAD(手柄/物理键, 默认高亮一项 + 方向键移动焦点).
 * TOUCH 方案: 默认不高亮任何项、点哪算哪、无效区不震动/无反应.
 * 所有选择/键盘模板都应查询本函数, 不要各自用"是否连手柄"凑合判断. */
bool input_scheme_touch(void) { return s_has_touch; }

/* 全局活动标记: 游戏 joypad 有任意键按住时置位, 供 main 主循环刷新休眠计时.
 * 游戏输入走独立通道 (input_get_held_gb_joypad), 不经由 os 物理键/触摸,
 * 否则会被误判为"无输入"而提前休眠. 标志位方式对时钟无依赖, 由 main 每帧消费. */
static bool s_activity_flag = false;
void input_mark_activity(void) { s_activity_flag = true; s_last_any_ms = now_ms(); }
bool input_consume_activity(void) { bool a = s_activity_flag; s_activity_flag = false; return a; }

/* V1.2.x: 游戏中"退出键"反馈 — BACK/HOME 不在 joypad 位图内, 单独在此触发游戏震动
 * (方向/确认等游戏操作键走 joypad 上升沿, 不与本处重复). */
static void fb_game_exit_vib(menu_action_t a) {
    if (s_gamepad_gamemode &&
        (a == MENU_ACTION_BACK || a == MENU_ACTION_HOME)) {
        vibrator_tap_game(30, 120);
    }
}

void input_init(void) {
    /* V1.5.x 修复: 统一"最后活动"时钟必须从开机即有值(初始化为开机时刻), 否则开机后
     * 未产生任何输入的静置期间 s_last_any_ms 恒为 0, 屏保监视任务的
     * `input_last_any_ms() != 0` 守卫永假 → 自动空闲屏保/睡眠永不触发. */
    s_last_any_ms = now_ms();
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
         *   BOOT: 短按=返回上级, 长按3s=回主菜单 (用户需求: 长按返回键3秒强制回桌面)
         *   KEY : 短按=确认,     长按=收藏当前项 */
        s_btns[0] = (btn_ctx_t){ BTN_GPIO_RIGHT, ST_IDLE, 0, 3000, MENU_ACTION_BACK, MENU_ACTION_HOME, false, "BOOT" };
        s_btns[1] = (btn_ctx_t){ BTN_GPIO_LEFT,  ST_IDLE, 0, 2000, MENU_ACTION_CONFIRM, MENU_ACTION_BT_SEARCH, false, "KEY" };
    } else {
        /* 无触摸机型: 只剩两颗物理键, 要让两键也能闭环操作到蓝牙映射界面.
         *   BOOT: 短按=下一步(右移/选中右移), 长按=返回上级
         *   KEY : 短按=确认(进入),             长按2s=蓝牙搜索 */
        s_btns[0] = (btn_ctx_t){ BTN_GPIO_RIGHT, ST_IDLE, 0, 0, MENU_ACTION_RIGHT, MENU_ACTION_BACK, false, "BOOT" };
        s_btns[1] = (btn_ctx_t){ BTN_GPIO_LEFT,  ST_IDLE, 0, 2000, MENU_ACTION_CONFIRM, MENU_ACTION_BT_SEARCH, false, "KEY" };
    }
    /* 开关键 (V1.5.x): rel_short=true — 短按动作改"释放判定":
     *   按住 <0.5s 松手 → POWER_LOCK(锁屏进壁纸); 持续按住 ≥0.5s 不投(避免长按关机途中误锁屏),
     *   到 2s 由 input_power_should_sleep 独立轮询弹"是否关机"确认框.
     *   long_action 保持 NONE (长按关机不由本键 tick 投递). */
    s_btns[2] = (btn_ctx_t){ BTN_GPIO_PWR, ST_IDLE, 0, POWER_HOLD_MS, MENU_ACTION_POWER_LOCK, MENU_ACTION_NONE, true, "PWR" };

    /* V1.0.68: 软关机键 GPIO1 同时配置 deep sleep 唤醒 (长按2s软关机后按下唤醒)
     * V1.5.x: 任意物理键唤醒 — 三颗键都加入 ext1 掩码 (按键按下=低电平, 已统一上拉) */
    rtc_gpio_pullup_en(BTN_GPIO_PWR);
    esp_sleep_enable_ext1_wakeup_io(
        (1ULL << BTN_GPIO_PWR) | (1ULL << BTN_GPIO_LEFT) | (1ULL << BTN_GPIO_RIGHT),
        ESP_EXT1_WAKEUP_ANY_LOW);
}

static menu_action_t tick(btn_ctx_t *b) {
    bool pressed = (gpio_get_level(b->gpio) == 0);
    uint32_t now = now_ms();
    switch (b->state) {
        case ST_IDLE:
            if (pressed) { b->state = ST_DEBOUNCE; b->press_start = now; }
            break;
        case ST_DEBOUNCE:
            /* V1.5.x: 按下防抖 (原 DEBOUNCE 只防释放抖动; 按下即触发改为防住
             * 按下沿抖动误触发). rel_short 键(开关键)防抖通过后只进入 PRESSED,
             * 短按动作延后到"释放时"按按住时长判定 (见 ST_PRESSED 释放分支);
             * 普通键防抖通过后立即投递短按动作, 不等松手. */
            if (pressed) {
                if (now - b->press_start >= DEBOUNCE_MS) {
                    b->state = ST_PRESSED;
                    if (!b->rel_short) return b->short_action;
                }
            } else {
                b->state = ST_IDLE;   /* 抖动, 取消本次按下 */
            }
            break;
        case ST_PRESSED:
            if (pressed) {
                uint32_t thr = (b->long_ms > 0) ? b->long_ms : LONG_PRESS_MS;
                if (now - b->press_start > thr) {
                    /* V1.0.40: 长按发射后进入 ST_LONG_FIRED, 等按键释放才回 IDLE.
                     * V1.5.x 提示: 开关键(rel_short)长按 2s 在此进入 LONG_FIRED,
                     * 投递 long_action=NONE (长按关机由 input_power_should_sleep
                     * 独立轮询弹确认框), 此后释放不再误触发锁屏. */
                    b->state = ST_LONG_FIRED;
                    return b->long_action;
                }
            } else {
                /* V1.5.x: rel_short 键(开关键)短按判定 — 按住 <0.5s 松手 → 投短按(锁屏);
                 * 按住 ≥0.5s 松手 → 不投 (此时长按关机意图, 避免松手误锁屏).
                 * 普通键短按已在按下瞬间投递, 释放不再重复触发. */
                b->state = ST_IDLE;
                if (b->rel_short && (now - b->press_start) < PWR_SHORT_MS)
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
static tp_point_t s_touch_cached;               /* 触点0 (单点兼容读) */
static tp_point_t s_touch_multi[TP_MAX_POINTS]; /* 全场触点缓存 (输入层每 tick 一次读) */
static int        s_touch_cached_count = 0;     /* 实际触点数量 (0..TP_MAX_POINTS) */
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
        s_touch_multi[0].pressed = false;
        s_touch_cached_count = 0;
        s_touch_fail_count = 0;
        *pt = s_touch_cached;
        return;
    }
    tp_point_t pts[TP_MAX_POINTS];
    int cnt = 0;
    if (!touch_panel_read_points(pts, &cnt)) {
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
            s_touch_multi[0].pressed = false;
            s_touch_cached_count = 0;
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
        s_touch_cached = pts[0];
        for (int i = 0; i < TP_MAX_POINTS; i++) {
            s_touch_multi[i] = (i < cnt) ? pts[i] : pts[0];
            if (i >= cnt) s_touch_multi[i].pressed = false;
        }
        s_touch_cached_count = cnt;
        touch_jump_filter(&s_touch_cached, s_touch_cached.pressed);
    }
    *pt = s_touch_cached;
}

static void touch_map_screen(int16_t rx, int16_t ry, int *sx, int *sy);
static menu_action_t touch_gesture_poll(void);

void input_poll_touch(void) {
    if (s_touch_blocked) { s_touch_down = false; s_tap_x = s_tap_y = -1; return; }  /* 休眠禁触屏 */
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
    if (s_touch_blocked) return MENU_ACTION_NONE;   /* 休眠禁触屏: 无触摸手势动作 */
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
    /* 休眠禁触屏 (统一触摸手势入口): input_get_action 与 input_get_touch_action
     * 都经此处理触摸, 屏保(壁纸)激活时一律不产生手势/不访问触摸/不刷新 s_last_any_ms,
     * 彻底阻断"壁纸态碰触屏被当作新输入而唤醒". 此前只 gate input_get_touch_action,
     * 漏掉 input_get_action 内部的调用 → 触摸点击/底部上滑仍会唤醒. */
    if (s_touch_blocked) { s_tap_x = s_tap_y = -1; s_touch_down = false; return MENU_ACTION_NONE; }
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
            /* V1.4.x: 触摸按下不再在此统一震动 — "点空白/无效区不震".
             * 震动改由 os 层在"实际命中有效 UI 元素(os_handle_touch 消费成功)"时触发一次,
             * 由此实现"点中才震、点空白无反应"(见 os_core/main 分发处). */
        }
        break;

    case TG_PRESS:
        if (pt.pressed) {
            s_tg_x = pt.x;
            s_tg_y = pt.y;
            /* V1.0.9x: 游戏列表/收藏区域长按(按住即成, 无需松手) → 立即投递 LONG_PRESS.
             * 满足"长按收藏不用放手"的需求. 需按住期间几乎不动(<TOUCH_STILL_MAX),
             * 且按下点在状态栏以下(游戏区). 已触发或正在拖动(位移超阈值)则跳过. */
            if (!s_tg_long_fired && s_tg_sy0 >= TOUCH_STATUS_H &&
                (int32_t)(t - s_tg_t0) >= TOUCH_LONG_PRESS_MS &&
                (int32_t)(t - s_tg_t0) < 9000) {
                int dx = s_tg_x - s_tg_x0, dy = s_tg_y - s_tg_y0;
                int adx = dx < 0 ? -dx : dx;
                int ady = dy < 0 ? -dy : dy;
                if (adx < TOUCH_STILL_MAX && ady < TOUCH_STILL_MAX) {
                    s_tg_long_fired = true;
                    s_tg_state = TG_WAIT_RELEASE;
                    s_tap_x = s_tg_sx0; s_tap_y = s_tg_sy0;
                    ESP_LOGI(TAG, "长按 %lums -> LONG_PRESS(按住) @ %d,%d",
                             (unsigned long)(t - s_tg_t0), s_tap_x, s_tap_y);
                    out = MENU_ACTION_LONG_PRESS;
                }
            }
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
                    ESP_LOGW(TAG, "触摸状态超时 9s 复位 (可能芯片读失败导致状态残留)");
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
            } else if (dt >= TOUCH_LONG_PRESS_MS && s_tg_sy0 >= TOUCH_STATUS_H &&
                       adx < TOUCH_SWIPE_MIN && ady < TOUCH_SWIPE_MIN) {
                /* V1.5.x: 长按时长已到但手指微移 (20~40px) — 用户意图是长按
                 * (收藏/拖动), 位移恰好破坏 LONG_PRESS 判定. 若下沉到 CONFIRM 会
                 * 误打开游戏/误触操作 (暴龙机"自动打开"的次要路径), 这里直接吞掉. */
                ESP_LOGI(TAG, "长按微移 %lums dx=%d dy=%d -> 忽略 (防误触)", (unsigned long)dt, adx, ady);
            } else if (adx < TOUCH_SWIPE_MIN && ady < TOUCH_SWIPE_MIN) {
                /* 位移小 = 点击 (tap 坐标用旋转后的逻辑坐标, 供阅读器分区命中).
                 * 震动: 按下瞬间已触发 (TG_IDLE 按下沿), 快速点击(<0.5s)不再补震,
                 * 避免"咔哒"两次; 按住 >0.5s 松手统一补"弹起" (见下方释放收尾). */
                out = MENU_ACTION_CONFIRM;
                s_tap_x = s_tg_sx0; s_tap_y = s_tg_sy0;
                ESP_LOGI(TAG, "触摸点击 (%lums) raw=%d,%d -> %d,%d",
                         (unsigned long)dt, s_tg_x0, s_tg_y0, s_tap_x, s_tap_y);
            } else if (s_swipe_turn && ady < TOUCH_SWIPE_MIN * 2 &&
                       dx != 0 && adx >= TOUCH_SWIPE_MIN) {
                /* 阅读器横滑翻页: 横位移显著(≥40)且竖向较小(<80)视为左右滑动.
                 * 屏逻辑坐标已由 input 旋转, 此处用物理方向 (横向滑动朝向) 判左右.
                 * 向左滑(dx<0)=上一页, 向右滑(dx>0)=下一页. */
                out = (dx < 0) ? MENU_ACTION_LEFT : MENU_ACTION_RIGHT;
                ESP_LOGI(TAG, "阅读器横滑 (%d,%d)->(%d,%d) dx=%d -> %s",
                         s_tg_x0, s_tg_y0, s_tg_x, s_tg_y, dx,
                         (dx < 0) ? "LEFT" : "RIGHT");
            } else if (s_back_swipe &&
                       s_tg_sy0 >= TOUCH_BACK_EDGE_Y &&  /* 起手在底部带(y>=285, 15px=5%屏高) */
                       dy <= -TOUCH_BACK_SWIPE_DY &&
                       adx < TOUCH_SWIPE_MIN) {          /* V1.0.xx: 竖直上滑(横向<40px)才算返回 */
                /* V1.0.68: 物理屏幕底部中间区域往上滑 -> 返回 (V1.0.90: 跟随屏幕旋转).
                 * V1.0.94+: 1s 内连续两次上滑 -> 第二次投递确认, 即"一次返回一次确认"退出
                 * (游戏/全屏页: 第1次上滑弹退出确认框, 第2次上滑确认退出, 不再强制 HOME).
                 * V1.0.xx: 手势"整体判断" — 起点在底部带 + 竖直上滑位移足够 才识别为返回;
                 * 底部带内其余位移一律视为点击 (见下方兜底分支), 不再"先画区域截止"吞事件.
                 * 全局生效: 所有页面底部控件 (按钮/进度条等) 的点击不再受手势规则影响. */
                static uint32_t s_last_sw_ms = 0;
                bool double_sw = s_last_sw_ms && (int32_t)(t - s_last_sw_ms) <= TOUCH_DOUBLE_SWIPE_MS;
                s_last_sw_ms = t;
                out = double_sw ? MENU_ACTION_CONFIRM : MENU_ACTION_BACK;
                ESP_LOGI(TAG, "底部上滑 (%d,%d)->(%d,%d) -> %s",
                         s_tg_x0, s_tg_y0, s_tg_x, s_tg_y,
                         double_sw ? "CONFIRM(退出)" : "BACK");
            } else if (s_tg_sy0 >= TOUCH_BACK_EDGE_Y) {
                /* V1.0.xx: 起点在底部带但非上滑手势 (点击时手指轻微滑动≥40px / 斜滑 / 横滑):
                 * 整段手势整体判断后仍视为点击, 不再落入"非手势位移"被吞 (NONE).
                 * 修: 底部按钮/进度条等控件位于 y>=285 手势带内, 点击稍带位移就要多点几次.
                 * 全局生效: 任何页面底部区域的点击都不再被手势规则吞掉. */
                out = MENU_ACTION_CONFIRM;
                s_tap_x = s_tg_sx0; s_tap_y = s_tg_sy0;
                ESP_LOGI(TAG, "底部带非上滑位移 (%d,%d)->(%d,%d) -> 点击",
                         s_tg_x0, s_tg_y0, s_tg_x, s_tg_y);
            }
            /* V1.0.8x: 已删除左右/上下滑动 -> 方向键. 打开仅靠点击确定位置. */
            /* V1.4.x: 按住 >0.5s 松手 → 补一次"弹起"震动 (机械按键按下去/放上来
             * 的段落感; 快速点击按下时已震, 此处 <0.5s 不补, 保证一次点击一声).
             * 主菜单/软件管家页 (os_core 置 ctx_block) 不补. */
            if (!s_key_sim_ctx_block && dt > TOUCH_HOLD_VIB_MS) vibrator_click();
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
void input_set_swipe_back(bool enable) { s_back_swipe = enable; }
bool input_touch_last_action(void) {
    return s_last_action_touch;
}

menu_action_t input_get_action(void) {
    /* V1.0.68: 记录最近一次动作来源 (触摸 vs 物理键/手柄), 供确认框区分
     * "底部上滑(BACK)"与"物理 BACK 键" (上滑再划一次=确认, 物理 BACK=取消) */
    s_last_action_touch = false;
    for (int i = 0; i < 3; i++) {
        menu_action_t a = tick(&s_btns[i]);
        if (a != MENU_ACTION_NONE) { s_tap_x = s_tap_y = -1; s_last_any_ms = now_ms(); fb_game_exit_vib(a); return a; }
    }
    /* 游戏中 (nav 关闭 且 gamemode 生效): 手柄"退出到菜单"键(F_EXIT→HOME)仍常开,
     * 作为游戏退出键. 手柄"返回"键(F_BACK)在游戏内不作为退出键, 不映射成 BACK
     * (用户规范: 遥控返回键 ≠ 硬件返回键; 退出确认只由 F_EXIT 与硬件返回键触发).
     * 仅真正进入游戏(引擎置 gamemode=true)才允许; 按键映射(只关 nav、不置 gamemode)
     * 时 F_EXIT 照常被当作被映射的键捕获, 绝不误触发退出.
     * 边沿触发 + released 守卫(防连接伊始卡键); 菜单模式则交给下方导航块统一处理.
     * 硬件返回键(物理 BOOT)仍走 tick() 产生 MENU_ACTION_BACK. */
    if (s_gamepad_gamemode && !s_gamepad_nav_enabled && bt_manager_is_connected()) {
        static bool e_prev = false, e_released = true;
        bool e = bt_manager_is_key_pressed(F_EXIT);
        if (!e) e_released = true;
        if (e && !e_prev && e_released) {
            e_released = false; s_tap_x = s_tap_y = -1;
            s_last_any_ms = now_ms();
            fb_game_exit_vib(MENU_ACTION_HOME); return MENU_ACTION_HOME;
        }
        e_prev = e;
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
        /* V1.0.39+ : 摇杆强制 4 方向键. 方向键按住时带自动重复(450ms后每110ms一次,
         * 最长1.5s)实现菜单持续滚动; 回正=松手, A/B/HOME 仅边沿. */
        static const struct { func_t key; menu_action_t act; bool rpt; } nav[] = {
            /* 用户要求: 菜单按住方向键要持续滚动 → 方向键 rpt=true
             * (重复: 按下后 450ms 开始, 每 110ms 一次, 单次最长 1.5s 防卡键).
             * LEFT 长按 500ms 的 LONG_LEFT 仍保留(菜单忽略、gam4980 当确认补充键). */
            { F_UP,    MENU_ACTION_UP,      true },
            { F_DOWN,  MENU_ACTION_DOWN,    true },
            { F_LEFT,  MENU_ACTION_LEFT,    true },
            { F_RIGHT, MENU_ACTION_RIGHT,   true },
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
                    s_last_any_ms = now_ms();
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
                    s_last_any_ms = now_ms();
                    return MENU_ACTION_LONG_LEFT;
                }
                /* 长按自动重复(仅方向键): 按住期间持续重复(无 hold-cap), 满足"一直按着方向键一直跑"
                 * (后续加震动需长按持续反馈). released_seen 守卫仍在, 防"连接好就卡住". */
                if ((int32_t)(now - st[i].repeat_next_ms) >= 0) {
                    st[i].repeat_next_ms = now + NAV_REPEAT_RATE;
                    s_tap_x = s_tap_y = -1;
                    s_last_any_ms = now_ms();
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
            s_last_any_ms = now_ms();
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
        s_last_any_ms = now_ms();
        fb_game_exit_vib(ta);          /* V1.2.x: 游戏内底部上滑=返回 → 游戏震动 */
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
    if (s_touch_blocked) return false;   /* 休眠禁触屏: 无点击 */
    if (s_tap_x < 0) return false;
    if (x) *x = s_tap_x;
    if (y) *y = s_tap_y;
    s_tap_x = s_tap_y = -1;
    s_last_any_ms = now_ms();   /* 点按被消费即视为活动, 刷新统一休眠时钟 */
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
    if (s_touch_blocked) return false;   /* 休眠禁触屏: 无触摸坐标 */
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
        s_last_any_ms = now_ms();   /* 有手指按下 → 视为活动, 重置休眠时钟 */
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

/* V1.7.x: 无副作用"当前按住坐标"查询 (只读 s_touch_down/s_touch_sx/sy, 不消费任何
 * 状态机). 供"按住反黑"等按压反馈使用 — 不受 input_get_touch_pos 的 multi-consumer
 * 竞态影响 (main.c 每帧已调用过一次, 弹窗/页面 render 再调会破坏 s_o_armed 状态). */
bool input_touch_now(int *x, int *y) {
    if (s_touch_blocked || !s_touch_down) return false;
    if (x) *x = s_touch_sx;
    if (y) *y = s_touch_sy;
    return true;
}

/* V1.0.xx: 多点触控 — 返回当前触摸点 (最多 2 点, 已映射屏幕坐标).
 * 复用触摸缓存 (touch_read_once), 须与 input_get_touch_pos/input_get_action 同帧读取. */
void input_get_touch_multi(tp_point_t pts[TP_MAX_POINTS], int *count) {
    if (s_touch_blocked) { if (count) *count = 0; return; }   /* 休眠禁触屏: 无多点 */
    if (count) *count = 0;
    if (!pts) return;
    for (int i = 0; i < TP_MAX_POINTS; i++) pts[i].pressed = false;
    tp_point_t p0;
    touch_read_once(&p0);   /* 确保本帧已刷新缓存 (含全部触点) */
    int c = s_touch_cached_count;
    if (c <= 0 || !s_touch_cached.pressed) {
        return;
    }
    int n = (c > TP_MAX_POINTS) ? TP_MAX_POINTS : c;
    if (count) *count = n;
    for (int i = 0; i < n; i++) {
        int sx = s_touch_multi[i].x, sy = s_touch_multi[i].y;
        touch_map_screen((int16_t)sx, (int16_t)sy, &sx, &sy);
        rotate_screen_coord(&sx, &sy);
        pts[i].pressed = true;
        pts[i].x = (int16_t)sx;
        pts[i].y = (int16_t)sy;
    }
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
        if (!gb_sel) gb_sel = bt_manager_is_phys_pressed(P_BTN_9);
        if (!gb_start) gb_start = bt_manager_is_phys_pressed(P_BTN_10);
        if (gb_sel)   j &= ~(1 << 2); /* Select */
        if (gb_start) j &= ~(1 << 3); /* Start */
    }
    /* 设备物理键兼容: KEY(GPIO18)=A, BOOT(GPIO0)=B.
     * 注意: s_btns[0]=BOOT(GPIO0), s_btns[1]=KEY(GPIO18), 索引勿反. */
    if (input_is_held(1)) j &= ~(1 << 0);  /* KEY → A */
    if (input_is_held(0)) j &= ~(1 << 1);  /* BOOT → B */
    /* V1.0.67: 网页手柄 (WiFi AP, 手机浏览器) */
    if (web_gamepad_is_running()) {
        j &= web_gamepad_get_joypad_state();
    }
    /* V1.0.68: 游戏内屏幕虚拟按键 (游戏设置里开启) */
    if (virtual_keys_is_enabled()) {
        /* V1.0.xx: 多点触控 — 第一指控方向/摇杆, 第二指只控 A/B/SEL/STA.
         * 两指同时按下也不互相抢占, 保证格斗等游戏可用虚拟手柄. */
        tp_point_t pts[TP_MAX_POINTS];
        int c = 0;
        input_get_touch_multi(pts, &c);
        bool d0 = (c >= 1) && pts[0].pressed;
        bool d1 = (c >= 2) && pts[1].pressed;
        int x0 = d0 ? pts[0].x : 0;
        int y0 = d0 ? pts[0].y : 0;
        int x1 = d1 ? pts[1].x : 0;
        int y1 = d1 ? pts[1].y : 0;
        j &= virtual_keys_poll_multi(x0, y0, d0, x1, y1, d1);
    }
    /* V1.2.x: 游戏震动 — 仅游戏模式(引擎已置 gamemode)下, 检测 joypad 上升沿
     * (某键从松开→按下) 触发一次游戏震动 ("按一下震动一下"). 按住不重复,
     * 松手再按才再震. 覆盖 上下左右/A/B/Select/Start 与手柄/虚拟按键. */
    static uint8_t s_jp_prev = 0xFF;
    if (s_gamepad_gamemode) {
        uint8_t pressed = (uint8_t)(s_jp_prev & (uint8_t)(~j));   /* 本帧新按下的位 (1→0) */
        s_jp_prev = j;
        if (pressed) {
            /* 引擎优化开关 (触屏/手柄/虚拟按键在游戏模式统一反应):
             *  - 按键点击音 (按键声音开关)
             *  - 游戏操作震动 (触摸震动开关) */
            if (s_eng_sound) tone_play_effect(TONE_EFFECT_CONFIRM);
            if (s_eng_vib)   vibrator_tap_game(30, 120);
        }
    } else {
        s_jp_prev = j;   /* 非游戏态: 仅同步, 不产生震动 */
    }
    /* 任意游戏键按住 (低电平有效: 位=0 即按下) = 活动中, 刷新休眠计时.
     * 这样"正在玩不睡, 放下手柄 3 分钟睡", 对所有游戏界面一致生效. */
    if (j != 0xFF) input_mark_activity();
    return j;
}
