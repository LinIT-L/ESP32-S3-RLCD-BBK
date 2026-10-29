# components/services/miniz — 通用 zip/inflate 库 (miniz)
#
# ★ 2026-10-27: 从 engines/book_reader/miniz 提成独立组件。
#   原因: 商店资源包解压(am_asset)也要用 inflate —— 原来它只支持 stored(method=0)
#         ⇒ deflate 压缩的 pal.zip 109 个文件全被跳过 ⇒ 仙剑提示"资源未安装"。
#   现在 apps 与 book_reader 共享同一份 miniz, 不重复编译、不重复造轮子。
idf_component_register(
    SRCS "miniz.c" "miniz_tdef.c" "miniz_tinfl.c" "miniz_zip.c"
    INCLUDE_DIRS "."
    REQUIRES fatfs
)

# 第三方库的告警不是我们的问题, 屏蔽掉(与原来在 book_reader 下编译时一致)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)
