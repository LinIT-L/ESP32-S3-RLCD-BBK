idf_component_register(
    SRCS "gb_emu.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES gbc_emu vid_tune
    REQUIRES
        heap
        log
    )

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
)
