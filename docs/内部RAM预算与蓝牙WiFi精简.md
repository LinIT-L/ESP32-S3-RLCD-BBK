# 内部 RAM 预算 与 蓝牙/WiFi 精简

> 2026-10-01 · 起因：`esp_wifi_init` 报 `ESP_ERR_NO_MEM`，WiFi 完全打不开，频谱仪一直"射频未就绪"。

## 1. 结论（一句话）

**不是"系统吃太多"，是三笔大账叠在一起**：蓝牙初始化吃掉 **97.5KB**、WiFi 自己要 **~34KB 静态缓冲**、
频谱仪采集区又从堆里挖走 **64KB** —— 而 S3 能给的动态内部堆一共只有 **234KB**。

## 2. 实测账目（都是设备日志原文，不是估算）

| 时刻 | 内部空闲 | 最大连续块 | 证据 |
|---|---|---|---|
| 开机（app_main 起） | 154,047 B | 81,920 B | `[AUDIT] internal ...` |
| 进桌面（`OS 桌面基线`） | 153,887 B | 82,428 B | `MEM: [MEM] OS 桌面基线` |
| **蓝牙初始化后** | **56,371 B**（−97.5KB） | **31,744 B** | `HOUSEKEEP: sweep done: int_free=56371` |
| WiFi 需要 | ~50KB **且需大块连续** | — | `Expected to init 4 rx buffer, actual is 1` |

失败点原文：
```
E (11815) WIFI_MANAGER: esp_wifi_init 失败: ESP_ERR_NO_MEM
E (3730)  wifi:Expected to init 4 rx buffer, actual is 1      ← 要 4 个静态 RX 缓冲, 只分到 1 个
```

## 3. 根因拆解

| 项 | 占用 | 说明 |
|---|---|---|
| 蓝牙（控制器 + Bluedroid 主机 + HIDH） | **97.5 KB** | 一次性、常驻；本项目最大单一消耗 |
| WiFi 静态缓冲 | ~34 KB | `STATIC_TX=16` ×1.6KB ≈ 26KB + `STATIC_RX=4` ×1.7KB |
| 频谱仪采集区 | **64 KB** | `SOC_RESERVE_MEMORY_REGION(0x3fcd0000, 0x3fce0000)` |
| IRAM 常驻代码 | 127 KB | 与 DRAM **共享同一块 SRAM**，IRAM 少 1KB = 堆多 1KB |
| S3 动态内部堆总量 | 234 KB | 以上全部从这里出 |

## 4. 本次精简（只动"条目上限 / 产线开关"，不动任何功能能力）

### 4.1 蓝牙

| 键 | 改前 | 改后 | 理由 |
|---|---|---|---|
| `BT_ALLOCATION_FROM_SPIRAM_FIRST` | n | **y** | Bluedroid 主机侧 `osi_malloc` 改走 PSRAM（IDF 官方"省内部 RAM"开关） |
| `BT_CTRL_BLE_ADV_REPORT_FLOW_CTRL_NUM` | 100 | 10 | 广播上报队列长度，只影响同时滞留条数 |
| `BT_CTRL_BLE_ADV_REPORT_DISCARD_THRSHOLD` | 20 | 5 | 同上 |
| `BT_CTRL_SCAN_DUPL_CACHE_SIZE` | 40 | 10 | 扫描去重缓存，小一点只会"多上报"不会漏设备 |
| `BT_ALARM_MAX_NUM` | 50 | 10 | 蓝牙定时器槽 |
| `BT_SMP_MAX_BONDS` | 15 | 6 | 绑定记录数，手柄场景够 |
| `BT_BLE_50_DTM_TEST_EN` / `BT_BLE_42_DTM_TEST_EN` | y | **n** | DTM = 产线射频认证测试模式，产品固件无用 |

### 4.2 WiFi

| 键 | 改前 | 改后 | 理由 |
|---|---|---|---|
| `ESP_WIFI_STATIC_TX_BUFFER_NUM` | 16 | **8** | 每个 1.6KB → 直接省 ~13KB 内部 RAM |
| `ESP_WIFI_MGMT_SBUF_NUM` | 16 | 6 | Kconfig 下限；管理帧短缓冲 |
| `ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` | 8 | 6 | 小型收发场景足够 |
| `ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM` | 7 | 0 | 不用 ESP-NOW；腾出的硬件密钥槽还能给 SoftAP |

> 注：`CONFIG_ESP32_WIFI_*` 是老别名段，已同步成同一组值，避免两套值打架。
> 备份：`sdkconfig.bak_mem` / `sdkconfig.defaults.bak_mem`。

### 4.3 KPI（改完要看到）

`HOUSEKEEP: sweep done: int_free=?` 这行：蓝牙初始化后内部空闲应从 **56KB → ≥85KB**、
最大连续块从 **31KB → ≥50KB**，并且 `wifi_manager_enable()` 不再报 NO_MEM。

## 5. 频谱仪采集区：分两阶段收（**不靠推测，靠实测**）

采集区钉在 `0x3fcd0000`，硬件 aperture 满窗口 64KB，但**调制解调器只写前 n 个字**（n = 控制字低位样本数）。
所以只要证明"只写 n 个字"，就能只保留够用的一段：

| 阶段 | `ESPSDR_APERTURE_BYTES` | 样本上限 | 省下 |
|---|---|---|---|
| **一（当前）** | 64KB（满窗口） | 16380 | 0 —— 只为拿到证据 |
| 二（拿到证据后） | 20KB | 4096 | **44KB** |

**证据怎么拿**：`espsdr_capture()` 现在每帧把**整个保留窗口**填哨兵，抓完逐字复核 `[n, 窗口)` 是否仍是哨兵。
- 通过 → 日志出现 `越写守卫通过: 窗口 16384 字 (64 KB), 本次用 2048 字, 其后 14336 字未被写入`
- 越写 → 报 `⚠ modem 越写采集区: 首个越写字 #x, 最高 #y`，该帧作废，界面状态行显示 `!OVR`

守卫**常开**（无论窗口多大），所以窗口越小也不会静默踩堆。

## 6. 后备杠杆（本次未动，需要时再上）

| 杠杆 | 可回收 | 代价 |
|---|---|---|
| 模拟器 IRAM（`md_emu` 9.2KB + `gam4980` 4.5KB + `arduboy_avr` 3.4KB） | ~17KB | 模拟器热点循环回到 flash，帧率略降 |
| 蓝牙 BLE5.0 扩展广播/周期广播/同步/厂商 HCI | ~5KB | 极少数新设备可能受影响 |
| 大 `.data/.bss` 迁 PSRAM（tinyusb 12.5KB / nes_emu 3.5KB） | ~10KB | USB DMA 缓冲挪 PSRAM 有风险，需逐个验证 |
| `BT_BTU_TASK_STACK_SIZE` 4352→3072 | ~1.3KB | 栈水位需确认 |

## 7. 顺带修掉的一个真 bug

`page_sdr.c` 原先在 `os_hw_request(OS_HW_WIFI)` 之后、`espsdr_enter()` 失败时直接 `return`，
**引用计数永不归零** → WiFi 被"永久征用"。已改为"开了才关"的存档/还原（`s_wifi_owned`）。

同时**不再用 `os_hw_request(OS_HW_WIFI)`**：管家的 stop 就是 `wifi_manager_disable()`，
而用户自己开的 WiFi 没走过管家（ref 从 0 起）→ 退出时 ref 归零会**把用户的 WiFi 关掉**。
（若要统一走管家，需要先把 `wifi_manager_enable/disable` 的所有调用点都改走 `os_hw_*`，属后续重构。）
