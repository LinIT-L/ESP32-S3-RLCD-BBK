# components/sd_scan — TF/SD 卡挂载与扫描库 (从 menu 拆出)
# 挂载/卸载/容量/文件扫描(游戏/图片)/重命名/格式化 + SD 监控任务
idf_component_register(
    SRCS "sd_scan.c"
    INCLUDE_DIRS "."
    REQUIRES sdmmc fatfs esp_driver_gpio esp_driver_sdmmc
    PRIV_REQUIRES nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-error=format-truncation
)
