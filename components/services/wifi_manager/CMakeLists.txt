# components/wifi_manager — Wi-Fi / 网络辅助统一组件
# 收拢: Wi-Fi STA 连接管理 + DNS 服务器(dns_server) + MAC OUI 字典(macoui) + Wi-Fi 万用表/嗅探(wifi_probe).
idf_component_register(
    SRCS "wifi_manager.c" "dns_server.c" "macoui.c" "wifi_probe.c"
    INCLUDE_DIRS "." "include"
    REQUIRES nvs_flash svc_time svc_nvs esp_wifi esp_event esp_netif lwip
    EMBED_FILES oui_db.bin
)

target_compile_options(${COMPONENT_LIB} PRIVATE
)
