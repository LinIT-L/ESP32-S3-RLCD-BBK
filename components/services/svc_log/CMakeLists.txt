# components/svc_log — 日志服务 (缓存/落盘/串口暂停) + 崩溃现场自留证 + 崩溃前日志环
idf_component_register(
    SRCS "svc_log.c" "svc_crash.c" "svc_logring.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        sd_scan        # svc_crash: 现场记"卡当时归谁"(sd_arb_state, 纯内存读)
        heap           # svc_crash: panic 时记录内部堆/PSRAM 余量 (heap_caps_get_*)
        xtensa         # svc_crash: XtExcFrame —— 从 info->frame 取 PC/PS/EXCCAUSE/EXCVADDR
    REQUIRES
        freertos esp_common
        esp_system esp_hw_support   # svc_crash: panic_info_t / RTC_NOINIT / backtrace
        nvs_flash                   # svc_crash: 现场转存, 真掉电也能留
        log                         # svc_logring: esp_log_set_vprintf 接管输出
    )
