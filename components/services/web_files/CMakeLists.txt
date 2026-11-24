# components/services/web_files — TF 卡网页文件管理（第三回合）
# 不新建 HTTP Server、不初始化 Wi-Fi：HTTP 走 web_core，热点/STA 走 wifi_manager，
# 文件系统只经 sd_scan + POSIX/FatFs，认证复用 web_core 的 PIN→Bearer。
idf_component_register(
    SRCS "web_files.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_server      # httpd_req_t / httpd_uri_t / httpd_resp_* / httpd_query_key_value
        web_core             # 唯一 HTTP 服务 + 统一认证
        wifi_manager         # STA 地址 / AP 热点（APSTA）
        keyvault             # PIN 校验（与收藏夹同一套 PIN）
        sd_scan              # 挂载状态 / 容量
        heap                 # heap_caps_malloc（搬运缓冲放 PSRAM）
        esp_hw_support       # esp_random
        freertos
)
