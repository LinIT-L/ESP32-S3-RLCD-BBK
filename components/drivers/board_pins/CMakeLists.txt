# board_pins — **引脚唯一真相源**（三张板型表 + 运行时当前表；不含识别/NVS/驱动逻辑）
#
# ★ 2026-10-13 建: 以前按键引脚在 3 个地方各写一份
#   (main/user_config.h · components/system/input/input.c · components/ui/wallpapers/sleep.c)
#   ⇒ 改板子只改一处即可, 不会再漏。
# ★ 2026-10-18: 板型识别统一归 `services/hw_cfg`；本组件只剩"表"，不再依赖 nvs_flash，
#   也不再使用 legacy i2c 驱动（那正是 CONFIG_I2C_SKIP_LEGACY_CONFLICT_CHECK 的来源）。
# ⚠ 只放**引脚号/极性**这类常量; 任何驱动逻辑都不许放进来。
idf_component_register(
    SRCS "board_pins.c"
    INCLUDE_DIRS "include"
    REQUIRES driver log
)
