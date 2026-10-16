# board_pins — **载板引脚唯一真相源** (纯头文件组件, 无逻辑)
#
# ★ 2026-10-13 建: 以前按键引脚在 3 个地方各写一份
#   (main/user_config.h · components/system/input/input.c · components/ui/wallpapers/sleep.c)
#   ⇒ 改板子只改一处即可, 不会再漏。
# ⚠ 只放**引脚号/极性**这类常量; 任何驱动逻辑都不许放进来。
idf_component_register(
    SRCS "board_pins.c"
    INCLUDE_DIRS "include"
    REQUIRES driver nvs_flash
)
