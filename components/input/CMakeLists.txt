idf_component_register(
    SRCS "input.c"
    INCLUDE_DIRS "."
    REQUIRES
        menu
        driver
        touch_panel
        web_gamepad
        virtual_keys
)
