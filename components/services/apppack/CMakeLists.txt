# components/services/apppack — LAPP 单文件应用资源包解析 (L1 服务层)
# 只依赖 stdio; 不依赖 os/apps (供上层使用)
idf_component_register(
    SRCS "apppack.c"
    INCLUDE_DIRS "include"
    REQUIRES log
)
