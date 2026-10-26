# gam4980 模拟器 wrapper
idf_component_register(
    LDFRAGMENTS "linker.lf"
    SRCS
        "libretro.c"
        "gam4980_emu.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/main" "${CMAKE_SOURCE_DIR}/components/services/audio_player"
    PRIV_REQUIRES "st7305"
    REQUIRES "vid_tune" "fatfs" "virtual_keys" "web_gamepad" "fonts" "bt_ctl" "input" "ui_common" "cheater" "audio_player" "vibrator"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-format
    -O2
)
