idf_component_register(SRCS "vibrator.c"
                       INCLUDE_DIRS "."
                       REQUIRES board_cfg driver audio_player)