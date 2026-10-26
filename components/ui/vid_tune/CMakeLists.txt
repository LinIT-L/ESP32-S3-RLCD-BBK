idf_component_register(
    SRCS "vid_tune.c"
    INCLUDE_DIRS "include"
    REQUIRES
        st7305
        input
        svc_nvs
        ui_common
)
