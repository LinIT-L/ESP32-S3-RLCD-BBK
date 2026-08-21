# vpet — 暴龙机 (虚拟宠物) 引擎宿主.
# 基于 vpet-emu-zepp 的 E0C6200 4-bit CPU 核心 (e0c6200_cpu.c) 模拟第一代
# 虚拟宠物设备 (Tamagotchi P1/P2、Digimon 等), ROM 从 TF 卡 /sdcard/vpet/ 加载.
# 生命周期由 engine_manager + menu 驱动, 与 arduboy/lavax 一致.
idf_component_register(
    SRCS
        "e0c6200_cpu.c"
        "vpet_emu.c"
    INCLUDE_DIRS
        "."
        "include"
        "${CMAKE_SOURCE_DIR}/components/st7305"
        "${CMAKE_SOURCE_DIR}/components/tone_player"
        "${CMAKE_SOURCE_DIR}/components/virtual_keys"
    REQUIRES
        "st7305"
        "tone_player"
        "virtual_keys"
        "fatfs"
)
