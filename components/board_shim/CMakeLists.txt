# board_shim: board_rlcd + board_speaker 兼容层
# 适配 gb_emu 到 st7305 + audio_player

idf_component_register(
    SRCS "board_shim.c"
    INCLUDE_DIRS "include"
    REQUIRES
        st7305
        audio_player
        virtual_keys
        log
)
