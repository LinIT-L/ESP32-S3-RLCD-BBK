# lvgl_port — LVGL 9.x → ST7305 (400×300 1bpp 反射屏) + 触摸 移植层
#
# 归层: components/system/ → system (rank 3)。跟 os_hid.c / os_net.c 同类 —— 系统级硬件胶水,
# 不是 UI 部件: 它需要 input(system) 与 st7305(drivers), 放 ui 层会构成 ui→system 反向依赖。
# 职责: ①lv_init + 显示对象(1bpp, 双缓冲, PSRAM) ②flush 回调 → st7305 批量位图
#       ③tick(esp_timer 2ms) + lv_timer_handler 任务 ④触摸 pointer indev
idf_component_register(
    SRCS "lvgl_port.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl st7305 input esp_timer esp_common
)
