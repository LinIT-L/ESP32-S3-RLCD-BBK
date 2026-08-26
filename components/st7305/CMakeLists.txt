# ST7305 反射式 LCD 驱动 (自包含, 不依赖 u8g2)
# 初始化序列来自微雪官方 u8g2_st7305, 显示数据格式直接对接 ST7305 原生 2-bit gray

idf_component_register(
    SRCS "st7305.c" "bbk_boot.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer
)
