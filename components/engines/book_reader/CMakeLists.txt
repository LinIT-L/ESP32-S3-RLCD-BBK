idf_component_register(
    SRCS "font_book.c" "book_reader.c" "book_flow.c" "fb2_convert.c" "epub_convert.c"
         "gb2312_uni.c"
         "libunibreak/linebreak.c" "libunibreak/linebreakdata.c"
         "libunibreak/linebreakdef.c" "libunibreak/unibreakbase.c"
         "libunibreak/unibreakdef.c" "libunibreak/eastasianwidthdef.c"
    INCLUDE_DIRS "." "libunibreak"
    REQUIRES miniz st7305 driver fatfs freertos esp_timer audio_player fonts nvs_flash svc_nvs input
)

# V1.0.89+: 字库二进制已迁移到 appdata 分区字库区 (esp_partition_mmap 运行时加载,
# 经 fonts/font_part 共享加载器), 不再嵌入固件 (target_add_binary_data 已移除).

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)
