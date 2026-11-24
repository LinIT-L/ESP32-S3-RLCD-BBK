# components/engines/engine_api — 引擎层唯一 API 面 (header-only, V1.2 模块化)
# 引擎只允许 include 本 facade, 不得直连 st7305/input/bt_ctl/cheater/board_shim 等底层.
# 校验: tools/check_deps.py (引擎层的 include 白名单)
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES
        st7305
        input
        ui_common
        virtual_keys
        cheater
        bt_ctl
        web_gamepad
        audio_player
)
