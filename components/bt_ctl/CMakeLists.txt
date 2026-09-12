# bt_ctl — 蓝牙子系统独立组件 (从 menu 拆出).
# 提供 BLE 扫描/HID 设备管理/按键映射/连接管理; 供 menu_system 与 os(gamepad) 共用.
# 依赖: 官方 esp_hid(Bluedroid BLE HID Host) + bt + nvs_flash.
idf_component_register(
    SRCS "bt_core.c" "bt_manager.c" "bt_map.c" "bt_kbd.c" "esp_hid_gap.c"
    INCLUDE_DIRS "include"
    REQUIRES bt esp_hid nvs_flash esp_system esp_timer
)

# 显式公开 include: 依赖本组件的组件 (menu/os) 需经此看到 bt_manager.h 等.
target_include_directories(${COMPONENT_LIB} PUBLIC "${COMPONENT_DIR}/include")

if(CONFIG_BT_ENABLED)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_BT_ENABLED=1)
endif()
