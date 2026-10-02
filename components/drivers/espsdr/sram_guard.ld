/* 采集区 (0x3fcd0000 起, 见 espsdr.h 的 ESPSDR_APERTURE_BYTES) 不能与静态段重叠。
 * 基址固定 0x3fcd0000, 窗口大小只影响保留多少 (省下来的回堆), 所以断言只看基址。
 * 与官方 esp-sdr/targets/esp32s3/sram_guard.ld 一致 (只做链接期断言)。 */
ASSERT(_iram_end <= 0x403c0000, "S3 RF dump overlaps IRAM")
ASSERT(_data_end <= 0x3fcd0000, "S3 RF dump overlaps data")
ASSERT(_bss_end  <= 0x3fcd0000, "S3 RF dump overlaps BSS")
