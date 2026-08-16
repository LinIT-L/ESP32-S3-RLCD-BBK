set(SOURCES "menu_system.c" "font_zh.c" "sd_scan.c" "file_browser.c" "favorites.c" "wallpapers.c")

set(REQUIRES_LIST st7305 driver fatfs esp_driver_gpio sdmmc usbh_msc gam4980 arduboy_avr audio_player gb_emu gbc_emu nes_emu board_shim engine_manager book_reader web_gamepad virtual_keys esp_http_client tone_player touch_panel mbedtls)
set(PRIV_REQUIRES_LIST bt esp_hid nvs_flash)

if(CONFIG_BT_ENABLED)
    list(APPEND SOURCES "bt_manager.c")
endif()

# V1.0.46: Wi-Fi 连网功能 (STA 连接, 虚拟键盘输入密码)
if(CONFIG_ESP_WIFI_ENABLED)
    list(APPEND SOURCES "wifi_manager.c")
    list(APPEND PRIV_REQUIRES_LIST esp_wifi esp_netif esp_http_client)
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

# V1.0.64: 菜单中文字库紧凑二进制 (原 C 源码 1.67MB -> 252KB, Flash XIP)
target_add_binary_data(${COMPONENT_LIB} "font_zh.bin" BINARY)
