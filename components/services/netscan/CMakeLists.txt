# components/services/netscan — 局域网扫描能力服务 (L1): 端口探测 / ARP 解析 / MAC 厂商
idf_component_register(
    SRCS "netscan.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip log
)
