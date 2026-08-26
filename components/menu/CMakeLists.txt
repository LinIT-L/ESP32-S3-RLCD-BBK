set(SOURCES "menu_system.c" "file_browser.c" "favorites.c" "diagnosis_v2.c" "whiteboard.c" "terminal.c" "app_cleanup.c")

# V1.x: 壁纸/wifi_manager/fonts/kbd_lib/sd_scan/bt_ctl 已拆为独立组件.
set(REQUIRES_LIST st7305 driver fatfs esp_driver_gpio sdmmc usbh_msc usb_hid gam4980 arduboy_avr audio_player gb_emu nes_emu board_shim engine_manager book_reader web_gamepad virtual_keys esp_http_client tone_player touch_panel mbedtls input vpet vibrator wallpapers wifi_manager fonts kbd_lib sd_scan bt_ctl)
# V1.x: 方案还原 — 使用官方 esp_hid(Bluedroid BLE HID Host), 不再引用 bluepad32/btstack.
set(PRIV_REQUIRES_LIST bt esp_hid nvs_flash spi_flash esp_system esp_timer esp_netif lwip)

if(CONFIG_BT_ENABLED)
    # 蓝牙全部从头重写, 底稿 = ESP-IDF 官方 examples/bluetooth/esp_hid_host.
    # 旧 bt_manager/hid_translator/btstack_* 已隔离到 docs/_bt_old_backup/, 不再编译.
    # list(APPEND SOURCES "bt_core.c")
endif()

# V1.0.46: Wi-Fi 连网 (STA), 启用时额外接入 HTTP 客户端
if(CONFIG_ESP_WIFI_ENABLED)
    list(APPEND REQUIRES_LIST esp_http_client)
endif()

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "."
    REQUIRES ${REQUIRES_LIST}
    PRIV_REQUIRES ${PRIV_REQUIRES_LIST}
)

# V1.0.46: 启用 Wi-Fi 支持 (WIFI_SUPPORT=1)
target_compile_definitions(${COMPONENT_LIB} PRIVATE WIFI_SUPPORT=1)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)

# 字库二进制已随 font_zh/font_zh16 迁至 components/fonts (target_add_binary_data 在 fonts/CMakeLists.txt)
