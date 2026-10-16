idf_component_register(
    SRCS "gb_emu.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        gbc_emu
        heap
        log
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O2
)
