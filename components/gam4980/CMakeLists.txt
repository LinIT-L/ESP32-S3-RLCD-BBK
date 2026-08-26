# gam4980 模拟器 wrapper
idf_component_register(
    SRCS
        "libretro.c"
        "gam4980_emu.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/main" "${CMAKE_SOURCE_DIR}/components/menu" "${CMAKE_SOURCE_DIR}/components/audio_player"
    PRIV_REQUIRES "st7305"
    REQUIRES "fatfs" "virtual_keys" "web_gamepad"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-format
    -O2
)
