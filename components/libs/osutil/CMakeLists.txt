# osutil — 最底层基础工具 (rank 0: drivers/libs/ui/services/system/engines/apps 都可依赖)
#
# 现有内容:
#   include/os_time.h  lintos_now_ms() —— 毫秒时间戳 (原 6 处 static now_ms 的合并)
#   include/os_file.h  文件整体替换 / 掉电恢复 (FatFs 的 f_rename 不覆盖已存在目标,
#                      "先删再 rename" 在两步之间掉电会丢档 —— 顺序收敛成一份)
#
# 约定: 只放"与业务无关、与硬件无关"的纯工具。任何带状态或依赖外设的东西不要放这里。
#   ⚠ os_file.c 只用 libc, 不 include ESP-IDF ⇒ 主机测试可直接编译 (见 tests/)。
idf_component_register(
    SRCS "os_file.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer
)
