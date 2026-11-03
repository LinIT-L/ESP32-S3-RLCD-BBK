# lavax_bench — LavaX VM 自动测试与基准（仅测试，不参与正式固件）
#
# 由环境变量/CMake 变量 LINTOS_LAVAX_BENCH=1 时才会被 main 组件引用；
# 默认不编译进固件（见 main/CMakeLists.txt 的条件分支）。
idf_component_register(
    SRCS "lav_asm.c" "lav_bench.c"
    INCLUDE_DIRS "include"
    REQUIRES lavax
)
