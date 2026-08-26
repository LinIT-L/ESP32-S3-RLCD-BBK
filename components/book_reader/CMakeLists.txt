idf_component_register(
    SRCS "font_book.c" "book_reader.c" "book_flow.c" "fb2_convert.c" "epub_convert.c"
         "miniz/miniz.c" "miniz/miniz_tdef.c" "miniz/miniz_tinfl.c" "miniz/miniz_zip.c"
         "libunibreak/linebreak.c" "libunibreak/linebreakdata.c"
         "libunibreak/linebreakdef.c" "libunibreak/unibreakbase.c"
         "libunibreak/unibreakdef.c" "libunibreak/eastasianwidthdef.c"
    INCLUDE_DIRS "." "libunibreak" "miniz"
    REQUIRES st7305 driver fatfs freertos esp_timer audio_player
)

# 嵌入多档 GB2312 点阵字库到 flash (XIP 只读, 运行时切换字号)
# 精简(挤空间给 Meshtastic ota_0): 保留仿宋全套(16/24/32)+黑体24; 删除黑体16/32(~1.28MB).
# 黑体16/32 档由 font_book.c 回退到黑体24; TF 卡 /sdcard/fonts 仍可覆盖任意档.
target_add_binary_data(${COMPONENT_LIB} "assets/book16_fs.fnt" BINARY)
target_add_binary_data(${COMPONENT_LIB} "assets/book24_fs.fnt" BINARY)
target_add_binary_data(${COMPONENT_LIB} "assets/book32_fs.fnt" BINARY)
target_add_binary_data(${COMPONENT_LIB} "assets/book24_hz.fnt" BINARY)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)
