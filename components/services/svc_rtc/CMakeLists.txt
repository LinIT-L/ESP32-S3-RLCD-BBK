# components/svc_rtc — 外挂 RTC（DS3231MZ+）读写 + 自动校时
idf_component_register(
    SRCS "svc_rtc.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES hw_cfg svc_time touch_panel
    REQUIRES
        esp_driver_i2c freertos esp_timer
    )
