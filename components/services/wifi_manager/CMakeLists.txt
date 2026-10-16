# components/wifi_manager — Wi-Fi / 网络辅助统一组件
# 收拢: Wi-Fi STA 连接管理 + DNS 服务器(dns_server) + MAC OUI 字典(macoui) + Wi-Fi 万用表/嗅探(wifi_probe).
# ★ 2026-10-13: oui_db.bin(294KB) **不再 EMBED 进固件**, 改由 macoui.c 首次查询时从 TF 卡读
#   (卡上没有该文件 ⇒ macoui_lookup() 返回 false, 上层显示"未知厂商", 优雅降级)。
idf_component_register(
    SRCS "wifi_manager.c" "dns_server.c" "macoui.c" "wifi_probe.c"
    INCLUDE_DIRS "." "include"
    REQUIRES nvs_flash svc_time svc_nvs esp_wifi esp_event esp_netif lwip bt_ctl
)

target_compile_options(${COMPONENT_LIB} PRIVATE
)
