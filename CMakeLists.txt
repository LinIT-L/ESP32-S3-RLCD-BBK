# ESP32-S3-RLCD-4.2 步步高 4980/4988 游戏模拟器
# 基于 gam4980 libretro 核心 + 自包含 ST7305 驱动

cmake_minimum_required(VERSION 3.16)

# 跳过 git submodule 检查
set(ENV{IDF_SKIP_CHECK_SUBMODULES} "1")
set(IDF_SKIP_CHECK_SUBMODULES 1)

# 排除不需要的 ESP-IDF 组件
# V1.0.67: 移除 esp_http_server/esp_https_server 排除, 供 web_gamepad(网页手柄) 使用
set(EXCLUDE_COMPONENTS
    mqtt
    openthread
    protobuf-c
    cmock
    unity
    spiffs
    json
    tcpip_adapter
    protocomm
    wifi_provisioning
    esp_local_ctrl
    esp_netif_stack
)

# === V1.x: 方案还原 — 使用官方 esp-idf 蓝牙 (Bluedroid + esp_hid), 移除 bluepad32/btstack ===

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(LinTOS)
