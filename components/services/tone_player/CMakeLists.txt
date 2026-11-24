idf_component_register(SRCS "tone_player.c"
                       INCLUDE_DIRS "."
    PRIV_REQUIRES audio_player
                       REQUIRES
        driver esp_timer
    )
