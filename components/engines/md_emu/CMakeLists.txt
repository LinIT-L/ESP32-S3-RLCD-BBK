idf_component_register(
    LDFRAGMENTS "linker.lf"
    SRC_DIRS
        "."
        "src"
        "gwenesis/src/bus"
        "gwenesis/src/cpus/M68K"
        "gwenesis/src/cpus/Z80"
        "gwenesis/src/sound"
        "gwenesis/src/vdp"
        "gwenesis/src/io"
        "gwenesis/src/savestate"
    INCLUDE_DIRS
        "include"
        "gwenesis/src"
        "gwenesis/src/bus"
        "gwenesis/src/cpus/M68K"
        "gwenesis/src/cpus/Z80"
        "gwenesis/src/sound"
        "gwenesis/src/vdp"
        "gwenesis/src/io"
        "gwenesis/src/savestate"
    REQUIRES
    st7305
    shared_memory
    input
        audio_player
        board_shim
        esp_timer
        freertos
        heap
        log
        esp_common
        esp_hw_support
        esp_psram
        cxx
        pthread
        esp_partition
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-const-variable -Wno-unused-value -Ofast)
target_compile_definitions(${COMPONENT_LIB} PRIVATE GWENESIS_AUDIO_ACCURATE=0)
