# SDLPal 移植层
#
# 分成两块：
#   1) 垫片：sdl_shim / sdl_audio / pal_port（本项目写的）
#   2) SDLPal 核心：third_party/sdlpal/*.c（GPLv3，原样引入，不改它的代码）
#
# ⛔ 关掉的模块（对应 pal_config.h 的开关）：
#   aviplay / glslp / mini_glloader / video_glsl / mp3play / oggplay / opusplay /
#   midi_timidity / midi_tsf  —— 都要额外依赖（OPUS/OGG/GL/音色库），ESP32 上没有。
set(SDLPAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sdlpal")

# 音乐后端: "opl" = 片上 RIX/OPL 合成(体积 0, 吃 CPU) ; "mp3" = 预渲染曲目(省 CPU, 占 TF)
set(PAL_MUSIC_MODE "opl" CACHE STRING "pal music backend: opl|mp3")

set(SDLPAL_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/pal_lazy.c"   # LINTOS: 仙剑大缓冲懒分配
    "${SDLPAL_DIR}/palcommon.c"
    "${SDLPAL_DIR}/util.c"
    "${SDLPAL_DIR}/global.c"
    "${SDLPAL_DIR}/palcfg.c"
    "${SDLPAL_DIR}/palette.c"
    # ---- 基础层：视频/输入/音频/资源/文字 ----
    "${SDLPAL_DIR}/video.c"
    "${SDLPAL_DIR}/input.c"
    "${SDLPAL_DIR}/audio.c"
    "${SDLPAL_DIR}/sound.c"
    "${SDLPAL_DIR}/midi.c"
    "${SDLPAL_DIR}/resampler.c"
    "${SDLPAL_DIR}/res.c"
    "${SDLPAL_DIR}/font.c"
    "${SDLPAL_DIR}/text.c"
    "${SDLPAL_DIR}/overlay.c"
    # ---- 游戏逻辑层 ----
    "${SDLPAL_DIR}/script.c"
    "${SDLPAL_DIR}/scene.c"
    "${SDLPAL_DIR}/map.c"
    "${SDLPAL_DIR}/battle.c"
    "${SDLPAL_DIR}/fight.c"
    "${SDLPAL_DIR}/itemmenu.c"
    "${SDLPAL_DIR}/magicmenu.c"
    "${SDLPAL_DIR}/ui.c"
    "${SDLPAL_DIR}/uibattle.c"
    "${SDLPAL_DIR}/uigame.c"
    "${SDLPAL_DIR}/play.c"
    "${SDLPAL_DIR}/game.c"
    "${SDLPAL_DIR}/ending.c"
    "${SDLPAL_DIR}/rngplay.c"
    "${SDLPAL_DIR}/yj1.c"
    # 片头/结局动画：aviplay.c 要 SDL_Overlay + AudioCVT + SDL_mutex，ESP32 上没有也不值得做。
    # 这里用 pal_stub.c 提供空实现，PAL_PlayAVI 直接返回（跳过动画，不影响正片）。
    "pal_stub.c"
    # ---- 入口 ----
    # ⛔ main.c 里的 main() 用 setjmp/longjmp 管退出，不能直接当 ESP-IDF 入口。
    #    只对这一个文件把 main 改名成 pal_main，再由 pal_entry.c 包装成 pal_run()。
    "${SDLPAL_DIR}/main.c"
    "pal_entry.c"
)

if(PAL_MUSIC_MODE STREQUAL "opl")
    list(APPEND SDLPAL_SRCS
        "${SDLPAL_DIR}/rixplay.cpp"
        "${SDLPAL_DIR}/adplug/rix.cpp"
        "${SDLPAL_DIR}/adplug/player.cpp"
        "${SDLPAL_DIR}/adplug/binfile.cpp"
        "${SDLPAL_DIR}/adplug/binio.cpp"
        "${SDLPAL_DIR}/adplug/fprovide.cpp"
        "${SDLPAL_DIR}/adplug/emuopls.cpp"
        "${SDLPAL_DIR}/adplug/surroundopl.cpp"
        # ---- OPL 合成核: emu8950 (MIT, 轻量整数核) ----
        # 换掉 dosbox_opls / mame_opls / nuked_opl: 那三个在 ESP32-S3 上实测
        # 实时因子 < 1 (任务看门狗被打爆), emu8950 是 RP2040 都能实时跑的核。
        # 上游目录 adplug/emu8950/ 保持原样, 适配只在 adplug/emu8950_opl.cpp。
        "${SDLPAL_DIR}/adplug/emu8950_opl.cpp"
        "${SDLPAL_DIR}/adplug/emu8950/emu8950.c"
        "${SDLPAL_DIR}/adplug/emu8950/emuadpcm.c")
    message(STATUS "PAL 音乐后端: OPL 片上合成")
else()
    list(APPEND SDLPAL_SRCS "pal_music.c")
    list(APPEND PAL_MUSIC_DEFS PAL_USE_MP3_MUSIC=1)
    message(STATUS "PAL 音乐后端: 预渲染 MP3")
endif()

idf_component_register(
    SRCS "sdl_shim.c" "sdl_audio.c" "pal_port.c" ${SDLPAL_SRCS}
    INCLUDE_DIRS "include" "include/SDL" "${SDLPAL_DIR}" "${SDLPAL_DIR}/sdl_compat"
    REQUIRES
        # fonts = 系统 16px 宋体字库（font_zh16，mmap 自 appdata）；SDLPal 直接用它当汉字字模，
        # 不再需要把字模编进 flash，也不必往 TF 卡放字模文件。
        st7305 audio_player fonts virtual_keys sd_scan
    # PAL_LARGE 标记的大缓冲走 extram_bss 段，需要这个 fragment 才能收进 PSRAM
    LDFRAGMENTS "linker.lf"
)

# ★ 2026-10-25 [LINTOS] MP3 后端标记：让 sdl_audio.c 知道"音乐走外部解码"，
#   从而把 SDL 回调块长压到 256 帧 —— 这个块长只决定**音效触发延迟**，
#   越小越跟手（实测 2048 帧/46ms 时"走路挥剑对话慢一大拍"）。
if(PAL_MUSIC_DEFS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${PAL_MUSIC_DEFS})
endif()

# SDLPal 是 2009 年的 C 代码，在 -Werror=all 下会爆 warning；本项目只要求它能编过。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-all)

# ★ 2026-10-25 [LINTOS] 曾经试过把整个 mus.mkf(约 750KB) 一次性读进 PSRAM
#   (USE_RIX_MKF_FILE_BUFFER=1)，目的是"换曲不再碰 SD"。
#   ⚠ 实测**不可用**: 进游戏载入到 20% 时整机卡死, 串口连心跳都没有 ——
#     一次性 fread 750KB 到 PSRAM 会把 SD/FatFs 通道堵死(长时间同步读),
#     而且当时 /sdcard 本身已经处于不健康状态。
#   现在的做法: 保持逐曲读卡, 但单曲缓冲分配在 PSRAM(rix.cpp rewind 里),
#     这样既不会内部 RAM 分配失败, 也不会长时间占着 SD。
# target_compile_definitions(${COMPONENT_LIB} PRIVATE USE_RIX_MKF_FILE_BUFFER=1)

# 只对 SDLPal 的 main.c 把 main 改名 pal_main（其它文件不受影响）
set_source_files_properties("${SDLPAL_DIR}/main.c"
    PROPERTIES COMPILE_DEFINITIONS "main=pal_main")
# main.c 里 setjmp/longjmp 需要它
set_source_files_properties("${SDLPAL_DIR}/main.c"
    PROPERTIES COMPILE_OPTIONS "-Wno-error")
