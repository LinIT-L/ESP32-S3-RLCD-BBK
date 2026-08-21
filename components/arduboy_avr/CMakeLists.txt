# arduboy_avr - Arduboy AVR 模拟核心 (simavr ATmega32u4)
# 精简自 UVE5 对讲机项目: 仅保留模拟核心, 无内置游戏, 游戏从 TF 卡 /sdcard/AB/*.hex 加载.
# 按需加载/卸载: 进入菜单懒加载, 返回主菜单释放全部内存.
idf_component_register(
    SRCS
        "arduboy_avr.c"
        # simavr 最小 CPU 模拟集 (无 GDB/ELF/UART/ADC/USB 等重量外设, 用 stub 替代)
        "avr/simavr/sim/sim_avr.c"
        "avr/simavr/sim/sim_core.c"
        "avr/simavr/sim/sim_hex.c"
        "avr/simavr/sim/sim_io.c"
        "avr/simavr/sim/sim_irq.c"
        "avr/simavr/sim/sim_utils.c"
        "avr/simavr/sim/sim_interrupts.c"
        "avr/simavr/sim/sim_cycle_timers.c"
        "avr/simavr/sim/sim_cmds.c"
        "avr/simavr/sim/sim_vcd_file.c"
        "avr/simavr/sim/sim_gdb_stub.c"
        "avr/simavr/sim/sim_elf_stub.c"
        # 外设 (mega32u4 实际使用: ioport/spi/timer/eeprom/extint/watchdog/flash)
        "avr/simavr/sim/avr_eeprom.c"
        "avr/simavr/sim/avr_flash.c"
        "avr/simavr/sim/avr_extint.c"
        "avr/simavr/sim/avr_watchdog.c"
        "avr/simavr/sim/avr_ioport.c"
        "avr/simavr/sim/avr_timer.c"
        "avr/simavr/sim/avr_spi.c"
        # 未用外设 stub (编译期空实现, 进一步减小体积)
        "avr/simavr/sim/avr_uart_stub.c"
        "avr/simavr/sim/avr_adc_stub.c"
        "avr/simavr/sim/avr_acomp_stub.c"
        "avr/simavr/sim/avr_twi_stub.c"
        "avr/simavr/sim/avr_usb_stub.c"
        # ATmega32u4 核心 + 虚拟 SSD1306 OLED
        "avr/simavr/cores/sim_mega32u4.c"
        "avr/simavr/parts/ssd1306_virt.c"
    INCLUDE_DIRS
        "include"
        "avr"
        "avr/simavr"
        "avr/simavr/sim"
        "avr/simavr/cores"
        "avr/simavr/cores/avr"
        "avr/simavr/parts"
    PRIV_REQUIRES "st7305" "freertos" "esp_timer" "driver" "audio_player" "virtual_keys"
)

# 启用未用外设的 stub 实现 (见 *_stub.c), 关闭其真实实现
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SIMAVR_STUB_UART=1
    SIMAVR_STUB_ADC=1
    SIMAVR_STUB_ACOMP=1
    SIMAVR_STUB_TWI=1
    SIMAVR_STUB_USB=1
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-format
    -Wno-char-subscripts
    -Wno-builtin-macro-redefined
    -O3
    # simavr 的 malloc/calloc/realloc/free 优先指向内部 SRAM/DMA (见 simavr_mem_override.h),
    # AVR 解释器每条指令随机读 flash/data, PSRAM 延迟太高导致卡顿; 核心仅 ~40KB 内部放得下.
    -include simavr_mem_override.h
)

# ESP-IDF 默认 -fno-jump-tables 会把 avr_run_one 的巨型 switch 编译成比较链,
# 每条 AVR 指令解码多出多次分支. 解释器核心恢复跳表, 解码更快.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -fjump-tables
)
