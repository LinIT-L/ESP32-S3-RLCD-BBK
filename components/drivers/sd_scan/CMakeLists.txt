# components/sd_scan — TF/SD 卡挂载与扫描库 (从 menu 拆出)
# 挂载/卸载/容量/文件扫描(游戏/图片)/重命名/格式化 + SD 监控任务
idf_component_register(
    SRCS "sd_scan.c" "sd_arb.c"
    INCLUDE_DIRS "."
    REQUIRES sdmmc fatfs esp_driver_gpio esp_driver_sdmmc freertos
    PRIV_REQUIRES
        nvs_flash
        esp_pm         # ★ 2026-11-25: sd_arb 的 ESP_PM_NO_LIGHT_SLEEP 锁
                       #   (覆盖"一次逻辑访问 = 多次事务"之间的浅睡空档)
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    # ★ 2026-10-02 版本管理整合: 撤掉下面两条屏蔽 —— 它们曾让"定义了从未被调用"的静态函数长期不报警
    #   (全仓实测 25+ 个)。本项目自研代码要保持这层闸门; 第三方引擎目录另论。
    -Wno-error=format-truncation
)
