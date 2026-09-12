idf_component_register(
  INCLUDE_DIRS "include"
  SRC_DIRS "." "src" "smsplus" "smsplus/cpu" "smsplus/sound"
  PRIV_INCLUDE_DIRS "." "smsplus" "smsplus/cpu" "smsplus/sound"
  REQUIRES
        shared_memory
        input
        audio_player
        board_shim
        st7305
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
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-const-variable $<$<COMPILE_LANGUAGE:C>:-std=gnu17> $<$<COMPILE_LANGUAGE:C>:-Wno-discarded-qualifiers> $<$<COMPILE_LANGUAGE:C>:-Wno-error=address-of-packed-member>)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LSB_FIRST=1)
