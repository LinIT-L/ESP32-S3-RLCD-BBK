# components/services/espnow — ESP-NOW 点对点/广播 + 发现/配对 (2026-10-20, 2026-11-24 扩)
# 与 wifi_manager 共用同一个 WiFi 驱动: 本组件只负责 esp_now_* 这一层。
#   ① espnow_link.c  — 初始化 / peer / 收发 / 环形队列 (原生 esp_now API)
#   ② espnow_proto.c — 应用层帧格式 (magic/type/seq/CRC), 参考 IDF 官方示例 CC0
#   ③ espnow_peer.c  — 设备发现 / 配对表 / 设备列表 (自研, 借鉴 esp-now 机制)
idf_component_register(
    LDFRAGMENTS "linker.lf"
    SRCS "espnow_link.c" "espnow_proto.c" "espnow_peer.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi freertos
    PRIV_REQUIRES wifi_manager esp_event svc_nvs esp_timer esp_hw_support
)
