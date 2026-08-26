# components/os — 标准化模块化 OS 内核
idf_component_register(
    SRCS "os_core.c"
         "os_pages.c"
    INCLUDE_DIRS "include"
    REQUIRES st7305
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)