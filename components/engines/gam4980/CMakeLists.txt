# gam4980 模拟器 wrapper
idf_component_register(
    LDFRAGMENTS "linker.lf"
    SRCS
        "libretro.c"
        "gam4980_emu.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/main" "${CMAKE_SOURCE_DIR}/components/services/audio_player"
    # st7305: gam4980_emu.h 公共接口用 st7305_handle_t / 状态栏回调 ⇒ 必须公开
    PRIV_REQUIRES audio_player bt_ctl cheater fonts input ui_common vibrator vid_tune virtual_keys web_gamepad osutil
    REQUIRES
        "fatfs" "st7305"
    )

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-format
    -O2
)
