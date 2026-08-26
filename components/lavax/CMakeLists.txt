# lavax — 文曲星 LavaX 虚拟机 (lavaxvm 统一跨平台核心移植).
# core/source/ 为 lavaxvm 汇编出的可移植核心 (纯 stdio), lavax_emu.c 为
# 本固件的宿主(PSRAM 内存/运行循环), lavax_platform_api.c 填补 rg_* 平台接口.
idf_component_register(
    SRCS
        "lavax_emu.c"
        "core/source/lava.c"
        "core/source/file.c"
        "core/source/font.c"
        "core/source/gmode16.c"
        "core/source/hardware.c"
        "core/source/lcd.c"
        "core/source/main.c"
        "core/source/myctype.c"
        "core/source/pinyin.c"
        "core/source/py2gb.c"
        "core/source/platform.c"
        "core/source/lavax_platform_api.c"
    INCLUDE_DIRS
        "."
        "core/source"
        "core/include"
        "${CMAKE_SOURCE_DIR}/components/st7305"
        "${CMAKE_SOURCE_DIR}/components/input"
        "${CMAKE_SOURCE_DIR}/components/virtual_keys"
    REQUIRES
        "st7305"
        "input"
        "virtual_keys"
        "fatfs"
)

# lavaxvm 老代码风格宽松, 屏蔽常见告警; 用内部 RAM 稀缺故不强制 PSRAM 栈.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-implicit-function-declaration
    -Wno-format
    -Wno-char-subscripts
    -Wno-sign-compare
    -O2
)