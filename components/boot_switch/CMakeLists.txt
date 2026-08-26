# boot_switch — 双系统(双固件)开机切换 (factory <-> ota_0 经 otadata)
idf_component_register(
    SRCS
        "boot_switch.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        "esp_partition"
        "app_update"
        "bootloader_support"
        "esp_system"
        "log"
)