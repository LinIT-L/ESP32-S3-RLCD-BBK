idf_component_register(
    SRCS "self_test.c" "display_test.c"
    INCLUDE_DIRS "." "include"
    PRIV_REQUIRES audio_player nes_emu
    REQUIRES
        st7305
        heap
        driver
    )
