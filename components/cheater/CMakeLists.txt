# cheater — 修改机内存搜索核心 + 共享UI (引擎无关)
idf_component_register(
    SRCS "cheater.c" "cheat_ui.c" "cheat_float.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES "esp_common" "heap" "st7305" "input" "bt_ctl" "ui_common" "fonts"
)