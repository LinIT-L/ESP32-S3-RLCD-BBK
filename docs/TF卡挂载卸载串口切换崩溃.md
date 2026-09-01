# TF 卡"挂载到电脑/USB-MSC"切换串口时崩溃、无限重启 —— 排查与最终方案

> 状态：✅ 已彻底解决（2026-08-29 实测挂载/卸载正常）。
> 方案：esp_tinyusb v2 按需切换 + 纯 MSC(U盘) + 挂载期间暂停串口控制台 + 看门狗时限调整。

## 〇、最终结论（2026-08-29，实测验证）

问题分两层，均已解决：

1. **挂载时崩溃/无限重启**（历史遗留）：控制台往"已卸载的 USB Serial/JTAG"写日志导致。
   → 采用「按需切换」方案（与虚拟键鼠一致）：
   - 正常模式 = USB Serial/JTAG 控制台（不装 TinyUSB）；
   - 挂载 = 切到**纯 MSC 标准 U盘**设备（`esp_tinyusb v2.2.1`）；
   - 挂载期间用 `log_console_suspend(true)` **暂停串口控制台**（日志只写 `/sdcard/log/bbk.log`），
     从根源杜绝日志写向已卸载串口。
   - 键鼠模式保持原样（HID+CDC）。

2. **卸载时重启**（本轮新发现）：是 `sd_scan` 的 **6 秒卡死看门狗超时误伤**，不是真实阻塞。
   → 实测：临时禁用 `sd_op_watchdog_begin`（不再设超时）后，挂载→退出挂载**完美正常、不再重启**。
   → 说明卸载链条（删 storage + 卸 TinyUSB + 重挂 VFS `f_mount`）本身能完成，只是耗时超过 6 秒，
     被看门狗强制 `esp_restart()`。
   → **正确修法**：不要把看门狗永久关掉（会失去防真实卡死保护），应**把时限从 6000 调大**（如 15000~20000ms），
     再实测确认不误伤且仍能兜住真实死锁。

### 关键文件现状
- `components/usb_hid/usb_hid.c` —— 按需切换管理器：`usb_composite_enter_msc/enter_hid/exit`（卸载 Serial/JTAG ↔ 装 TinyUSB，切换全程压制日志）。
- `components/usbh_msc/usbh_msc_sdspi.c` —— 纯 MSC 挂载：`log_console_suspend` + `tinyusb_msc_new_storage_sdmmc`（USB 模式）。
- `main/main.c` —— `log_console_suspend()`：挂载期间暂停串口控制台、只写文件日志。
- `components/sd_scan/sd_scan.c` —— `sd_op_watchdog_begin` 当前为**临时禁用**（排查用），待改回加大时限。
- `components/os/pages/page_storage.c` —— 挂载/卸载后台任务，`sd_op_watchdog_begin(6000)` 包住卸载。

---

# 以下为历史排查过程（2026-08-28 原文档）

## 一、现象

- 在 存储 → 挂载到电脑 触发 USB-MSC 时，**一点挂载就重启**；
- 崩溃后设备**无限重启**，且 USB Serial/JTAG 串口处于坏状态，系统不再枚举出 `/dev/cu.usbmodem*`，无法再正常烧录/看日志；
- 该问题为历史遗留，非本轮 BT/UI 改动引入。

## 二、根因（已确证）

问题出在 **USB Serial/JTAG 与 TinyUSB(USB-MSC) 共用 ESP32-S3 的 USB 引脚、以及控制台通道的占用冲突**：

1. 挂载逻辑（`components/usbh_msc/usbh_msc_sdspi.c::usbh_msc_start`）会先 `usb_serial_jtag_driver_uninstall()` 卸载 USB Serial/JTAG 驱动，以释放 GPIO 19/20 去切 TinyUSB 的 USB-MSC。
2. 但本固件的 **副控制台挂在 USB Serial/JTAG 上**（见下），副控制台仍持续往"已卸载的驱动"写日志 → 触发崩坏/复位。
3. 复位后再次进入类似状态，形成**无限重启**，且 Serial/JTAG 未复位 → USB 串口不再出现。

### sdkconfig 实测值（`build/config/sdkconfig.h`）

```c
#define CONFIG_ESP_CONSOLE_UART_DEFAULT 1                        // 主控制台 = UART0
#define CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG 1          // 副控制台 = USB Serial/JTAG  ← 元凶
#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED 1
```

即主控制台走 UART0，但**副控制台同时往 USB Serial/JTAG 打日志**。切换 USB-MSC 时卸载 Serial/JTAG 后，副控制台写日志即崩溃。

## 三、官方/开源给出的解决方案（参考链接见文末）

核心是 **让 USB Serial/JTAG 不受控制台占用**，使其能被 USB-MSC 干净切换：

1. **关闭副控制台 USB Serial/JTAG**（对本项目即可）：
   - sdkconfig：`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=n`
   - 或 `Component config → ESP System Settings → Channel for console output` 改为 `None` / `UART`。
   - 代价：串口日志不再走 `/dev/cu.usbmodem`（USB-Serial/JTAG），改走 UART0。

2. **USB/tinyusb 初始化后 `vTaskDelay(~500ms)` 再打日志**，避开切换窗口期打印崩溃。

3. **彻底方案：改用 TinyUSB CDC 做串口**，绕开"板载 Serial/JTAG 会在 host 拉 DTR 时自动重启"这个外设特性（社区常用做法）。

## 四、本项目的挂载/卸载相关文件

- `components/usbh_msc/usbh_msc_sdspi.c` —— `usbh_msc_start/stop`（Serial/JTAG 卸载/重装 + tinyusb MSC，注释里已对齐 esp-idf #9826/#15912，也提到 ROM secondary console 占用问题）。
- `components/os/pages/page_storage.c` —— 存储页：挂载/卸载走后台任务（`mount_worker_task`/`unmount_worker_task`），用 `sd_op_watchdog_begin(6000)` 包住阻塞调用。
- `components/sd_scan/sd_scan.c` —— `sd_unmount_vfs_keep_card`/`sd_remount_vfs_from_card`/看门狗。

> 注意：曾误判为"6 秒卡死看门狗强制重启"（`sd_scan.c` 里 `sd_op_watchdog_begin→esp_restart`），据此去掉过 `esp_restart()`。但"一点挂载就重启"不可能由 6 秒超时触发，故该判断被推翻，相关改动已还原。真正的崩溃源是上述"副控制台挂 USB Serial/JTAG"。

## 五、现场后果与恢复

- 当前设备卡在无限重启、USB 串口消失；需**进入下载模式**（按住 BOOT + 按 RST）让 `/dev/cu.usbmodem*` 稳定出现后，再烧录稳定固件打断循环。
- 稳定固件已构建于 `dist/merged_16mb.bin`（已还原 sd_scan 看门狗）。

## 六、待确认动作（未执行）

1. 是否关闭 `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG`（=n）后再验证挂载/卸载不再重启（后续日志改从 UART0 读）；
2. 或按官方方案改用 TinyUSB CDC 作为串口。

## 参考来源

- esp32.com 论坛：[How do I enable USB CDC on boot?](https://esp32.com/viewtopic.php?t=30926)
- GitHub PR：[DECtalk_ESPress #37 — 为什么用 TinyUSB CDC 而非板载 Serial/JTAG](https://github.com/lllucius/DECtalk_ESPress/pull/37/files)
- 日文博客（ESP32-S3 USB Composite(HID+CDC)）：把 console output 设 None 消除 USB 初始化复位 [xcr4k](https://xcr4k.hatenablog.com/entry/develop_composite_usb_device)
- ESP-IDF 文档：[与 ESP32-S3 创建串口连接 / 进入下载模式](https://docs.espressif.com/projects/esp-idf/zh_CN/v5.3.1/esp32s3/get-started/establish-serial-connection.html)