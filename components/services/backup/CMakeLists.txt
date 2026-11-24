# components/services/backup — 通用备份策略服务
#
# 只做「策略 + 文件保障」: 目录/命名/份数/原子写/CRC 校验/轮转。
# 不解析业务数据、不打包、不加密（决策理由见 include/svc_backup.h）。
#
# 依赖说明:
#   nvs_flash  — bkp_save_nvs/bkp_load_nvs 遍历命名空间用
#   vfs/fatfs  — fopen/stat/rename 走 VFS，SD 卡由 fatfs 提供
#   esp_rom    — esp_rom_crc32_le（ROM 里的硬件级 CRC，零代码体积）
#   heap       — heap_caps_malloc(MALLOC_CAP_SPIRAM)
# 刻意**不依赖** mbedtls: 不加密就不该把加密库算进本模块的依赖链。
idf_component_register(
    SRCS "svc_backup.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common log heap nvs_flash esp_rom
    PRIV_REQUIRES
        vfs fatfs esp_partition osutil   # osutil: os_file_write_atomic (替换 + 掉电恢复)
    )
