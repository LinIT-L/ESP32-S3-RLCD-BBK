/**
 * @file input.c
 * @brief 按键输入处理 - 非阻塞状态机: 短按/长按
 */
#include "input.h"
#include "board_pins.h"
#include "hw_cfg.h"
/* 按键引脚 ★2026-10-13 新载板: SW1=GPIO1(PWR) SW2=GPIO0(右) SW3=GPIO4(左/确认) */
#define BTN_GPIO_LEFT   (board_pins_get()->btn_left)   /* ★ 按板型表 */
#define BTN_GPIO_RIGHT  (board_pins_get()->btn_right)
/* V1.0.68: 软关机键 GPIO1: 短按=确认, 长按0.5s=返回主菜单, 长按2s=软关机 */
#define BTN_GPIO_PWR    (board_pins_get()->btn_pwr)
#include "power_mgr.h"   /* [电源收拢] 电源管理(power_mgr): 开机键唤醒引脚初始化(唯一唤醒源) */
#define POWER_HOLD_MS   2000
#define PWR_SHORT_MS    500    /* V1.5.x: 开关键短按判定阈值 — 按住<0.5s 松手=锁屏(短按);
                                * 达到 2s(POWER_HOLD_MS) 由 input_power_should_sleep 弹关机确认 */
#include "driver/gpio.h"
#include "driver/rtc_io.h"   /* ★ 2026-11-22: 解 RTC 侧 pad 锁 (isolate / hold) */
#include "soc/gpio_reg.h"    /* ★ 2026-11-22: 直接读输出使能位, 判断按键脚是否被抢成输出 */
#include "soc/gpio_periph.h" /* ★ 2026-11-23: GPIO_PIN_MUX_REG —— 读 IO_MUX 的 FUN_IE/SLP_* */
#include "soc/io_mux_reg.h"  /* ★ 2026-11-23: FUN_IE / FUN_PU / SLP_SEL / SLP_IE 位定义 */
#include "soc/rtc_io_reg.h"  /* ★ 2026-11-23: RTC_IO_TOUCH_PADn_MUX_SEL —— pad 接数字域还是 RTC 域 */
#include "soc/soc.h"         /* REG_READ */
#include "esp_pm.h"          /* V1.5.x: 游戏锁频 (PM 降频下锁 240MHz 防卡顿) */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"   /* ★ 2026-10-31: 触摸采样互斥 (多任务共读 GT911) */
#include "esp_log.h"
#include "esp_timer.h"       /* ★ 2026-11-22: 电源键边沿/识别延迟测量 (µs 级时间戳) */
#include "bt_manager.h"
#include "bt_kbd.h"      /* 蓝牙键盘 1:1 输入: 动作码 BTK_ACT_* 与逐键边沿 */
#include "touch_panel.h"
#include "web_gamepad.h"
#include "virtual_keys.h"
#include "vibrator.h"   /* V1.2.x: 游戏震动 (joypad 边沿 / 退出键) */
#include "tone_player.h" /* 引擎按键点击音 (引擎优化"按键声音"开关) */
#include "gesture_back.h" /* 系统导航手势识别器: 底部上滑=返回 / 状态栏长按=回桌面 */

#define TAG "INPUT"
#define DEBOUNCE_MS 30
#define LONG_PRESS_MS 500

/* 引擎优化开关 (page_engine 通过 input_set_engine_* 写入):
 *  - 引擎按键点击音 s_eng_sound
 *  - 引擎游戏操作震动 s_eng_vib (仅游戏模式防线, 不碰 UI/按键模拟震动)
 *
 * ★ 2026-11-23 linit「所有按键声音默认全部关闭」: 按键点击音默认 **关**。
 *   这里只是兜底默认值 (egs_load() 会用 NVS 里的用户选择覆盖它); 若用户从未
 *   进过引擎设置页, 保持关 —— 之前是 true, 等于"没设置过就吵"。
 *   震动不含在"按键声音"里, 默认仍为开 (用户本次只要求关声音)。 */
static bool s_eng_sound = false;
static bool s_eng_vib   = true;
void input_set_engine_sound(bool on) { s_eng_sound = on; }
void input_set_engine_vib(bool on)   { s_eng_vib   = on; }

/* ============ 蓝牙键盘 1:1 输入 (2026-11-22) ============
 * 设计要点 (为什么不是"再写一套按键状态机"):
 *   键盘与手柄的**导航语义完全一致** (方向/确定/返回), 差别只在键盘还有字符。
 *   所以这里不新建状态机, 而是把键盘的导航键**合成到一个位图**里, 由 menu_action
 *   的现有轮询一起消费 (见 input_poll_nav 里 `|| kbd_nav_raw(...)`) ——
 *   防抖/重复/长按全部复用同一套, 不会出现"手柄和键盘手感不一致"。
 *   字符键则直投文本焦点 (sink), 与导航互不干扰。 */
static input_text_sink_t s_text_sink = NULL;
static uint32_t now_ms(void);   /* 定义在下方; 键盘事件先用到 */
static volatile uint32_t s_kbd_nav_bits = 0;   /* bit n = func_t n 当前被键盘按住 */
/* ★ 2026-11-22: 蓝牙键盘最近一次"任意键按下"的时刻 (含被下面丢弃的字符键).
 * 屏保"按任意硬件键退出"要认字母键 —— 而字母键在无文本焦点时走不到任何动作,
 * 只记时刻就够 (用电平/计数会因释放事件丢失而卡在"一直按着")。 */
static volatile uint32_t s_kbd_last_press_ms = 0;

static int kbd_act_to_func(int act)
{
    switch (act) {
    case BTK_ACT_UP:    return F_UP;
    case BTK_ACT_DOWN:  return F_DOWN;
    case BTK_ACT_LEFT:  return F_LEFT;
    case BTK_ACT_RIGHT: return F_RIGHT;
    case BTK_ACT_ENTER: return F_CONFIRM;
    case BTK_ACT_ESC:   return F_BACK;
    default:            return -1;
    }
}

/* 键盘按住态查询 (供 input_poll_nav 与手柄状态 OR 起来) */
static bool kbd_nav_raw(func_t f)
{
    if (f < 0 || f >= FUNC_MAX) return false;
    return (s_kbd_nav_bits & (1u << (int)f)) != 0;
}

void input_text_sink_set(input_text_sink_t cb)
{
    s_text_sink = cb;
    ESP_LOGI(TAG, "文本输入焦点: %s", cb ? "已注册" : "空");
}

void input_text_sink_clear(input_text_sink_t cb)
{
    /* 只清自己: 防止 A 页面退出时把 B 页面刚注册的焦点清掉 (覆盖页/快速切页) */
    if (s_text_sink == cb) s_text_sink = NULL;
}

bool input_text_has_sink(void) { return s_text_sink != NULL; }

void input_kbd_event(int act, char ch, bool pressed)
{
    if (act == BTK_ACT_IGNORE) return;

    /* ★ 2026-11-22: 先记"键盘上有键按下" —— 必须放在下面所有分流之前。
     *   原来字符键在无文本焦点时被直接丢掉, 屏保态按字母键就完全没有痕迹,
     *   用户看到的就是"键盘按了没反应、退不出壁纸"。 */
    if (pressed) {
        uint32_t t = now_ms();
        s_kbd_last_press_ms = t ? t : 1u;   /* 0 保留给"从未按过" */
    }

    /* ① 有文本焦点 → 全交给它 (方向键用于光标移动, 不能同时触发系统导航) */
    if (s_text_sink) {
        if (pressed) s_text_sink(act, ch);   /* 松开不投递: 文本层只关心"打进来了什么" */
        return;
    }

    /* ② 无文本焦点 → 只让导航键生效; 字符键丢弃 (没有输入目标) */
    int f = kbd_act_to_func(act);
    if (f < 0) return;
    if (pressed) s_kbd_nav_bits |=  (1u << f);
    else         s_kbd_nav_bits &= ~(1u << f);
}

/* ==== 触摸手势识别参数 ====
 * 位移阈值用原始像素(与分辨率无关); 区域判定用屏幕坐标(400x300). */
#define TOUCH_TAP_MAX_MS     300   /* 单击最长时长 */
#define TOUCH_SWIPE_MIN      40    /* 滑动最小位移(原始像素), 低于算点击 */
#define TOUCH_LONG_MS        3000  /* V1.0.68: 状态栏长按 3 秒 = 返回主菜单 */
#define TOUCH_LONG_PRESS_MS  2000  /* V1.0.9x: 游戏名/收藏栏长按 2s = 收藏/取消收藏; 应用管理长按弹窗也走此阈值 */
#define TOUCH_HOLD_VIB_MS    500   /* V1.4.x: 按住超过此毫秒松手 → 补一次"弹起"震动 (按键模拟) */
#define TOUCH_STILL_MAX      20    /* 长按判定时允许的抖动位移 */
#define TOUCH_STATUS_H       24    /* 状态栏高度: 长按此区域(按下点 y<24) = 返回主菜单 */
/* ==== 底部上滑返回 (2026-09-19 手感优化) ====
 * 参考开源做法:
 *   - Android 系统手势导航: 底部 20dp 系统区, 但**区内轻点仍透传给应用**, 只接管滑动
 *   - Android SwipeBackLayout: ViewDragHelper 的 edge tracking —— **只在边缘捕获**, 中间区完全不碰
 *   - LVGL: gesture_min_velocity(相邻点差) + gesture_min_distance 双判据; scroll_limit 内视为点击
 *   - Android Slidr: edgeSize=18% 屏宽, velocityThreshold=2400px/s, distanceThreshold=25% 屏宽
 *
 * 本实现据此保持"零侵入":
 *   ① 判定时机不变 —— 仍在**松手瞬间**判定, 点击/长按/拖动/阅读器横滑逻辑一行不改
 *   ② 起手 x 范围不变 —— 仍只认中间 150px 带(125..275), 左右两侧留给摇杆与游戏
 *   ③ 滑动中只多画一条**纯显示**的指示条(不消费事件、不改事件流向, 与 toast 同层)
 *   ④ 位移不足仍是点击 —— 底部控件(MP3 按钮/进度条等)照常可点
 * 比原来好的地方: 边缘带加高(15→50px 更容易起手) + 加速度判据(快速轻扫也认) + 有过程反馈。 */
#define TOUCH_BACK_EDGE_Y    250   /* 起手须在底部 50px 带内 (≈17%屏高; 原 285=15px 太窄, 容易滑空) */
#define TOUCH_BACK_EDGE_MINX 125   /* 起手 x 下限: 只认中间 150px 带, 左右两侧留给摇杆/游戏(维持不变) */
#define TOUCH_BACK_EDGE_MAXX 275   /* 起手 x 上限 */
#define TOUCH_BACK_DIST      45    /* 第1档: 上滑 ≥45px 松手 = 返回 */
#define TOUCH_BACK_DIST_FAST 25    /* 轻扫: 上滑 ≥25px 且够快 = 返回 (flick, 参考 LVGL 速度判据) */
#define TOUCH_BACK_DIST2     80    /* 第2档: 上滑 ≥80px 松手 = 强制退出 (一次滑到底, 不必滑两次) */
#define TOUCH_BACK_VEL_MIN   600   /* 轻扫速度阈值 px/s (Slidr 手机端 2400 按屏高换算到 300px 屏) */
#define TOUCH_BACK_HINT_MIN  18    /* 位移超过此值才显示跟手指示条 (避免点击/微动时闪条) */
#define TOUCH_DOUBLE_SWIPE_MS 1000  /* 1s 内连续两次上滑 = 强制退出 (与第2档并存, 两条路都通) */

/* 本机是否带触摸屏 (按固化板型判定, 见 board_has_touch_by_design), 决定物理键语义分支.
 * 有触摸:  BOOT=返回上级/回主菜单, KEY=确认/收藏
 * 无触摸:  BOOT=下一步/返回上级,  KEY=确认/直达蓝牙映射 */
static bool s_has_touch = false;

/* V1.4.x: 上下文屏蔽按键模拟震动 (按下/松手). 由 os_core 每帧驱动:
 * 主菜单/软件管家页 = true (拖动图标/管理列表不咔哒), 弹窗/二级页/程序 = false. */
static bool s_key_sim_ctx_block = false;
void input_set_key_sim_ctx_block(bool block) { s_key_sim_ctx_block = block; }

/* V1.7.x (2026-09-28): 本次触摸释放是否已判定为系统导航手势 (底部上滑 BACK / 强退 CONFIRM).
 * 导航手势的触觉统一由动作处理侧发出 (os_handle_action 的 UI 震动), 因此松手时的
 * 通用"弹起"震动 (dt > TOUCH_HOLD_VIB_MS) 要跳过 —— 否则"按住 >0.5s 再上滑返回"
 * 会响两下 (弹起 + 动作). 用户规范: 一次上滑返回只响后面那一下. */
static bool s_tg_nav_release = false;

/* V1.1.0: 底部上滑=返回 的开关改由 gesture_back 模块 (gb_enabled) 统一管理,
 * 由 os_push/os_pop 按页的 swipe_back_disabled 设置; 见 gesture_back.c/.h. */

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

/* 前向声明: input_init() 在定义之前就要创建这个 5ms 扫描任务 */
static void input_key_task(void *arg);
static bool s_key_task_started = false;

/* ★ 2026-11-22: 物理键动作流水 (供 `keys` 快照判读)
 *   s_act_cnt      开机以来物理键产生的动作**总数** —— 按了键这个数不涨, 就说明动作
 *                  压根没从输入层出来(引脚/状态机问题), 与上层分发无关;
 *   s_last_act     最近一个动作码 (menu_action_t), 配合 s_last_act_ms 判断"刚那个键生效没";
 *   s_last_act_ms  那个动作发生的时刻。
 *   用途: "按键没反应"时让用户按一下再跑 keys, 一眼看总数有没有变。 */
static uint32_t s_act_cnt     = 0;
static int      s_last_act    = 0;
static uint32_t s_last_act_ms = 0;

/* 本板是否带触摸屏 —— 只看**固化后的板型 ID**, 不看触摸芯片探测结果。
 *   板型由 hw_cfg 识别一次写进 NVS, 之后每次开机直接读存档(0 次 I2C);
 *   换板型只能重刷完整固件(cfg_ver 变 ⇒ 存档作废 ⇒ 重新识别一次)。
 *   微雪原版的 tp_sda/tp_scl 只是"留给用户自己加触摸"的字段, 不能据此判有触摸。 */
static bool board_has_touch_by_design(void) {
    switch (board_pins_get()->id) {
    case BOARD_ID_XIAOZHI:   /* 一体版: 屏 + 触摸同一片排线 */
    case BOARD_ID_STD:       /* 标准扩展版: 15/7/17/2 带触摸 */
        return true;
    default:                 /* 微雪原版: 原板无触摸 */
        return false;
    }
}

/* 按机型配置物理键语义 —— 只在 input_init 里调一次, 之后固定不变.
 * 2026-11-21 linit 定规: "固定按键, 不要乱自动识别; 自动识别只要一次"。
 * 历史 bug: 这里曾在应用管家页按触摸探测结果重配 0/4 键, 触摸一抖动(被 recover 成 NONE)
 * 就把 BOOT 从"返回"改成"右向" ⇒ 用户看到"返回/确认按不动"。 */
static void input_cfg_keys(void) {
    if (s_has_touch) {
        /* 有触摸机型: BOOT 短按=返回, 长按3s=回主菜单; KEY=确认 */
        s_btns[0] = (btn_ctx_t){ BTN_GPIO_RIGHT, ST_IDLE, 0, 3000, MENU_ACTION_BACK, MENU_ACTION_HOME, false, "BOOT" };
        s_btns[1] = (btn_ctx_t){ BTN_GPIO_LEFT,  ST_IDLE, 0, 2000, MENU_ACTION_CONFIRM, MENU_ACTION_BT_SEARCH, false, "KEY" };
    } else {
        /* 无触摸机型: BOOT 短按=右向, 长按=返回; KEY=确认 */
        /* ★ 2026-10-17 (社区反馈): long_ms 原为 0 ⇒ 走全局 500ms ⇒ 极易误触"返回";
         *   无触摸机型只有几个键, 长按必须明显区别于短按 ⇒ 改 2000ms。 */
        s_btns[0] = (btn_ctx_t){ BTN_GPIO_RIGHT, ST_IDLE, 0, 2000, MENU_ACTION_RIGHT, MENU_ACTION_BACK, false, "BOOT" };
        s_btns[1] = (btn_ctx_t){ BTN_GPIO_LEFT,  ST_IDLE, 0, 2000, MENU_ACTION_CONFIRM, MENU_ACTION_BT_SEARCH, false, "KEY" };
    }
}
/* ★ 2026-11-21: input_recheck_touch() 已删除 —— 键位由固化板型在 input_init 定死,
 *   不需要任何运行期重判; 它后来变成了空函数且无人调用。 */

/* V1.0.41: 手柄导航键开关. 按键映射期间设为 false, 防止手柄按键产生 action
 * 干扰映射流程 (如映射"返回"键时 MENU_ACTION_BACK 会终止映射). */
static bool s_gamepad_nav_enabled = true;
void input_set_gamepad_nav_enabled(bool enabled) {
    /* ★ 2026-11-24: 关闭期间(按键映射/游戏内) bt_manager 仍会锁存上升沿,
     *   重新打开时若不清, 会立刻补投一个映射/游戏期间按下的旧键 —— 表现为
     *   "退出映射就自己走一格"。 */
    if (enabled && !s_gamepad_nav_enabled) bt_manager_clear_key_edges();
    s_gamepad_nav_enabled = enabled;
}

/* V1.2.x: 游戏模式标志. 引擎运行循环进入游戏置 true, 退出置 false.
 * 详情见 input.h. 区分"nav 关闭"的两种场景: 游戏中(允许返回/退出键) 与 按键映射中
 * (返回键应被当作被映射的键捕获, 不能误触发退出). */
static bool s_gamepad_gamemode = false;
/* V1.3.x: 游戏模式变化回调 (暗黑UI模式等按游戏态切换反色用, 游戏阻塞主循环需即时通知) */
static input_gamemode_cb_t s_gamemode_cb = NULL;
void input_set_gamepad_gamemode(bool enabled) {
    if (s_gamepad_gamemode == enabled) return;
    /* ★ 2026-11-22 linit「游戏时候没必要锁跑 240MHz 能不能自调整」:
     *   原来这里进游戏就 acquire ESP_PM_CPU_FREQ_MAX ("bbk_game"), 全程钉死 240MHz,
     *   暂停菜单/看对话/慢节奏场景也在满频烧电。
     *   现在**不持常驻锁**, 完全交给 DFS 按真实负载调:
     *     · 引擎每帧算满(几乎没有 IDLE) → DFS 自己保持 240MHz, 不会掉帧;
     *     · 暂停/菜单/慢节奏(有 IDLE 窗口) → DFS 自动降到 80MHz 省电。
     *   按键交互仍走 input_pm_boost (任意输入 400ms 满频), 由
     *   input_get_held_gb_joypad() 每帧调 input_pm_boost_tick() 负责释放 ——
     *   引擎是阻塞循环, 主循环的 tick 不会跑, 必须在引擎采样点里释放。 */
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

/* ★ 2026-11-22: 任意硬件键是否按着 —— 屏保/动态壁纸"按任意键退出"的判据.
 * 三类来源分别取最可靠的形态:
 *   ① 物理三键: 直接读 GPIO 电平 (最底层, 不经过任何状态机, 按下去就一定为真);
 *   ② 手柄: bt_manager_any_key_pressed() 遍历 phys 全域 —— 含肩键/Start/Select
 *      这些没映射成功能键、平时不产生动作的按钮;
 *   ③ 蓝牙键盘: 只认"最近 300ms 内有按下事件" —— 键盘的按住态没有可靠位图
 *      (字符键会被丢弃), 用时间窗比计数更不容易卡死; 主循环 32ms 一轮,
 *      按键按下后必然在窗口内被看到。
 * 不含触摸: 触摸唤醒由 wp_screensaver_touch_wake_enabled() 独立控制。 */
bool input_any_hw_pressed(void)
{
    for (int i = 0; i < 3; i++) {
        if ((int)s_btns[i].gpio < 0) continue;      /* 该板无此键/同脚禁用 */
        if (gpio_get_level(s_btns[i].gpio) == 0) return true;   /* 低电平=按下 */
    }
    if (bt_manager_any_key_pressed()) return true;
    uint32_t kt = s_kbd_last_press_ms;
    if (kt != 0 && (uint32_t)(now_ms() - kt) < 300u) return true;
    return false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 按键"整组失效"现场诊断 + 自愈 (2026-11-22, 排查"确认/返回 按下去没反应")
 *
 * 【症状】运行中(非重启)确认键/返回键同时失去响应, 重启后立刻恢复。
 *
 * 【已知基线, 实测】正常时 `keys` 快照:
 *     [0] BOOT GP0  level=1 dir=0(输入) state=IDLE
 *     [1] KEY  GP4  level=1 dir=0(输入) state=IDLE
 *     [2] PWR  GP44 level=1 dir=0(输入) state=IDLE
 * ⚠ 曾经把 power_mgr 启动日志里的 "(PWR44=0 LEFT4=0 RIGHT0=0)" 当成"三键电平全低"
 *   的证据 —— **那是误读**: 它打印的是 ext1 唤醒掩码 `esp_sleep_get_ext1_wakeup_status()`,
 *   非深睡唤醒时 ws=0, 所以三项必然显示 0, 与引脚电平无关。真正电平看
 *   `INPUT: GPIO ret=0 K1 B1 P1` 或本文件的 `keys` 快照。
 *
 * 【为什么仍保留这道自愈】三根独立的键物理上不可能同时按下, 一旦真读到"全低",
 * 只能是 pad 层出问题(上拉被关 / 输入通路被切断 / 被别的驱动配成输出)。
 * 此时状态机会各自走到 ST_PRESSED → ST_LONG_FIRED 然后**永远等释放**, 而电平
 * 永不回高 ⇒ 按键彻底没反应, 只有重启重新 gpio_config 才恢复。
 * 但**故障现场的引脚状态还没抓到**(用户重启后即恢复), 所以这条自愈是"兜底",
 * 不是已证实根因的修复。复现时先跑 `keys`, 看 level/dir/state + 自愈次数定性。
 *
 * 【自愈判据】三键同时低电平持续 ≥ 200ms。合法手势里不存在"三键同按"这个
 * 动作 (没有任何功能绑定), 所以这条判据不会误伤真实按键。
 * 触发后: 按 input_init 的口径重配三脚为上拉输入 + 状态机全部回 IDLE,
 * 并打 ERROR 日志 (带自愈次数), 每次故障只修一次(3s 冷却)。
 * ═══════════════════════════════════════════════════════════════════════════ */
#define KEY_ALL_LOW_MS      200u    /* 三键同低持续这么久 ⇒ 判为整组失效 */
#define KEY_HEAL_COOLDOWN   3000u   /* 自愈冷却, 防止真短路时刷屏 */
static uint32_t s_all_low_since = 0;
static uint32_t s_last_heal_ms  = 0;
static int      s_heal_cnt      = 0;

/* ★★★ 2026-11-23 现场实测: 旧判据在小智板上**永不成立** ★★★
 *   判据 A(`low == n`, 三键全低)要求三根脚同时为 0; 但小智板 PWR = GPIO44 =
 *   LTC2954 的 /INT, 空闲恒高 ⇒ low 最多到 2, n=3 ⇒ `low != n` 恒真。
 *   结果: 自愈一次都不会跑。2026-11-23 05:37 现场 keys 快照正是这样 ——
 *     BOOT GP0 level=0 LONG_FIRED / KEY GP4 level=0 LONG_FIRED / PWR GP44 level=1
 *     自愈次数=0, 而两键已经恒低 18 分钟, 用户只能重启。
 *   ⇒ 实际故障形态是"**只有 0/4 这两根 RTC 域脚**一起读到 0"(浅睡往返/掉压后
 *     pad 停在睡眠态), 必须给它单独的判据 B。
 *   阈值 20s: 没有任何合法手势需要按住一个键 20 秒(最长的是 BOOT 3s 回桌面、
 *   PWR 2s 关机弹窗), 所以不会误伤真实按键; 机械卡键也只修一次(见 s_stuck_repaired)。 */
#define KEY_STUCK_MS        20000u
static uint32_t s_low_since[3]      = {0};  /* 每键进入低电平的时刻, 0=当前为高 */
static uint8_t  s_stuck_repaired[3] = {0};  /* 该键已修过一次仍低 ⇒ 不再重复重配 */

/* 引脚当前是否被使能为**输出** (1=输出, 0=输入)。
 * IDF 没有 gpio_get_direction() 的公开查询接口, 直接读 GPIO 输出使能寄存器 ——
 * 按键脚正常必须是输入(0); 若为 1 说明被别的驱动抢配成输出了, 电平就再也读不到真实按键。 */
static int key_pin_is_output(int pin)
{
    if (pin < 0 || pin > 48) return 0;
    return (int)((REG_READ(GPIO_ENABLE_REG) >> pin) & 1u);
}

/* ★★★ 2026-11-23: 解三道 pad 锁 —— 开机与运行中自愈共用同一序列 ★★★
 *   顺序不能改: 全局冻结 → 单脚 hold → RTC hold/隔离。
 *   ⚠ 被 hold / isolate 的 pad **会静默忽略随后的 gpio_config()** (2026-11-22 实测),
 *     所以任何"重配按键脚"的动作之前都必须先跑这一遍, 否则只是看起来配成功了。
 *   正常冷启动时全部是空操作, 只在"pad 被锁"的现场才真正起作用。 */
static void input_pad_unlock_pins(const int *kp, int n)
{
    gpio_deep_sleep_hold_dis();                 /* ① 解全局 pad 冻结 */
    for (int i = 0; i < n; i++) {
        if (kp[i] < 0) continue;
        gpio_hold_dis((gpio_num_t)kp[i]);       /* ② 解单脚 hold */
        if (kp[i] <= 21) {
            /* ③ 解 RTC hold 与隔离 (rtc_gpio_isolate 的实质是关掉 FUN_IE/WPU/WPD
             *    再锁住不许改 ⇒ 数字侧恒读 0, 只有 0~21 的 RTC 域脚会中招)。
             *    ★ 2026-11-23 更正: 上面这句只对"pad 已被接到 RTC 域"的情况成立;
             *      isolate 本身不动 MUX_SEL(见 rtc_domain_sel 注释)。这里保留
             *      rtc_gpio_deinit 的真正作用是**把 MUX_SEL 清回数字域** —— 这才是
             *      浅睡往返后按键恢复的关键一步。 */
            rtc_gpio_hold_dis((gpio_num_t)kp[i]);
            rtc_gpio_deinit((gpio_num_t)kp[i]);
        }
    }
}

/* 按 input_init 的同一口径把三脚恢复成"输入 + 上拉" (不碰 PWR 与 KEY 同脚的特例) */
static void input_keys_reinit_pins(void)
{
    uint64_t mask = 0;
    int kp[3], kn = 0;
    for (int i = 0; i < 3; i++) {
        if ((int)s_btns[i].gpio < 0) continue;
        mask |= (1ULL << s_btns[i].gpio);
        kp[kn++] = (int)s_btns[i].gpio;
    }
    if (!mask) return;
    /* ★ 2026-11-23: 先解锁再配置。原来这里只有 gpio_config —— 对"pad 被浅睡/掉压
     *   锁住"的现场完全无效(配完电平照旧是 0), 自愈等于没做。 */
    input_pad_unlock_pins(kp, kn);
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    /* 与 input_init 一致: 这几个脚不参与浅睡的"禁用+浮空"切换, 否则下次浅睡又坏一次。 */
    for (int i = 0; i < kn; i++)
        if (kp[i] >= 0) gpio_sleep_sel_dis((gpio_num_t)kp[i]);
    for (int i = 0; i < 3; i++) {
        s_btns[i].state = ST_IDLE;      /* 别让状态机继续"等释放" */
        s_btns[i].press_start = 0;
    }
}

static void input_key_guard(void)
{
    uint32_t now = now_ms();
    int n = 0, low = 0;
    /* ★ 2026-11-23 复审修正: 原来是 `int stuck` 只记**第一个**超时的键。
     *   本次故障是 GPIO0 与 GPIO4 **同时**卡低 ⇒ 只标记 index 小的那个,
     *   另一个既不会被静音(每 20s 继续发 CONFIRM/BT_SEARCH), 又因
     *   s_stuck_repaired[0]=1 让判据 B 永久不再触发。改成位图逐个处理。 */
    uint8_t stuck_mask = 0;             /* bit i = 第 i 键低电平已超 KEY_STUCK_MS */
    for (int i = 0; i < 3; i++) {
        if ((int)s_btns[i].gpio < 0) { s_low_since[i] = 0; continue; }
        n++;
        if (gpio_get_level(s_btns[i].gpio) == 0) {
            low++;
            if (s_low_since[i] == 0) s_low_since[i] = now ? now : 1u;
            else if ((uint32_t)(now - s_low_since[i]) >= KEY_STUCK_MS)
                stuck_mask |= (uint8_t)(1u << i);
        } else {
            s_low_since[i] = 0;         /* 松开 ⇒ 重新起算, 并恢复"可再修"资格 */
            s_stuck_repaired[i] = 0;
        }
    }

    /* ---- 判据 A (原有): 整组全低持续 200ms ---- */
    bool heal = false;
    if (n >= 2 && low == n) {
        if (s_all_low_since == 0) s_all_low_since = now ? now : 1u;
        else if ((uint32_t)(now - s_all_low_since) >= KEY_ALL_LOW_MS) heal = true;
    } else {
        s_all_low_since = 0;            /* 只要有一根是高电平 ⇒ 正常, 清计时 */
    }
    /* ---- 判据 B (2026-11-23 新增): 单键卡低 20s ---- */
    /* 只要**任意**一个超时键还没被修过 ⇒ 就值得再修一次。 */
    if (!heal && stuck_mask) {
        for (int i = 0; i < 3; i++)
            if ((stuck_mask & (uint8_t)(1u << i)) && !s_stuck_repaired[i]) { heal = true; break; }
    }
    if (!heal) return;
    if (s_last_heal_ms && (uint32_t)(now - s_last_heal_ms) < KEY_HEAL_COOLDOWN) return;

    s_last_heal_ms  = now ? now : 1u;
    s_all_low_since = 0;
    /* 本次所有超时键都记为"已修过"; 修完仍低就不再重复重配(见 input_key_task 的静音分支)。
     * 判据 A 触发时 stuck_mask 可能为空, 那时只做整组重配, 不动逐键标记。 */
    for (int i = 0; i < 3; i++)
        if (stuck_mask & (uint8_t)(1u << i)) s_stuck_repaired[i] = 1;
    s_heal_cnt++;
    ESP_LOGE(TAG, "按键卡死! 判据=%s low=%d/%d stuck=0x%X → 解锁 pad + 重配为上拉输入 (第 %d 次自愈)",
             stuck_mask ? "单键卡低20s" : "整组全低200ms", low, n,
             (unsigned)stuck_mask, s_heal_cnt);
    /* 故障快照: 电平 / 方向 / 状态机 / 低电平时长 / PM 浅睡开关 / 心跳 / 最近动作。
     * 一次性打完, 极低频(只在真故障时), 不占常态开销。 */
    {
        extern volatile uint32_t g_os_loop_beat;
        esp_pm_config_t pmc = {0};
        (void)esp_pm_get_configuration(&pmc);
        ESP_LOGE(TAG, "  uptime=%us 心跳=%u 最近动作=%d(%u ms 前) PM=%d/%dMHz light_sleep=%d",
                 (unsigned)(esp_timer_get_time() / 1000000LL),
                 (unsigned)g_os_loop_beat, s_last_act,
                 (unsigned)(s_last_act_ms ? (now - s_last_act_ms) : 0),
                 pmc.max_freq_mhz, pmc.min_freq_mhz, (int)pmc.light_sleep_enable);
    }
    for (int i = 0; i < 3; i++)
        if ((int)s_btns[i].gpio >= 0)
            ESP_LOGE(TAG, "  修复前 %s GP%-2d level=%d dir=%d state=%d 低电平已 %u ms",
                     s_btns[i].name, (int)s_btns[i].gpio,
                     gpio_get_level(s_btns[i].gpio),
                     key_pin_is_output((int)s_btns[i].gpio),
                     (int)s_btns[i].state,
                     (unsigned)(s_low_since[i] ? (now - s_low_since[i]) : 0));
    input_keys_reinit_pins();
    for (int i = 0; i < 3; i++)
        if ((int)s_btns[i].gpio >= 0)
            ESP_LOGE(TAG, "  修复后 %s GP%-2d level=%d dir=%d",
                     s_btns[i].name, (int)s_btns[i].gpio,
                     gpio_get_level(s_btns[i].gpio),
                     key_pin_is_output((int)s_btns[i].gpio));
    /* 重配已把状态机打到 IDLE; 逐键计时也清零, 下一轮重新起算 */
    for (int i = 0; i < 3; i++) s_low_since[i] = 0;
}

/* ★★★ 2026-11-23 根因取证: RTC 域路由位 —— "pad 被搬去模拟 RTC 模块"的唯一直接证据 ★★★
 *   `RTC_IO_TOUCH_PADn_REG` 的 `MUX_SEL`(bit19): 0=接数字 GPIO 模块, 1=接模拟 RTC 模块。
 *   IDF 源码原注释(esp_hal_gpio/esp32s3/rtc_io_ll.h, rtcio_ll_function_select):
 *       "0: GPIO connected to digital GPIO module. 1: GPIO connected to analog RTC module."
 *   一旦为 1, 数字侧 `gpio_get_level()` 恒读 0 —— 与"键真被按住"在电平上无法区分,
 *   只能靠这一位定性。这正是 2026-11-23 现场「GP0/GP4 同时恒低、GP44 正常」的成因:
 *   本板 `wake_key_mask()` 退化成 {GP0,GP4}, 它们同时是 EXT1 唤醒脚, 浅睡前
 *   `ext1_wakeup_prepare()` 把这两根 pad 接到 RTC 域, 醒来没人切回来。
 *   谁会把这一位置 1(全项目逐条核对过, 只有这两条):
 *     · IDF 的 `ext1_wakeup_prepare()` —— 浅睡/深睡前各跑一次, 掩码内的 RTC pad 全切
 *     · `rtc_gpio_init()` —— 本工程无调用
 *     (注意 `rtc_gpio_isolate()` **不**动这一位: 它只关 RTC 域的上下拉与输入输出使能,
 *      所以"被 isolate 的脚读不到数字电平"这个旧说法只在 pad 已经接到 RTC 域时才成立。)
 *   谁把它清回 0: rtc_gpio_deinit() —— `gpio_config()` 内部会替每个 RTC 脚调一次,
 *   所以"重配 GPIO 后按键恢复"的机理就是这一步。
 *   只有 GPIO0~21 有这一位; >21(如本板 PWR=GP44)返回 -1, 即"无此概念"。 */
static int rtc_domain_sel(int pin)
{
    if (pin < 0 || pin > 21) return -1;          /* 非 RTC 域脚: 没有这个位 */
    int ch = rtc_io_number_get((gpio_num_t)pin);
    if (ch < 0 || ch > 14) return -1;            /* 15+ 是 XTAL pad, 不参与按键 */
    /* TOUCH_PAD0..14 寄存器连续, 步长 4 字节(0x84/0x88/0x8C/... 见 soc/rtc_io_reg.h) */
    const uint32_t reg = RTC_IO_TOUCH_PAD0_REG + (uint32_t)ch * 4u;
    return (REG_READ(reg) & RTC_IO_TOUCH_PAD0_MUX_SEL) ? 1 : 0;
}

/* 现场快照 (remcon `keys` 命令调用): 电平 / 方向 / 状态机 / 自愈次数一次看全.
 * 排"按键没反应"时先看 level 是否恒 0、dir 是否为 1(被配成输出) —— 这两项就能定性。 */
void input_keys_dump(void)
{
    ESP_LOGW(TAG, "=== KEYS 快照 (自愈次数=%d) ===", s_heal_cnt);
    const char *stn[4] = { "IDLE", "DEBOUNCE", "PRESSED", "LONG_FIRED" };
    uint32_t now = now_ms();
    for (int i = 0; i < 3; i++) {
        if ((int)s_btns[i].gpio < 0) {
            ESP_LOGW(TAG, "  [%d] %-4s 未配置(gpio<0)", i, s_btns[i].name);
            continue;
        }
        /* ★ 2026-11-23: 加"低电平已持续多久" —— 这是区分"真按住"与"pad 恒读 0"的
         *   唯一直接证据。人手不可能按住一个键几十秒; 出现低电平持续 >20s 而
         *   动作流水不动, 就是 pad 停在睡眠态(见 input_init 的 gpio_sleep_sel_dis 段)。 */
        ESP_LOGW(TAG, "  [%d] %-4s GP%-2d level=%d dir=%d(%s) state=%s low=%ums short=%d long=%d",
                 i, s_btns[i].name, (int)s_btns[i].gpio,
                 gpio_get_level(s_btns[i].gpio),
                 key_pin_is_output((int)s_btns[i].gpio),
                 key_pin_is_output((int)s_btns[i].gpio) ? "输出!" : "输入",
                 stn[(int)s_btns[i].state & 3],
                 (unsigned)(s_low_since[i] ? (now - s_low_since[i]) : 0),
                 (int)s_btns[i].short_action, (int)s_btns[i].long_action);
        /* ★★★ 2026-11-23: IO_MUX 寄存器级证据 —— level/dir 只能证明"读到 0",
         *   定不了"为什么是 0"。这两个位是唯一能定性的硬指标:
         *     FUN_IE(bit9)=0 ⇒ **数字输入被关死**, level 恒 0 (pad 被 disable,
         *                       典型是停在睡眠态 / 被 isolate)
         *     FUN_PU(bit8)=0 ⇒ 上拉没了, 引脚悬空(外部再一拉就是 0)
         *     SLP_SEL(bit1)=1 ⇒ 该脚睡眠期间改用 SLP_* 影子(SLP_IE bit4)
         *   正常松手、运行中: Fun_IE=1 Fun_Pu=1 且 level=1。
         *   ⇒ 下次故障只要这一行就能判断是"pad 被关"还是"外部拉低", 不必再推断。 */
        {
            uint32_t mux = REG_READ(GPIO_PIN_MUX_REG[s_btns[i].gpio]);
            ESP_LOGW(TAG, "      MUX[%d]=0x%08x FUN_IE=%d FUN_PU=%d MCU_SEL=%d SLP_SEL=%d SLP_IE=%d",
                     (int)s_btns[i].gpio, (unsigned)mux,
                     (int)((mux & FUN_IE) ? 1 : 0), (int)((mux & FUN_PU) ? 1 : 0),
                     (int)((mux >> 12) & 0x7),
                     (int)((mux & SLP_SEL) ? 1 : 0), (int)((mux & SLP_IE) ? 1 : 0));
            /* ★ 2026-11-23: 数字域 / RTC 域路由 —— IO_MUX 那一行看不到这根 pad 到底
             *   归谁管, 只有这一位能证明"是不是被 EXT1 唤醒预备搬走了"。 */
            const int rd = rtc_domain_sel((int)s_btns[i].gpio);
            if (rd >= 0)
                ESP_LOGW(TAG, "      RTC[%d] MUX_SEL=%d %s", (int)s_btns[i].gpio, rd,
                         rd ? "!! pad 已接模拟 RTC 模块 ⇒ 数字读键必为 0"
                            : "(数字域, 正常)");
        }
    }
    ESP_LOGW(TAG, "  any_hw=%d has_touch=%d (level=0 且 dir=1 即引脚被误配成输出)",
             (int)input_any_hw_pressed(), (int)s_has_touch);
    /* 物理键动作流水: 让用户"按一下再看这一行"就能判定输入层有没有出动作。
     * 配合 os 层的分发日志, 可把"按了没反应"切成三段: 引脚 → 状态机 → 上层分发。 */
    ESP_LOGW(TAG, "  动作流水: 累计=%u 最近=%d (%u ms 前) bt_scanning=%d",
             (unsigned)s_act_cnt, s_last_act,
             (unsigned)(s_last_act_ms ? (now - s_last_act_ms) : 0),
             (int)bt_manager_is_scanning());
    /* ★ 2026-11-23: 浅睡开关 —— 判"pad 是不是被睡眠态切走"的必要背景。
     *   屏保档(wallpapers.c) 无条件 light_sleep_enable=true, 不看 USB。 */
    {
        esp_pm_config_t pmc = {0};
        if (esp_pm_get_configuration(&pmc) == ESP_OK)
            ESP_LOGW(TAG, "  PM: %d/%dMHz light_sleep=%d",
                     pmc.max_freq_mhz, pmc.min_freq_mhz, (int)pmc.light_sleep_enable);
    }
}

/* V1.5.x: 休眠(屏保)禁触屏 — 屏保激活时置 true, 触摸入口全部短路:
 * 不刷新 s_last_any_ms、不产生 tap/坐标/手势动作, 杜绝"休眠中碰屏误唤醒". */
static volatile bool s_touch_blocked = false;
void input_set_touch_blocked(bool block) { s_touch_blocked = block; }
bool input_touch_blocked(void) { return s_touch_blocked; }

/* 输入方案: 统一"选择式 UI"(弹窗/列表/键盘) 的交互策略.
 * 优先规则 (V1.6.x 调整):
 *   · **接上手柄 → GAMEPAD(默认高亮一项 + 方向键移动焦点)** —— 即使设备同时带触屏。
 *     用户规范: 连着手柄时, 二级菜单要有默认选中项、能用方向键上下选;
 *   · 有触屏且未接手柄 → TOUCH(默认不高亮任何项、点哪算哪、无效区不震动);
 *   · 无触屏 → GAMEPAD。
 * 说明: 手柄方案只是"多画一个高亮黑块", 触屏的点击(hit-test)/滑动/拖动逻辑完全独立,
 *       二者并存 —— 手柄黑块在时, 手指点哪一项照样直接选中。 */
bool input_scheme_touch(void)
{
    /* ★ 2026-11-21: 这里问的是"**触摸此刻能不能用**"(决定 UI 要不要画默认高亮项),
     *   而不是"这台机器设计上有没有触摸"。板型固化后 s_has_touch 表示后者,
     *   若触摸真坏了还按触摸方案渲染, 页面就没有高亮项、只能靠键摸黑操作 ⇒ 用实探结果。 */
    if (touch_panel_is_present() && !bt_manager_is_connected()) return true;
    return false;
}

/* 全局活动标记: 游戏 joypad 有任意键按住时置位, 供 main 主循环刷新休眠计时.
 * 游戏输入走独立通道 (input_get_held_gb_joypad), 不经由 os 物理键/触摸,
 * 否则会被误判为"无输入"而提前休眠. 标志位方式对时钟无依赖, 由 main 每帧消费. */
static bool s_activity_flag = false;

/* ===== 睿频: 交互升频 / 静止回落 (2026-11-22) =====
 * 背景: esp_pm 的 DFS 只有在**没有任何 ESP_PM_CPU_FREQ_MAX 锁**被持有时才降到
 *   min_freq(80MHz)。原来全仓只有游戏持锁(input_set_gamepad_gamemode), 于是:
 *     · 桌面翻页/拖动/动画/页面切换 —— 全程 80MHz, "该快的时候不快";
 *     · 若反过来永久持锁 —— 24h 满频 240MHz, "该省的时候不省"(实测模组发烫)。
 * 做法: 任意真实输入 → 持 MAX 锁, 400ms 内无新输入就自动释放。
 *   ⇒ 按着/拖动期间 240MHz(跟手), 松手 0.4s 后回落 80MHz(省电)。
 *   锁是 ESP_PM_CPU_FREQ_MAX, 只抬高 freq 上限, 不阻止浅睡(浅睡由 NO_LIGHT_SLEEP 控制),
 *   所以和屏保浅睡、蓝牙省电都不冲突。 */
#define INPUT_BOOST_HOLD_MS  400

static esp_pm_lock_handle_t s_boost_lock = NULL;
static volatile uint32_t    s_boost_until = 0;
static volatile bool        s_boost_held  = false;

void input_pm_boost_now(void) {
    if (!s_boost_lock &&
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "bbk_boost", &s_boost_lock) != ESP_OK)
        return;                                  /* PM 未启用: 静默跳过(不影响输入) */
    if (!s_boost_held) {
        esp_pm_lock_acquire(s_boost_lock);
        s_boost_held = true;
        /* 只在**状态跳变**时打, 每次交互最多一条, 用于实证"升频真的发生了"。
         * (不能靠 remcon 的 `pm` 命令验证: rc_exec 每条命令都调 input_mark_activity,
         *  查询动作本身就会把锁又置 active —— 测出来永远是 1, 见 2026-11-22 记录) */
        ESP_LOGI(TAG, "PM boost ↑ 240MHz (交互升频)");
    }
    s_boost_until = now_ms() + INPUT_BOOST_HOLD_MS;
}

void input_pm_boost_tick(void) {
    if (!s_boost_held) return;
    /* 用有符号差判超时, 天然处理 32 位毫秒回绕(49.7 天) */
    if ((int32_t)(now_ms() - s_boost_until) >= 0) {
        esp_pm_lock_release(s_boost_lock);
        s_boost_held = false;
        ESP_LOGI(TAG, "PM boost ↓ 释放 → DFS 回落 80MHz");
    }
}

void input_mark_activity(void) { s_activity_flag = true; s_last_any_ms = now_ms(); input_pm_boost_now(); }

/* ★ 2026-10-09 (linit 要求: "正在玩不睡, 但按电源键要能锁屏"):
 *   "游戏里一直按着键" 与 "真实输入事件" 必须**分成两个通道**, 不能共用一个时钟。
 *   原来二者都刷 s_last_any_ms ⇒ 后果:
 *     ① 游戏中按住键(或按键脚悬空假按)每帧刷新它, 屏保监督任务判"有人操作"
 *        ⇒ **按电源键进屏保后下一拍立刻被踢出来** = "锁屏键进不去动态壁纸";
 *     ② 空闲时钟永不老化 ⇒ 自动睡眠也永不触发。
 *   现在: 真实事件 → input_mark_activity(); 按住游戏键 → input_mark_game_held(),
 *   它只用于"别自动进屏保", **不参与"退出屏保"的判定**。 */
static volatile bool s_game_held = false;
void input_mark_game_held(bool held)
{
    /* 放下手柄(按住→松开)算一次真实事件: 重新计时, 于是"放下手柄 N 分钟后睡"仍成立 */
    if (s_game_held && !held) s_last_any_ms = now_ms();
    s_game_held = held;
}
bool input_game_key_held(void) { return s_game_held; }
bool input_consume_activity(void) { bool a = s_activity_flag; s_activity_flag = false; return a; }

/* V1.2.x: 游戏中"退出键"反馈 — BACK/HOME 不在 joypad 位图内, 单独在此触发游戏震动
 * (方向/确认等游戏操作键走 joypad 上升沿, 不与本处重复).
 *
 * ★ 2026-11-23 linit「模拟器里按退出键弹"退出程序?"时震动两次」根因:
 *   物理 BOOT(GPIO0) 在游戏内**同时**是 joypad 的 B 位 —— input_get_held_gb_joypad()
 *   里有一句 "if (input_is_held(0)) j &= ~(1 << 1)" 就是 BOOT → B。于是同一根手指按下:
 *       ① joypad 位图 B 位上升沿 → vibrator_tap_game()   ← 第 1 次
 *       ② tick() 产生 MENU_ACTION_BACK → 本函数          ← 第 2 次(重复)
 *   两次紧挨着, 听感/手感就是"退出键震两下"。
 *   修法: 由物理 BOOT 产生的退出动作**不再重复震**(kname=="BOOT" 时跳过);
 *   手柄 F_EXIT(不在 joypad 位图)与触摸上滑(kname=NULL)仍照原样震一次。
 *   同理 BOOT 长按 3s 的 HOME 也跳过 —— 按下沿已经震过, 不该 3 秒后再震一次。 */
static void fb_game_exit_vib(menu_action_t a, const char *kname) {
    if (!s_gamepad_gamemode) return;
    if (a != MENU_ACTION_BACK && a != MENU_ACTION_HOME) return;
    if (kname && strcmp(kname, "BOOT") == 0) return;   /* 已在 joypad 边沿震过 */
    vibrator_tap_game(30, 120);
}

void input_init(void) {
    /* ★ 2026-11-22: 把蓝牙键盘的字符/动作接到本层 (1:1 输入)。
     *   放在最前面: 键盘可能在 input_init 之前就连上了 (自动回连), 越早注册越不会漏键。
     *   回调本身很轻, 只在真正有键按下时被调用。 */
    bt_manager_set_kbd_sink(input_kbd_event);
    /* V1.5.x 修复: 统一"最后活动"时钟必须从开机即有值(初始化为开机时刻), 否则开机后
     * 未产生任何输入的静置期间 s_last_any_ms 恒为 0, 屏保监视任务的
     * `input_last_any_ms() != 0` 守卫永假 → 自动空闲屏保/睡眠永不触发. */
    s_last_any_ms = now_ms();

    /* ═══════════════════════════════════════════════════════════════════════
     * ★★★ 2026-11-22 实测根因修复:「确认/返回按下去没反应, 只有断电才好」
     *   串口现场铁证: `E BOD: Brownout detector was triggered` —— 关机时
     *   ltc2954_power_cut() 提前把 KILL 拉低, 供电轨跌落, 芯片是**掉压复位**,
     *   不是干净深睡。而 RTC 域(含 pad hold / rtc_gpio_isolate 状态)在掉压复位
     *   下**不归零**(这正是"要拔电池才恢复"的原因)。
     *   ⇒ 上一次关机前为深睡加的三道锁会继续生效:
     *        gpio_deep_sleep_hold_en()   全局 pad 冻结
     *        gpio_hold_en(键脚)          单脚锁存
     *        rtc_gpio_isolate(键脚)      切断数字域→pad
     *   ⚠ 被 hold / isolate 的 pad **会静默忽略随后的 gpio_config()** ——
     *     所以原来那句 gpio_config() 看起来"配置成功", 实际引脚还是坏的。
     *   症状完全对得上实测分布: 坏的是 GPIO0 / GPIO4 (≤21, RTC 域),
     *   GPIO44 正常(>21, isolate 对它本来就是空操作)。
     *   修法: 配置之前**无条件**把三道锁逐个解开。这步本身不耗电、不改语义,
     *   正常冷启动时全部是空操作, 只在"上次没关干净"时才真正起作用。
     * ═══════════════════════════════════════════════════════════════════════ */
    /* 解锁前的原始电平 —— 这是"上次有没有关干净"的唯一现场证据:
     *   三个键都是松开的, 所以**正常必须全 1**; 出现 0 就是 pad 被 hold/isolate 了。 */
    ESP_LOGI(TAG, "解 pad 锁前: K%d B%d P%d",
             gpio_get_level(BTN_GPIO_LEFT), gpio_get_level(BTN_GPIO_RIGHT),
             gpio_get_level(BTN_GPIO_PWR));

    {
        const int kp[3] = { BTN_GPIO_LEFT, BTN_GPIO_RIGHT, BTN_GPIO_PWR };
        /* 三道锁一次解完。序列写在 input_pad_unlock_pins() 里 —— 运行中自愈
         * (input_keys_reinit_pins) 调的是同一个函数, 不允许两处各写一套。 */
        input_pad_unlock_pins(kp, 3);
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BTN_GPIO_LEFT) | (1ULL << BTN_GPIO_RIGHT) | (1ULL << BTN_GPIO_PWR),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t _ret = gpio_config(&io_conf);

    /* ═══════════════════════════════════════════════════════════════════════
     * ★★★ 2026-11-23 根因修复: 按键 pad **不参与浅睡的配置切换** ★★★
     *   sdkconfig 里 `CONFIG_PM_SLP_DISABLE_GPIO=y`, IDF 在启动时
     *   (esp_sleep_startup_init, components/esp_hw_support/sleep_gpio.c) 会对
     *   **所有** GPIO 执行:
     *       gpio_sleep_set_direction(pin, GPIO_MODE_DISABLE);   // 关 FUN_IE/输出
     *       gpio_sleep_set_pull_mode(pin, GPIO_FLOATING);       // 关上下拉
     *       gpio_sleep_sel_en(pin);                             // 睡时切到上面这套
     *   即"睡时切到禁用+浮空, 醒来切回"。而屏保档是 `light_sleep_enable=true`
     *   (wallpapers.c, 且**不看 USB 状态**), 于是屏保→浅睡→唤醒这一趟反复发生。
     *
     *   2026-11-23 05:37 现场(只读 keys 快照, 未复位):
     *       [0] BOOT GP0  level=0 dir=0(输入) state=LONG_FIRED
     *       [1] KEY  GP4  level=0 dir=0(输入) state=LONG_FIRED
     *       [2] PWR  GP44 level=1 dir=0(输入) state=IDLE
     *       自愈次数=0 · 最近动作=11(POWER_LOCK) 18 分钟前 · 此后零动作
     *   ⇒ 两键恒读 0 达 18 分钟(人手不可能), 且**只有 0/4 这两根 RTC 域脚坏**,
     *     GPIO44(>21, 非 RTC 域) 完全正常 —— 与 2026-11-22 记录的"掉压复位后
     *     只剩 0/4 坏"是同一类 pad 残留, 只是这次触发源是浅睡往返而非掉压。
     *
     *   修法: 三个键脚显式 gpio_sleep_sel_dis() —— 让它们**不参与**上面那套切换,
     *   浅睡期间保持当前配置(输入+上拉)。这与 audio_player 对功放脚的做法一致
     *   (audio_player.c: "用官方 gpio_sleep_sel_dis() 让这一脚在睡眠期间保持当前电平")。
     *   代价: 键松开时上拉不流通电流, 只有按下才耗电 ⇒ 浅睡电流影响可忽略;
     *   换来的是按键在任何省电档下都不会再被切走。
     *   ⚠ 自愈路径(input_keys_reinit_pins)也做了同样的事, 否则"修好→再睡→又坏"。
     *   ⚠ **实际调用放在下方 power_mgr_pin_init() 之后** —— 它内部的 keys_config()
     *     还会再跑一次 gpio_config, 必须让"清 SLP_SEL"是最后一步。
     *
     *   ★★★ 2026-11-23 补充: 上面只挡住了"睡眠影子"这一半, 挡不住下面这一半 ★★★
     *   IO_MUX 的 SLP_SEL 与 RTC_IO 的 MUX_SEL 是**两个寄存器里的两个位**, 互不相干:
     *     · SLP_SEL=0  ⇒ 睡眠期间沿用"活动配置", 不会被切成 禁用+浮空   ← 上面这段治的
     *     · MUX_SEL=1  ⇒ pad 被接到**模拟 RTC 模块**, 数字 GPIO 模块与它断开,
     *                     `gpio_get_level()` 恒读 0                        ← 病灶在这里
     *   把 MUX_SEL 置 1 的是 IDF 的 `ext1_wakeup_prepare()`(浅睡/深睡前各跑一次),
     *   而它被触发的前提是"键脚被登记成 EXT1 唤醒源"。本板 `wake_key_mask()` 退化后
     *   正好是 {GP0,GP4} ⇒ 每次浅睡都把这两根 pad 搬去 RTC 域, 醒来无人搬回。
     *   ⇒ 根治点在 `power_mgr.c: power_mgr_pin_init()` —— **不再开机常驻注册 EXT1**,
     *     只在真正要深睡的那一刻注册。本文件不负责、也不应该在这里补收尾动作
     *     (那属于给症状兜底, 且 `esp_light_sleep_start()` 在 IDF 空闲任务内部调用,
     *      固件没有可靠的醒来回调点)。排查时用 `keys` 快照里的 `RTC[n] MUX_SEL=` 定性。
     * ═══════════════════════════════════════════════════════════════════════ */

    ESP_LOGI(TAG, "GPIO ret=%d K%d B%d P%d", _ret,
             gpio_get_level(BTN_GPIO_LEFT), gpio_get_level(BTN_GPIO_RIGHT),
             gpio_get_level(BTN_GPIO_PWR));

    /* 触摸面板 (V1.0.65): 未检测到芯片时 read 恒返回 false, 零开销
     * ★ 2026-10-18: 引脚按**板型表运行时**下发（原来是编译期 BOARD_TP_* 宏 = 小智的 5/13/12/6，
     *   在标准扩展版上既打不开触摸、还会把该板的 LCD DC(5) 配成开漏上拉 ⇒ 屏不刷新）。 */
    {
        const board_pins_t *bp = board_pins_get();
        tp_pins_t tp = { bp->tp_sda, bp->tp_scl, bp->tp_int, bp->tp_rst };
        touch_panel_set_pins(&tp);

        /* ★ 2026-10-22: 只要**本板有触摸引脚**就初始化驱动 ——
         *   探不到芯片也无害 (init 返回 NONE, read 恒 false), 但 I2C 总线 / RST / 看门狗
         *   必须先生好, 否则"触摸排线后来补焊好了"的时候, 后台补探任务
         *   (main.c touch_retry_task → touch_panel_recover) 连总线都没有, 永远救不回来。
         *   原来只在 hw_cfg_enabled(HW_TOUCH) 时才 init ⇒ 一旦开机没探到就被判死刑。 */
        if (bp->tp_sda >= 0 && bp->tp_scl >= 0) {
            touch_panel_init();
        } else {
            ESP_LOGW("INPUT", "本板无触摸引脚 ⇒ 跳过触摸初始化");
        }
        /* ⛔⛔ 2026-10-22 血的教训: 这里**绝对不能**再试"另一套板型的触摸引脚"。
         *   小智板型下 GPIO7 = LCD DC、GPIO11 = LCD CS；标准/微雪板型下 GPIO5 = LCD DC。
         *   触摸驱动把那些脚改成 I2C 后, `gpio_reset_pin()` 只归还成输入,
         *   **LCD 驱动不会重配** ⇒ 屏幕立刻撕裂/花屏, 触摸也读不通 (实测失败率 100%)。
         *   跨板判断必须在 LCD 初始化之前做 (hw_cfg_init 里补探命中会直接改板型+重启)。 */
    }
    /* ★★★ 2026-11-21 linit「不需要重复判断版型, 判断一次就固化死」★★★
     *   键位语义**只认固化后的板型 ID**, 不再看每次开机的触摸芯片探测结果。
     *   板型由 hw_cfg 识别一次写进 NVS, 之后每次开机直接读存档(0 次 I2C);
     *   要换板型只能重刷完整固件(cfg_ver 一变 ⇒ 旧存档作废 ⇒ 重新识别一次)。
     *   ⇒ "触摸冷启动应答慢 → 返回键变成方向键 / 确认按不动"这一整类问题从根上消失。 */
    s_has_touch = board_has_touch_by_design();
    ESP_LOGI(TAG, "键位定案(固化板型=%d): %s机型 → BOOT(GPIO%d)=%s / KEY(GPIO%d)=确认",
             (int)board_pins_get()->id, s_has_touch ? "有触摸" : "无触摸",
             (int)BTN_GPIO_RIGHT, s_has_touch ? "返回" : "右向", (int)BTN_GPIO_LEFT);
    input_cfg_keys();                          /* 按结果配置物理键语义 */
    /* 开关键 (V1.5.x): rel_short=true — 短按动作改"释放判定":
     *   按住 <0.5s 松手 → POWER_LOCK(锁屏进壁纸); 持续按住 ≥0.5s 不投(避免长按关机途中误锁屏),
     *   到 2s 由 tick 状态机投递 POWER_SHUTDOWN (★2026-11-25 改为 action 通道).
     *   ★★ 为什么不再用独立 GPIO 轮询(input_power_should_sleep) ★★
     *     那条路依赖主循环每帧调用, 模拟器/引擎前台阻塞时永远跑不到; 且撞脚机型
     *     (PWR==LEFT/RIGHT)恒 false。改成 tick 投递后与"返回键退出框"走同一条
     *     input_get_action 通道, 任何界面/引擎/板型都一致生效。 */
    /* ★★★ 2026-10-16: 板型可能把"电源键"和"确认键"放在同一个脚(微雪=GPIO18) ⇒
     *   若两条目读同一 GPIO, 状态机互相打架 ⇒ **按键全部点不动**。
     *   这里统一处理: 同脚时**只保留 KEY(确认)条目**, 电源键条目禁用(pin=-1)。 */
    if (BTN_GPIO_PWR == BTN_GPIO_LEFT) {
        s_btns[2] = (btn_ctx_t){ (gpio_num_t)(-1), ST_IDLE, 0, POWER_HOLD_MS,
                                 MENU_ACTION_NONE, MENU_ACTION_NONE, false, "PWR(同脚,禁用)" };
    } else {
        s_btns[2] = (btn_ctx_t){ BTN_GPIO_PWR, ST_IDLE, 0, POWER_HOLD_MS,
                                 MENU_ACTION_POWER_LOCK, MENU_ACTION_POWER_SHUTDOWN, true, "PWR" };
    }

    /* [电源收拢] 唤醒/睡眠引脚(RTC域上拉 + EXT1唤醒掩码)统一由电源模块 sleep.c 负责
     * (wp_power_pin_init: 仅开机键 PWR(GPIO1) 为唯一唤醒源). input.c 在此只负责
     * 普通 GPIO 读键(短按/长按/关机计时均读数字电平), 不参与电源/唤醒配置. */
    power_mgr_pin_init();

    /* ★ 2026-11-23: 键脚不参与浅睡的配置切换 —— **必须放在最后**
     *   (power_mgr_pin_init 内部还会跑一次 gpio_config)。见上方大段说明。 */
    if (BTN_GPIO_LEFT  >= 0) gpio_sleep_sel_dis((gpio_num_t)BTN_GPIO_LEFT);
    if (BTN_GPIO_RIGHT >= 0) gpio_sleep_sel_dis((gpio_num_t)BTN_GPIO_RIGHT);
    if (BTN_GPIO_PWR   >= 0) gpio_sleep_sel_dis((gpio_num_t)BTN_GPIO_PWR);

    if (BTN_GPIO_PWR >= 0) {
        /* 不依赖 power_mgr 的 keys_config 掩码 —— 这里显式配一次"输入 + 上拉"。
         * 幂等; 放在 power_mgr_pin_init() 之后, 不会被打回。 */
        gpio_config_t pc = {
            .pin_bit_mask = 1ULL << BTN_GPIO_PWR,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&pc);
        /* ★★★ 2026-11-28: LTC2954ITS8-2 的 /INT(U6.5→GPIO1) 是**开漏输出**,
         *   必须外部上拉才能读到"松开=高"。Schematic1_4/111.tel 的 GPIO1 网络
         *   只有 U2.39+U6.5 两个焊盘, **没有外部上拉电阻**(R7 100k 拉的是 KILL#/GPIO44)。
         *   固件必须把 GPIO1 的上拉坐实 —— 数字域 + RTC 域**两处都开**:
         *     · 数字域 pullup: 运行期读到确定的高电平(按键状态机读它)
         *     · RTC 域 pullup: 深睡/浅睡时 pad 切到 RTC 域, 数字域掉电后仍要有上拉,
         *       否则 /INT 浮空 ⇒ EXT1 误判"按下" ⇒ 关机瞬间自唤醒
         *   这里在 power_mgr_pin_init(keys_config) 之后**再显式确认一次**, 不依赖别处顺序。 */
        gpio_set_pull_mode((gpio_num_t)BTN_GPIO_PWR, GPIO_PULLUP_ONLY);
        if (BTN_GPIO_PWR <= 21) rtc_gpio_pullup_en((gpio_num_t)BTN_GPIO_PWR);  /* GPIO1 ∈ RTC 域 */
    }

    /* ★ 2026-11-22: 物理键专职扫描任务 (5ms)。与 UI 帧率完全解耦 ——
     *   空闲态主循环 60ms/圈，按键若还挤在里面扫，松手锁屏必然慢。 */
    if (!s_key_task_started) {
        s_key_task_started = true;
        if (xTaskCreate(input_key_task, "key_scan", 3072, NULL, 3, NULL) != pdPASS) {
            s_key_task_started = false;
            ESP_LOGE(TAG, "按键扫描任务创建失败 ⇒ 物理键将无响应");
        } else {
            ESP_LOGI(TAG, "物理键扫描任务已启动 (5ms 周期, prio3, 栈 3KB 内部 RAM)");
        }
    }
}

/* ★★★ 2026-11-21 linit「按键要固定, 不要乱自动识别」★★★
 *   这里原本有一条"必须先见到一次高电平才认这根键"的门槛(用来防 GPIO4 空脚恒低),
 *   但 GPIO4 已实测确认是 **Down 确认键**(空闲 1 / 按下 0), 门槛的根因不成立;
 *   反而会吞掉"开机/切页瞬间就按住"的那一次按键 ⇒ 用户感觉确认/返回偶尔按不动。
 *   现已删除: 三根脚都是实键, 直接按电平判定。 */
static menu_action_t tick(btn_ctx_t *b) {
    if (b->gpio < 0) return MENU_ACTION_NONE;        /* 该键已禁用(同脚/无此键) */
    bool pressed = (gpio_get_level(b->gpio) == 0);   /* 低电平=按下 */
    uint32_t now = now_ms();
    switch (b->state) {
        case ST_IDLE:
            if (pressed) {
                if (b->rel_short) {
                    /* 2954 的 /INT 最短低电平 32ms(datasheet tDB,OFF), 与本文件
                     * DEBOUNCE_MS(30ms) 几乎重合。芯片自身已做 32ms 硬件去抖,
                     * 固件不再叠加 ⇒ 开关键跳过 ST_DEBOUNCE 直接进 PRESSED。
                     * 其余机械键仍走 30ms 防抖。 */
                    b->state = ST_PRESSED;
                    b->press_start = now;
                } else {
                    b->state = ST_DEBOUNCE;
                    b->press_start = now;
                }
                ESP_LOGD(TAG, "[dbg] key %s DOWN det=%d", b->name, b->gpio); }
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
                /* ★ 2026-11-28: `>=` 而非 `>` —— 按住满 2s 的**当帧**就投关机动作,
                 *   不要求松手(松手后才弹窗是旧行为, 用户按到 3s 会先被 2954 硬件断电)。 */
                if ((now - b->press_start) >= thr) {
                    /* V1.0.40: 长按发射后进入 ST_LONG_FIRED, 等按键释放才回 IDLE.
                     * ★ 2026-11-25: 开关键(rel_short)长按 2s 在此投递
                     *   long_action=MENU_ACTION_POWER_SHUTDOWN(不再是 NONE),
                     *   与返回键走同一 action 通道 ⇒ 引擎/模拟器阻塞主循环时照样生效;
                     *   进入 LONG_FIRED 后释放不再误触发锁屏. */
                    b->state = ST_LONG_FIRED;
                    return b->long_action;
                }
            } else {
                b->state = ST_IDLE;
                if (b->rel_short) {
                    /* 开关键语义(两条互斥、无空隙):
                     *   · 按住未达 2s 就松手 ⇒ 锁屏(POWER_LOCK)
                     *   · 按住满 2s(松不松都一样) ⇒ 关机确认框(见 ST_PRESSED 长按分支) */
                    ESP_LOGI(TAG, "[PWR] 点按→投锁屏 (按住 %u ms)",
                             (unsigned)(now - b->press_start));
                    s_last_any_ms = now;
                    return b->short_action;
                }
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

/* ═══════════════════════════════════════════════════════════════════════════
 * ★★★ 2026-11-22 【松手锁屏延迟过大】根因与修复 ★★★
 *   用户反馈: 语义要**松手才锁屏**(保持不变), 但"松手后半天才锁屏"。
 *   根因不在 2954, 也不在状态机: main.c 帧节奏在**没有触摸**时是 60ms/圈
 *   (`budget_ms = touch_hit ? 16 : 60`, 2026-11-08 为省电/给 DFS 降频窗口改的)。
 *   物理键不产生 touch_hit ⇒ 松手后要等最多 60ms 才被 tick() 看到, 再叠加 2954
 *   /INT 释放延迟与 30ms 防抖 ⇒ 主观就是"松手了还不锁屏"。
 *   修法: 三键状态机从主循环**搬出来**, 由本 5ms 任务专职扫描; 动作进单生产者/
 *   单消费者小队列, 产生动作立刻 notify 主循环来取 —— 识别延迟与 UI 帧率解耦。
 *   ⛔ 语义不变: 开关键仍是 rel_short=true(**松手才投**锁屏), 只是"松手→投递"
 *      从"最多 60ms"降到"≤5ms + 一次唤醒"。
 * ═══════════════════════════════════════════════════════════════════════════ */
#define KEY_Q_LEN 8
typedef struct { menu_action_t act; const char *name; } key_evt_t;
static key_evt_t         s_key_q[KEY_Q_LEN];
static volatile uint8_t  s_key_q_w = 0, s_key_q_r = 0;
static TaskHandle_t      s_key_wake_task = NULL;   /* 主循环任务: 有动作就踢它一脚 */

static void key_q_push(menu_action_t a, const char *name)
{
    uint8_t n = (uint8_t)((s_key_q_w + 1) % KEY_Q_LEN);
    if (n == s_key_q_r) return;      /* 满: 丢弃最新的, 绝不覆盖尚未消费的旧动作 */
    s_key_q[s_key_q_w].act  = a;
    s_key_q[s_key_q_w].name = name;
    s_key_q_w = n;
    if (s_key_wake_task) xTaskNotifyGive(s_key_wake_task);
}

static menu_action_t key_q_pop(const char **name)
{
    if (s_key_q_r == s_key_q_w) return MENU_ACTION_NONE;
    menu_action_t a = s_key_q[s_key_q_r].act;
    if (name) *name = s_key_q[s_key_q_r].name;
    s_key_q_r = (uint8_t)((s_key_q_r + 1) % KEY_Q_LEN);
    return a;
}

/* ★★★ 2026-11-22 修「固定 5ms 轮询把浅睡彻底废掉」★★★
 *   CONFIG_FREERTOS_USE_TICKLESS_IDLE=y 且 IDLE_TIME_BEFORE_SLEEP=8 tick(1000Hz ⇒ 8ms):
 *   只要**任何**任务以 <8ms 周期阻塞, 空闲任务就永远算不出 ≥8 tick 的空闲窗口,
 *   tickless idle 一次都不会触发 ⇒ 屏保档 `esp_pm_configure(light_sleep_enable=true)`
 *   ("80MHz + 浅睡"那条省电策略) 形同虚设, 整夜 80MHz 常亮。
 *   这里改**自适应采样**: 有键按下/防抖/长按中、或刚有动作 → 5ms 精细采样(松手锁屏延迟不变);
 *   完全空闲 → 30ms(≈33 次/秒唤醒, 留出 22ms 睡眠窗口)。
 *   按下识别最多晚 30ms, 而按下本来就要过 DEBOUNCE_MS(30ms) 才投动作, 感知不变;
 *   **松手→投锁屏**仍在快档(按键按下期间一直快)内完成, 原有的响应性不回退。 */
/* ★★ 2026-11-22 修「确认键/返回键偶发按不动」(实机反馈, 根因在采样周期与防抖耦合) ★★
 *   失效链路(两处互锁, 都在本函数):
 *   ① active 判定写在 tick() **之前** —— 采到"刚按下"的那一轮, 状态还是 ST_IDLE,
 *      于是 active=false, 这一轮仍然按空闲周期睡过去, 该立刻转快档却没转。
 *   ② KEY_IDLE_MS 原来取的 30ms **正好等于 DEBOUNCE_MS(30ms)**。防抖要求
 *      "按住 ≥30ms", 而采样间隔也是 30ms ⇒ 从采到按下到满足防抖只剩**一次**机会
 *      (t+30 那一拍)。用户在这拍之前松手(轻点 30~60ms, 叠加"真实按下比采到早
 *      0~30ms"的相位误差), tick() 就在 ST_DEBOUNCE 看到 !pressed ⇒ 判为抖动回
 *      ST_IDLE, **整次按键被丢弃, 不产生任何动作** ⇒ 主观"偶发按不动"。
 *      原 5ms 固定周期时 t+5..t+30 有 6 次机会, 所以以前不容易撞上。
 *   修法(最小、不改语义、不改防抖阈值):
 *   · active 改到 tick() 之后判断 ⇒ 采到按下的当轮立即转 5ms 快档;
 *   · KEY_IDLE_MS 30→10: 把"采到按下"相对真实按下的相位误差从 ≤30ms 压到 ≤10ms,
 *     使真实按住 30~40ms 即稳定生效(与原 5ms 周期时的 30~35ms 基本一致)。
 *     10ms 仍 > IDLE_TIME_BEFORE_SLEEP(8 tick = 8ms), 浅睡窗口保留(约 2ms/10ms);
 *     若要换回更省的 30ms, 必须同时把 DEBOUNCE_MS 调小, 否则本条失效会重现。 */
#define KEY_IDLE_MS        10    /* 空闲轮询周期: 必须明显 < DEBOUNCE_MS(30), 且 > 浅睡门槛 8ms */
#define KEY_FAST_HOLD_MS   400   /* 动作后继续快档的时间(连击/连按跟手) */

static void input_key_task(void *arg)
{
    (void)arg;
    uint32_t last_act_ms = 0;
    for (;;) {
        input_key_guard();
        bool active = false;
        for (int i = 0; i < 3; i++) {
            if ((int)s_btns[i].gpio < 0) continue;
            /* ★ 2026-11-23: 自愈修过一次仍然低 ⇒ 不是 pad 状态问题(是键真卡住/短路),
             *   这期间**不再产生动作**, 否则会每 20s 重发一次 CONFIRM/BT_SEARCH,
             *   表现成"隔一会儿自己弹一次蓝牙扫描"。松开后 s_stuck_repaired 自动清零。 */
            if (s_stuck_repaired[i] && gpio_get_level(s_btns[i].gpio) == 0) {
                s_btns[i].state = ST_IDLE;
                continue;
            }
            menu_action_t a = tick(&s_btns[i]);
            if (a != MENU_ACTION_NONE) { key_q_push(a, s_btns[i].name); active = true; }
            /* ★ 必须在 tick() **之后**判断: tick 内部会把 IDLE→DEBOUNCE,
             *   本行才能第一时间看到"有键按下"并切 5ms 快档。改回上方即为原缺陷。 */
            if (s_btns[i].state != ST_IDLE) active = true;
        }
        uint32_t now = now_ms();
        if (active) last_act_ms = now;
        /* ★ 2026-11-25: 电源键长按走本状态机的 tick() 投递 MENU_ACTION_POWER_SHUTDOWN,
         *   不再用独立 GPIO 轮询 + 事件标志 —— 那条路在引擎阻塞时不生效。 */
        bool fast = active || (last_act_ms && (uint32_t)(now - last_act_ms) < KEY_FAST_HOLD_MS);
        vTaskDelay(pdMS_TO_TICKS(fast ? 5 : KEY_IDLE_MS));
    }
}

/* ★ 2026-11-24: 手柄功能键上升沿 → 踢主循环一脚。
 *   与物理键同一条唤醒通道 (s_key_wake_task)。没有它, 空闲 60ms 帧周期下
 *   边沿最多要等一整个帧预算才被 input_get_action 取走, "及时反馈"仍然不成立。 */
static void input_pad_edge_wake(void)
{
    if (s_key_wake_task) xTaskNotifyGive(s_key_wake_task);
}

void input_set_wakeup_task(void *t)
{
    s_key_wake_task = (TaskHandle_t)t;
    bt_manager_set_key_edge_cb(input_pad_edge_wake);   /* 手柄边沿也走这条唤醒 */
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
static bool     s_tg_dragged;          /* ★ 本次手势曾拖动过(粘连): 一旦置位就不再判长按.
                                        *   防"翻列表时被误判长按 → 自动收藏"。 */

/* 底部上滑=返回 的跟手/速度状态已迁至 gesture_back 模块 (gesture_back.c),
 * 由 gesture_back_on_press/on_move/on_release 统一管理, 此处不再保留。 */

/* 最近一次"点击(tap)"的屏幕坐标 (400x300), -1 表示无未消费的点击.
 * main.c 用 input_consume_tap 取走后清零, 用于"点哪进哪"的 hit-test. */
static int s_tap_x = -1, s_tap_y = -1;

/* V1.0.66: 当前触摸的实时屏幕坐标 (供主菜单跟手拖动读取)
 * ★ 2026-10-31: 改成 volatile —— 这几个字现在被**多个任务**读:
 *   主循环(input_poll_touch 写) / 引擎帧循环 / board_shim 视频刷新任务(快捷栏读)。
 *   不加 volatile 时 -O2 会把读缓存进寄存器, 视频任务永远看到旧值 ⇒
 *   "快捷栏展开后触摸穿透/坐标错乱"。 */
static volatile bool s_touch_down = false;
/* 坐标保持普通 int: 32 位对齐读写在 Xtensa 上不会撕裂, 且 touch_map_screen/
 * rotate_screen_coord 接收的是 int*, 加 volatile 反而要到处强转。 */
static int  s_touch_sx = 0, s_touch_sy = 0;
/* 最后一次触摸状态更新时间 (ms) —— 供"本帧没人采样"时按需补采 (见 input_touch_resample) */
static volatile uint32_t s_touch_upd_ms = 0;
/* ★ 2026-10-31: 覆盖层(下拉快捷栏)触摸独占锁 —— 定义提前到触摸状态旁边,
 *   因为 touch_gesture_poll() 也要读它 (屏蔽页面级手势)。 */
static volatile bool s_overlay_lock = false;

/* V1.0.68: 每 tick 只读一次触摸芯片并缓存, 供 touch_gesture_poll 与
 * input_poll_touch 共用, 避免 GT911 读后清状态被两次读互相偷走事件. */
static tp_point_t s_touch_cached;               /* 触点0 (单点兼容读) */
static tp_point_t s_touch_multi[TP_MAX_POINTS]; /* 全场触点缓存 (输入层每 tick 一次读) */
static int        s_touch_cached_count = 0;     /* 实际触点数量 (0..TP_MAX_POINTS) */
static bool       s_touch_cached_valid = false;
static uint32_t   s_touch_cache_tick = 0;
static void touch_jump_filter(tp_point_t *pt, bool pressed);   /* 前向声明 */

/* ★★★ 2026-10-31: 触摸采样加互斥。
 *   背景: 同一个缓存现在会被**两个任务**同时访问 ——
 *     ① 主循环 input_poll_touch (正常页面)
 *     ② board_shim 视频刷新任务里的快捷栏 (input_touch_resample, 游戏里补采)
 *   原来只有"按 tick 去重", 但两个任务跨 tick 同时进来时都会读 I2C 并**互相覆盖**
 *   s_touch_cached / _count / _valid ⇒ 坐标与按下状态错位 (快捷栏手势乱、虚拟按键抽风)。
 *   互斥只包住"真正读 I2C"这一段; 同 tick 命中缓存时不进锁 (零开销)。
 *   取锁超时 20ms: 触摸芯片挂死时宁可这一帧回放旧值, 也不能把渲染任务吊死。 */
static SemaphoreHandle_t s_touch_mtx = NULL;

static void touch_read_locked(tp_point_t *pt) {
    /* 未探测到触摸芯片 → 直接视为"未按压"跳过读取 (无触摸机型零开销) */
    if (!touch_panel_is_present()) {
        s_touch_cached.pressed = false;
        s_touch_multi[0].pressed = false;
        s_touch_cached_count = 0;
        *pt = s_touch_cached;
        return;
    }
    tp_point_t pts[TP_MAX_POINTS];
    int cnt = 0;
    if (touch_panel_read_points(pts, &cnt)) {
        s_touch_cached = pts[0];
        for (int i = 0; i < TP_MAX_POINTS; i++) {
            s_touch_multi[i] = (i < cnt) ? pts[i] : pts[0];
            if (i >= cnt) s_touch_multi[i].pressed = false;
        }
        s_touch_cached_count = cnt;
        touch_jump_filter(&s_touch_cached, s_touch_cached.pressed);
    }
    /* 读失败: **保留上一帧状态**, 不伪造"松开" —— 飞线 I2C 一个误码就打断进行中
     * 的手势, 表现为触摸跳变/只能慢慢拖。
     * 这里**不做**次数判断: 原来有"连续 3 次强制释放"和"连续 12 次请求恢复"两条,
     * 前者与"保留上一帧"自相矛盾且 48ms 就触发, 后者与看门狗重复计数。
     * 恢复统一由 touch_panel 的 500ms 看门狗负责 (它自己统计 fails/失败率/卡死),
     * 唯一统计源, 不再多头判断 (2026-11-21)。 */
    *pt = s_touch_cached;
}

static uint32_t s_idle_read_ms = 0;   /* 空闲降频: 上次真正读 I2C 的时刻 */

static void touch_read_once(tp_point_t *pt) {
    uint32_t tick = xTaskGetTickCount();
    if (s_touch_cached_valid && tick == s_touch_cache_tick) {
        *pt = s_touch_cached;   /* 本 tick 已读过, 直接回放缓存 */
        return;
    }
    /* ★★★ 2026-11-02 linit「触摸降频省电」★★★
     *   主循环每 16ms 探一次 ⇒ I2C 事务 ~62 次/秒，CST836U 也就一直全速扫描（约 1mA）。
     *   手指没按住时降到 **20Hz（50ms 一次）**，按下沿最多晚 50ms（无感）；
     *   一旦按下立刻恢复每帧读，拖动/滑动/双击的手感不变。 */
    if (s_touch_cached_valid && !s_touch_cached.pressed) {
        uint32_t now = tick * portTICK_PERIOD_MS;
        if ((uint32_t)(now - s_idle_read_ms) < 50u) { *pt = s_touch_cached; return; }
        s_idle_read_ms = now;
    }
    bool locked = false;
    if (!s_touch_mtx) {
        /* 首次: 创建后置标, 允许并发创建时后到者复用 (FreeRTOS mutex 语义) */
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        if (m) s_touch_mtx = m;
    }
    if (s_touch_mtx) locked = (xSemaphoreTake(s_touch_mtx, pdMS_TO_TICKS(20)) == pdTRUE);
    /* 拿到锁后再判一次: 可能刚被另一个任务读完 (同一 tick 内双击都来读) */
    if (s_touch_cached_valid && xTaskGetTickCount() == s_touch_cache_tick) {
        if (locked) xSemaphoreGive(s_touch_mtx);
        *pt = s_touch_cached;
        return;
    }
    s_touch_cached_valid = true;
    s_touch_cache_tick = tick;
    touch_read_locked(pt);
    if (locked) xSemaphoreGive(s_touch_mtx);
}

static void touch_map_screen(int16_t rx, int16_t ry, int *sx, int *sy);
static menu_action_t touch_gesture_poll(void);
static void rotate_screen_coord(int *x, int *y);
static void touch_alive_tick(bool pressed);   /* 按下即刷新空闲时钟 (见函数处说明) */

/* ⛔ 2026-11-22: 触摸快照 s_touch_sx/s_touch_sy 的**唯一坐标语义 = 已按屏幕旋转换算的逻辑坐标**。
 *   原来只有 touch_gesture_poll() 里调了 rotate_screen_coord(), 而下面这两个写入者没调 ⇒
 *   同一组全局变量里同时存在"逻辑坐标"和"横屏物理坐标"两种值, 谁最后写听谁的。
 *   竖屏(电子书 rot=2/3)时后果最明显: 阅读器菜单/拖动拿到的坐标与 book_menu_geom() 的
 *   竖屏几何不是一个坐标系 —— 手指上下滑被当成横向位移、滚动条命中乱判, 再叠加
 *   "刚拖动过就吞掉这次点击"的抑制逻辑, 表现就是**点菜单项没反应**(用户报的"返回书库点不动")。
 *   现在三个写入者统一旋转, 语义只此一处。 */

void input_poll_touch(void) {
    if (s_touch_blocked) { s_touch_down = false; s_tap_x = s_tap_y = -1; return; }  /* 休眠禁触屏 */
    tp_point_t pt;
    touch_read_once(&pt);
    s_touch_down = pt.pressed;
    /* ★ 2026-11-22: 主循环每帧都会走到这里(与主菜单/引擎/覆盖层无关),
     *   空闲时钟的"按住"刷新放在此处最不会漏 (手势机那条路径也调, 有 1s 去重)。 */
    touch_alive_tick(pt.pressed);
    if (pt.pressed) {
        touch_map_screen(pt.x, pt.y, &s_touch_sx, &s_touch_sy);
        rotate_screen_coord(&s_touch_sx, &s_touch_sy);   /* 与手势机同一坐标系 */
    }
    s_touch_upd_ms = now_ms();
}

/* ★★★ 2026-10-31 (快捷栏 / 引擎触摸冲突): **按需补采一次触摸**。
 *   主循环每帧调 input_poll_touch, 但游戏里主任务被引擎阻塞时不跑 ⇒ 快照会"冻住"。
 *   覆盖层(快捷栏)在自己的帧上下文里发现快照过期就调这里补一次 —— 幂等:
 *   touch_read_once 按 tick 去重, 同一 tick 不会重复读 I2C。
 *   只更新"当前按住状态", **不碰** 手势状态机 (s_o_armed/s_o_zone),
 *   所以不会打断底部上滑返回 / 屏蔽带判定。 */
void input_touch_resample(void) {
    if (s_touch_blocked) { s_touch_down = false; return; }
    tp_point_t pt;
    touch_read_once(&pt);
    s_touch_down = pt.pressed;
    if (pt.pressed) {
        touch_map_screen(pt.x, pt.y, &s_touch_sx, &s_touch_sy);
        rotate_screen_coord(&s_touch_sx, &s_touch_sy);   /* 与手势机同一坐标系 */
    }
    s_touch_upd_ms = now_ms();
}

/* 触摸快照多久没更新了 (ms)。覆盖层据此判断"要不要自己补采"。 */
uint32_t input_touch_age_ms(void) {
    if (s_touch_upd_ms == 0) return 0xFFFFFFFFu;
    return now_ms() - s_touch_upd_ms;
}

/* ★ 2026-10-31: 释放所有"触摸派生"的按键状态。
 *   快捷栏收起时必须调一次: 覆盖层独占期间虚拟手柄读不到点(见 input_get_touch_multi),
 *   但引擎内部的 s_joypad / 摇杆状态是**上一帧的**, 不清就会一直"按着方向"
 *   ⇒ 用户描述的"退出快捷栏后触摸异常 / 角色自己走"。
 *   这里把虚拟手柄强制喂一次"全部松开", 引擎下一帧读到的就是未按下。 */
void input_touch_release_all(void) {
    virtual_keys_poll_multi(0, 0, false, 0, 0, false);
    s_touch_down = false;
    s_touch_upd_ms = now_ms();
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
    /* ★★★ 2026-10-18 linit 实测修复: 电源键与 LEFT/RIGHT 撞脚时(微雪 PWR==KEY==GPIO18),
     *   input_init 已把"电源键条目"禁用(见上), 但本函数原先**仍在轮询该 GPIO** ⇒
     *   在"连接蓝牙"等界面按住这个键(=长按搜索/确认键)到 2s, 会**顺带弹出"是否关机?"**
     *   (用户实测: 某些界面按该键弹出关机确认)。撞脚时此脚是 UI 键, 不是电源键 → 停轮询。 */
    if (BTN_GPIO_PWR < 0 || BTN_GPIO_PWR == BTN_GPIO_LEFT || BTN_GPIO_PWR == BTN_GPIO_RIGHT)
        return false;
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
                ESP_LOGD("INPUT", "触摸跳变被滤除: (%d,%d)->(%d,%d)",
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

int input_get_screen_rotation(void) {
    return s_screen_rot;
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
/* V1.0.90 / ★ 2026-11-22 重写: 判断逻辑坐标 (sx,sy) 是否落在**用户看到的屏幕底部**。
 *
 * ⛔ 旧实现按"物理屏底边"逐旋转 switch 换算 —— **坐标系错了**, 这是竖屏上滑返回
 *    完全不触发的根因:
 *      · rot2 分支判 `sx >= ST7305_HEIGHT-thick`(逻辑 x 大) —— 那是竖屏的**视觉右边**;
 *      · rot3 分支判 `sx < thick`(逻辑 x 小) —— 视觉左边;
 *      · rot1 分支判 `sy < thick`(逻辑 y 小) —— 180° 时的**视觉顶部**。
 *    但 `rotate_screen_coord()` 输出的是"显示逻辑坐标": 它的 **y 轴恒朝下**
 *    (横屏 0..299, 竖屏 0..399), x 轴恒朝右。所以"视觉底部"在逻辑坐标里
 *    **永远是 sy 最大侧**, 与物理屏底落在哪条边毫无关系。
 *    竖屏时用户从视觉底部(sy≈399)起手 ⇒ 旧判据要 sx≥280 或 sx<20 ⇒ 永远不命中。
 *
 * 现在只保留一条几何: 逻辑底边 thick 像素内 + 按**逻辑宽**居中的 150px 带。
 *   · 横屏 400×300: y≥300-thick, x∈[125,275]   (与原默认分支完全一致, 行为不变)
 *   · 竖屏 300×400: y≥400-thick, x∈[75,225]
 *   · 180° 400×300: y≥300-thick, x∈[125,275]   (视觉底 = 逻辑 y 大, 修掉旧反向)
 * thick 为厚度 (逻辑像素)。 */
static inline int screen_logical_w(void) {
    return (s_screen_rot == 2 || s_screen_rot == 3) ? ST7305_HEIGHT : ST7305_WIDTH;
}
static inline int screen_logical_h(void) {
    return (s_screen_rot == 2 || s_screen_rot == 3) ? ST7305_WIDTH : ST7305_HEIGHT;
}

bool input_in_bottom_zone(int sx, int sy, int thick) {
    const int h = screen_logical_h();
    const int w = screen_logical_w();
    const int band = TOUCH_BACK_EDGE_MAXX - TOUCH_BACK_EDGE_MINX;   /* 150 */
    const int x0 = (w - band) / 2;
    return sy >= (h - thick) && sx >= x0 && sx <= x0 + band;
}

/* ★★★ 2026-11-22 linit「触摸屏有按就不要休眠，重新计算休眠时间」★★★
 *
 * 原状(为什么需要这一步):
 *   触摸只有在**产生动作**(点击/长按/上滑)时, input_get_action() 才刷 s_last_any_ms。
 *   于是两类真实操作完全不参与空闲计时:
 *     ① 手指按住不动 (还没松手/没到长按阈值) —— 一直不刷;
 *     ② 长距离拖动列表/滑动条, 松手时位移超阈值 ⇒ 判成 NONE (不产生动作) —— 始终不刷。
 *   结果: 拖了 30 秒列表, 松手那一刻空闲时钟还停在 30 秒前, **1 分钟不到就息屏了**,
 *         体感就是"我明明一直在操作, 它却说我闲着"。
 *
 * 现在: 只要触点处于按下状态就刷新空闲时钟(按下沿立刻刷, 按住期间 1s 心跳补刷)。
 *   · 只刷**空闲时钟**, 不调 input_pm_boost_now()、不改手势机、不产生任何动作
 *     ⇒ "按住不动"照样不算点击也不算长按, 手感与原来完全一致;
 *   · 必须在 s_touch_blocked 早退**之后**调用: 屏保态触摸被屏蔽, 若那时也刷时钟,
 *     屏保监督任务的唤醒判定(input_last_any_ms > s_ss_enter_ms)会自激 ⇒ 永远退不出壁纸。
 *   · 触点卡死/悬空假按最多把息屏推后 1s —— 不值得为它加状态机(真卡死时整个触摸 UI 早就乱了)。 */
static uint32_t s_touch_alive_ms = 0;
/* 屏保期总闸: 屏保的触摸唤醒由 os_core 的**长按 1 秒**手势判定, 而屏保监督任务的
 * 唤醒条件只有 `input_last_any_ms() > enter_ms` 一条。若屏保态也刷这个时钟,
 * 监督任务会立刻判"有人操作"⇒ 变成"一碰就醒", 绕过 1 秒确认。
 * ⇒ 进屏保时 wallpapers 调 input_set_touch_alive(false), 唤醒时恢复 true。 */
static volatile bool s_touch_alive_on = true;
void input_set_touch_alive(bool on) { s_touch_alive_on = on; if (!on) s_touch_alive_ms = 0; }

static void touch_alive_tick(bool pressed)
{
    if (!s_touch_alive_on) return;
    if (!pressed) { s_touch_alive_ms = 0; return; }
    uint32_t t = now_ms();
    if (s_touch_alive_ms && (int32_t)(t - s_touch_alive_ms) < 1000) return;
    s_touch_alive_ms = t;
    s_last_any_ms = t;
}

static menu_action_t touch_gesture_poll(void) {
    /* V1.2 性能: 本函数每帧被 input_get_action 调用, 其中"点击/长按/上滑"等日志
     * 原本用 ESP_LOGI —— 每次点按都会经阻塞式控制台写出(一条 ≈5ms @115200).
     * 已统一降为 ESP_LOGD (默认级别静默); 仅保留"状态超时复位"等异常告警为 WARN.
     * 排查时: esp_log_level_set("INPUT", ESP_LOG_DEBUG). */
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
    /* ★ 2026-11-22: 只要手指按着就算"有人在操作", 立即重置空闲计时(见 touch_alive_tick 注释) */
    touch_alive_tick(pt.pressed);
    /* ★★★ 2026-10-31: 覆盖层(下拉快捷栏)独占期间, **页面级手势一律不产生**。
     *   否则用户横向拖音量条时, 只要纵向位移够大就会被判成"底部上滑返回"，
     *   直接把游戏/页面退掉 (用户报的"快捷栏手势与游戏操作冲突")。
     *   这里只同步"是否按住"(覆盖层要读), 坐标仍由 input_poll_touch / 覆盖层自己写，
     *   两边都走 touch_map_screen 的同一套映射，不会出现两套坐标系。 */
    if (s_overlay_lock) {
        s_touch_down = pt.pressed;
        s_tg_state = TG_IDLE;        /* 复位手势机: 解锁后不会接着用半截手势 */
        s_tg_long_fired = false;
        s_tg_dragged    = false;
        return MENU_ACTION_NONE;
    }
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
            s_tg_dragged    = false;   /* 新手势: 还没拖动过 */
            s_tg_nav_release = false;   /* 新手势开始: 清上一轮的导航手势标记 */
            /* 底部上滑=返回: 通知 gesture_back 模块本次按下起点 (逻辑坐标) */
            gesture_back_on_press(s_tg_sx0, s_tg_sy0, t);
            /* V1.4.x: 触摸按下不再在此统一震动 — "点空白/无效区不震".
             * 震动改由 os 层在"实际命中有效 UI 元素(os_handle_touch 消费成功)"时触发一次,
             * 由此实现"点中才震、点空白无反应"(见 os_core/main 分发处). */
        }
        break;

    case TG_PRESS:
        if (pt.pressed) {
            s_tg_x = pt.x;
            s_tg_y = pt.y;
            /* ★ 2026-11-24 linit「拖动列表被误判长按收藏」:
             *   长按判定原来只看**当前净位移** < 20px ⇒ 拖动列表后又拖回起点附近
             *   (或慢慢挪) 仍会被判成长按, 于是"翻列表翻着翻着就被收藏了"。
             *   这里加**粘连标志**: 本次手势只要出现过 ≥TOUCH_STILL_MAX 的位移,
             *   就永久标记为"拖过了", 之后即使手指回到原处也绝不再判长按。
             *   语义 = 动过就不是长按, 与用户直觉一致; 长按收藏仍需按住不动 2s。 */
            {
                int dx0 = s_tg_x - s_tg_x0, dy0 = s_tg_y - s_tg_y0;
                int adx0 = dx0 < 0 ? -dx0 : dx0;
                int ady0 = dy0 < 0 ? -dy0 : dy0;
                if (adx0 >= TOUCH_STILL_MAX || ady0 >= TOUCH_STILL_MAX) s_tg_dragged = true;
            }
            /* 底部上滑=返回: 速度采样/跨档触觉由 gesture_back 模块在 move 时统一处理 */
            gesture_back_on_move(s_touch_sx, s_touch_sy, t);
            /* V1.0.9x: 游戏列表/收藏区域长按(按住即成, 无需松手) → 立即投递 LONG_PRESS.
             * 满足"长按收藏不用放手"的需求. 需按住期间几乎不动(<TOUCH_STILL_MAX),
             * 且按下点在状态栏以下(游戏区). 已触发或正在拖动(位移超阈值)则跳过. */
            if (!s_tg_long_fired && !s_tg_dragged && s_tg_sy0 >= TOUCH_STATUS_H &&
                (int32_t)(t - s_tg_t0) >= TOUCH_LONG_PRESS_MS &&
                (int32_t)(t - s_tg_t0) < 9000) {
                int dx = s_tg_x - s_tg_x0, dy = s_tg_y - s_tg_y0;
                int adx = dx < 0 ? -dx : dx;
                int ady = dy < 0 ? -dy : dy;
                if (adx < TOUCH_STILL_MAX && ady < TOUCH_STILL_MAX) {
                    s_tg_long_fired = true;
                    s_tg_state = TG_WAIT_RELEASE;
                    s_tap_x = s_tg_sx0; s_tap_y = s_tg_sy0;
                    ESP_LOGD(TAG, "长按 %lums -> LONG_PRESS(按住) @ %d,%d",
                             (unsigned long)(t - s_tg_t0), s_tap_x, s_tap_y);
                    out = MENU_ACTION_LONG_PRESS;
                }
            }
            /* V1.0.68: 长按状态栏 3 秒 -> 投递 HOME (回桌面, 任何界面生效).
             * 几何+时长判定已迁至 gesture_back 模块 (gesture_home_decide);
             * 与 game-collection 长按共享的触发守卫 (s_tg_long_fired) 仍在此处. */
            if (!s_tg_long_fired &&
                gesture_home_decide(s_tg_sx0, s_tg_sy0, s_touch_sx, s_touch_sy,
                                    (uint32_t)(t - s_tg_t0))) {
                s_tg_long_fired = true;
                s_tg_state = TG_WAIT_RELEASE;
                ESP_LOGD(TAG, "长按状态栏 3s -> HOME");
                out = MENU_ACTION_HOME;
            }
            /* V1.0.xx: 触摸状态超时保护 — 按住超过 9 秒且无位移, 强制复位.
             * 防止触摸芯片读失败导致 s_touch_down 状态残留, 菜单无法滑动.
             * (5 秒被主菜单长按删除用, 故提到 9 秒避免误复位) */
            if ((int32_t)(t - s_tg_t0) >= 9000) {
                int dx = s_tg_x - s_tg_x0, dy = s_tg_y - s_tg_y0;
                int adx = dx < 0 ? -dx : dx;
                int ady = dy < 0 ? -dy : dy;
                if (adx < 10 && ady < 10) {
                    ESP_LOGD(TAG, "触摸状态超时 9s 复位 (可能芯片读失败导致状态残留)");
                    s_tg_state = TG_IDLE;
                    s_tg_long_fired = false;
                    s_tg_dragged    = false;
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
            /* V1.1.0: 系统导航手势(底部上滑=返回/强制退出)统一由 gesture_back 模块判定.
             * 上滑判定必须在点击/阅读器横滑之前, 否则 <36px 的慢上滑和带轻微横移的快上滑
             * 都会被"点击"分支吃掉 (这正是旧实现"所有页面上滑都失效"的真因).
             * 这里一次性算出 gb, 后续 if/else 链按 NONE/BACK/CONFIRM 分流. */
            menu_action_t gb = gesture_back_on_release(s_touch_sx, s_touch_sy, t);

            /* V1.0.68: 长按(≥800ms)且按住期间未移动 → LONG_PRESS (游戏列表收藏等).
             * 松手时判定: 拖动(移动超 20px)不触发, 避免拖动误收藏.
             * ★ 2026-11-24: 加 !s_tg_dragged —— 拖动过又拖回起点附近也算拖动, 不判长按. */
            if (!s_tg_dragged && dt >= TOUCH_LONG_PRESS_MS && s_tg_sy0 >= TOUCH_STATUS_H &&
                adx < TOUCH_STILL_MAX && ady < TOUCH_STILL_MAX) {
                out = MENU_ACTION_LONG_PRESS;
                s_tap_x = s_tg_sx0; s_tap_y = s_tg_sy0;
                ESP_LOGD(TAG, "长按 %lums -> LONG_PRESS @ %d,%d",
                         (unsigned long)dt, s_tap_x, s_tap_y);
            } else if (dt >= TOUCH_LONG_PRESS_MS && s_tg_sy0 >= TOUCH_STATUS_H &&
                       adx < TOUCH_SWIPE_MIN && ady < TOUCH_SWIPE_MIN) {
                /* V1.5.x: 长按时长已到但手指微移 (20~40px) — 用户意图是长按
                 * (收藏/拖动), 位移恰好破坏 LONG_PRESS 判定. 若下沉到 CONFIRM 会
                 * 误打开游戏/误触操作 (暴龙机"自动打开"的次要路径), 这里直接吞掉. */
                ESP_LOGD(TAG, "长按微移 %lums dx=%d dy=%d -> 忽略 (防误触)", (unsigned long)dt, adx, ady);
            } else if (adx < TOUCH_SWIPE_MIN && ady < TOUCH_SWIPE_MIN) {
                /* 位移小 = 系统导航手势(底部上滑)优先, 非上滑则回落为点击.
                 * tap 坐标用旋转后的逻辑坐标, 供阅读器分区命中.
                 * 震动: 按下瞬间已触发 (TG_IDLE 按下沿), 快速点击(<0.5s)不再补震,
                 * 避免"咔哒"两次; 按住 >0.5s 松手统一补"弹起" (见下方释放收尾). */
                if (gb != MENU_ACTION_NONE) {
                    out = gb;
                    s_tg_nav_release = true;   /* 导航手势: 触觉交给动作处理侧, 此处不补震 */
                    ESP_LOGD(TAG, "底部上滑 -> %s",
                             gb == MENU_ACTION_BACK ? "返回" : "强制退出");
                } else {
                    out = MENU_ACTION_CONFIRM;
                    s_tap_x = s_tg_sx0; s_tap_y = s_tg_sy0;
                    ESP_LOGD(TAG, "触摸点击 (%lums) raw=%d,%d -> %d,%d",
                             (unsigned long)dt, s_tg_x0, s_tg_y0, s_tap_x, s_tap_y);
                }
            } else if (s_swipe_turn && ady < TOUCH_SWIPE_MIN * 2 &&
                       dx != 0 && adx >= TOUCH_SWIPE_MIN) {
                /* 阅读器横滑翻页: 横位移显著(≥40)且竖向较小(<80)视为左右滑动.
                 * 屏逻辑坐标已由 input 旋转, 此处用物理方向 (横向滑动朝向) 判左右.
                 * 向左滑(dx<0)=上一页, 向右滑(dx>0)=下一页. */
                out = (dx < 0) ? MENU_ACTION_LEFT : MENU_ACTION_RIGHT;
                ESP_LOGD(TAG, "阅读器横滑 (%d,%d)->(%d,%d) dx=%d -> %s",
                         s_tg_x0, s_tg_y0, s_tg_x, s_tg_y, dx,
                         (dx < 0) ? "LEFT" : "RIGHT");
            } else {
                /* 较大位移且非阅读器横滑: 先尝试解析为"底部上滑=返回", 再兜底为底部带点击.
                 * gesture_back 内部已排除"非向上/横移过大/起点不在底部带"的情况, 返回 NONE.
                 * 兜底: 起点在底部带 + 位移较小 (≤80px) → 视为点击, 保证底部控件可点. */
                if (gb != MENU_ACTION_NONE) {
                    out = gb;
                    s_tg_nav_release = true;   /* 导航手势: 触觉交给动作处理侧, 此处不补震 */
                    ESP_LOGD(TAG, "底部上滑 -> %s",
                             gb == MENU_ACTION_BACK ? "返回" : "强制退出");
                } else if (gesture_back_in_bottom_zone(s_tg_sx0, s_tg_sy0) &&
                           adx < 80 && ady < 80) {
                    out = MENU_ACTION_CONFIRM;
                    s_tap_x = s_tg_sx0; s_tap_y = s_tg_sy0;
                    ESP_LOGD(TAG, "底部带非上滑位移 (%d,%d)->(%d,%d) -> 点击",
                             s_tg_x0, s_tg_y0, s_tg_x, s_tg_y);
                }
                /* 其余 (明显误滑 / 非阅读器的横向大位移) → 不产生动作 (NONE) */
            }
/* V1.0.8x: 已删除左右/上下滑动 -> 方向键. 打开仅靠点击确定位置. */
    /* V1.4.x: 按住 >0.5s 松手 → 补一次"弹起"震动 (机械按键按下去/放上来
     * 的段落感; 快速点击按下时已震, 此处 <0.5s 不补, 保证一次点击一声).
     * 主菜单/软件管家页 (os_core 置 ctx_block) 不补.
     * V1.7.x: 底部上滑等系统导航手势也不补 —— 它的触觉由动作处理侧统一发出,
     * 否则"按住 >0.5s 再上滑返回"会连响两下. */
    /* 点击震动统一在 main.c 分发处做（os_handle_touch 命中才震），这里只管长按松手的"弹起" */
    if (!s_key_sim_ctx_block && !s_tg_nav_release && dt > TOUCH_HOLD_VIB_MS) vibrator_click();
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
    /* V1.1.0: 已删除"跟手"指示条 (用户明确不要黑线). 旧 input_back_gesture_state 与
     * s_bg_hint/armed/armed2/pct/finger_y 一并清掉, 导航手势(底部上滑=返回)统归
     * gesture_back 模块; 开关由 gesture_back_enable(os_push/os_pop 联动)管控. */
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
    /* ★ 2026-11-22: 三键状态机已搬到 input_key_task(5ms 周期), 这里只消费它排好的动作。
     *   原来这里每帧跑一遍 tick() ⇒ 采样周期 = UI 帧周期(空闲 60ms), 正是
     *   "松手后锁屏慢"的来源。"三键整组失效"体检(input_key_guard)也一并搬走,
     *   避免与扫描任务并发操同一批 GPIO。 */
    const char *kname = "-";
    menu_action_t a = key_q_pop(&kname);
    if (a != MENU_ACTION_NONE) {
        s_tap_x = s_tap_y = -1; s_last_any_ms = now_ms(); fb_game_exit_vib(a, kname);
        /* ★ 2026-11-22: 动作流水 —— 排"按键按了没反应"必须先把**输入层到底有没有
         *   产生动作**钉死。这里每产生一个动作打一行(量极小), 与 os 层的分发日志配对:
         *   有这条没下面那条 ⇒ 动作在半路被吞; 连这条都没有 ⇒ 按键/状态机没出动作。 */
        s_act_cnt++;
        s_last_act = (int)a;
        s_last_act_ms = s_last_any_ms;
        /* ★ 2026-11-22 性能: 原为 ESP_LOGI —— 每产生一个动作就阻塞式写一条 ≈5ms@115200,
         *   连点/连按时会持续拖慢输入投递。降为 DEBUG (与本文件其它每按日志口径一致),
         *   排查"按键没反应"时 `loglevel debug` 即可看到。 */
        ESP_LOGD(TAG, "ACT %s -> %d (第 %u 个)", kname, (int)a, (unsigned)s_act_cnt);
        return a;
    }
    /* ★★★ 2026-11-24 linit「实体手柄长按收藏键无效」: 手柄功能键里只有下面 nav[] 那 7 个
     *   进了导航表, **F_FAV(多功能/收藏键) 一个动作都不产生** —— 长按既收藏不了, 也没有
     *   任何提示, 因为压根没有事件上行。这里补一条独立的长按检测:
     *     · 只在 nav 开启且非游戏态生效: 游戏内 F_FAV 是引擎的功能键(SELECT/START/F 等),
     *       绝不能吞; 按键映射向导(nav 关闭)时保持原样, 长按的正是"要映射的那个键"。
     *     · 短按不发动作(与原行为一致), 按住 ≥700ms 投递一次 KEY_FAV。
     *     · 投 KEY_FAV 而不是 LONG_PRESS: 后者是全系统"长按"语义(应用管家长按弹窗、
     *       存储页长按删除…), 拿它当收藏键会在别的页面误触发别的长按操作。
     *   收藏结果(已收藏/收藏失败/已取消收藏 + 震动)由 os_pane 统一给出, 不在这里重复。 */
    if (s_gamepad_nav_enabled && !s_gamepad_gamemode && bt_manager_is_connected()) {
        static bool     fav_prev  = false;
        static bool     fav_fired = false;
        static uint32_t fav_t0    = 0;
        uint32_t tn = now_ms();
        bool f = bt_manager_is_key_pressed(F_FAV);
        if (f && !fav_prev) { fav_t0 = tn; fav_fired = false; }
        if (f && !fav_fired && (int32_t)(tn - fav_t0) >= 700) {
            fav_fired = true;                    /* 长按只发一次, 松手后才能再发 */
            s_tap_x = s_tap_y = -1;
            s_last_any_ms = tn;
            ESP_LOGD(TAG, "手柄收藏键长按 -> KEY_FAV");
            return MENU_ACTION_KEY_FAV;
        }
        if (!f) fav_fired = false;
        fav_prev = f;
    }
    /* 游戏中 (nav 关闭 且 gamemode 生效): 手柄"退出到菜单"键(F_EXIT) 作为游戏退出键。
     * ★★★ 2026-11-24 linit「退出键行为不一致」: 原来这里返回 **HOME**, 而 HOME 是
     *   "长按返回 3s 逃生口"的语义 —— os_core 里 HOME 直接 os_force_home(不询问),
     *   eng_task_run_loop 里 home_hard ⇒ 不弹确认直接 break。于是**短按手柄退出键会
     *   绕过确认框直接退出游戏**, 与界面退出(短按返回 → 弹「退出程序?」)是两套行为。
     *   现在改投 BACK: 与 F_BACK/物理返回键**完全同路**, 由引擎统一走 exit_confirm,
     *   该弹框就弹框, 一套逻辑。
     * 手柄"返回"键(F_BACK)在游戏内仍不作为退出键, 不映射成 BACK
     * (用户规范: 遥控返回键 ≠ 硬件返回键)。
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
            fb_game_exit_vib(MENU_ACTION_BACK, NULL); return MENU_ACTION_BACK;
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
            /* ★★★ 2026-11-24 【手柄连点没反馈 / 越点越慢】快路 ★★★
             *   根因: 本块原来是**电平查询**, 而主循环空闲帧周期是 60ms
             *   (main.c: budget_ms = touch_hit ? 16 : 60), 再叠加下面 NAV_DEBOUNCE_MS=30
             *   的稳定窗 —— 一次短促点击整段落在两次采样之间就被彻底丢弃; 点得越快,
             *   落在间隙的概率越大, 体感就是"越快越没反应"。
             *   现在: bt_manager 在**每份 HID 报告**(≈60~100Hz, 与 UI 帧率无关)里已确认
             *   上升沿并锁存, 这里取到就直接投递, 跳过节拍防抖; 唤醒主循环由
             *   bt_manager 的 key_edge 回调负责 (见 input_set_wakeup_task)。
             *   ⛔ 电平路径与长按重复完全保留: 按住滚动仍由下面的 raw/new_db 驱动,
             *      短脉冲走快路、按住走电平, 两者不会重复投递 (快路已把 debounced 置位)。 */
            if (bt_manager_take_key_edge(nav[i].key)) {
                st[i].prev_raw      = true;
                st[i].raw_change_ms = now;
                st[i].debounced     = true;
                st[i].released_seen = true;   /* 事件域已做稳定确认, 不受"开局卡键"守卫影响 */
                st[i].press_start_ms  = now;
                st[i].repeat_next_ms  = now + NAV_REPEAT_DELAY;
                st[i].long_emitted    = false;
                ESP_LOGI(TAG, "nav边沿 key=%d act=%d (HID事件域)", (int)nav[i].key, (int)nav[i].act);
                s_tap_x = s_tap_y = -1;
                s_last_any_ms = now_ms();
                return nav[i].act;
            }
            /* ★ 2026-11-22: 手柄与蓝牙键盘的导航**同一个位图消费** —— 键盘按键
             *   由 input_kbd_event 合成到 kbd_nav_raw, 这里 OR 进来即可复用
             *   下面整套防抖/自动重复/长按逻辑, 手感与手柄完全一致。 */
            bool raw = bt_manager_is_key_pressed(nav[i].key) || kbd_nav_raw(nav[i].key);
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
                ESP_LOGD(TAG, "nav键%d 未见过松开即按下, 疑似卡住, 忽略", (int)nav[i].key);
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
        fb_game_exit_vib(ta, NULL);    /* V1.2.x: 游戏内底部上滑=返回 → 游戏震动 */
        return ta;
    }
    return MENU_ACTION_NONE;
}

bool input_is_held(int idx) {
    if (idx < 0 || idx > 1) return false;
    if (s_btns[idx].gpio < 0) return false;   /* ★ 板型没接这颗键 ⇒ 别拿 -1 去读电平(会恒判按下) */
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

/* ★ 2026-10-30 (A6): "底部拖动屏蔽带"页面级开关。
 * 默认 true = 保持既有行为 (底带内起手 + 位移≥40px 时不给下层坐标, 防菜单/列表被误拖)。
 * 全屏绘图页 (白板) 需要整面可用 —— 底带在画布下沿, 屏蔽会让笔迹被凭空截断。
 * 由 os_push/os_pop 按模块的 `no_bottom_shield` 标记统一设置, **不依赖各页自行开关**,
 * 避免某页 on_exit 被跳过导致全局卡在关闭态。 */
static bool s_bottom_shield = true;

void input_set_bottom_shield(bool on) { s_bottom_shield = on; }

/* ★ 引擎内 overlay 钩子 (见 input.h): os 层注入实现, 引擎每帧调用。
 * 用函数指针而不是直接调 os_* —— 引擎组件不依赖 os, 避免破坏分层单向。 */
static engine_overlay_fn s_engine_quickbar_fn = NULL;
/* ★ 2026-10-31: 拆成「收手势/执行动作」与「只画」两段。
 *   原因: board_shim 的视频刷新任务是在**持有帧锁**的情况下调 overlay 的,
 *   而快捷栏里点一下 Wi-Fi 会走 esp_wifi_start() (1~3 秒) ——
 *   锁被占住这么久, 模拟任务一帧都提交不了 ⇒ 整个游戏冻住 (用户报的"点 Wi-Fi 就卡死")。
 *   拆开后: tick 在锁外跑 (动作再慢也不占锁), draw 在锁内跑 (只画像素, 微秒级)。 */
static engine_overlay_tick_fn s_engine_quickbar_tick_fn = NULL;
static engine_overlay_fn      s_engine_quickbar_draw_fn = NULL;

void engine_overlay_set_quickbar(engine_overlay_fn fn) { s_engine_quickbar_fn = fn; }
void engine_overlay_set_quickbar_split(engine_overlay_tick_fn tick, engine_overlay_fn draw)
{
    s_engine_quickbar_tick_fn = tick;
    s_engine_quickbar_draw_fn = draw;
}

void engine_overlay_quickbar(void *lcd)
{
    if (s_engine_quickbar_tick_fn) s_engine_quickbar_tick_fn();
    if (s_engine_quickbar_draw_fn) { if (lcd) s_engine_quickbar_draw_fn(lcd); return; }
    if (s_engine_quickbar_fn && lcd) s_engine_quickbar_fn(lcd);
}

void engine_overlay_quickbar_tick(void)
{
    if (s_engine_quickbar_tick_fn) s_engine_quickbar_tick_fn();
}

void engine_overlay_quickbar_draw(void *lcd)
{
    if (!lcd) return;
    if (s_engine_quickbar_draw_fn) s_engine_quickbar_draw_fn(lcd);
    else if (s_engine_quickbar_fn) s_engine_quickbar_fn(lcd);
}

/* ★ 覆盖层触摸独占锁 (2026-10-29, 下拉快捷栏): 置位后普通调用者一律返回"无触摸",
 *   避免从顶部下拉时下方主菜单/列表跟着滚动。覆盖层自己用 _raw 版本读真实坐标。
 * ★ 2026-10-31: 加 volatile —— 写方(主循环/视频任务)与读方(引擎任务)不同,
 *   不加会被编译器缓存成常量, 表现为"锁不住 / 收不起来"。
 *   (变量定义已提前到触摸状态区, 见 s_overlay_lock) */

void input_set_overlay_lock(bool on) { s_overlay_lock = on; }
bool input_overlay_locked(void)      { return s_overlay_lock; }

/* ★★★ 2026-10-31: 覆盖层读触摸 —— **只读快照, 不跑手势状态机**。
 *   原来这里是把锁临时放开再调 input_get_touch_pos(): 那会完整执行
 *   s_o_armed / s_o_zone / 屏蔽带判定, 而这些状态由主循环**同时**消费 ⇒
 *   两个任务抢同一套锁存状态 → 底部上滑返回/拖动判定错乱, 表现就是
 *   "快捷栏关掉之后触摸变怪"。现在只取"当前是否按住 + 屏幕坐标"。 */
bool input_get_touch_pos_raw(int *x, int *y)
{
    if (s_touch_blocked || !s_touch_down) return false;
    if (x) *x = s_touch_sx;
    if (y) *y = s_touch_sy;
    return true;
}

bool input_get_touch_pos(int *x, int *y) {
    if (s_touch_blocked) return false;   /* 休眠禁触屏: 无触摸坐标 */
    if (s_overlay_lock) return false;    /* 覆盖层独占: 快捷栏手势中/展开中 → 下层拿不到 */
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
            /* 屏蔽带关闭时 (白板等全屏绘图页) 恒为 false → 永不隐藏坐标 */
            s_o_zone = s_bottom_shield &&
                       input_in_bottom_zone(s_o_sx, s_o_sy,
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
static bool s_dual_touch = false;
void input_set_dual_touch(bool on) { s_dual_touch = on; }
bool input_dual_touch(void) { return s_dual_touch; }

void input_get_touch_multi(tp_point_t pts[TP_MAX_POINTS], int *count) {
    if (s_touch_blocked) { if (count) *count = 0; return; }   /* 休眠禁触屏: 无多点 */
    if (count) *count = 0;
    if (!pts) return;
    for (int i = 0; i < TP_MAX_POINTS; i++) pts[i].pressed = false;
    /* ★★★ 2026-10-31 (快捷栏与引擎抢触摸的根因之一):
     *   原来这里**不看** s_overlay_lock ⇒ 快捷栏展开时, 虚拟手柄照样拿得到触点,
     *   于是"同一组触摸"被两方同时处理: 用户拖音量条, 游戏同时收到方向键;
     *   收起后引擎的 s_joypad 还停在按下态 ⇒ 角色自己走。
     *   覆盖层独占期间一律返回"没有触点": 下游 virtual_keys_poll_multi 收到
     *   d0=false 会自动把摇杆/按键全部松开, 不需要引擎各写一份释放逻辑。 */
    if (s_overlay_lock) return;
    tp_point_t p0;
    touch_read_once(&p0);   /* 确保本帧已刷新缓存 (含全部触点) */
    int c = s_touch_cached_count;
    if (c <= 0 || !s_touch_cached.pressed) {
        return;
    }
    int n = (c > TP_MAX_POINTS) ? TP_MAX_POINTS : c;
    /* ★ 2026-10-20: 闸门改成"虚拟手柄开着就允许第二点" ——
     *   原来只在拼 joypad 那一处临时开, 引擎自己直接调多点时会被拦成单点。 */
    bool dual = s_dual_touch || virtual_keys_is_enabled();
    if (!dual && n > 1) {
        for (int i = 1; i < TP_MAX_POINTS; i++) pts[i].pressed = false;
        n = 1;
    }
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
    /* ★ 2026-11-22: 引擎是**阻塞循环**(page_engine / pal_entry / lavax), 主循环的
     *   input_pm_boost_tick() 在里面根本不跑。这里每帧释放一次过期升频锁,
     *   否则游戏里按一下键就永久 240MHz, 等于把刚拆掉的常驻锁又装回去。
     *   顺序: 先释放过期的 → 再采样 → 有键时 input_mark_activity 重新升频。 */
    input_pm_boost_tick();
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
        bool dual_prev = s_dual_touch;
        s_dual_touch = true;               /* ★ 仅此刻允许第二点: 虚拟手柄在场 */
        input_get_touch_multi(pts, &c);
        s_dual_touch = dual_prev;          /* ★ 恢复外部设置, 不能无条件写 false (会关掉别人开的多点) */
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
             *  - 按键点击音 (按键声音开关) —— 用 TONE_EFFECT_KEY: **1/5 音量**
             *    (用户规范 2026-09-29: 游戏里连按虚拟按键原音量过吵)
             *  - 游戏操作震动 (触摸震动开关) */
            /* ★ 2026-11-22 性能: 改用非阻塞版。本函数每帧被游戏/Lua 页轮询调用,
             *   原来 tone_play_effect(TONE_EFFECT_KEY) 内部 vTaskDelay(45ms)
             *   ⇒ 每次按键把 UI 线程(或引擎线程)按住 45ms, 连按就明显卡顿。
             *   音色/时长/音量不变 (1200Hz/45ms/1-5 音量), 只是立刻返回。 */
            if (s_eng_sound) tone_key_click_async();
            /* ★ 2026-11-24: 时长 30→20ms, 与 gam4980 的虚拟手柄通道统一 ——
             *   30ms 对连点偏"重", 20ms 已足够有机械键手感, 连点节奏更跟手。 */
            if (s_eng_vib)   vibrator_tap_game(20, 120);
        }
    } else {
        s_jp_prev = j;   /* 非游戏态: 仅同步, 不产生震动 */
    }
    /* 任意游戏键按住 (低电平有效: 位=0 即按下) = "正在玩不睡".
     * ⚠ 走 input_mark_game_held() 而非 input_mark_activity(): 前者只抑制"自动进屏保",
     *   不参与"退出屏保"判定 —— 否则游戏中按电源键进的屏保会被"键还按着"立刻踢出来。 */
    input_mark_game_held(j != 0xFF);
    return j;
}
