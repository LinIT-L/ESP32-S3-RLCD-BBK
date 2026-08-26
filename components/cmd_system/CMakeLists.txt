# 本地 stub 组件, 替代 espressif/cmd_system (避免联网下拉).
# 仅提供 cmd_system.h; 函数实现为空(NOT_SUPPORTED), bluepad32 关闭 console 后不再调用.
idf_component_register(SRCS "cmd_system.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_timer)