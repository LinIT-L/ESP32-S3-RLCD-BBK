idf_component_register(
    SRCS "engine_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES gam4980 gb_emu lavax vpet
    REQUIRES
        st7305
    )