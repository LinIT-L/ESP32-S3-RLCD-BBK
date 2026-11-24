# cheater — 修改机内存搜索核心 + 共享UI (引擎无关)
idf_component_register(
    SRCS "cheater.c" "cheat_ui.c" "cheat_float.c"
    INCLUDE_DIRS "."
    # st7305: cheat_ui.h / cheat_float.h 的公共接口直接暴露 st7305_handle_t ⇒ 必须公开
    REQUIRES "st7305"
    PRIV_REQUIRES "esp_common" "heap" "input" "bt_ctl" "ui_common" "vibrator" "fonts"
)