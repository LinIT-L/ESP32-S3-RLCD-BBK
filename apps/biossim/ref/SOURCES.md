# ref/ — 真实 BIOS 素材与出处（只做本地学习参考）

> ⚠ **不要外发**：本目录里的素材来自真机固件，**有厂商版权**（不是开源代码）。
> 放这里只为本地对照学习；不要随固件镜像 / 公开仓库一起分发。

## 1. `asus_aptio_v_real_dump.json`（220 KB）

**是什么**：一份**真机 ASUS 主板（AMD 平台，Aptio V）的完整 BIOS 菜单树**，由 UEFI 固件里的
IFR（Internal Forms Representation）解析导出。`footer_text` 字段写着 `Auto-Imported from IFR Dump`。

**规模（实测）**：**89 个一级菜单 / 887 个可改条目 / 2403 个候选值**。

**为什么有用**：这是"**真实选项**"的唯一可信来源。之前我自己编的选项名（如 `Boot option filter`）
虽然也见于某些 AMI 主板，但**不是真机导出的**；这份 dump 里的名字/取值/层级都是真的，例如：

```
[Serial ATA Controller 0 Settings] SATA mode          = IDE Mode   可选: IDE Mode, AHCI Mode, RAID Mode
[SATA Configuration]               SATA Mode          = AHCI       可选: AHCI, RAID
[SATA Configuration]               NVMe RAID mode     = Disabled   可选: Disabled, Enabled
[SATA Configuration]               RAID Type          = RAID       可选: RAID, RAID5
[Trusted Computing]                Storage Hierarchy  = Disabled   可选: Disabled, Enabled
[Trusted Computing]                TPM 2.0 UEFI Spec Version = TCG_1_2  可选: TCG_1_2, TCG_2
[Trusted Computing]                TPM 2.0 InterfaceType     = CRB    可选: CRB, TIS
[AMD fTPM configuration]           AMD fTPM switch    = Enabled    可选: Enabled, Disabled
[CPU Configuration]                SVM Mode           = Disabled   可选: Disabled, Enabled   ← AMD 的 VT-x
[USB Configuration]                Legacy USB Support = Enabled    可选: Enabled, Disabled, Auto
[USB Configuration]                XHCI Hand-off      = Enabled    可选: Enabled, Disabled
[Network Stack Configuration]      Network Stack      = Disabled   可选: Disabled, Enabled
[Boot]                             Bootup NumLock State = On       可选: On, Off
```

真机里还有一类**只读信息行**（`type:"text"` 或 `value:"[Info]"`）+ 段落分隔行
（`--- TPM 2.0 Device Found ---`、`--- Boot Option Priorities ---`）——
这些"非可改行"是我之前完全没做的，也是不像的原因之一。

**数据结构**（可直接拿来做渲染器的驱动格式）：

```json
{ "title": "...", "theme": "award_blue|ami_grey", "footer_text": "...",
  "tabs": [ { "name": "Main", "items": [
      { "type":"item",    "label":"System Time", "value":"13:37:00", "options":["13:37:00"] },
      { "type":"text",    "label":"--- Boot Option Priorities ---", "value":"" },
      { "type":"submenu", "label":"CPU Configuration", "value":"► Press Enter", "items":[ ... ] } ] } ] }
```

## 2. 真实配色（**事实数值**，用于自己实现，不复制他人代码）

来源：公开仓库 `Bl4ckWing/BIOSConfigSimulation` 的主题文件。
⚠ 该仓库**没有 LICENSE**（默认全版权保留）→ 因此这里**只记录颜色数值这类事实**，
**没有复制它的 CSS 代码**；实现要自己写。

| 角色 | Award 蓝（90 年代） | AMI 灰（2000 年代 OEM） |
|---|---|---|
| 底色 | `#0000AA` | `#C0C0C0` |
| 正文 | `#E0E0E0`（浅灰白） | `#000080`（海军蓝） |
| 数值文字 | `#FFFF55`（黄） | `#000000`（黑） |
| **选中行** | **底 `#AA0000` + 字 `#FFFF55`（红底黄字）** | **底 `#000080` + 字 `#FFFFFF`（深蓝底白字）** |
| 分隔线 | `#FFFFFF` 2px | `#FFFFFF` 2px |
| 外框 | `4px double` 白 | `3px solid` 深蓝 |
| 字体 | `Courier New` | `Lucida Console` / `Monaco` |

> ★ **这就是"一点都不像"的根因**：我原来做成"黑白反色"，而真机是
> **Award 红底黄字 / AMI 深蓝底白字**，且**数值列是彩色的**（黄/黑），不是同色。

## 3. 真实版式骨架（从公开模板的结构类名读出）

```
bios-app
├─ header            ← 标题栏（Award: 浅灰底 + 深蓝字）
├─ nav-bar           ← 顶部横排一级菜单；nav-item.active 高亮
├─ content-area      ← 选项区
│    └─ menu-row（label ……… value），menu-row.selected 才用高亮色
├─ footer            ← 顶部 2px 白线 + 按键提示（如 ESC: Back/Exit  F10: Save & Exit  ↑↓←→: Select Item  ENTER: Select/Submenu）
└─ overlay / dialog-box / dialog-title / dialog-content(.btn-group)   ← 弹窗（选择值、保存确认）
```

## 4. 其他开源案例（供参考，未使用其代码）

| 项目 | 许可 | 能提供什么 |
|---|---|---|
| `Bl4ckWing/BIOSConfigSimulation` | **无 LICENSE** | JSON→单文件 HTML 的生成器、两套主题、**真实 IFR dump** |
| `Vortex-Deskware/BIOSHsim` | **Apache-2.0** | 经典 Award 界面 + **开机自检/内存计数画面**（可合法参考） |
| `BoringBoredom/UEFI-Editor`（Aptio V Editor） | MIT 系 | **解析真机 BIOS 的 IFR** → 真实选项/取值（含隐藏项、`setup_var_cv`） |
| **tianocore/edk2 (OVMF)** | BSD-2 | **真实 UEFI 固件本体**：QEMU 里能跑起来 → 可截真实 Setup 界面 |

## 5. 想拿"真机像素级截图"的唯一前置

本机 `brew` 被 Xcode 许可拦住（与 `/usr/bin/git` 同一个 shim 问题）：

```
sudo xcodebuild -license accept      # ← 需要你执行一次（要 sudo 密码）
```

之后我就能 `brew install qemu` → 用 **开源 OVMF/EDK II 固件**跑真实 UEFI Setup →
**截图当 1:1 参照**（这是完全合法的开源固件，不像真机 dump 有版权问题）。
