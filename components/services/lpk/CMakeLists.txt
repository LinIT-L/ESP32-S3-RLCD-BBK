# components/services/lpk — LPK 自包含资源包读取器 + 读写分流挂载层 + /lpk 的 VFS 适配 (2026-11-01)
#
# 分层（前两层 + 模式判定都**零 ESP 依赖**，宿主机直接编译同一份源码跑单测）：
#   ① lpk1.c       格式读取器（纯 C）
#   ①b lpk_manifest.c  包内 manifest.json 的读取与校验（纯 C，安装器/注册层唯一判据）
#   ② lpk_mount.c  读写分流判定，文件 I/O 由调用方注入（纯 C）
#   ②b lpk_mode.c  资源模式判定（LPK 单文件 / ZIP 目录）—— 纯 C，可单测
#   ③ lpk_vfs.c    真机入口：把 ② 注册成 ESP-IDF VFS 的 /lpk
# 格式见 include/lpk1.h 与 docs/规范/LPK自包含资源包格式.md。
#
# ⚠ vfs 必须显式 REQUIRES：本组件不是 main，拿不到默认组件列表里的头文件路径。
#   宿主机测试（tests/lpk）只编译 ①②②b，不会拉进 ESP 头。
idf_component_register(
    SRCS "lpk1.c" "lpk_mount.c" "lpk_mode.c" "lpk_vfs.c" "lpk_manifest.c"
    INCLUDE_DIRS "include"
    REQUIRES vfs esp_timer
)
