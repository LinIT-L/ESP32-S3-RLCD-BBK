# components/drivers/espsdr — L1 驱动层: ESP32-S3 原始 I/Q 采集
# 依赖 esp_phy / esp_wifi (ROM PHY 符号 + 起 PHY 用), 只向下依赖 IDF 组件。
idf_component_register(
    SRCS "espsdr.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_phy esp_wifi esp_timer esp_event nvs_flash
)
# 把 64KB IDE 缓冲区的边界断言挂上去 (只含 ASSERT, 不改布局)。
# ⚠ 与 linker.lf 不同, 这种纯 .ld 要用 -T 传, 不会被 LDFRAGMENTS 发现。
target_link_options(${COMPONENT_LIB} INTERFACE
    "-T${CMAKE_CURRENT_LIST_DIR}/sram_guard.ld")
