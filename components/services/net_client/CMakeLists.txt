# components/net_client — LinTOS 唯一 HTTP(S) 客户端 (2026-10-15)
# 全项目联网取数都走这里 (Lua 桥 / 应用商店 / 以后的新应用):
# 单请求槽 ⇒ 天然串行, 不出现"每个调用方各建任务各起一套 TLS"的双份内部 RAM 占用。
idf_component_register(
    SRCS "net_client.c" "netc_fs_replace.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client mbedtls esp_event
)
