set(SOURCES "menu_system.c" "font_zh.c" "font_zh16.c" "sd_scan.c" "file_browser.c" "favorites.c" "wallpapers.c" "mini_apps.c" "diagnosis.c")

set(REQUIRES_LIST st7305 driver fatfs esp_driver_gpio sdmmc usbh_msc usb_hid gam4980 arduboy_avr audio_player gb_emu nes_emu board_shim engine_manager book_reader web_gamepad virtual_keys esp_http_client tone_player touch_panel mbedtls input vpet)
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

# V1.0.64: 菜单中文字库紧凑二进制 (正文冬青黑体24 GB2312全量, Flash XIP)
target_add_binary_data(${COMPONENT_LIB} "font_zh.bin" BINARY)
# V1.0.89: 菜单细字库 (冬青黑体 W3, 与 font_zh.bin 同字符集/格子, 设置->字体设置切换)
target_add_binary_data(${COMPONENT_LIB} "font_zh_light.bin" BINARY)
# V1.0.86: 软件介绍用文泉驿点阵宋体16 (GB2312全量), 独立字库接口 font_zh16
target_add_binary_data(${COMPONENT_LIB} "font_zh16.bin" BINARY)
