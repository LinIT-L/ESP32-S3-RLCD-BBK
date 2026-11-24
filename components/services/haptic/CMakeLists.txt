# 触觉效果库: 效果表 + 段式播放引擎 (驱动无关) + TM6604 PWM 后端
# svc_cfg: 只用于持久化「丰富震动」开关 (os_settings / haptic_rich)
idf_component_register(SRCS "haptic.c" "haptic_backend_pwm.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_timer vibrator svc_cfg
    )
