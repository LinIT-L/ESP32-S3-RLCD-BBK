idf_component_register(
    SRCS "usbh_msc_sdspi.c"
    INCLUDE_DIRS "include"
    REQUIRES
        tinyusb sdmmc driver
        sd_scan            # 卡所有权交接 (sd_vfs_handoff/sd_vfs_reclaim) + sd_arb 校验
        web_files          # ★ 2026-11-25: 交卡前必须由会话自己清场(quiesce/resume),
                           #   不能再指望调用方记得配对 —— 漏一次就是悬垂 FILE* 崩溃
    PRIV_REQUIRES
        esp_tinyusb        # tinyusb_msc_* 官方 storage API
        svc_log            # 挂载期间暂停串口控制台(只写文件缓冲)
        usb_hid            # 模式切换 / PHY 归属 / Serial-JTAG 驱动装卸
)
