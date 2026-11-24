# components/services/web_core — 全系统唯一 HTTP Server 管理者
# 收拢: httpd 生命周期 / URI 注册 / 公共 Bearer 认证 / 统一错误响应。
# 依赖刻意保持最小: 只管"怎么把请求送到应用"，不认识任何业务模块。
idf_component_register(
    SRCS "web_core.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_server
        esp_timer
        esp_hw_support
)
