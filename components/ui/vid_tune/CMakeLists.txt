idf_component_register(
    LDFRAGMENTS "linker.lf"
    SRCS "vid_tune.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES input svc_nvs ui_common
    REQUIRES
        st7305
    )
