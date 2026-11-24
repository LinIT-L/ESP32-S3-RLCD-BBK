# board_shim: board_rlcd + board_speaker 兼容层
# 适配 gb_emu 到 st7305 + audio_player

idf_component_register(
    SRCS "board_shim.c" "gray_quant.c"
    INCLUDE_DIRS "include"
    # input: board_shim.c 的 engine_overlay_quickbar 用 input.h (原先靠上层传递拿到, 属隐藏依赖)
    PRIV_REQUIRES audio_player cheater vid_tune virtual_keys input
    REQUIRES
        st7305
        log
    )
