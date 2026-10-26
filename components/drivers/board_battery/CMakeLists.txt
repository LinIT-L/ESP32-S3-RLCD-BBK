idf_component_register(
    SRCS "board_battery.c"
    INCLUDE_DIRS "include"
    # driver: gpio_config 读 CHRG; board_pins: 引脚唯一真相源 (.chrg)
    # 两者都只在本 .c 里用, 头文件不暴露 ⇒ PRIV_REQUIRES
    PRIV_REQUIRES driver board_pins
    PRIV_REQUIRES
        esp_adc
        esp_timer
        log
        # esp_hw_support: esp_rtc_get_time_us() —— "最近30天电压峰值"窗口的日计时 (跨深睡)。
        # ⛔ 不依赖 usb_hid/nvs: 窗口状态放 RTC 内存 + RTC 计时, 全断电自然重来。
        esp_hw_support
        # nvs_flash: 一个月电池日档 (每天 min/max) 持久化
        nvs_flash
)
