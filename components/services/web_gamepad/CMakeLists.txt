# components/services/web_gamepad — 网页手柄（第一回合起并入统一 Web 服务）
# 2026-11-01: 不再自己 httpd_start / esp_wifi_init；HTTP 走 web_core，热点走 wifi_manager。
idf_component_register(
    SRCS "web_gamepad.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_server      # 只为 httpd_req_t / httpd_uri_t 类型
        web_core
        wifi_manager
        board_battery
        nvs_flash svc_nvs
)
