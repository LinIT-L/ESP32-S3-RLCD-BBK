# BIOS 模拟器 · 装机与固件选项学习台

**单文件 HTML，双击即用**（macOS 上 Safari / Chrome 都行，不需要装任何东西、不联网）。
改这里**不会**影响你的真机 —— 它是练习台。

```
双击打开：  apps/biossim/biossim.html
```

---

## 能练什么

**4 家厂商界面**（同一批选项，按各家真实归类摆放）：

| 皮肤 | 版式 | 特点 |
|---|---|---|
| AMI Aptio | 顶部横排菜单 | 台机主板最常见，蓝底灰字，两列选项 |
| Award / Phoenix | 左侧竖菜单 | 老主机/品牌机，深蓝底黄字 |
| Dell | 左侧竖菜单 | OptiPlex/Latitude，灰白底黑字 |
| Lenovo ThinkPad | 左侧竖菜单 | 黑底白字 |

**真选项，不是编的**：`SATA Mode = IDE / AHCI / RAID (Intel RST) / VMD`、`CSM Support`、
`Boot option filter`、`Storage OpROM policy`、`Secure Boot` + `Secure Boot Mode` + `Key Management`、
`Security Device Support` / `TPM Device Selection (dTPM 2.0 / PTT)` / `TPM State`、
`Intel(R) AMT` + `AMT SOL / IDE-R` + `Mebx Password`、`VT-x` / `VT-d`、`Fast Boot`、`Legacy USB Support`、
`XHCI Hand-off`、`Boot Priority Order`（真实设备名，可上下移动）……
只读项（BIOS Version / CPU / 内存）真机里也是灰的，这里也改不动。

**8 个学习任务**：Win7 传统 / Win7 UEFI+CSM / Win10 / **Win11** / **Linux** /
**RST→AHCI（不重装）** / **IDE→AHCI（不蓝屏）**。
右侧面板逐条给 ✅/❌ 并写明"这一项为什么重要"，还会提示"现在这样会卡在哪一步"。

---

## 键位（跟真机一致）

| 键 | 作用 |
|---|---|
| ↑ ↓ | 移动选择 |
| ← → / Tab | 切一级菜单（在子菜单里 = 返回上级） |
| Enter | 进入子菜单 / 确认 / 枚举项直接改值 |
| `+` `-` / Space / PgUp PgDn | 改值；在**引导顺序**里是上移/下移 |
| Esc | 返回上级 |
| F1 | 帮助（这一项到底影响什么，做在右上面板） |
| F9 | 载入默认值 |
| F10 | 保存并退出（模拟） |

---

## 记住这个顺序（真机也这么走）

1. **先定 Boot Mode / CSM** —— 决定 GPT 还是 MBR、U 盘按什么方式引导
2. **再定 Secure Boot / TPM** —— 决定能不能装 Win11、Win7 会不会被签名挡住
3. **最后定 SATA 模式** —— AHCI / RAID(RST) / IDE，**"装系统看不到硬盘"九成出在这一步**

## 最容易踩的三个坑（任务里都覆盖了）

- **Win11**：UEFI + Secure Boot **开** + TPM **2.0** 开；`SATA` 必须是 **AHCI**（开着 RST/VMD 安装器看不到盘）
- **Linux**：`SATA` 必须 **AHCI**，有好 VMD 一定要关 —— 否则安装器"没有可用磁盘"
- **已装好系统改 AHCI**：**先在系统里把 AHCI 驱动启用**（改注册表 `msahci`/`iaStorV` 的 `Start=0`，
  或先进一次安全模式），**再**进 BIOS 改 —— 顺序反了就是 `INACCESSIBLE_BOOT_DEVICE` / `0x7B` 蓝屏

---

## 自测（不用开浏览器也能验）

```bash
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  --headless=new --disable-gpu --no-sandbox --virtual-time-budget=3000 \
  --dump-dom "file://$PWD/biossim.html?selftest=1" | grep -o '自测.*'
```

或直接点页面上的「跑自测」。覆盖 21 项断言：判定引擎（默认值下 Win11 必须不通过、只改错一项必须报错、
RAID 下 Linux 必须提示看不到盘……）、4 皮肤 × 31 页都能列条目、`PAGES/TASKS` 里引用的 key 都存在、
只读项改不动、枚举回绕、F10 后 dirty 归零、子菜单进出、引导顺序可移动。

**深链**（便于分享"直接看这一页"）：
`?brand=dell&task=win11&page=Security&sub=Trusted Computing&boedit=1`

---

## 后续：搬到 LinTOS 掌机上（可选）

这个程序目前是**给你在电脑上练手**的。若要在掌机上跑，走项目已有的**外置应用（Lua 单文件）**链路：
`app.*`（px/rect/texts/zh/key/touch/nvs/…）已经能画界面与读键，
`docs/Lua单文件程序规则.md` 有契约回调与 API 全表，`tools/` 里有打包进 `apps` 分区的脚本。
数据层（`ITEMS/TASKS/PAGES/SUBPAGES`）可以**原样搬**，只把渲染换成 400×300 单色版式即可。
