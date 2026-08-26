idf_component_register(
    SRCS
        "gbc_emu.c"
        "host_gnuboy.c"
        "make_color.c"
        "third_party/gnuboy/src/cpu.c"
        "third_party/gnuboy/src/emu.c"
        "third_party/gnuboy/src/fastmem.c"
        "third_party/gnuboy/src/hw.c"
        "third_party/gnuboy/src/lcd.c"
        "third_party/gnuboy/src/lcdc.c"
        "third_party/gnuboy/src/loader.c"
        "third_party/gnuboy/src/mem.c"
        "third_party/gnuboy/src/rtc.c"
        "third_party/gnuboy/src/save.c"
        "third_party/gnuboy/src/sound.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
        "third_party/gnuboy/include"
    REQUIRES
        board_shim
        audio_player
        esp_timer
        esp_partition
        esp_system
        esp_common
        freertos
        heap
        log
)

# esp-box-emu gnuboy (V1.0.53): 核心全部 PSRAM 指针化 + fastmem 直映射.
# 编译选项与其上游保持一致.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -O3
    -Wno-misleading-indentation
    -Wno-implicit-fallthrough
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-discarded-qualifiers
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    GNUBOY_NO_MINIZIP
    GNUBOY_NO_SCREENSHOT
    IS_LITTLE_ENDIAN
)
