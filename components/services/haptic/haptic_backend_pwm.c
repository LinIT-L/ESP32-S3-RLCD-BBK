/**
 * @file haptic_backend_pwm.c
 * @brief 默认后端: TM6604 (SOT23-6, 一路 PWM 直驱 1508 X 轴 LRA).
 *
 * 把效果库的"强度 0..100"翻译成现有 vibrator 服务的能力:
 *   set(s)  → vibrator_set(s)   (占空比 = s*255/100, 频率固定 190Hz = LRA 谐振点)
 *   set(0)  → vibrator_stop()
 *   stop()  → vibrator_stop()
 *
 * 能力边界 (据 vibrator.c / vibrator.h 静态审计):
 *   ✅ 强度: 0..100 → 8bit 占空比 (256 级), 运行期可随时改 (每段边界都改)
 *   ✅ 时长: 由上层定时器决定, 任意 ms
 *   ✅ 开/关: duty 归 0 (新板 EN 硬件常开, 没有软件断电脚)
 *   ❌ 频率: 固定 190Hz, 不可调 (改了就不是谐振点, 振幅会掉)
 *   ❌ 制动/波形: TM6604 无寄存器、无 I2C; 主动制动与谐振跟踪是芯片内部行为, 固件无法控制
 *   ❌ 力度曲线/ramp 由芯片完成: 不支持, 只能靠上层分段改占空比
 *
 * ⛔ 输出通道(vibrator_set/stop/claim)只有本文件可以 include; haptic.c 只为注册
 *   "基础反馈→效果"的语义钩子用到 vibrator_set_rich_hook(), 不写 LEDC; Lua 桥完全不碰。
 */
#include "haptic.h"
#include "vibrator.h"

static bool be_init(void)
{
    vibrator_init();          /* 幂等 */
    return vibrator_ready();
}

static bool be_ready(void)
{
    return vibrator_ready();
}

static void be_set(uint8_t strength)
{
    if (strength == 0) vibrator_stop();
    else               vibrator_set(strength);
}

static void be_stop(void)
{
    vibrator_stop();
}

/* 播放期间独占马达: 让旧的 tap / click / 花样入口直接返回, 不再起定时器。
 * 只挡"会起定时器"的入口, 不挡 vibrator_set/stop —— 那是本后端的输出通道。 */
static void be_claim(bool on)
{
    vibrator_set_owner_haptic(on);
}

static const haptic_backend_t s_pwm = {
    .name  = "tm6604-pwm",
    .init  = be_init,
    .ready = be_ready,
    .set   = be_set,
    .stop  = be_stop,
    .claim = be_claim,
};

const haptic_backend_t *haptic_backend_pwm(void)
{
    return &s_pwm;
}
