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
target_add_binary_data(${COMPONENT_LIB} "assets/book20.fnt" BINARY)
target_add_binary_data(${COMPONENT_LIB} "assets/book24.fnt" BINARY)
target_add_binary_data(${COMPONENT_LIB} "assets/book28.fnt" BINARY)
target_add_binary_data(${COMPONENT_LIB} "assets/book32.fnt" BINARY)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)
