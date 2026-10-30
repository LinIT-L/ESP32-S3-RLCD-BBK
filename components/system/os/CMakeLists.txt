# components/system/os — L2 系统层内核 (V1.2 模块化)
# 目录约定:
#   kernel/    页面栈/事件路由/输入分发/内存生命周期/服务装配
#   services/  全局服务 (状态栏/修改机图标, 不占页面栈)
#   include/   内核契约头 (os.h 对外, os_internal.h 页契约)
#
# 应用页已拆到 L4 apps 组件 (components/apps) —— os 不再依赖 apps:
#   应用注册由 main 在 os_init() 后调 apps_register_all(); os 只装配自己的服务. 
idf_component_register(
 SRCS "kernel/os_core.c"
 "kernel/os_version.c"   # ★ 版本管理: 固件版本/构建信息唯一来源
 "kernel/os_services.c"
 "kernel/os_housekeeper.c"
 "kernel/os_dialog.c"
 "kernel/os_app.c"
 "kernel/os_pane.c"
 "kernel/os_theme.c"
 "kernel/os_hw.c"
 "kernel/os_notify.c"
 "kernel/os_wifi.c"
 "kernel/os_hid.c"
 "kernel/os_net.c"
 "kernel/os_keyvault.c"
 "kernel/os_mgr.c"
 "services/svc_statusbar.c"
 "services/svc_quickbar.c"
 "services/svc_modtool.c"
 INCLUDE_DIRS "include"
 # ★ 2026-10-13: md_emu 默认停用(省 715KB), 恢复见 os.h 的 LINTOS_ENABLE_MD
 #
 # ★★ 2026-10-28 【构建依赖瘦身 45 → 27】★★
 #   逐项核对依据 = 本组件 include/*.h 与 kernel+services 的 .c 实际引用(脚本枚举),
 #   再叠加"符号级/传递"复核。删掉的 18 项在 os 里没有任何 #include、没有符号引用,
 #   且下游(apps / ui/wallpapers / system/input)都已自行声明:
 #     引擎类   gam4980 gb_emu gbc_emu nes_emu lavax book_reader vpet
 #     UI/工具  kbd_lib virtual_keys board_shim calc lua
 #     服务/IDF web_gamepad usbh_msc esp_http_client esp_netif lwip spi_flash
 #   ⚠ 但 sms_emu / arduboy_avr **必须保留**: apps/pages/page_engine.c 直接
 #     #include "sms_emu.h" / "arduboy_avr.h", 而 apps/CMakeLists 没有声明这两项 ——
 #     它是靠 os 的 REQUIRES 传递拿到的。删掉会直接让 apps 编译失败。
 #     (正确的修法是先给 apps 补声明, 再删这里 —— 留给后续步骤)
 #   ⚠ nvs_flash / esp_system / esp_timer 也不能删: os_mgr.c、os_app.c 用 nvs_handle_t
 #     与 nvs_get_str, 另有 esp_system.h / esp_timer.h 的直接 include。
 REQUIRES power_mgr st7305 ui_common input touch_panel audio_player tone_player vibrator engine_manager wallpapers usb_hid usb_bt usb_net wifi_manager fonts sd_scan board_battery espsdr svc_cfg svc_nvs esp_system bt_ctl keyvault favorites esp_timer nvs_flash
 sms_emu arduboy_avr
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    # ★ 2026-10-02 版本管理整合: 撤掉下面两条屏蔽 —— 它们曾让"定义了从未被调用"的静态函数长期不报警
    #   (全仓实测 25+ 个)。本项目自研代码要保持这层闸门; 第三方引擎目录另论。
 -Wno-error=format-truncation
)

# ============================================================================
#  ★ 版本管理整合 (2026-10-02): 把 git 描述 + 构建时刻编进固件
#  - git 描述 = git describe --tags --always --dirty, 即"这版固件对应哪个提交"
#  - 构建时刻 = 本次 CMake 配置时间 (增量编译不重配 → 会偏旧, 判版本请看 git 描述)
#  - ⚠ macOS 上 /usr/bin/git 是 Xcode shim(会直接报 license 失败) → 优先 homebrew
#  - 取不到 git(未入库/无 git) 时退化为 "unknown", 不影响构建
# ============================================================================
find_program(LINTOS_GIT_EXECUTABLE NAMES git
             PATHS /opt/homebrew/bin /usr/local/bin /usr/bin)
if(LINTOS_GIT_EXECUTABLE)
    execute_process(COMMAND ${LINTOS_GIT_EXECUTABLE} describe --tags --always --dirty
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                    OUTPUT_VARIABLE LINTOS_GIT_DESC
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
endif()
if(NOT LINTOS_GIT_DESC)
    set(LINTOS_GIT_DESC "unknown")
endif()

# 内部迭代号 = **距最近 tag 的提交数** —— 让"细粒度"自动来, 不必手改版本号。
# 用 rev-list 直接数, 不解析 describe 文本: HEAD 正好在 tag 上时 describe 是 "vX.Y.Z-dirty"
# (没有 -N- 段), 解析法会误退化成"总提交数"(实测得到 +63, 应为 +0)。
if(LINTOS_GIT_EXECUTABLE)
    execute_process(COMMAND ${LINTOS_GIT_EXECUTABLE} describe --tags --abbrev=0
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                    OUTPUT_VARIABLE LINTOS_LAST_TAG
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
if(LINTOS_GIT_EXECUTABLE AND LINTOS_LAST_TAG)
    execute_process(COMMAND ${LINTOS_GIT_EXECUTABLE} rev-list --count ${LINTOS_LAST_TAG}..HEAD
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                    OUTPUT_VARIABLE LINTOS_GIT_ITER
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    set(LINTOS_ITER_BASE "距 ${LINTOS_LAST_TAG}")
elseif(LINTOS_GIT_EXECUTABLE)
    # 还没有任何 tag: 退化为总提交数 (仍单调递增, 可用)
    execute_process(COMMAND ${LINTOS_GIT_EXECUTABLE} rev-list --count HEAD
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                    OUTPUT_VARIABLE LINTOS_GIT_ITER
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    set(LINTOS_ITER_BASE "总提交数(尚无 tag)")
endif()
# ⚠ 必须用 STREQUAL "" 判空 —— CMake 的 `if(NOT "0")` 为**真**(数字 0 视为假),
#   会把正确的迭代号 0 误当成"没取到", 连带把迭代基数写成"无 git"。
if(LINTOS_GIT_ITER STREQUAL "")
    set(LINTOS_GIT_ITER "0")
    set(LINTOS_ITER_BASE "无 git")
endif()

string(TIMESTAMP LINTOS_BUILD_STAMP "%Y-%m-%d %H:%M")
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    LINTOS_GIT_DESC="${LINTOS_GIT_DESC}"
    LINTOS_GIT_ITER="${LINTOS_GIT_ITER}"
    LINTOS_BUILD_STAMP="${LINTOS_BUILD_STAMP}"
)
# 版本号一律从 lintos_version.h 读 —— **不要在这里写死**。
# 2026-10-31: 这行曾硬编码 "内部 1.3.1 (对外 1.3)", 而实际版本早已是 2.0,
#   每次编译都在刷假的版本号, 排查"固件到底是哪一版"时误导过一轮。
file(READ "${CMAKE_CURRENT_LIST_DIR}/include/lintos_version.h" LINTOS_VER_H_TXT)
string(REGEX MATCH "#define[ \t]+LINTOS_VERSION_PUBLIC[ \t]+\"([^\"]*)\"" _lv_pub "${LINTOS_VER_H_TXT}")
set(LINTOS_VER_PUBLIC "${CMAKE_MATCH_1}")
string(REGEX MATCH "#define[ \t]+LINTOS_VERSION_STR[ \t]+\"([^\"]*)\"" _lv_str "${LINTOS_VER_H_TXT}")
set(LINTOS_VER_STR "${CMAKE_MATCH_1}")
message(STATUS "[LinTOS] 版本 ${LINTOS_VER_STR} (对外 ${LINTOS_VER_PUBLIC}) +${LINTOS_GIT_ITER} (迭代基数: ${LINTOS_ITER_BASE}) / git=${LINTOS_GIT_DESC} / build=${LINTOS_BUILD_STAMP}")
