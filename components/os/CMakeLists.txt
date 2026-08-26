# components/os — 标准化模块化 OS 内核
# 目录约定:
#   kernel/    页面栈/事件路由/输入分发/内存生命周期/页面装配
#   services/  全局服务 (状态栏/音量/电量/蓝牙等, 不占页面栈)
#   pages/     应用页模块 (每页一个 .c, 私有 static state)
idf_component_register(
    SRCS "kernel/os_core.c"
         "kernel/os_pages.c"
         "services/svc_statusbar.c"
    INCLUDE_DIRS "include"
    REQUIRES st7305 ui_common
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)
