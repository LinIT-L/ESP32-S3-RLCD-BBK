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

# === V1.2 模块化: 五层分组目录 (drivers/services/system/engines/ui/libs/tools) ===
# ESP-IDF 默认只扫描 components/ 下一层; 分组后须显式登记各级子目录.
# 铁律: 依赖只允许 上→下 (apps→system→services→drivers), 同层互调须登记白名单.
# 校验脚本: tools/check_deps.py
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_SOURCE_DIR}/components/drivers
    ${CMAKE_SOURCE_DIR}/components/services
    ${CMAKE_SOURCE_DIR}/components/system
    ${CMAKE_SOURCE_DIR}/components/engines
    ${CMAKE_SOURCE_DIR}/components/ui
    ${CMAKE_SOURCE_DIR}/components/libs
    ${CMAKE_SOURCE_DIR}/components/tools
)

# === V1.x: 方案还原 — 使用官方 esp-idf 蓝牙 (Bluedroid + esp_hid), 移除 bluepad32/btstack ===

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(LinTOS)

# === V1.2 分层依赖守卫接入构建 (批次 6) ===
# 每次构建前自动跑 tools/check_deps.py; 出现「未登记的反向依赖」直接让构建失败,
# 防止分层腐化 (ESP-IDF 的 REQUIRES 只管链接顺序, 不管 include 可见性 —— 必须静态兜底).
# 只读检查, 不改任何构建产物; 想临时跳过: idf.py build --target <别的目标> 或直接跑 python3 脚本。
add_custom_target(lin_dep_guard ALL
    COMMAND ${CMAKE_COMMAND} -E env PYTHONPATH= python3
            ${CMAKE_SOURCE_DIR}/tools/check_deps.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "LinTOS 分层依赖守卫 (tools/check_deps.py)"
    VERBATIM
)
# 让最终镜像依赖该检查 → 守卫不过就不出固件
add_dependencies(${CMAKE_PROJECT_NAME}.elf lin_dep_guard)
