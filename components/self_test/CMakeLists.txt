idf_component_register(
    SRCS "self_test.c" "display_test.c"
    INCLUDE_DIRS "." "include"
    REQUIRES
        nes_emu
        audio_player
        heap
        driver
)
