# arduboy 模拟器 wrapper - Arduboy2 兼容层 + 内置小游戏
idf_component_register(
    SRCS
        "arduboy.c"
        "arduboy_emu.c"
        "game_breakout.c"
        "game_snake.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/main" "${CMAKE_SOURCE_DIR}/components/menu" "${CMAKE_SOURCE_DIR}/components/st7305"
    PRIV_REQUIRES "st7305"
)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-format
    -O2
)
