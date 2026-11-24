--@lapp v1
--@app name=故障信息库
--@app cat=运维
--@app desc=电脑故障码离线查询: 蓝屏/引导/设备管理器/系统错误, 代码/英文/拼音搜索
--@app version=2
--@app build=202611230935
--@app icon=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB/wAAAAAAAAf/wAAAAAAAH//wAAAAAAB///wAAAAAAP///gAAAAAB/wH/AAAAAAP4AD+AAAAAB/AAH8AAAAAH4AAPwAAAAA/f//fgAAAAD5//8+AAAAAfH//x8AAAAB8AAAHwAAAAPwAAAfgAAAA+AAAA+AAAAD4AAAD4AAAAPh//8PgAAAA+H//w+AAAAD4f//D4AAAAPgAAAPgAAAA+AAAA+AAAAD8AAAH4AAAAHwAAAfAAAAAfH/wB8AAAAA+f/APgAAAAD9/8B+AAAAAH4AAPwAAAAAfwAB/AAAAAA/gAP6AAAAAB/wH/cAAAAAD///74AAAAAH///fwAAAAAH//z/gAAAAAH/8H/AAAAAAH/AP+AAAAAAAAAf8AAAAAAAAA/4AAAAAAAAB/wAAAAAAAAD/gAAAAAAAAH/AAAAAAAAAP+AAAAAAAAAf8AAAAAAAAA/+AAAAAAAAB/4AAAAAAAAD/gAAAAAAAAH+AAAAAAAAAP4AAAAAAAAA/gAAAAAAAAD+AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
-- 图标自绘: blit_app_icon(x, y[, inv=true])
-- ── LAPP 图标自绘前奏 (打包器注入; 读内嵌 base64, 不碰文件系统) ──
local _APPICON = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB/wAAAAAAAAf/wAAAAAAAH//wAAAAAAB///wAAAAAAP///gAAAAAB/wH/AAAAAAP4AD+AAAAAB/AAH8AAAAAH4AAPwAAAAA/f//fgAAAAD5//8+AAAAAfH//x8AAAAB8AAAHwAAAAPwAAAfgAAAA+AAAA+AAAAD4AAAD4AAAAPh//8PgAAAA+H//w+AAAAD4f//D4AAAAPgAAAPgAAAA+AAAA+AAAAD8AAAH4AAAAHwAAAfAAAAAfH/wB8AAAAA+f/APgAAAAD9/8B+AAAAAH4AAPwAAAAAfwAB/AAAAAA/gAP6AAAAAB/wH/cAAAAAD///74AAAAAH///fwAAAAAH//z/gAAAAAH/8H/AAAAAAH/AP+AAAAAAAAAf8AAAAAAAAA/4AAAAAAAAB/wAAAAAAAAD/gAAAAAAAAH/AAAAAAAAAP+AAAAAAAAAf8AAAAAAAAA/+AAAAAAAAB/4AAAAAAAAD/gAAAAAAAAH+AAAAAAAAAP4AAAAAAAAA/gAAAAAAAAD+AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
local function blit_app_icon(x, y, inv)
  local d = _APPICON
  local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
  local buf, acc, nbits = {}, 0, 0
  for i = 1, #d do
    local ch = d:sub(i, i)
    if ch == "=" then break end
    local v = B64:find(ch, 1, true)
    if v then
      acc = acc * 64 + (v - 1); nbits = nbits + 6
      if nbits >= 8 then
        nbits = nbits - 8
        buf[#buf + 1] = math.floor(acc / (2 ^ nbits)) % 256
        acc = acc % (2 ^ nbits)
      end
    end
  end
  for row = 0, 63 do
    for col = 0, 63 do
      local idx = row * 8 + (col >> 3) + 1
      local byte = buf[idx] or 0
      local on = (math.floor(byte / (2 ^ (7 - (col % 8)))) % 2) == 1
      if on ~= (inv == true) then app.px(x + col, y + row, inv and app.WHITE or app.BLACK) end
    end
  end
end
-- ── 以上为注入前奏, 以下为应用原始代码 ──
--
-- ============================================================================
-- 故障信息库 / PC Fault Code Reference — LinTOS 单文件 Lua 应用
--
-- 目标设备: ESP32-S3 / ST7305 400x300 1bpp / 触摸 + 三键
-- 只用固件已注册的 app.* API (逐条核对 components/apps/lua_bridge/luat_lib_*.c)
--
-- 页面:
--   home    标题 + 搜索框 + 7 层入口 + 最近查询 + 常用 + 台阶栏
--   list    码(定宽列) | 中文含义 | 层标签, 分页滚动
--   detail  六段: 错误信息 / 原因 / 处理 / 验证 / 注意事项 / 来源
--   search  软键盘输入 (52 键 ASCII → 代码 / 英文 / 拼音)
--
-- 为什么搜索要带拼音:
--   系统键盘只有 ASCII, 打不出汉字。所以每条数据带一个 ASCII 检索词
--   (英文缩写 + 中文关键词全拼/首字母), 输入 "neicun" 或 "nc" 都能命中「内存」。
--
-- 数据: 全部内嵌在本文件 DB 表里 (无外部文件, 卸载应用即彻底清干净)。
--   字段顺序: 代码, 英文报错, 中文含义, 类别, 适用系列, 可能原因,
--             检查步骤, 建议, 数据风险, 是否送修, 官方来源, 备注, 检索词
--   来源分级: 联想 HMM 错误码表 / 联想知识库 / 联想支持文档 = 官方;
--             "经验参考" = 社区经验, 未在官方文档中逐条核实。
-- ============================================================================

local W, H = app.W, app.H
local BK, WH = app.BLACK, app.WHITE

-- 动作码降级保护 (老固件可能没注册)
local K_UP    = (type(app.UP)    == "number") and app.UP    or 1
local K_DOWN  = (type(app.DOWN)  == "number") and app.DOWN  or 2
local K_LEFT  = (type(app.LEFT)  == "number") and app.LEFT  or 3
local K_RIGHT = (type(app.RIGHT) == "number") and app.RIGHT or 4
local K_OK    = (type(app.OK)    == "number") and app.OK    or 5
local K_BACK  = (type(app.BACK)  == "number") and app.BACK  or 6

-- ------------------------------- 布局常量 --------------------------------
local SB_H    = 0           -- 顶栏已去掉 (linit 2026-11-23: 第一排标题不显示)
local ROW_H   = 22          -- 列表行高
local ROWS    = 10          -- 列表一屏行数 (让出顶部标题行)
local LINE_H  = 20          -- 详情正文行高
local DET_ROWS= 12          -- 详情一屏行数
local PAD     = 8
local TXT_W   = W - PAD * 2 - 9 -- 正文可用宽 (左侧留 9px 给字段标记块)

-- 首页几何
-- ★ 2026-11-23 linit: 顶栏去掉 + 全 16px ⇒ 行高按 16px 字号重排 (12px 时代的几何作废)
-- ★ 2026-11-23 第二轮: 首页改成「搜索为核心 + 7 层辅助入口」, 取消 11 个硬件分类导航
--   (硬件类别保留在数据里做检索词, 不再当导航)。可用高度 0..247, 台阶栏盖子线 252 起。
local HOME = {
    box    = { x = PAD, y = 4, w = W - PAD * 2, h = 148 },  -- 上半屏大框: 装数据列表
    rec    = { x = PAD, y = 176, w = W - PAD * 2, rh = 20, n = 3 },
}
-- ★ 2026-11-23 第三轮 (linit): 首页精简 —— 去掉标题排「故障信息库/共 N 条」与
--   第二排搜索提示条。大框 (占屏高一半, R5 圆角 + 1px 描边) 里**直接铺数据**,
--   不留空白、不写占位提示; 框下是最近查询; 搜索统一走底部台阶栏。
--   7 层入口 / 常用栏不再上首页 (LAYER_* 与 open_layer 保留, 需要时可恢复;
--   层名同义词仍进检索串, 打「蓝屏」「设备管理器」照样命中整层)。
local HOME_R    = 5              -- 大框圆角半径 R5
local HOME_ROWS = 6              -- 大框内可见数据行数 (148px 高: 10 + 6*22 = 142)
local HOME_RH   = 22             -- 框内数据行高
local DATA_CODE_W = 108          -- 框内码列定宽 (与列表页同思路: 长码不截断)
local CHIP_W, CHIP_H, CHIP_GAP = 94, 20, 2   -- 层入口方块 (首页已不画, 留作恢复)
local CHIP_Y0 = 74

-- ------------------------------- 类别 ------------------------------------
-- 硬件类别只做检索维度 (数据字段 cat) 与详情标签, 首页不再拿它当导航
local CATS = {
    "启动引导", "硬盘存储", "内存",     "主板BIOS", "电源电池", "散热风扇",
    "显示屏幕", "键盘输入", "安全密码", "网络无线", "蜂鸣报警",
}
local C_BOOT, C_DSK, C_RAM, C_MB, C_PWR, C_FAN, C_DSP, C_KBD, C_SEC, C_NET, C_BEEP =
      1,        2,       3,      4,     5,      6,      7,      8,     9,      10,     11
local C_DEV, C_OS = 12, 13

-- 七层: 按「码出现在哪个界面」分, 这是首页唯一导航维度
local LAYER_ORDER = { "POST", "BEEP", "BOOT", "BSOD", "DEVMGR", "SYS", "SELF" }
local LAYER_NAME = {
    POST = "固件自检码", BEEP = "蜂鸣码",       BOOT = "引导失败",
    BSOD = "蓝屏 STOP",  DEVMGR = "设备管理器", SYS = "系统错误", SELF = "开机提示",
}
local LAYER_SHORT = {
    POST = "自检", BEEP = "蜂鸣", BOOT = "引导", BSOD = "蓝屏",
    DEVMGR = "设备", SYS = "系统", SELF = "提示",
}
-- 层名同义词进检索串: 打「蓝屏」「设备管理器」「引导」这类中文分类词也能命中整层
local LAYER_ALIAS = {
    POST   = "固件自检 自检码 post bios",
    BEEP   = "蜂鸣 报警音 beep beeping",
    BOOT   = "引导 启动失败 引导错误 boot bcd efi",
    BSOD   = "蓝屏 蓝屏代码 蓝屏错误 stop bsod lanping",
    DEVMGR = "设备管理器 设备代码 驱动 code devmgr device",
    SYS    = "系统错误 系统码 更新 安装 恢复 hresult win32 sys",
    SELF   = "开机提示 屏幕提示 报错文字 self",
}
local CATS_CN = {
    [C_BOOT] = "启动引导", [C_DSK] = "硬盘存储", [C_RAM] = "内存",
    [C_MB] = "主板BIOS",   [C_PWR] = "电源电池", [C_FAN] = "散热风扇",
    [C_DSP] = "显示屏幕",  [C_KBD] = "键盘输入", [C_SEC] = "安全密码",
    [C_NET] = "网络无线",  [C_BEEP] = "蜂鸣报警", [C_DEV] = "设备驱动",
    [C_OS] = "操作系统",
}
local LC = {}   -- 每层条数 (build_index 填), 首页方块上显示真实条数

-- 常用码 (按现场出现频率挑的 6 条, 首页只显示前 3 条)
local HOT = { "0199", "0175", "0251", "0271", "2100", "Fan error" }

-- ------------------------------- 数据库 ----------------------------------
-- 来源简称:
--   HMM   = ThinkPad Hardware Maintenance Manual 错误码表 (download.lenovo.com)
--   KB    = 联想知识库 (iknow.lenovo.com.cn / lenovo.com.cn/wiki)
--   SUP   = 联想支持文档 (support.lenovo.com)
--   X390  = 联想官方 X390 错误消息页 (download.lenovo.com/pccbbs/pubs/x390)
local DB = {


-- ========== POST · 固件自检数字码 (联想 ThinkPad 官方 HMM 错误码表) ==========

{ "0135", "Error 0135: System/CPU Fan failure", "机箱或 CPU 风扇故障", C_FAN, "商用台式 ThinkCentre / 家用台式",
  "1) 风扇积灰卡死或转速不足 2) 风扇供电线未插 3) 风扇本体损坏 4) 拆装后忘插风扇线",
  "1) 断电开机箱, 检查风扇是否转 2) 重新插紧风扇供电线 3) 清灰后复测 4) 换同规格风扇",
  "先查线再查灰最后换扇; 不要长期带故障运行",
  "无", "现场可修", "KB", "台式机自检的风扇报错, 笔记本对应 Fan error",
  "0135 cpu fan", "POST", "联想",
  "1) 开机风扇是否转 2) BIOS 中风扇转速是否可读 3) 清灰换硅脂后是否消失; 转但报错 ⇒ 测速线" },

{ "0162", "Error 0162: Setup data integrity check failure", "BIOS 设置数据完整性校验失败", C_MB, "商用台式 ThinkCentre",
  "1) CMOS 设置区校验失败 2) 异常断电 3) CMOS 电池没电",
  "1) 开机按 F1 进 BIOS 2) 按 F9 载入默认 3) F10 保存退出 4) 反复出现换 CMOS 电池",
  "恢复默认即可; 反复出现查电池",
  "无", "现场可修", "KB", "企业批量机型常见",
  "0162 bios", "POST", "联想",
  "1) 是否改过 BIOS 设置或刷过 BIOS 2) F9+F10 后是否消失 3) 仍报 ⇒ NVRAM 区损坏" },

{ "0164", "Error 0164: Memory size decreased", "内存容量减少", C_RAM, "商用台式 ThinkCentre",
  "1) 内存条被拔掉或未插紧 2) 内存条损坏 3) 插槽接触不良 4) 混插不兼容内存",
  "1) 断电开机箱确认内存条是否在位 2) 重插并擦净金手指 3) 逐条单插测试 4) 换同规格内存",
  "先确认是否被人拔过内存; 再逐条排查",
  "无", "现场可修", "KB", "加装内存后出现多为接触问题",
  "0164 ram", "POST", "联想",
  "1) BIOS 识别容量与实装是否一致 2) 是否拔过内存/接触不良 3) 重插后容量恢复即消失" },

{ "0175", "Bad CRC1, stop POST task", "EEPROM 校验和不正确, 自检中止", C_MB, "ThinkPad 全系",
  "1) 主板 EEPROM 校验和损坏 2) BIOS 刷新中断或刷错版本 3) 主板 RTC/EEPROM 老化",
  "1) 断开电源与电池, 静置 1 分钟再开机 2) 用官方 BIOS 更新包按说明重刷 3) 重刷后仍报 0175 基本可判主板故障",
  "先试重刷官方 BIOS; 无效按主板故障处理, 不要再反复刷",
  "无 (不涉及用户数据); 刷 BIOS 期间不可断电", "必须返厂", "HMM", "官方 HMM 的 FRU 指向系统主板",
  "0175 bios eeprom crc", "POST", "联想",
  "1) 断电静置后连开 3 次, 每次必报 ⇒ 固定故障 2) 进 BIOS 按 F9 载入默认再退出, 仍报 ⇒ 排除设置错误 3) 重刷官方 BIOS 后消失 ⇒ 刷写问题; 仍报 ⇒ 主板 EEPROM" },

{ "0176", "System Security - The system has been tampered with", "系统安全 - 检测到系统被篡改", C_SEC, "ThinkPad T/X/P/L/E",
  "1) 安全芯片检测到固件/安全设置被非授权修改 2) 换过主板或刷过非官方 BIOS 3) 安全芯片电池异常",
  "1) 进 BIOS 按 F9 载入默认, F10 保存重启 2) 回想近期是否刷过非官方 BIOS 或换过主板 3) 反复出现记下完整报错送检",
  "先恢复 BIOS 默认设置; 仍报错需服务站检查安全芯片",
  "无", "必须返厂", "HMM", "部分机型只在开机时提示一次",
  "0176 system security the", "POST", "联想",
  "1) 回想近期是否刷过非官方 BIOS/换过主板 2) F9+F10 后是否消失 3) 反复出现记下完整报错送检, 不要反复尝试改安全设置" },

{ "0177", "Bad SVP data, stop POST task", "超级用户密码校验和错误, 自检中止", C_SEC, "ThinkPad T/X/P/L/E",
  "1) EEPROM 中超级密码区校验和不正确 2) 刷写 BIOS 或掉电导致密码区损坏",
  "1) 断电静置后重试, 确认是否必现 2) 不要再尝试设置/清除密码 3) 记录完整报错后送检",
  "该码属主板级故障, 官方处理方式是更换主板; 无法通过清 CMOS 解决",
  "无", "必须返厂", "HMM", "与 0175 不同, 0177 几乎不可能是软件问题",
  "0177 svp eeprom", "POST", "联想",
  "1) 断电静置后复测是否必现 2) 是否改过超级密码 3) 必现且与密码操作无关 ⇒ 主板级, 直接送检" },

{ "0182", "Bad CRC2. Enter BIOS Setup and load Setup defaults", "CRC2 校验错误, 需载入 BIOS 默认值", C_MB, "ThinkPad 全系",
  "1) CMOS 中 CRC2 设置区校验和不正确 2) CMOS 电池电压偏低导致设置丢失 3) 异常断电",
  "1) 开机按 F1 进 BIOS 2) 按 F9 载入默认值 3) 按 F10 保存退出并重启",
  "多数情况按 F9+F10 即可恢复; 反复出现则更换 CMOS 电池",
  "无", "现场可修", "HMM", "与 0251 常同时出现, 说明 CMOS 电池该换了",
  "0182 crc2 bios cmos cmos dianchi", "POST", "联想",
  "1) F9+F10 后是否消失 2) 隔夜冷启动是否复现 ⇒ 指向 CMOS 电池 3) 万用表测纽扣电池低于 2.8V 即换" },

{ "0183", "Bad CRC of Security Settings in EFI Variable", "EFI 变量中安全设置校验错误", C_SEC, "ThinkPad T/X/P/L/E",
  "1) EFI 变量里的安全设置校验和不正确 2) Secure Boot 密钥库被改动 3) BIOS 升级未完成",
  "1) 进 ThinkPad Setup 检查 Security 页设置 2) 按 F10 保存退出 3) 不行再载入默认值 F9+F10",
  "先保存一次设置; 无效再恢复默认; 仍报错送检",
  "无", "现场可修", "HMM", "官方处置顺序就是先 F10 再换主板",
  "0183 efi crc secure boot", "POST", "联想",
  "1) 进 BIOS 保存一次设置后是否消失 2) 关掉 Secure Boot 再打开是否复现 3) 升级 BIOS 后复测" },

{ "0185", "Bad startup sequence settings", "启动顺序设置损坏", C_BOOT, "ThinkPad 全系",
  "1) 启动顺序在 CMOS 中的记录损坏 2) 换过硬盘/加装过启动设备 3) 断电导致设置丢失",
  "1) 进 BIOS 看 Boot 页启动项是否为空或乱序 2) 按 F9 载入默认 3) F10 保存后重启",
  "恢复默认启动顺序即可; 若启动项里看不到硬盘, 转查硬盘检测",
  "无", "现场可修", "HMM", "常与硬盘未被识别同时出现",
  "0185 boot order", "POST", "联想",
  "1) 进 BIOS 看 Boot 页启动项是否为空或乱序 2) F9 载入默认后是否恢复 3) 恢复后仍看不到硬盘 ⇒ 转查硬盘识别" },

{ "0187", "EAIA data access error", "EAIA 数据访问错误 (EEPROM 读取失败)", C_MB, "ThinkPad T/X/P/L/E",
  "1) 访问 EEPROM 失败 2) 主板 EEPROM 芯片或供电异常 3) 主板受潮/虚焊",
  "1) 断电静置后复测是否必现 2) 不要自行拆主板 3) 记录报错送服务站检测",
  "官方 FRU 指向系统主板, 需硬件维修",
  "无", "必须返厂", "HMM", "EAIA = 资产信息区, 与序列号写入相关",
  "0187 eaia eeprom", "POST", "联想",
  "1) 断电静置后复测 2) 是否与 0175/0188 同时出现 3) 必现 ⇒ 主板 EEPROM, 不要自行拆机" },

{ "0188", "Invalid RFID Serialization Information Area", "RFID 序列化信息区域无效", C_MB, "ThinkPad T/X/P/L/E",
  "1) EEPROM 块 0/1 校验和不正确 2) 换过主板或序列号写入失败 3) EEPROM 老化",
  "1) 进 BIOS 尝试重新写入序列号/UUID (需服务站权限) 2) 记录报错 3) 送检",
  "涉及序列号区, 用户侧无法修复",
  "无", "必须返厂", "HMM", "官方处置: 用 ThinkPad Setup 重设 S/N 或 UUID",
  "0188 rfid eeprom", "POST", "联想",
  "1) 是否换过主板或写过序列号 2) BIOS 中序列号是否为空/异常 3) 必现需服务站重写, 现场无解" },

{ "0189", "Invalid RFID configuration information area", "RFID 配置信息区域无效", C_MB, "ThinkPad T/X/P/L/E",
  "1) EEPROM 块 4/5 校验和不正确 2) 主板配置区被误写 3) EEPROM 老化",
  "1) 进 BIOS 尝试重设机型/序列号 2) 记录完整报错 3) 送服务站",
  "与 0188 同属 EEPROM 区损坏, 需专用工具重写",
  "无", "必须返厂", "HMM", "常与 0188 连报",
  "0189 rfid eeprom", "POST", "联想",
  "1) 是否与 0188 同时出现 2) BIOS 中机型/序列号是否完整 3) 必现 ⇒ 主板配置区损坏, 服务站重写" },

{ "0191", "System Security - Invalid Remote Change requested", "系统安全 - 请求的远程更改无效", C_SEC, "ThinkPad T/X/P/L/E",
  "1) 远程管理下发的配置更改失败 2) 安全设置与当前状态冲突 3) BIOS 设置被外部工具改动",
  "1) 进 ThinkPad Setup 重新保存一次设置 (F10) 2) 确认没有挂起的远程管理任务 3) 复现则送检",
  "多数刷新设置即消失; 企业机型检查管理端策略",
  "无", "现场可修", "HMM", "企业批量部署环境更常见",
  "0191 system security invalid remote", "POST", "联想",
  "1) 是否接入过远程管理/批量部署 2) 断开管理端后是否消失 3) 仍报错查 BIOS 安全设置是否被策略锁定" },

{ "0192", "System Security - Embedded Security hardware tamper detected", "系统安全 - 检测到嵌入式安全硬件被篡改", C_SEC, "ThinkPad T/X/P/L/E",
  "1) 安全芯片检测到外壳/电路被打开 2) 安全芯片故障 3) 主板受潮短路",
  "1) 断电静置后复测 2) 确认机器近期是否拆修 3) 送服务站检测安全芯片",
  "属安全芯片告警, 无法用设置绕过",
  "无", "必须返厂", "HMM", "拆机后首次开机会触发",
  "0192 tamper tpm", "POST", "联想",
  "1) 是否设置过安全芯片密码 2) 清除安全芯片后是否消失 3) 必现且清不掉 ⇒ 安全芯片硬件故障" },

{ "0199", "System Security - Security password retry count exceeded", "系统安全 - 超出安全密码重试次数", C_SEC, "ThinkPad 全系",
  "1) 连续 3 次输错超级用户密码 (加电/BIOS 密码) 2) 键盘误触或输入法问题 3) 忘记密码",
  "1) 确认大小写与键盘布局后重试 2) 进 ThinkPad Setup 用正确密码解锁 3) 记不起密码不要乱试, 直接联系服务站",
  "还有正确密码就进 BIOS 保存一次即可清除; 密码遗忘需服务站凭购机凭证处理",
  "无", "现场可修", "KB", "继续乱试会锁死, 官方建议确认密码后再试",
  "0199 svp", "POST", "联想",
  "1) 确认是否输错过开机密码 2) 静置 30 分钟后再试 3) 计数继续增长 ⇒ 有人反复试密码或恶意尝试" },

{ "01C8", "Two or more modem devices are found", "检测到两个以上调制解调器设备", C_MB, "ThinkPad 老机型 (含 Mini-PCI 插槽)",
  "1) 同时装了 Mini-PCI 卡与调制解调器子卡 2) 扩展卡地址冲突",
  "1) 关机断电取下电池 2) 拆下多余的 Mini-PCI 卡或调制解调器子卡 3) 只留一个后开机 4) 也可按 Esc 忽略提示继续启动",
  "拔掉多余模块即可; 只需一个通讯模块",
  "无", "现场可修", "HMM", "老机型专属, 新机型已无此报错",
  "01c8 modem", "POST", "联想",
  "1) 是否同时接了内置与外接调制解调器 2) 拔掉外接设备后是否消失 3) BIOS 中禁用对应设备复测" },

{ "01C9", "More than one Ethernet devices are found", "检测到多个以太网设备", C_MB, "ThinkPad 老机型",
  "1) 加装了第二个有线网卡 2) 扩展坞内置网卡与机身网卡冲突",
  "1) 关机断电 2) 取下后加装的网卡 3) 或按 Esc 忽略提示继续启动",
  "保留一个网卡即可; 用扩展坞时报错先拔坞站",
  "无", "现场可修", "HMM", "扩展坞场景常见",
  "01c9 more than one ethernet", "POST", "联想",
  "1) 是否装了双网卡或外接网卡 2) 拔掉外接网卡后是否消失 3) BIOS 禁用板载网卡复测" },

{ "01CA", "More than one Wireless LAN devices are found", "检测到多个无线网卡", C_NET, "ThinkPad 老机型",
  "1) 加装了第二块无线网卡 (如换 WiFi 6 网卡未拆原卡) 2) 卡座接触不良被识别两次",
  "1) 关机断电取电池 2) 拆下后加装的无线网卡 3) 重新插紧保留的那块 4) 或按 Esc 忽略继续启动",
  "升级网卡时务必拆掉原卡, 不要两块并存",
  "无", "现场可修", "HMM", "换网卡后最常见的报错",
  "01ca wifi", "POST", "联想",
  "1) 是否装了双无线网卡 2) 拔掉外接无线网卡后是否消失 3) BIOS 禁用无线复测" },

{ "0200", "Hard disk error - The hard disk is not working", "硬盘错误 - 硬盘无法工作", C_DSK, "ThinkPad 全系",
  "1) 硬盘/SSD 松动或接触不良 2) 硬盘本体故障 3) BIOS 中硬盘模式 (AHCI/RAID) 被改 4) 主板 SATA 通道故障",
  "1) 关机断电重新插拔硬盘 2) 进 BIOS 载入默认 (F9+F10) 3) 换一块已知正常的盘测试 4) 仍报错则查主板",
  "先重插再换盘; 盘上有数据时不要反复通电尝试",
  "有: 硬盘可能已损坏, 拆下前不要初始化或格式化", "需备件", "HMM", "老机型用词为 hard disk, 新机型对应 2100/2110",
  "0200 hdd ssd", "POST", "联想",
  "1) 听硬盘是否转动、有无异响 2) BIOS 中是否识别到硬盘 3) 换线换口后仍不识别 ⇒ 盘体故障, 先备份" },

{ "0210", "Keyboard error (021x)", "键盘错误 (伴随两声短鸣)", C_KBD, "ThinkPad 全系",
  "1) 键盘排线松动 2) 键盘进液/按键卡死 3) 外接键盘冲突 4) 键盘控制芯片故障",
  "1) 拔掉所有外接键盘与鼠标 2) 逐个按下所有按键看是否卡住 3) 关机重插键盘排线 (需拆机) 4) 进 BIOS 跑键盘自检",
  "先排除外接设备与卡键; 进液需尽快断电送修",
  "无", "需备件", "HMM", "021x 中 x 为具体键码",
  "0210 0211 021x", "POST", "联想",
  "1) 外接键盘是否正常 ⇒ 说明内置排线/键盘 2) BIOS 中内置键盘是否可用 3) 换键盘复测" },

{ "0220", "Monitor type error", "显示器类型与 CMOS 记录不符", C_DSP, "ThinkPad 老机型 / 台式",
  "1) 换过屏或外接显示器 2) CMOS 中显示配置损坏 3) 屏线接触不良",
  "1) 进 BIOS 载入默认值 (F9+F10) 2) 断开外接显示器后开机 3) 仍报错检查屏线",
  "以恢复默认设置为主; 换屏后需在 BIOS 重新识别",
  "无", "现场可修", "HMM", "老机型专属",
  "0220 monitor cmos", "POST", "联想",
  "1) 是否接了外接显示器 2) 换线/换显示器是否消失 3) 仅内置屏报错 ⇒ 屏线或屏" },

{ "0230", "Shadow RAM error - Shadow RAM fails at offset nnnn", "影子内存错误", C_RAM, "ThinkPad 全系",
  "1) 内存条故障影响影子内存区 2) 内存插槽接触不良 3) 主板内存控制器故障",
  "1) 重新插拔内存, 用橡皮擦净金手指 2) 双条内存时逐条单插测试 3) 换已知良好内存 4) 仍报错查主板",
  "先换内存验证; 换内存无效按主板故障处理",
  "无", "需备件", "HMM", "与 0231/0232 一起判断内存链路",
  "0230 shadow ram", "POST", "联想",
  "1) 内存单条交替测试 2) 换槽后是否消失 3) 换条无效 ⇒ 按主板故障处理" },

{ "0231", "System RAM error - System RAM fails at offset nnnn", "系统内存错误", C_RAM, "ThinkPad 全系",
  "1) 内存条损坏 2) 金手指氧化/插槽进灰 3) 内存与机型不兼容 (频率/容量)",
  "1) 断电取电池, 重插内存并擦净金手指 2) 单条逐一测试定位坏条 3) 换同规格内存 4) 仍报错查主板",
  "九成是内存条或接触问题, 先单条替换法",
  "无", "需备件", "HMM", "offset nnnn 是出错地址",
  "0231 ram", "POST", "联想",
  "1) 记录报错 offset, 换条后 offset 是否变化 2) 单条单槽测试 3) 换条不复现 ⇒ 内存条" },

{ "0232", "Extended RAM error - Extended RAM fails at offset nnnn", "扩展内存错误", C_RAM, "ThinkPad 全系",
  "1) 扩展内存条故障 2) 双通道混插不同规格 3) 插槽故障",
  "1) 取下扩展内存只用板载内存测试 2) 逐条单插 3) 换同规格同容量内存",
  "混插不同频率/容量是最常见诱因, 尽量成对同规格",
  "无", "需备件", "HMM", "与 0231 区别: 报的是扩展槽那一条",
  "0232 ram", "POST", "联想",
  "1) 优先测扩展槽内存 2) 拔掉扩展内存只留板载复测 3) 换条验证" },

{ "0250", "System battery error - System battery is dead", "系统电池 (CMOS 电池) 失效", C_PWR, "ThinkPad 全系",
  "1) 主板纽扣电池电压不足 2) 电池座接触不良 3) 长期不用导致电池耗尽",
  "1) 接电源适配器连续充电 8 小时以上 2) 进 BIOS 重设日期时间 3) 仍报错更换 CMOS 纽扣电池",
  "换 CMOS 电池后必须重设时间, 否则会转报 0251/0271",
  "无", "现场可修", "HMM", "官方步骤就是先充 8 小时再换电池",
  "0250 cmos", "POST", "联想",
  "1) 万用表测纽扣电池电压, 低于 2.8V 即换 2) 换电池后 F9+F10 是否消失 3) 新电池很快又报 ⇒ 主板漏电" },

{ "0251", "System CMOS checksum bad - Default configuration used", "CMOS 校验和错误, 已使用默认配置", C_PWR, "ThinkPad 全系 / 台式 / 一体机",
  "1) CMOS 纽扣电池没电 2) 主板断电导致设置丢失 3) 更换主板后未重设",
  "1) 接电源充电 8 小时 2) 进 BIOS 重设日期时间与启动项 3) F10 保存 4) 反复出现就换 CMOS 电池",
  "换电池 + 重设时间即可; 反复出现说明电池确实没电了",
  "无", "现场可修", "HMM", "开机时间回到很早的年份就是它的典型表现",
  "0251 cmos", "POST", "联想",
  "1) F9+F10 后是否消失 2) 隔夜冷启动是否复现 ⇒ 电池 3) 每次开机时间归零 ⇒ 电池或 RTC 电路" },

{ "0252", "Password checksum bad - The password is cleared", "密码校验和错误, 密码已被清除", C_SEC, "ThinkPad 老机型",
  "1) 密码存储区校验和出错 2) CMOS 电池耗尽导致密码区丢失",
  "1) 进 BIOS 重新设置开机密码 (如需要) 2) 检查 CMOS 电池电压 3) 保存退出",
  "密码被清除属保护性结果, 重设密码或保持无密码均可",
  "无", "现场可修", "HMM", "与 0251 同源",
  "0252 cmos", "POST", "联想",
  "1) 是否改过密码 2) 重设密码后是否消失 3) 反复出现且密码被清 ⇒ 安全芯片/CMOS 异常" },

{ "0253", "EFI Variable Block Data was destroyed", "EFI 变量块数据被破坏", C_MB, "ThinkPad 新机型 (UEFI)",
  "1) EFI 变量区被写坏 2) 异常断电/强制关机 3) 安全启动密钥写入中断",
  "1) 进 BIOS 载入默认值 2) 检查 Secure Boot 设置 3) 反复出现送服务站重刷 BIOS",
  "先恢复默认; 变量区损坏需专用工具重建",
  "无", "必须返厂", "HMM", "常与「UEFI 变量存储将满」同时出现",
  "0253 efi uefi", "POST", "联想",
  "1) 是否刷过 BIOS 或改过 Secure Boot 2) F9+F10 后是否恢复 3) 必现 ⇒ EFI 变量区损坏, 需专用工具" },

{ "0260", "System timer error", "系统定时器错误", C_MB, "ThinkPad 老机型",
  "1) 主板定时器电路故障 2) CMOS 电池失效连带 3) 主板老化",
  "1) 接电源充电 8 小时并更换 CMOS 电池 2) 进 BIOS 重设时间 3) 仍报错送检",
  "先排除 CMOS 电池, 再判主板",
  "无", "需备件", "HMM", "与 0270 常一起出现",
  "0260 timer cmos", "POST", "联想",
  "1) 是否超频或改过外频 2) 换 CMOS 电池后是否消失 3) BIOS 默认后仍报 ⇒ 主板时钟电路" },

{ "0270", "Real-Time Clock Error - System Real Time Clock is not functional", "实时时钟错误, RTC 不工作", C_MB, "ThinkPad 全系",
  "1) RTC 电路故障 2) CMOS 电池完全没电 3) 主板 RTC 晶振异常",
  "1) 更换 CMOS 电池并充电 2) 进 BIOS 重设时间 3) 断电后时间是否还能保持 4) 不能保持则送检",
  "换电池后仍丢时间 = 主板 RTC 故障",
  "无", "需备件", "HMM", "与 0271 区别: 0270 是硬件不工作, 0271 只是没设置",
  "0270 rtc", "POST", "联想",
  "1) BIOS 中时间是否能保存 2) 换 CMOS 电池后是否消失 3) 仍不保存 ⇒ RTC 电路/主板" },

{ "0271", "Check Date and Time settings", "请检查日期和时间设置", C_MB, "ThinkPad 全系 / 台式 / 一体机",
  "1) 机器里没有设置日期时间 2) CMOS 电池没电导致每次开机清零 3) 换主板/刷 BIOS 后未设置",
  "1) 开机按 F1 进 BIOS 2) 设置正确的日期与时间 3) F10 保存退出 4) 下次开机若又变, 换 CMOS 电池",
  "单纯没设置就设一次; 反复归零说明电池没电",
  "无", "现场可修", "HMM", "装系统前必过这一关, 否则证书/更新会报错",
  "0271 bios", "POST", "联想",
  "1) 系统时间是否正确 2) 改对后保存重启是否消失 3) 每次都变慢 ⇒ 电池" },

{ "1962", "Error 1962: No operating system found", "未找到操作系统", C_BOOT, "家用台式 IdeaCentre / 一体机 / 商用台式",
  "1) 启动顺序里硬盘不在首位 2) 系统引导记录损坏 3) 硬盘未被识别 4) UEFI/Legacy 模式与系统不匹配",
  "1) 进 BIOS 把硬盘设为第一启动项 2) 确认 BIOS 能识别硬盘型号 3) 用 PE 检查分区是否 active 4) 修复引导或重装系统",
  "八成都是引导问题, 先修引导再考虑重装",
  "无", "现场可修", "KB", "国内台式/一体机最高频报错",
  "1962 no os found", "POST", "联想",
  "1) BIOS 中是否识别硬盘 2) 启动项是否为空 3) 进 PE 看分区表与引导文件是否完整" },

{ "2000", "ThinkVantage Active Protection sensor diagnostics failed", "硬盘主动保护传感器自检失败", C_DSK, "ThinkPad T/X 系列 (带 APS)",
  "1) 硬盘防摔传感器故障 2) 主板传感器电路异常 3) 拆装后传感器未复位",
  "1) 断电静置后复测 2) 进 BIOS 查看硬盘保护设置 3) 送服务站检测传感器",
  "属硬件自检失败, 无法用软件关闭规避",
  "无", "必须返厂", "KB", "APS 传感器集成在主板上",
  "2000 aps", "POST", "联想",
  "1) 是否摔过/震动后出现 2) 官方 APS 诊断工具是否报传感器异常 3) 必现 ⇒ 传感器或主板" },

{ "2100", "Detection error on HDD0 (Main HDD)", "主硬盘检测错误", C_DSK, "ThinkPad T/X/P/L/E",
  "1) 硬盘/SSD 未插紧 2) 硬盘本体损坏 3) 主板存储接口故障 4) 硬盘被 BIOS 判为不兼容",
  "1) 关机断电重新插拔硬盘 2) 进 BIOS 看是否识别到型号与容量 3) 换已知良好硬盘测试 4) 仍不识别则查主板",
  "先重插, 再换盘; 换盘正常即原盘故障",
  "有: 盘可能已损坏, 不要格式化, 先做数据救援", "需备件", "HMM", "新机型写作 Storage Device (M.2)",
  "2100 hdd", "POST", "联想",
  "1) BIOS 中是否识别 HDD0 2) 换线换口后是否识别 3) 换盘测试, 仍不识别 ⇒ 接口/主板" },

{ "2101", "Detection error on HDD1 / SSD0", "第二块硬盘 (或 SSD0) 检测错误", C_DSK, "ThinkPad 双硬盘机型",
  "1) 第二块盘未插紧 2) M.2 接口接触不良 3) 盘本体故障",
  "1) 重插该盘并确认卡扣到位 2) BIOS 中确认是否识别 3) 换盘测试",
  "双盘机型逐盘替换即可定位",
  "有: 先备份再动盘", "需备件", "HMM", "机型不同编号指向不同槽位",
  "2101 m2", "POST", "联想",
  "1) 确认第二盘位是否装盘 2) 换线换口复测 3) 换盘交叉验证" },

{ "2102", "Detection error on SSD (M.2)", "M.2 固态硬盘检测错误", C_DSK, "ThinkPad 新机型",
  "1) M.2 SSD 未插紧或螺丝未压紧 2) SSD 故障 3) 接口/主板故障 4) 不支持该规格 (如双面颗粒)",
  "1) 重插 M.2 并压紧螺丝 2) BIOS 中确认型号是否可见 3) 换一块 M.2 测试 4) 仍不行查主板",
  "M.2 没压紧是最常见原因, 先重装",
  "有: SSD 可能损坏, 先备份", "需备件", "HMM", "与 2112 配对: 2102 检测不到, 2112 读到但出错",
  "2102 m2 ssd", "POST", "联想",
  "1) 确认 M.2 是否插到底、螺丝是否固定 2) 换 M.2 槽复测 3) 换盘验证" },

{ "2110", "Read error on HDD0 (Main HDD)", "主硬盘读取错误", C_DSK, "ThinkPad T/X/P/L/E",
  "1) 硬盘出现坏道 2) 盘体机械故障 3) 接口接触不良 4) 数据线/主板通道问题",
  "1) 立刻备份重要数据 2) 用硬盘检测工具看健康度 (SMART) 3) 重插后复测 4) 坏道增多就换盘",
  "能读到但读错, 说明盘在衰退, 优先备份再换盘",
  "高: 盘随时可能彻底失效, 不要跑全盘修复/低格", "需备件", "HMM", "与 2100 区别: 2110 是读错误不是识别不到",
  "2110 smart", "POST", "联想",
  "1) SMART 看 C5/C6 是否非零 2) 换线后是否仍报 3) 低格或换盘" },

{ "2111", "Read error on SSD0", "SSD0 读取错误", C_DSK, "ThinkPad 双硬盘机型",
  "1) SSD 出现坏块 2) 主控异常 3) 接口接触不良",
  "1) 备份数据 2) 查看 SSD 健康度与写入量 3) 重插复测 4) 仍报错换盘",
  "SSD 读错通常意味着颗粒/主控问题, 尽早换",
  "高: 先备份", "需备件", "HMM", "部分机型编号为 2111 对应 HDD1",
  "2111 ssd", "POST", "联想",
  "1) 查 SSD 健康度与剩余寿命 2) 换槽复测 3) 换盘验证" },

{ "2112", "Read error on SSD2 (M.2)", "M.2 固态硬盘读取错误", C_DSK, "ThinkPad 新机型",
  "1) M.2 SSD 坏块 2) 主控过热降级 3) 接口接触不良 4) 固件问题",
  "1) 备份数据 2) 查看 SMART/温度 3) 重插复测 4) 更新 SSD 固件或换盘",
  "优先备份; 换盘前先确认不是过热导致",
  "高: 先备份", "需备件", "HMM", "笔记本长时间高负载后出现要查散热",
  "2112 m2 ssd", "POST", "联想",
  "1) 检查 M.2 散热与接触 2) 换槽复测 3) 换盘验证" },

{ "2200", "Machine Type and Serial Number are invalid", "机型和序列号无效", C_MB, "ThinkPad 全系",
  "1) 主板 EEPROM 中机型/序列号区损坏 2) 换过主板未写入信息 3) EEPROM 老化",
  "1) 记录 BIOS 首页显示的机型与序列号 2) 送服务站用专用工具重写 3) 保修期内凭购机凭证处理",
  "用户侧无法修复, 必须服务站重写",
  "无", "必须返厂", "HMM", "影响保修识别, 尽早处理",
  "2200 machine type and serial", "POST", "联想",
  "1) BIOS 中机型/序列号是否为空 2) 是否换过主板 3) 需服务站重写, 现场无解" },

{ "2201", "Machine UUID is invalid", "机器 UUID 无效", C_MB, "ThinkPad 全系",
  "1) UUID 区数据损坏 2) 换主板未写入 3) EEPROM 异常",
  "1) 进 BIOS 查看 UUID 是否为空或全 F 2) 送服务站重写 UUID 3) 企业环境同步更新管理端记录",
  "必须服务站处理; 企业资产管理需同步修正",
  "无", "必须返厂", "HMM", "与 2200 常同时出现",
  "2201 uuid", "POST", "联想",
  "1) BIOS 中 UUID 是否全 0 或异常 2) 是否换过主板 3) 需服务站重写" },

-- ========== BEEP · 蜂鸣组合码 (联想 HMM 蜂鸣表; 节奏按「长/短 + 停顿」对照) ==========

{ "1 短", "1 short beep", "系统启动正常", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "自检通过, 无故障",
  "1) 无需处理 2) 若仍黑屏, 转查显示与内存",
  "正常提示音",
  "无", "现场可修", "KB", "部分机型正常开机也会响一声",
  "short beep", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "1 长 2 短", "1 long, 2 short beeps", "显示/显卡问题", C_BEEP, "ThinkPad 全系 / 台式",
  "1) 显卡或显示输出故障 2) 独显未插紧 3) 屏线异常",
  "1) 外接显示器验证 2) 重插独显 3) 官方 HMM: 立即关机送修",
  "先外接验证再送修",
  "无", "需备件", "HMM", "",
  "long short beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "1-1-1", "1-1-1 beeps", "系统加电初始化失败", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 主板供电异常 2) 电源输出不稳 3) 主板电容/供电模块故障",
  "1) 换电源测试 2) 最小化配置 (只留 CPU+一条内存) 3) 仍响送修主板",
  "先换电源, 再最小化",
  "无", "需备件", "KB", "三短一组的起手码",
  "1-1-1 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "1-1-2", "1-1-2 beeps", "主板错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 主板电路故障 2) BIOS 芯片损坏 3) 供电异常",
  "1) 清 CMOS (跳线或电池) 2) 最小化配置测试 3) 仍响送修",
  "先清 CMOS, 无效送修",
  "无", "必须返厂", "KB", "与 1-1-1 相邻, 判主板",
  "1-1-2 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "1-1-3", "1-1-3 beeps", "CMOS 或电池失效", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) CMOS 电池没电 2) CMOS 芯片故障 3) 清除 CMOS 后未设置",
  "1) 换 CR2032 纽扣电池 2) 进 BIOS 重设时间 3) F10 保存",
  "换电池 + 重设时间",
  "无", "现场可修", "KB", "换完电池务必重设时间",
  "1-1-3 cmos", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "1-1-4", "1-1-4 beeps", "ROM BIOS 校验错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) BIOS 芯片内容损坏 2) 刷写中断 3) 芯片老化",
  "1) 用官方工具重刷 BIOS 2) 支持双 BIOS 的机型切换备份 BIOS 3) 仍错送修",
  "重刷或切备份 BIOS",
  "无", "现场可修", "KB", "刷写期间不可断电",
  "1-1-4 rom bios", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "1-2-1", "1-2-1 beeps", "系统时钟错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 主板时钟电路故障 2) 晶振异常 3) CMOS 电池失效连带",
  "1) 换 CMOS 电池 2) 重设时间 3) 仍错送修",
  "先换电池",
  "无", "需备件", "KB", "",
  "1-2-1 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "1-2-2", "1-2-2 beeps", "DMA 初始化失败", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) DMA 控制器故障 2) 主板芯片组异常 3) 扩展卡冲突",
  "1) 拔掉所有扩展卡测试 2) 清 CMOS 3) 仍错送修",
  "先拔扩展卡排除冲突",
  "无", "现场可修", "KB", "",
  "1-2-2 dma", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "1-2-3", "1-2-3 beeps", "DMA 页寄存器错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) DMA 页寄存器故障 2) 主板芯片组异常",
  "1) 清 CMOS 2) 拔扩展卡 3) 送修",
  "多为主板级",
  "无", "必须返厂", "KB", "",
  "1-2-3 dma", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "1-3-1", "1-3-1 beeps", "RAM 刷新错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 内存条故障 2) 内存插槽接触不良 3) 主板内存控制器异常",
  "1) 重插内存并擦金手指 2) 单条替换测试 3) 换插槽",
  "先重插再换条",
  "无", "现场可修", "KB", "",
  "1-3-1 ram", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "1-3-2", "1-3-2 beeps", "基本内存错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 内存条损坏 2) 插槽氧化 3) 内存不兼容",
  "1) 单条测试 2) 擦金手指 3) 换同规格内存",
  "替换法定位坏条",
  "无", "现场可修", "KB", "",
  "1-3-2 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "1-3-3", "1-3-3 beeps", "基本内存错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 内存条损坏 2) 插槽接触不良 3) 双条混插不兼容",
  "1) 逐条单插 2) 清插槽灰尘 3) 换同规格",
  "替换法, 别混插",
  "无", "现场可修", "KB", "",
  "1-3-3 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "1-3-3-1", "1-3-3-1 beeps (one short, three short, three short, one short)", "内存或主板内存控制器故障", C_BEEP, "ThinkPad 全系 / ThinkPad Edge / 台式",
  "1) 内存条未插紧 2) 金手指氧化 3) 内存不兼容 (频率/代别不符) 4) 内存插槽或主板控制器损坏",
  "1) 断电取电池, 重插内存并擦净金手指 2) 双条时逐条单插 3) 换同规格内存 4) 仍响判主板",
  "官方 HMM: 内存问题, 立即关机送修; 实操先重插内存",
  "无", "需备件", "HMM", "ThinkPad 经典码, 与 1-3-3 不同 (多一组)",
  "1-3-3-1 thinkpad", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "1-4-1", "1-4-1 beeps", "基本内存地址线错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 内存插槽地址线损坏 2) 主板内存控制器故障",
  "1) 换插槽测试 2) 换内存条 3) 仍错送修主板",
  "换槽换条都无效即主板",
  "无", "需备件", "KB", "",
  "1-4-1 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "1-4-2", "1-4-2 beeps", "基本内存校验错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 内存条局部损坏 2) 插槽接触不良",
  "1) 单条测试 2) 擦金手指 3) 换条",
  "替换法",
  "无", "现场可修", "KB", "",
  "1-4-2 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "2-1-1", "2-1-1 beeps", "前 64KB 基本内存错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 第一条内存故障 2) 插槽 1 损坏 3) 主板内存控制器异常",
  "1) 只留第一条内存测试 2) 换插槽 3) 换内存",
  "先测第一槽",
  "无", "需备件", "KB", "",
  "2-1-1 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "3-1-1", "3-1-1 beeps", "从 DMA 寄存器错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) DMA 控制器故障 2) 主板芯片组异常 3) 扩展卡冲突",
  "1) 拔扩展卡 2) 清 CMOS 3) 送修",
  "多为主板",
  "无", "必须返厂", "KB", "",
  "3-1-1 dma", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "3-1-1-3", "3-1-1-3 beeps", "PCI 资源分配问题", C_BEEP, "ThinkPad 全系 / 台式",
  "1) PCI/PCIe 设备资源冲突 2) 扩展卡故障 3) 主板资源表损坏",
  "1) 拔掉新加的扩展卡/坞站 2) 清 CMOS 载入默认 3) 仍响送修",
  "官方 HMM: 立即关机送修; 先拔新加设备",
  "无", "必须返厂", "HMM", "",
  "3-1-1-3 pci", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "3-1-2", "3-1-2 beeps", "主 DMA 寄存器错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 主板 DMA 控制器故障 2) 芯片组异常",
  "1) 清 CMOS 2) 拔扩展卡 3) 送修",
  "多为主板级",
  "无", "必须返厂", "KB", "",
  "3-1-2 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "3-1-3", "3-1-3 beeps", "主中断处理寄存器错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 中断控制器故障 2) 主板芯片组异常",
  "1) 清 CMOS 2) 拔扩展卡 3) 送修",
  "多为主板级",
  "无", "必须返厂", "KB", "",
  "3-1-3 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "3-2-4", "3-2-4 beeps", "键盘控制器错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 键盘接口/控制器故障 2) 键盘损坏或卡键 3) 主板 SuperIO 异常",
  "1) 换键盘 2) 换 USB 口 3) 清 CMOS 4) 送修",
  "先换键盘排除",
  "无", "现场可修", "KB", "",
  "3-2-4 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "3-3-4", "3-3-4 beeps", "显示内存错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 显卡显存故障 2) 独显未插紧 3) 核显共享内存异常",
  "1) 重插独显 2) 换用核显输出测试 3) 换显卡",
  "先重插再换卡",
  "无", "需备件", "KB", "",
  "3-3-4 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "3-4-2", "3-4-2 beeps", "显示错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 显卡故障 2) 显示输出接口异常 3) 显示器/线材问题",
  "1) 换线换口 2) 换显示器验证 3) 重插或更换显卡",
  "先排除线材与显示器",
  "无", "需备件", "KB", "",
  "3-4-2 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "3-4-3", "3-4-3 beeps", "未发现显示只读存储器", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 显卡 BIOS 芯片故障 2) 显卡未插紧 3) 核显被禁用",
  "1) 重插显卡 2) 换卡测试 3) 清 CMOS 恢复核显",
  "先重插, 再换卡",
  "无", "需备件", "KB", "",
  "3-4-3 rom", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "4 组 4 短", "Four cycles of four short beeps", "TCG 相关功能问题 (可能是 BIOS 代码校验失败)", C_BEEP, "ThinkPad 全系",
  "1) BIOS 代码校验失败 2) 安全启动/TPM 固件异常 3) BIOS 被非官方修改",
  "1) 重刷官方 BIOS 2) 清 TPM 3) 仍响送修",
  "重刷官方 BIOS 优先",
  "中: 重刷前确认 BitLocker 已暂停", "现场可修", "HMM", "",
  "tcg bios", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "4-2-1", "4-2-1 beeps", "时钟错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 主板时钟电路故障 2) 晶振异常",
  "1) 换 CMOS 电池 2) 清 CMOS 3) 送修",
  "多为主板级",
  "无", "必须返厂", "KB", "",
  "4-2-1 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "4-2-2", "4-2-2 beeps", "关机错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 电源管理电路异常 2) 主板供电故障",
  "1) 换电源 2) 清 CMOS 3) 送修",
  "先换电源",
  "无", "需备件", "KB", "",
  "4-2-2 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "4-2-3", "4-2-3 beeps", "A20 门错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 键盘控制器 A20 门故障 2) 主板芯片组异常",
  "1) 换键盘 2) 清 CMOS 3) 送修",
  "老机型专属码",
  "无", "必须返厂", "KB", "",
  "4-2-3 a20", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "4-2-4", "4-2-4 beeps", "保护模式中断错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) CPU/芯片组异常 2) 内存不稳",
  "1) 重插内存 2) 清 CMOS 3) 送修",
  "先查内存",
  "无", "需备件", "KB", "",
  "4-2-4 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "4-3-1", "4-3-1 beeps", "内存错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 内存条故障 2) 插槽接触不良",
  "1) 重插擦金手指 2) 单条测试 3) 换条",
  "替换法",
  "无", "现场可修", "KB", "",
  "4-3-1 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "4-3-4", "4-3-4 beeps", "时钟错误 (第二组)", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 主板时钟电路故障 2) 电池失效",
  "1) 换 CMOS 电池 2) 清 CMOS 3) 送修",
  "多为主板级",
  "无", "必须返厂", "KB", "",
  "4-3-4 beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "4-4-1", "4-4-1 beeps", "串行口错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 串口 (COM) 控制器故障 2) 串口设备冲突",
  "1) 拔掉串口设备 2) BIOS 里禁用串口 3) 送修",
  "可先禁用串口应急",
  "无", "现场可修", "KB", "",
  "4-4-1 com", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "4-4-2", "4-4-2 beeps", "并行口错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) 并口 (LPT) 控制器故障 2) 并口设备冲突",
  "1) 拔掉并口设备 2) BIOS 禁用并口 3) 送修",
  "可先禁用并口应急",
  "无", "现场可修", "KB", "",
  "4-4-2 lpt", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "4-4-3", "4-4-3 beeps", "数字协处理器错误", C_BEEP, "联想台式 / ThinkCentre (传统 BIOS)",
  "1) CPU 协处理器异常 2) 主板 CPU 供电不稳",
  "1) 清 CMOS 2) 检查 CPU 供电 3) 送修",
  "老平台专属",
  "无", "必须返厂", "KB", "",
  "4-4-3 cpu", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "5 短", "5 short beeps", "主板问题", C_BEEP, "ThinkPad 全系 / 台式",
  "1) 主板故障 2) 主板供电异常 3) BIOS 芯片异常",
  "1) 清 CMOS 2) 最小化配置 3) 官方 HMM: 立即关机送修",
  "判主板",
  "无", "必须返厂", "HMM", "",
  "short beeps", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

{ "5-5", "5 short beeps, pause, 5 more short beeps", "TCG/TPM 相关功能问题 (可能是 TPM 初始化失败)", C_BEEP, "ThinkPad 全系",
  "1) TPM/安全芯片初始化失败 2) 安全芯片被禁用或固件异常",
  "1) 进 BIOS 检查 Security Chip 设置 2) 尝试清除/重置 TPM 3) 仍报联系服务站",
  "可继续启动, 但安全功能受限",
  "中: 清 TPM 会丢失加密密钥, 先确认没开 BitLocker", "现场可修", "HMM", "官方说明: 可继续引导并联系技术支持",
  "5-5 tpm tcg", "BEEP", "联想",
  "1) 先数清长/短与停顿, 按「节奏」对照而不是按次数 2) 断电重插内存并换槽复测 3) 换一台同型号机器对比, 只有这台响 ⇒ 该机硬件" },

-- ========== BOOT · 引导失败 (Microsoft 启动管理器 / NTSTATUS) ==========

{ "0xC0000001", "A required device isn't connected or can't be accessed", "所需设备未连接或无法访问", C_BOOT, "全系",
  "1) 系统文件损坏 2) 内存故障导致加载失败 3) 硬盘坏道 4) 异常断电后文件系统损坏",
  "1) 用启动修复 2) 跑内存检测 (Windows 内存诊断) 3) chkdsk 检查磁盘 4) 反复出现查硬盘健康",
  "先修系统文件; 反复出现要查内存和硬盘",
  "中: chkdsk 修复前先备份重要数据", "现场可修", "KB", "伴随随机蓝屏时优先怀疑内存",
  "0xc0000001 status_unsuccessful", "BOOT", "Microsoft",
  "1) 拔掉所有外接设备 2) 启动修复 3) 内存单条测试" },

{ "0xC000000D", "STATUS_INVALID_PARAMETER - An invalid parameter was passed to a service or function", "引导参数无效: 启动管理器拿到的 BCD 或设备参数不合法", C_BOOT, "Windows 7/8/10/11 (UEFI 与 Legacy 都可能)",
  "1) BCD 里 device/osdevice 指向了不存在的分区 (克隆、换盘、扩容后常见) 2) 分区表被改动, 盘符或 ESP 分区号变了 3) 固件启动项参数与当前 UEFI/Legacy 模式不匹配 4) 第三方分区/引导工具改写过启动配置",
  "1) 进 WinRE → 疑难解答 → 命令提示符 2) @bootrec /scanos 3) @bootrec /rebuildbcd 4) @bcdedit /enum all 5) 确认 device 与 osdevice 指向的分区真实存在; 不存在先修分区表 6) UEFI 机器重建 ESP: @bcdboot C:\\Windows /s S: /f UEFI",
  "九成是 BCD 参数与真实分区对不上, 重建启动项即可, 不必重装",
  "中: 重建 BCD 前先导出备份, 否则可能连修复菜单都进不去", "现场可修", "MS", "NTSTATUS 0xC000000D 在引导阶段表示参数非法, 本身不代表硬件损坏",
  "0xc000000d c000000d invalid parameter bcd bootmgr yindao", "BOOT", "Microsoft",
  "1) bcdedit /enum 列出的分区都能访问 2) 重启不再出现该码 3) 双系统环境下另一套系统仍能正常启动" },

{ "0xC000000E", "A device which does not exist was specified", "指定的设备不存在 (引导设备丢失)", C_BOOT, "全系",
  "1) 引导配置指向的硬盘/分区不存在 2) 硬盘模式从 AHCI 改成 RAID/IDE 3) 换过硬盘或分区被删 4) 双系统引导项残留",
  "1) 进 BIOS 恢复默认 (F9+F10), 确认 SATA 模式与装系统时一致 2) 用 PE 修复 BCD 3) 检查分区表是否完好 4) 不行则重装",
  "先对齐硬盘模式; 再修引导; 最后才重装",
  "高: 修引导前先备份", "现场可修", "KB", "与 0xc0000225 常一起出现",
  "0xc000000e bcd ahci status_no_such_device", "BOOT", "Microsoft",
  "1) BIOS 是否识别启动盘 2) 换线换口复测 3) PE 下看分区与引导项" },

{ "0xC000000F", "The boot selection failed because a required device is inaccessible", "所需设备不可访问, 启动选择失败", C_BOOT, "全系",
  "1) 硬盘掉线或接触不良 2) 分区表损坏 3) 硬盘坏道 4) 引导项指向已删除分区",
  "1) 进 BIOS 看硬盘是否还在 2) 重插硬盘 3) 用 PE 看分区是否可见 4) 分区表损坏先备份再修复",
  "先确认硬盘在不在; 不在就是硬件/接触问题",
  "高: 分区操作前必须备份", "现场可修", "KB", "与 0xc000000e 的区别: 这个是设备读不到",
  "0xc000000f status_no_such_file", "BOOT", "Microsoft",
  "1) 启动盘是否在位 2) PE 下 diskpart 看分区 3) 重建 BCD" },

{ "0xC0000010", "STATUS_INVALID_DEVICE_REQUEST - The specified request is not a valid operation for the target device", "设备不支持该请求, 常见于动态磁盘/软镜像启动", C_BOOT, "Windows 7/8/10/11 引导阶段",
  "1) 系统装在动态磁盘且配置了软镜像 2) 主引导盘报不可恢复错误 3) 引导控制器驱动缺失",
  "1) 用安装介质引导, 从第二块盘启动 2) 检查磁盘是否被转为动态磁盘 3) 必要时转换回基本磁盘(先备份)",
  "动态磁盘软镜像在引导阶段极不可靠, 能转基本盘就转",
  "高: 磁盘结构操作前必须备份", "现场可修", "MS", "STATUS_INVALID_DEVICE_REQUEST",
  "0xc0000010 invalid_device_request dongtai cipan", "BOOT", "Microsoft",
  "1) diskpart 下 list disk 看是否标 Dyn 2) 换另一块成员盘能否启动 3) 转基本盘后是否恢复" },

{ "0xC0000017", "STATUS_NO_MEMORY - There isn't enough memory available to create a ramdisk device", "内存不足, 无法创建内存盘设备", C_BOOT, "Windows 7/8/10/11 引导阶段",
  "1) 物理内存不足或内存条故障 2) 内存盘(WinPE/PE 启动)配置过大 3) 引导项残留无效内存盘",
  "1) 拔掉多余外设后重启 2) 用安装介质进入 WinRE, 删除无效引导项 3) 内存条单条测试",
  "先怀疑内存条, 再怀疑引导项",
  "无", "现场可修", "MS", "引导内存盘创建失败",
  "0xc0000017 status_no_memory neicunpan yindao", "BOOT", "Microsoft",
  "1) 是否在 PE/WinPE 启动时出现(多见于内存盘模式) 2) 单条内存是否可过 3) WinRE 下 bcdedit /enum all 查无效项" },

{ "0xC0000034", "STATUS_OBJECT_NAME_NOT_FOUND - The object name is not found", "找不到引导所需文件, 常见于引导文件丢失或损坏", C_BOOT, "Windows 7/8/10/11 引导阶段",
  "1) bootmgr/winload 等引导文件丢失 2) BCD 条目指向错误分区 3) 磁盘被格式化或分区表改动",
  "1) 用安装介质进 WinRE 2) bootrec /fixmbr、/fixboot、/rebuildbcd 3) 仍无效用 bcdboot 重建引导文件",
  "引导文件层问题, 重建引导比重装系统快得多",
  "中: 重建引导前记下原分区布局", "现场可修", "MS", "STATUS_OBJECT_NAME_NOT_FOUND",
  "0xc0000034 object_name_not_found yindao wenjian diushi", "BOOT", "Microsoft",
  "1) WinRE 下 diskpart list vol 确认系统分区与引导分区 2) bcdedit /enum 看 device/path 是否指向正确分区 3) 重建后能否进入系统" },

{ "0xC0000098", "The Windows Boot Configuration Data file does not contain a valid OS entry", "BCD 中没有有效的系统条目", C_BOOT, "全系",
  "1) BCD 里的系统条目被删或损坏 2) 系统分区被格式化 3) 双系统卸载后残留",
  "1) PE 下用 bcdedit 查看条目 2) 重建 BCD 3) 确认系统分区是否还在 4) 不行则重装",
  "条目丢失可重建; 分区没了只能重装",
  "高: 先备份", "现场可修", "KB", "与 0xc0000225 同源",
  "0xc0000098 bcd bcd", "BOOT", "Microsoft",
  "1) PE 下 bcdedit 是否可读 2) bootrec /rebuildbcd 3) 仍失败则重建引导分区" },

{ "0xC00000E9", "STATUS_IO_DEVICE_ERROR - An unexpected I/O error has occurred", "发生意外的 I/O 错误, 多为硬盘/线缆/接口问题", C_BOOT, "Windows 7/8/10/11 引导阶段",
  "1) 硬盘数据线或接口接触不良 2) 硬盘出现坏道或即将失效 3) USB 设备/读卡器干扰引导",
  "1) 拔掉所有 USB 存储设备后重启 2) 重新插拔硬盘数据线与电源线, 换接口 3) 检查 SMART, 必要时换盘",
  "先拔 U 盘, 再查线, 最后查盘 —— 顺序别反",
  "高: 可能随时掉盘, 优先备份", "需备件", "MS", "STATUS_IO_DEVICE_ERROR",
  "0xc00000e9 io_device_error yingpan xianlan", "BOOT", "Microsoft",
  "1) 拔掉所有外接存储后是否恢复 2) 换 SATA 线/换口后是否恢复 3) SMART 的 C5/C6/05 是否非零" },

{ "0xC0000102", "STATUS_FILE_CORRUPT_ERROR - The file or directory is corrupted and unreadable", "文件或目录损坏且无法读取", C_BOOT, "Windows 7/8/10/11 引导阶段",
  "1) 文件系统结构损坏 2) 异常断电导致写入中断 3) 磁盘坏道",
  "1) 用安装介质进 WinRE 2) chkdsk C: /f /r 3) 仍无效则修复安装, 并检查磁盘健康",
  "先修文件系统, 再考虑重装",
  "高: 可能已有数据丢失", "现场可修", "MS", "STATUS_FILE_CORRUPT_ERROR",
  "0xc0000102 file_corrupt_error wenjian sunhuai", "BOOT", "Microsoft",
  "1) chkdsk 输出是否报坏簇 2) 修复后能否进系统 3) 同一位置反复损坏 ⇒ 磁盘坏道, 换盘" },

{ "0xC000014C", "STATUS_BCD_MISSING_REQUIRED_INFORMATION - The Boot Configuration Data file is missing some required information", "引导配置数据(BCD)缺少必需信息", C_BOOT, "Windows 7/8/10/11 引导阶段",
  "1) BCD 存储损坏或不完整 2) 引导分区被格式化/误删 3) 系统迁移后引导未重建",
  "1) WinRE 下 bootrec /rebuildbcd 2) 无效则 bcdboot C:\\Windows /s S: /f ALL 重建 3) 检查引导分区是否还在",
  "BCD 重建对这条码最有效",
  "中: 重建前导出 bcdedit /export 备份", "现场可修", "MS", "STATUS_BCD_MISSING_REQUIRED_INFORMATION",
  "0xc000014c bcd missing yindao peizhi", "BOOT", "Microsoft",
  "1) bcdedit /enum 是否报错或为空 2) 引导分区(通常 100–500MB FAT32)是否存在 3) 重建后能否进入系统" },

{ "0xC0000185", "STATUS_IO_DEVICE_ERROR - The I/O device reported an I/O error", "I/O 设备报错, 常见于 BitLocker/TPM 与固件变更后", C_BOOT, "Windows 7/8/10/11 引导阶段",
  "1) TPM 或安全启动状态被改动 2) BitLocker 无法解锁 3) 硬盘 I/O 异常",
  "1) 先确认 BitLocker 恢复密钥 2) 进 BIOS 恢复安全启动/TPM 原状态 3) 检查硬盘 I/O 与线缆",
  "先想「改过 BIOS 没」, 再想硬盘",
  "高: 涉及 BitLocker, 无密钥可能丢数据", "必须返厂", "MS", "STATUS_IO_DEVICE_ERROR",
  "0xc0000185 io_device_error bitlocker tpm anquanqidong", "BOOT", "Microsoft",
  "1) BIOS 里 Secure Boot/TPM 是否被关或重置 2) 输入恢复密钥能否继续 3) 关闭 BitLocker 后是否恢复" },

{ "0xC0000218", "STATUS_CANNOT_LOAD_REGISTRY_FILE - The registry file cannot be loaded", "无法加载注册表文件, SYSTEM hive 损坏", C_BOOT, "Windows 7/8/10/11 引导阶段",
  "1) SYSTEM hive 损坏 2) 磁盘坏道导致 hive 读取失败 3) 异常断电",
  "1) WinRE 下用 regback 恢复 SYSTEM hive 2) chkdsk 检查磁盘 3) 无效则修复安装",
  "hive 恢复比修系统快, 但先确认盘没坏",
  "高: 操作前先复制 hive 备份", "现场可修", "MS", "STATUS_CANNOT_LOAD_REGISTRY_FILE",
  "0xc0000218 cannot_load_registry_file zhucebiao hive", "BOOT", "Microsoft",
  "1) WinRE 下 reg load 能否挂载该 hive 2) regback 恢复后能否启动 3) chkdsk 是否报坏簇" },

{ "0xC0000221", "STATUS_IMAGE_CHECKSUM_MISMATCH - The image file is corrupted", "系统映像文件校验和不匹配, 多为驱动或系统文件损坏", C_BOOT, "Windows 7/8/10/11 引导阶段",
  "1) 某个 .sys 文件损坏 2) 磁盘坏道 3) 内存故障导致读取错误",
  "1) 蓝屏会给出具体文件名, 用同版本系统复制同名文件替换 2) chkdsk + sfc 3) 内存测试",
  "屏幕会直接告诉你哪个文件坏了, 照名字换",
  "中: 替换系统文件前先备份", "现场可修", "MS", "STATUS_IMAGE_CHECKSUM_MISMATCH",
  "0xc0000221 image_checksum_mismatch xiaoyan wenjian", "BOOT", "Microsoft",
  "1) 记下报错的文件名, 看其大小/日期是否异常 2) 替换后是否通过 3) 多个文件反复损坏 ⇒ 磁盘或内存问题" },

{ "0xC0000225", "The boot configuration data for your PC is missing", "引导配置数据缺失或损坏", C_BOOT, "全系",
  "1) BCD 文件损坏 2) 引导分区被格式化 3) 系统更新中断 4) 克隆迁移系统后引导未修",
  "1) 用 PE 进入命令行执行 bootrec /fixboot 与 /rebuildbcd 2) 或用启动修复 3) 克隆盘需重建引导 4) 仍失败则重装",
  "BCD 重建成功率较高, 优先修不要急着重装",
  "高: 先备份", "现场可修", "KB", "克隆/迁移系统后最常见",
  "0xc0000225 bcd status_not_found", "BOOT", "Microsoft",
  "1) 记下报错里的文件名 2) PE 下检查该文件是否存在/损坏 3) 修复引导或替换文件" },

{ "0xC0000428", "STATUS_INVALID_IMAGE_HASH - Windows cannot verify the digital signature for this file", "无法验证该文件数字签名, 常见于安全启动与驱动签名", C_BOOT, "Windows 7/8/10/11 引导阶段",
  "1) 引导文件或驱动无有效签名 2) 安全启动策略与文件不匹配 3) 文件被篡改",
  "1) 进 BIOS 检查 Secure Boot 设置 2) 用官方介质修复引导文件 3) 移除未签名驱动",
  "不要为了过这一关关安全启动, 先换官方文件",
  "无", "现场可修", "MS", "STATUS_INVALID_IMAGE_HASH",
  "0xc0000428 invalid_image_hash qianming anquanqidong", "BOOT", "Microsoft",
  "1) 报错文件名是什么, 是否第三方驱动 2) 恢复 Secure Boot 默认密钥后是否通过 3) 用官方安装介质启动是否正常" },

{ "Boot Failed / 自动进 F12", "Boot Failed (自动进入 F12 引导菜单)", "引导失败, 自动进入启动菜单", C_BOOT, "全系",
  "1) Windows 引导文件损坏 2) 硬盘引导分区被格式化 3) 双系统引导项被覆盖 4) 硬盘出现坏道",
  "1) 关机拔掉所有外置存储 2) 进 BIOS 确认硬盘在启动列表 3) 用 PE 修复 BCD/引导分区 4) 不行则备份后重装系统",
  "多数为引导文件损坏, 修引导优先于重装",
  "高: 动手前先备份", "现场可修", "KB", "官方说明: 此类报错一般是 Windows 系统引导文件损坏",
  "boot failed f12 bcd boot failed f12", "BOOT", "联想",
  "1) 是否接了 U 盘或移动硬盘 2) 拔掉后能否正常启动 3) 仍失败 ⇒ 引导文件或硬盘" },

{ "Bootmgr is missing", "BOOTMGR is missing", "缺少 BOOTMGR 引导管理器", C_BOOT, "全系 (UEFI/MBR 引导)",
  "1) 引导分区被格式化或隐藏分区被删 2) 引导文件损坏 3) 启动顺序指错盘 4) 双系统安装覆盖引导",
  "1) 进 BIOS 确认启动盘 2) 用 PE 修复引导 (bootrec /fixboot + /rebuildbcd) 3) 分区表异常先修复 4) 不行重装",
  "修引导优先; 不要直接格式化 C 盘",
  "高: 先备份", "现场可修", "KB", "与 NTLDR is missing 同类, 新旧引导之分",
  "bootmgr missing bootmgr", "BOOT", "Microsoft",
  "1) PE 看引导分区是否有 bootmgr 2) bcdboot 重建 3) 启动模式 UEFI/Legacy 与分区表是否匹配" },

{ "NTLDR is missing", "NTLDR is missing", "缺少 NTLDR 引导文件", C_BOOT, "老机型 (Legacy/MBR 引导)",
  "1) 用 Ghost 镜像装系统导致引导文件缺失 2) 引导分区被格式化 3) 硬盘坏道损坏引导文件",
  "1) 用原版系统镜像修复引导 2) 进 PE 检查 C 盘是否为活动分区 3) 重建 MBR/NTLDR 4) 不行则重装",
  "Ghost 装机是这个报错的头号原因, 改用官方镜像安装",
  "高: 先备份数据", "现场可修", "KB", "官方明确指出: 使用 ghost 镜像安装系统或引导被破坏",
  "ntldr missing ghost xp ntldr", "BOOT", "通用",
  "1) 是否装过双系统或改过分区 2) 用 PE 看根目录是否有 NTLDR 与 boot.ini 3) 修复引导, 无效再重装" },

{ "No bootable device", "No bootable device -- insert boot disk and press any key", "没有可启动设备", C_BOOT, "全系",
  "1) 硬盘未被识别 2) 启动项未指向硬盘 3) 硬盘分区表损坏 4) 插着 U 盘但无引导",
  "1) 拔掉所有外置存储 2) 进 BIOS 确认硬盘在启动列表 3) 检查硬盘接触 4) 修复分区表/引导",
  "和 Default Boot Device Missing 同一类, 处理顺序一致",
  "高: 修分区前备份", "现场可修", "KB", "旧机型措辞",
  "no bootable device no bootable", "BOOT", "通用",
  "1) BIOS 是否识别硬盘 2) 启动顺序 3) 换线换口复测" },

{ "Operating System Not Found", "Operating System not found", "未找到操作系统", C_BOOT, "老机型 / 台式",
  "1) 硬盘未被识别 2) 活动分区未设置 3) 引导记录损坏 4) 硬盘模式不匹配",
  "1) 进 BIOS 看硬盘是否可见 2) 重插硬盘 3) PE 下把系统分区设为活动 4) 修复 MBR",
  "先确认硬盘识别, 再修分区激活状态",
  "高: 分区操作前备份", "现场可修", "KB", "与 1962 同类, 台式更常见",
  "operating system not found os not found", "BOOT", "通用",
  "1) BIOS 是否识别硬盘 2) 分区表 GPT/MBR 与启动模式是否匹配 3) PE 下看分区是否标记活动" },

{ "PXE 网络启动", "Intel UNDI, PXE-2.1 (build 083) Start PXE over IPv4", "网卡 PXE 网络启动", C_BOOT, "全系 (商用机型更常见)",
  "1) BIOS 第一启动项被设为网卡 2) 硬盘引导丢失后自动转网络启动 3) 企业环境启用了 PXE 部署",
  "1) 进 BIOS 把硬盘设为第一启动项 2) 关闭 PXE/网络启动选项 3) 若改完仍转 PXE, 说明硬盘未被识别或引导损坏",
  "改启动顺序即可; 反复出现要查硬盘",
  "无", "现场可修", "KB", "PXE 报错本身不是故障, 是启动顺序问题",
  "pxe intel undi ipv4 pxe wangluo qidong", "BOOT", "通用",
  "1) BIOS 启动顺序是否把网络放到第一 2) 硬盘是否被识别 3) 改回硬盘启动是否正常" },

{ "Secure Boot 校验失败", "Secure Boot Violation / Invalid signature detected. Check Secure Boot Policy", "安全启动校验失败, 非法签名", C_BOOT, "全系 (UEFI 机型)",
  "1) 启动盘/系统的引导文件签名不被信任 2) 自行修改过系统引导 3) 装了非签名驱动 4) Secure Boot 密钥库异常",
  "1) 进 BIOS 的 Security 页 2) 需要装非签名系统时临时关闭 Secure Boot 3) 装完再打开 4) 企业机型按策略恢复默认密钥",
  "临时关闭 Secure Boot 可装机; 日常使用建议保持开启",
  "无", "现场可修", "SUP", "与 BitLocker 联动: 改 Secure Boot 可能触发恢复密钥",
  "secure boot secure boot anquanqidong", "BOOT", "通用",
  "1) 是否装过非签名系统或驱动 2) 关掉 Secure Boot 能否启动(仅用于验证) 3) 恢复默认密钥后复测" },

{ "启动修复循环", "Windows 自动修复循环 / 启动修复无法修复你的电脑", "开机反复进入自动修复", C_BOOT, "全系",
  "1) 系统更新失败回滚卡死 2) 驱动导致引导失败 3) 硬盘出现坏道 4) BCD 部分损坏",
  "1) 高级选项里选「卸载最近的更新」 2) 试安全模式 3) 命令行执行 sfc /scannow 与 chkdsk 4) 备份后重置或重装",
  "先卸更新与驱动, 再修系统文件, 最后才重装",
  "高: 重置/重装前备份", "现场可修", "KB", "累积更新失败是头号诱因",
  "qidong xiufu xunhuan", "BOOT", "Microsoft",
  "1) 记下每次修复报的失败项 2) PE 下 sfc 与 chkdsk 3) 回滚最近更新与驱动" },

{ "默认启动设备丢失", "Default Boot Device Missing or Boot Failed. Insert recovery media and hit any key", "找不到可启动设备", C_BOOT, "全系 (ThinkPad / ThinkBook / 小新 / 拯救者 / 台式 / 一体机)",
  "1) 启动顺序里硬盘不在首位 2) 硬盘未被识别或已损坏 3) 系统引导分区被破坏 4) UEFI/Legacy 模式与系统不匹配 5) 插着 U 盘/移动硬盘干扰",
  "1) 拔掉所有 U 盘、移动硬盘、存储卡后重启 2) 进 BIOS 把硬盘调到第一启动项 3) 确认 BIOS 是否能看到硬盘型号 4) 用 PE 修复引导或重装系统 5) 看不到硬盘则查硬盘接触与健康",
  "先拔外设再看启动顺序; 能看到盘就修引导, 看不到盘先解决硬盘识别",
  "高: 重装/修复引导前必须备份数据", "现场可修", "SUP", "官方给出三步: 查启动顺序 → 恢复默认 → 仍不行联系支持",
  "default boot device missing yindao qidong", "BOOT", "联想",
  "1) BIOS 中是否识别硬盘 2) 启动顺序是否正确 3) 进 PE 看引导分区是否存在" },

-- ========== BSOD · 蓝屏 STOP (Microsoft Bug Check Code Reference) ==========

{ "0x0000000A", "IRQL_NOT_LESS_OR_EQUAL", "内核在过高的 IRQL 上访问了可分页内存, 几乎总是驱动问题", C_OS, "Windows 7/8/10/11",
  "1) 第三方驱动(杀软/虚拟网卡/外设)在中断上下文访问分页内存 2) 内存条不稳定 3) 超频",
  "1) 记下蓝屏第二行的 .sys 文件名, 那是直接元凶 2) 安全模式卸载/回滚该驱动 3) 关掉超频与 XMP, 跑内存测试",
  "先看蓝屏上的驱动名, 有名字就直接换它; 没名字先测内存",
  "无", "现场可修", "MS", "Bug Check 0xA",
  "0x0a irql_not_less_or_equal qudong neicun chaoshi", "BSOD", "Microsoft",
  "1) 事件查看器筛 Kernel-Power 41 / BugCheck 1001 看重复出现的同一驱动 2) BlueScreenView 或 WinDbg !analyze -v 看 Probably caused by 3) 同一码换内存条后消失 ⇒ 内存问题而非驱动" },

{ "0x00000019", "BAD_POOL_HEADER", "内核内存池头部被写坏, 通常是驱动越界写", C_OS, "Windows 7/8/10/11",
  "1) 驱动越界写入池内存 2) 内存条故障 3) 磁盘/文件系统损坏连带",
  "1) 卸载最近安装的驱动与驱动级软件(杀软、虚拟光驱、沙箱) 2) 跑内存测试 3) 跑 sfc /scannow 与 chkdsk",
  "重点查「最近装了什么」, 这类码时间点比代码更有价值",
  "中: 可能伴随文件系统损坏, 先备份", "现场可修", "MS", "Bug Check 0x19",
  "0x19 bad_pool_header chitou yuejie", "BSOD", "Microsoft",
  "1) 看蓝屏参数第 2 个值(池类型)判断是分页池还是非分页池 2) 驱动验证器(verifier)只针对可疑驱动开启, 逼出越界写 3) 干净启动后不复现 ⇒ 第三方驱动" },

{ "0x0000001A", "MEMORY_MANAGEMENT", "内存管理发现严重不一致, 指向内存条、磁盘或驱动", C_OS, "Windows 7/8/10/11",
  "1) 内存条接触不良或损坏 2) 硬盘坏道导致页面文件读写错误 3) 驱动破坏页表",
  "1) 拔插内存, 单条交替测试 2) 关 XMP/超频 3) chkdsk /f /r 与 sfc /scannow",
  "先内存后磁盘, 两条腿都要查",
  "中: 排查期间不要做重要写入", "需备件", "MS", "Bug Check 0x1A",
  "0x1a memory_management neicun guanli neicuntiao", "BSOD", "Microsoft",
  "1) Windows 内存诊断(mdsched)或 MemTest86 跑满 2 轮 2) 单条单槽替换法定位坏条 3) 换条后仍 1A 且伴随 7A/24 ⇒ 转查磁盘与存储驱动" },

{ "0x0000001E", "KMODE_EXCEPTION_NOT_HANDLED", "内核模式异常无人处理, 多由驱动抛出非法指令", C_OS, "Windows 7/8/10/11",
  "1) 驱动缺陷 2) 内存故障 3) 系统文件损坏",
  "1) 记录蓝屏参数里的异常码与出错地址 2) 安全模式下卸载最近驱动 3) 内存测试 + sfc /scannow",
  "和 8E/3B 同族, 处理路径一致",
  "无", "现场可修", "MS", "Bug Check 0x1E",
  "0x1e kmode_exception_not_handled neihie yichang", "BSOD", "Microsoft",
  "1) 第一参数 0xC0000005 为访问违例, 0x80000003 为断点 2) WinDbg !analyze -v 看异常模块 3) 最近更新过驱动/系统补丁 ⇒ 回滚验证" },

{ "0x00000024", "NTFS_FILE_SYSTEM", "NTFS 文件系统驱动出错, 多与磁盘坏道或数据线相关", C_DSK, "Windows 7/8/10/11",
  "1) 磁盘坏道 2) SATA/NVMe 数据线或接口接触不良 3) 非正常关机导致 NTFS 结构损坏",
  "1) 备份数据(优先) 2) chkdsk C: /f /r 3) 换数据线/换接口, 查 SMART",
  "这条码先备份再修, 别反复重启硬跑",
  "高: 文件系统可能已损坏, 先取证备份", "需备件", "MS", "Bug Check 0x24",
  "0x24 ntfs_file_system yingpan huaidao", "BSOD", "Microsoft",
  "1) CrystalDiskInfo 看 05/C5/C6 是否非零 2) chkdsk 日志是否报坏簇 3) 换线换口后复现 ⇒ 盘体故障; 不复现 ⇒ 线缆/接口" },

{ "0x0000003B", "SYSTEM_SERVICE_EXCEPTION", "执行系统服务例程时发生异常, 常见于显卡驱动与安全软件", C_OS, "Windows 7/8/10/11",
  "1) 显卡/网卡驱动缺陷 2) 杀软或驱动级软件与内核冲突 3) 系统文件损坏",
  "1) 干净启动排除第三方软件 2) 用 DDU 清理显卡驱动后装官方稳定版 3) sfc /scannow 与 DISM 修复系统",
  "多半是驱动, 尤其是显卡",
  "无", "现场可修", "MS", "Bug Check 0x3B",
  "0x3b system_service_exception xianka qudong", "BSOD", "Microsoft",
  "1) 第一参数 0xC0000005 常见于非法访问 2) 蓝屏是否在特定操作(开游戏/插外设)时出现 3) DDU 重装显卡驱动后观察 24 小时" },

{ "0x00000044", "MULTIPLE_IRP_COMPLETE_REQUESTS", "同一 I/O 请求包被重复完成, 驱动缺陷", C_OS, "Windows 7/8/10/11",
  "1) 驱动重复完成同一个 IRP 2) 外设驱动与过滤驱动冲突 3) 老驱动在新系统上不兼容",
  "1) 卸载最近安装的外设/过滤驱动 2) 更新芯片组与存储驱动 3) 需要时用驱动验证器定位具体驱动",
  "少见但指向明确, 就是某个驱动",
  "无", "现场可修", "MS", "Bug Check 0x44",
  "0x44 multiple_irp_complete_requests irp chongfu", "BSOD", "Microsoft",
  "1) 是否插了特定外设才蓝屏 2) 拔掉该外设后是否消失 3) verifier /standard /driver <可疑驱动> 复现" },

{ "0x0000004E", "PFN_LIST_CORRUPT", "页帧号列表损坏, 内存管理结构被破坏", C_RAM, "Windows 7/8/10/11",
  "1) 内存条故障 2) 驱动破坏内存管理结构 3) 磁盘错误导致页数据异常",
  "1) 内存测试(单条交替) 2) 关超频 3) 更新存储与芯片组驱动, chkdsk",
  "先测内存, 这是最常见原因",
  "中: 可能伴随数据损坏", "需备件", "MS", "Bug Check 0x4E",
  "0x4e pfn_list_corrupt yezhen neicun", "BSOD", "Microsoft",
  "1) 第二参数 1/2/3/7 分别代表不同链表损坏, 7 多为驱动 2) MemTest86 至少两轮 3) 换条后不复现 ⇒ 内存; 仍复现 ⇒ 驱动" },

{ "0x00000050", "PAGE_FAULT_IN_NONPAGED_AREA", "访问了不存在的非分页内存地址, 驱动或内存故障", C_RAM, "Windows 7/8/10/11",
  "1) 驱动访问已释放的内存 2) 内存条不稳定 3) 文件系统/页面文件损坏",
  "1) 卸载最近驱动与驱动级软件 2) 内存测试 3) chkdsk /f 与 sfc /scannow",
  "有 .sys 名字就查驱动, 没名字先测内存",
  "中: 可能丢未保存数据", "现场可修", "MS", "Bug Check 0x50",
  "0x50 page_fault_in_nonpaged_area neicun duqu", "BSOD", "Microsoft",
  "1) 蓝屏参数第 4 个值是否指向明显示意(0x0 读/0x2 执行) 2) WinDbg !analyze -v 看 faulting module 3) 反复在开机阶段出现 ⇒ 查页面文件与磁盘" },

{ "0x00000051", "REGISTRY_ERROR", "注册表配置管理器出错, 多为注册表 hive 损坏", C_OS, "Windows 7/8/10/11",
  "1) 注册表文件损坏 2) 磁盘坏道导致 hive 读失败 3) 异常断电",
  "1) 用系统还原点恢复 2) chkdsk /f 检查磁盘 3) 严重时修复安装或重装",
  "这条往往意味着磁盘也开始不可靠, 一起查",
  "高: 注册表损坏可能无法进系统", "现场可修", "MS", "Bug Check 0x51",
  "0x51 registry_error zhucebiao sunhuai", "BSOD", "Microsoft",
  "1) 能否进安全模式: 能 ⇒ hive 局部损坏, 用还原点 2) 事件日志有无磁盘错误 3) chkdsk 报错即磁盘问题, 先换盘再修系统" },

{ "0x00000074", "BAD_SYSTEM_CONFIG_INFO", "系统配置信息(注册表 SYSTEM hive)损坏或缺失", C_OS, "Windows 7/8/10/11",
  "1) SYSTEM hive 损坏 2) 非正常断电 3) 硬件故障导致写入失败",
  "1) 启动修复 / 最后一次正确配置 2) 用 WinRE 的 regback 恢复注册表 3) 仍无效则修复安装",
  "先试启动修复, 再动注册表备份",
  "高: 需先备份, 操作失误会无法启动", "现场可修", "MS", "Bug Check 0x74",
  "0x74 bad_system_config_info peizhi sunhuai", "BSOD", "Microsoft",
  "1) WinRE 下能否加载 SYSTEM hive(bcdedit/reg load) 2) 恢复 regback 后是否可启动 3) 反复出现 ⇒ 磁盘写入不可靠, 查 SMART" },

{ "0x00000077", "KERNEL_STACK_INPAGE_ERROR", "内核栈页从磁盘换入失败, 磁盘或内存问题", C_DSK, "Windows 7/8/10/11",
  "1) 磁盘坏道 2) 内存条故障 3) 数据线接触不良",
  "1) chkdsk /f /r 2) 内存测试 3) 检查/更换数据线",
  "换页失败 = 读盘或读内存失败, 二者择一",
  "中: 说明存储已不可靠, 先备份", "需备件", "MS", "Bug Check 0x77",
  "0x77 kernel_stack_inpage_error huanru shibai", "BSOD", "Microsoft",
  "1) 第一参数 0xC0000001 为读失败, 0xC000009C/0xC000016A 为坏块 2) SMART 看重映射扇区 3) 换线后不复现 ⇒ 线缆" },

{ "0x0000007A", "KERNEL_DATA_INPAGE_ERROR", "内核数据换页失败, 典型磁盘坏道或存储驱动问题", C_DSK, "Windows 7/8/10/11",
  "1) 磁盘坏道/扇区读失败 2) SATA 线缆或接口松动 3) 存储驱动异常 4) 内存故障",
  "1) 立即备份数据 2) chkdsk /f /r 3) 换线换口 + SMART 检测 4) 内存测试",
  "先备份再修盘, 这条码之后经常跟着彻底掉盘",
  "高: 磁盘可能随时失效", "需备件", "MS", "Bug Check 0x7A",
  "0x7a kernel_data_inpage_error huaidao yingpan", "BSOD", "Microsoft",
  "1) 蓝屏参数第 3、4 个值是否同一扇区 ⇒ 固定坏道 2) CrystalDiskInfo 看 C5/C6 3) 换线后仍同扇区报错 ⇒ 盘体" },

{ "0x0000007B", "INACCESSIBLE_BOOT_DEVICE", "无法访问启动设备 (蓝屏 7B)", C_DSK, "全系 (老系统/改过 BIOS 模式)",
  "1) SATA 模式被改 (AHCI ↔ IDE/RAID) 2) 缺存储控制器驱动 3) 硬盘坏道 4) 迁移系统到新平台",
  "1) 进 BIOS 把 SATA 模式改回装系统时的设置 2) 安全模式启动看是否恢复 3) 注入对应驱动 4) 检查硬盘健康",
  "七成是 SATA 模式被改, 改回去即可",
  "中: 先备份再改模式", "现场可修", "KB", "Win7 装 Win10 机器上尤其常见",
  "7b inaccessible boot device sata ahci raid ahci raid", "BSOD", "Microsoft",
  "1) BIOS 磁盘模式(AHCI/RAID/IDE)是否与装系统时一致 2) 进 PE 看系统盘能否识别 3) 一致且能识别 ⇒ 存储驱动或引导修复" },

{ "0x0000007E", "SYSTEM_THREAD_EXCEPTION_NOT_HANDLED", "系统线程异常未被处理, 多由驱动引起", C_OS, "Windows 7/8/10/11",
  "1) 驱动抛出未处理异常 2) 系统文件损坏 3) 内存故障",
  "1) 记下蓝屏中的 .sys 2) 安全模式卸载/回滚 3) sfc /scannow + 内存测试",
  "和 1E/8E 同一处理路径",
  "无", "现场可修", "MS", "Bug Check 0x7E",
  "0x7e system_thread_exception_not_handled xiancheng yichang", "BSOD", "Microsoft",
  "1) 参数 1 为异常码, 参数 2 为出错地址 2) WinDbg !analyze -v 定位模块 3) 最近装过驱动/补丁 ⇒ 回滚" },

{ "0x0000007F", "UNEXPECTED_KERNEL_MODE_TRAP", "意外的内核陷阱, 常为硬件(内存/CPU)或超频导致", C_RAM, "Windows 7/8/10/11",
  "1) 内存条故障或混插不匹配 2) 超频/电压不稳 3) CPU 或主板故障",
  "1) 取消超频、恢复 BIOS 默认 2) 单条内存交替测试 3) 检查 CPU 温度与供电",
  "这条码硬件嫌疑最大, 少往软件上找",
  "中: 可能已发生静默数据错误", "需备件", "MS", "Bug Check 0x7F",
  "0x7f unexpected_kernel_mode_trap yingjian chaoshi neicun", "BSOD", "Microsoft",
  "1) 第二参数为 0x8 表示双重错误, 基本可判硬件 2) 单条单槽法逐条排除 3) 换电源/降频后是否消失" },

{ "0x0000008E", "KERNEL_MODE_EXCEPTION_NOT_HANDLED", "内核模式异常未被处理, 与 1E 同族", C_OS, "Windows 7/8/10/11",
  "1) 驱动缺陷 2) 内存问题 3) 显卡驱动异常",
  "1) 安全模式卸载最近驱动 2) 内存测试 3) sfc /scannow",
  "先看 .sys 名字, 再决定换驱动还是换内存",
  "无", "现场可修", "MS", "Bug Check 0x8E",
  "0x8e kernel_mode_exception_not_handled neihie yichang", "BSOD", "Microsoft",
  "1) 参数 1 为异常码(0xC0000005 最常见) 2) 蓝屏是否出现在特定程序运行时 3) 干净启动后不复现 ⇒ 第三方驱动" },

{ "0x0000009C", "MACHINE_CHECK_EXCEPTION", "CPU 报告机器检查异常, 属硬件级故障", C_MB, "Windows 7/8/10/11",
  "1) CPU/内存/主板硬件故障 2) 超频或供电不稳 3) 散热不良导致降级失败",
  "1) 恢复 BIOS 默认, 关闭超频与 XMP 2) 检查散热与电源 3) 逐件替换(内存→电源→主板)",
  "这条基本是硬件, 软件层只能排除超频",
  "中: 可能已产生计算错误", "必须返厂", "MS", "Bug Check 0x9C",
  "0x9c machine_check_exception cpu yingjian gongdian", "BSOD", "Microsoft",
  "1) 事件日志是否有 WHEA-Logger 错误(记录具体硬件单元) 2) 关超频后是否消失 3) 同一负载下另一台机器正常 ⇒ 本机硬件" },

{ "0x0000009F", "DRIVER_POWER_STATE_FAILURE", "驱动未在时限内完成电源状态切换, 多见于睡眠/唤醒", C_PWR, "Windows 7/8/10/11",
  "1) 网卡/USB/蓝牙驱动电源管理缺陷 2) 快速启动与休眠冲突 3) 外设不支持当前睡眠状态",
  "1) 关闭快速启动 2) 设备管理器逐个取消「允许计算机关闭此设备以节约电源」 3) 更新网卡/USB/蓝牙驱动",
  "睡眠相关蓝屏, 先关快速启动, 再逐个试网卡",
  "无", "现场可修", "MS", "Bug Check 0x9F",
  "0x9f driver_power_state_failure shuimian huanxing dianyuan", "BSOD", "Microsoft",
  "1) 是否只在睡眠/唤醒瞬间出现 2) powercfg /lastwake 与事件日志看唤醒源 3) 取消网卡节能后观察一晚" },

{ "0x000000A0", "INTERNAL_POWER_ERROR", "电源管理器内部错误, 电源状态转换失败", C_PWR, "Windows 7/8/10/11",
  "1) 电源策略/驱动冲突 2) 休眠文件损坏 3) 电池或电源适配器异常",
  "1) 关闭休眠并删除 hiberfil.sys 后重建 2) 更新芯片组与电源管理驱动 3) 换适配器/电池验证",
  "先重建休眠文件, 成本最低",
  "无", "现场可修", "MS", "Bug Check 0xA0",
  "0xa0 internal_power_error dianyuan guanli", "BSOD", "Microsoft",
  "1) 关掉休眠后是否不再出现 2) 是否总在插拔电源/电量低时出现 3) 换适配器复测, 排除供电" },

{ "0x000000A5", "ACPI_BIOS_ERROR", "ACPI BIOS 存在严重错误或不符规范", C_MB, "Windows 7/8/10/11",
  "1) BIOS 与操作系统 ACPI 实现不兼容 2) BIOS 版本过旧 3) 主板 ACPI 表损坏",
  "1) 升级主板 BIOS 到最新 2) 载入 BIOS 默认 3) 安装时加 ACPI 兼容参数(仅临时手段)",
  "先升 BIOS, 这是唯一正经解法",
  "中: 刷 BIOS 期间不可断电", "现场可修", "MS", "Bug Check 0xA5",
  "0xa5 acpi_bios_error bios jianrong", "BSOD", "Microsoft",
  "1) 是否在安装系统或首次启动就出现 2) 升级 BIOS 后是否消失 3) 同型号机器同版本 BIOS 是否复现" },

{ "0x000000BE", "ATTEMPTED_WRITE_TO_READONLY_MEMORY", "驱动试图写入只读内存", C_OS, "Windows 7/8/10/11",
  "1) 驱动写只读页 2) 驱动版本与系统不匹配 3) 安全软件 hook 冲突",
  "1) 卸载最近驱动/安全软件 2) 用官方签名驱动替换 3) sfc /scannow",
  "有 .sys 名字就精确了",
  "无", "现场可修", "MS", "Bug Check 0xBE",
  "0xbe attempted_write_to_readonly_memory zhixie neicun", "BSOD", "Microsoft",
  "1) 蓝屏参数第 3 个值是否为只读地址 2) WinDbg 看该地址属于哪个模块 3) 卸载该模块对应软件后复测" },

{ "0x000000C2", "BAD_POOL_CALLER", "驱动执行了错误的池内存分配/释放操作", C_OS, "Windows 7/8/10/11",
  "1) 驱动重复释放或错误释放池 2) 驱动在错误 IRQL 分配内存 3) 内存故障",
  "1) 卸载最近驱动 2) 内存测试 3) 用驱动验证器定位",
  "典型的驱动 bug, 通常伴随具体驱动名",
  "无", "现场可修", "MS", "Bug Check 0xC2",
  "0xc2 bad_pool_caller chicheng xuqiu", "BSOD", "Microsoft",
  "1) 参数 1 表示违规类型(7 为已释放后再次释放) 2) 蓝屏所列 .sys 即元凶 3) verifier 只开可疑驱动复现" },

{ "0x000000C4", "DRIVER_VERIFIER_DETECTED_VIOLATION", "驱动验证器发现驱动违规", C_OS, "Windows 7/8/10/11",
  "1) 开启了驱动验证器且驱动确有缺陷 2) 驱动未按规范处理 IRQL/内存 3) 测试环境遗留 verifier 配置",
  "1) 若并非主动排查: verifier /reset 关闭后重启 2) 若在排查: 按验证器日志修/换该驱动 3) 记录违规子码",
  "先确认是不是自己开的验证器忘了关",
  "中: 关闭验证器需重启两次", "现场可修", "MS", "Bug Check 0xC4",
  "0xc4 driver_verifier_detected_violation yanzhengqi", "BSOD", "Microsoft",
  "1) verifier /query 看当前验证的驱动 2) 参数 1 为违规子码, 可精确定位规则 3) verifier /reset 后是否恢复" },

{ "0x000000C5", "DRIVER_CORRUPTED_EXPOOL", "驱动在过高 IRQL 破坏了池内存", C_OS, "Windows 7/8/10/11",
  "1) 驱动越界写池 2) 驱动在 DISPATCH_LEVEL 访问分页内存 3) 内存故障",
  "1) 卸载最近驱动 2) 内存测试 3) 驱动验证器定位",
  "指向驱动写坏内存, 与 19/C2 同族",
  "无", "现场可修", "MS", "Bug Check 0xC5",
  "0xc5 driver_corrupted_expool chisunhuai", "BSOD", "Microsoft",
  "1) 蓝屏给出的地址附近模块即嫌疑 2) 安全模式稳定 ⇒ 第三方驱动 3) 干净启动逐项恢复定位" },

{ "0x000000CE", "DRIVER_UNLOADED_WITHOUT_CANCELLING_PENDING_OPERATIONS", "驱动卸载时未取消挂起操作", C_OS, "Windows 7/8/10/11",
  "1) 驱动卸载流程缺陷 2) 拔设备时驱动未清理 3) 驱动与系统版本不匹配",
  "1) 更新该设备驱动到最新 2) 卸载设备后重启再重装 3) 避免使用未签名老驱动",
  "多见于老外设与虚拟网卡",
  "无", "现场可修", "MS", "Bug Check 0xCE",
  "0xce driver_unloaded_without_cancelling_pending_operations xiezai", "BSOD", "Microsoft",
  "1) 是否在拔插设备/卸载软件后立刻蓝屏 2) WinDbg 看卸载中的模块名 3) 换新版驱动后复测" },

{ "0x000000D1", "DRIVER_IRQL_NOT_LESS_OR_EQUAL", "驱动在过高 IRQL 访问分页内存, 现场最典型的驱动蓝屏", C_OS, "Windows 7/8/10/11",
  "1) 第三方驱动缺陷(网卡/无线/杀软/虚拟网卡) 2) 驱动与系统版本不匹配 3) 内存不稳",
  "1) 记下蓝屏 .sys 名字 2) 安全模式卸载或回滚该驱动 3) 到厂商官网换对应系统版本的驱动",
  "这条码有名字就有答案, 名字就是嫌疑人",
  "无", "现场可修", "MS", "Bug Check 0xD1",
  "0xd1 driver_irql_not_less_or_equal wangka qudong", "BSOD", "Microsoft",
  "1) 事件日志 1001 BugCheck 记录里是否总是同一个驱动 2) BlueScreenView 看红色标记的驱动 3) 卸载该驱动后连续运行 24 小时无复现即可确认" },

{ "0x000000DA", "SYSTEM_PTE_MISUSE", "系统页表项被误用", C_OS, "Windows 7/8/10/11",
  "1) 驱动错误操作页表 2) 内存映射冲突 3) 驱动缺陷",
  "1) 卸载最近驱动 2) sfc /scannow 3) 驱动验证器定位",
  "少见, 依旧按驱动问题处理",
  "无", "现场可修", "MS", "Bug Check 0xDA",
  "0xda system_pte_misuse yetibiao", "BSOD", "Microsoft",
  "1) 蓝屏参数 1 为误用类型 2) 是否与特定外设/软件相关 3) 安全模式下是否消失" },

{ "0x000000EA", "THREAD_STUCK_IN_DEVICE_DRIVER", "线程卡死在设备驱动中, 绝大多数是显卡驱动", C_DSP, "Windows 7/8/10/11",
  "1) 显卡驱动死循环 2) 显卡过热/供电不足 3) 显卡硬件故障",
  "1) 用 DDU 清理显卡驱动后装官方稳定版 2) 关闭超频, 检查温度与供电 3) 换显卡/用集显验证",
  "先当显卡处理, 别先怀疑系统",
  "无", "现场可修", "MS", "Bug Check 0xEA",
  "0xea thread_stuck_in_device_driver xianka sika", "BSOD", "Microsoft",
  "1) 是否在 3D/视频场景出现 2) 事件日志 TDR 记录(显示驱动超时) 3) 用集显运行同一场景不复现 ⇒ 独显/驱动" },

{ "0x000000ED", "UNMOUNTABLE_BOOT_VOLUME", "无法挂载启动卷 (蓝屏 ED)", C_DSK, "全系",
  "1) 文件系统损坏 2) 硬盘坏道 3) 数据线/接口接触不良 4) 非正常关机",
  "1) 用 PE 执行 chkdsk /f /r 2) 检查硬盘 SMART 3) 换数据线/重插 4) 坏道多则备份换盘",
  "能修就先修, 修完立刻备份并观察是否复发",
  "高: 坏道扩散会丢数据, 优先备份", "需备件", "KB", "SSD 出现此码多为掉电或固件异常",
  "0xed unmountable boot volume unmountable", "BSOD", "Microsoft",
  "1) 进 PE 看分区能否挂载 2) chkdsk /f 是否修复 3) SMART 是否报坏道" },

{ "0x000000EF", "CRITICAL_PROCESS_DIED", "关键系统进程已终止, 系统无法继续运行", C_OS, "Windows 7/8/10/11",
  "1) 关键进程(如 csrss/wininit)被强杀或崩溃 2) 系统文件损坏 3) 存储 I/O 卡死导致进程无法退出",
  "1) 记下蓝屏是否显示进程名 2) 安全模式下 sfc /scannow 与 DISM /RestoreHealth 3) 检查磁盘与内存",
  "先当系统文件损坏处理, 其次查盘",
  "中: 可能丢未保存数据", "现场可修", "MS", "Bug Check 0xEF",
  "0xef critical_process_died guanjian jincheng", "BSOD", "Microsoft",
  "1) 能否进安全模式: 能 ⇒ 软件/系统文件 2) sfc 与 DISM 修复后是否复现 3) 同时出现 0x7A/0x24 ⇒ 转查磁盘与存储驱动" },

{ "0x000000F2", "HARDWARE_INTERRUPT_STORM", "硬件中断风暴, 某设备持续抢占中断", C_MB, "Windows 7/8/10/11",
  "1) 设备/驱动反复触发中断 2) 共享中断的其它设备异常 3) 主板或扩展卡故障",
  "1) 拔掉所有非必要扩展卡与外设 2) 逐个加回定位 3) 更新芯片组与相关驱动",
  "用「拔光再加回」的方法最快",
  "无", "需备件", "MS", "Bug Check 0xF2",
  "0xf2 hardware_interrupt_storm zhongduan fengbao", "BSOD", "Microsoft",
  "1) 是否总在同一设备使用中发生 2) 最小化配置(只留主板+显卡)是否稳定 3) 逐件加回复现即定位" },

{ "0x000000F4", "CRITICAL_OBJECT_TERMINATION", "关键系统进程或对象被终止", C_OS, "Windows 7/8/10/11",
  "1) 系统关键进程被强杀 2) 磁盘无法完成 I/O 导致进程卡死 3) 驱动阻止关键进程退出",
  "1) 检查磁盘(坏道会导致此码) 2) sfc /scannow 3) 卸载最近驱动/安全软件",
  "先查盘, 因为 I/O 卡死是最常见诱因",
  "高: 可能丢未保存数据", "现场可修", "MS", "Bug Check 0xF4",
  "0xf4 critical_object_termination guanjian jincheng", "BSOD", "Microsoft",
  "1) 参数 3 指向终止的对象类型(进程/线程) 2) SMART 与 chkdsk 结果 3) 换盘后不复现 ⇒ 存储" },

{ "0x000000F7", "DRIVER_OVERRAN_STACK_BUFFER", "驱动发生栈缓冲区溢出(GS 保护触发)", C_OS, "Windows 7/8/10/11",
  "1) 驱动存在栈溢出漏洞 2) 驱动版本与系统不兼容 3) 恶意代码利用",
  "1) 卸载/更新该驱动 2) 全盘杀毒 3) 更新系统补丁",
  "安全相关, 老驱动优先换掉",
  "无", "现场可修", "MS", "Bug Check 0xF7",
  "0xf7 driver_overran_stack_buffer zhanyichu", "BSOD", "Microsoft",
  "1) 蓝屏参数为栈 cookie 校验失败 2) 是否与特定驱动/软件绑定 3) 更新驱动后复测" },

{ "0x000000FC", "ATTEMPTED_EXECUTE_OF_NOEXECUTE_MEMORY", "试图执行被标记为不可执行的内存", C_OS, "Windows 7/8/10/11",
  "1) 驱动跳转到数据区执行 2) 驱动缺陷 3) 内存损坏",
  "1) 更新/回滚最近驱动 2) 内存测试 3) sfc /scannow",
  "多半是驱动 bug 或内存不稳",
  "无", "现场可修", "MS", "Bug Check 0xFC",
  "0xfc attempted_execute_of_noexecute_memory zhixing bukezhixing", "BSOD", "Microsoft",
  "1) 蓝屏参数 1 是否为 DEP 违规地址 2) 该地址属于哪个模块(WinDbg lm 查) 3) 关闭该软件后复测" },

{ "0x00000101", "CLOCK_WATCHDOG_TIMEOUT", "多处理器时钟看门狗超时, 某颗 CPU 核心无响应", C_MB, "Windows 7/8/10/11",
  "1) CPU 超频/降压不稳 2) 供电或散热问题 3) CPU/主板硬件故障",
  "1) 恢复 BIOS 默认, 关闭超频 2) 检查散热与供电 3) 逐件替换验证",
  "几乎都是硬件稳定性问题",
  "中: 可能已发生计算错误", "必须返厂", "MS", "Bug Check 0x101",
  "0x101 clock_watchdog_timeout cpu chaoshi gongdian", "BSOD", "Microsoft",
  "1) 参数 3 为超时的处理器序号, 是否固定同一核 2) 关超频/换散热后是否消失 3) 换 CPU 或主板交叉验证" },

{ "0x00000109", "CRITICAL_STRUCTURE_CORRUPTION", "内核关键结构被破坏, 可能是驱动篡改或恶意代码", C_OS, "Windows 7/8/10/11",
  "1) 驱动修改内核关键结构(常见于老杀软/外挂防护) 2) 恶意软件 3) 内存故障",
  "1) 全盘杀毒 2) 卸载驱动级安全/加速软件 3) 内存测试 + sfc /scannow",
  "这条码要当安全事件对待",
  "无", "现场可修", "MS", "Bug Check 0x109",
  "0x109 critical_structure_corruption neihie jiegou cuangai", "BSOD", "Microsoft",
  "1) 参数 4 指出被改的结构类型 2) 是否装了内核级防护/加速/外挂类软件 3) 干净启动+杀毒后是否复现" },

{ "0x0000010E", "VIDEO_MEMORY_MANAGEMENT_INTERNAL", "视频内存管理内部错误, 与显卡驱动相关", C_DSP, "Windows 7/8/10/11",
  "1) 显卡驱动缺陷 2) 显存不足或损坏 3) 多屏/混合显卡切换冲突",
  "1) DDU 清理后装官方驱动 2) 关闭硬件加速的软件 3) 减少多屏/降分辨率验证",
  "显卡驱动优先, 其次显存",
  "无", "现场可修", "MS", "Bug Check 0x10E",
  "0x10e video_memory_management_internal xiancun xianka", "BSOD", "Microsoft",
  "1) 是否在游戏/视频/多屏场景出现 2) 换驱动版本(升或降一个版本)是否消失 3) 用集显复测排除独显硬件" },

{ "0x00000116", "VIDEO_TDR_FAILURE", "显卡驱动超时(TDR)后未能恢复", C_DSP, "Windows 7/8/10/11",
  "1) 显卡驱动崩溃且恢复失败 2) 显卡过热/供电不足 3) 显存或 GPU 硬件故障",
  "1) DDU 清理后装官方稳定版驱动 2) 检查温度与供电, 关闭超频 3) 换显卡验证",
  "TDR 家族, 驱动与散热同时查",
  "无", "现场可修", "MS", "Bug Check 0x116",
  "0x116 video_tdr_failure xianka chaoshi tdr", "BSOD", "Microsoft",
  "1) 事件日志 Display 4101(TDR 恢复)是否频繁 2) 蓝屏参数 2 为 TDR 相关模块 3) 换到集显输出是否消失" },

{ "0x00000117", "VIDEO_TDR_TIMEOUT_DETECTED", "检测到显卡驱动超时(TDR)", C_DSP, "Windows 7/8/10/11",
  "1) 显卡驱动响应超时 2) GPU 负载或温度异常 3) 驱动/硬件缺陷",
  "1) 更新或回滚显卡驱动 2) 检查散热与供电 3) 关闭超频与硬件加速",
  "与 116 同族, 116 是恢复失败, 117 是检测到超时",
  "无", "现场可修", "MS", "Bug Check 0x117",
  "0x117 video_tdr_timeout_detected xianka chaoshi", "BSOD", "Microsoft",
  "1) 是否只在重负载下出现 2) 温度曲线是否过高 3) 降负载后是否消失" },

{ "0x00000119", "VIDEO_SCHEDULER_INTERNAL_ERROR", "视频调度器内部错误", C_DSP, "Windows 7/8/10/11",
  "1) 显卡驱动与系统显示栈不匹配 2) 多显卡驱动冲突 3) 驱动缺陷",
  "1) 清理旧驱动后重装官方版 2) 只保留一套显卡驱动 3) 更新系统补丁",
  "同一台机器别装两套显卡驱动",
  "无", "现场可修", "MS", "Bug Check 0x119",
  "0x119 video_scheduler_internal_error shipin diaodu", "BSOD", "Microsoft",
  "1) 是否装了多版本/多品牌显卡驱动 2) 清理后是否消失 3) 事件日志显示相关错误" },

{ "0x00000120", "BITLOCKER_FATAL_ERROR", "BitLocker 致命错误", C_SEC, "Windows 7/8/10/11",
  "1) TPM 通信失败 2) BitLocker 元数据损坏 3) 固件/安全芯片策略变更",
  "1) 先备份恢复密钥(重要) 2) 暂停 BitLocker 后检查 TPM 状态 3) 更新 BIOS/TPM 固件",
  "先拿到恢复密钥再动手, 否则可能锁死数据",
  "高: 操作不当会导致数据无法解密", "现场可修", "MS", "Bug Check 0x120",
  "0x120 bitlocker fatal error jiami tpm", "BSOD", "Microsoft",
  "1) manage-bde -status 查看保护状态 2) TPM.msc 看 TPM 是否就绪 3) 暂停加密后是否仍复现" },

{ "0x00000124", "WHEA_UNCORRECTABLE_ERROR", "硬件报告不可纠正错误(CPU/内存/总线/供电)", C_MB, "Windows 7/8/10/11",
  "1) CPU/内存/主板硬件故障 2) 供电不稳或超频 3) 散热不良 4) PCIe 设备异常",
  "1) 恢复 BIOS 默认, 关超频与 XMP 2) 逐件替换(内存→电源→主板) 3) 检查温度与插槽接触",
  "纯硬件码, 软件层只能排除超频与驱动",
  "中: 可能已产生数据错误", "必须返厂", "MS", "Bug Check 0x124",
  "0x124 whea_uncorrectable_error yingjian gongdian cpu", "BSOD", "Microsoft",
  "1) 事件日志 WHEA-Logger 事件 46/47 会标出 PCIe/内存/CPU 具体单元 2) 单条内存+集显最小配置复测 3) 换电源后是否消失(供电不稳极常见)" },

{ "0x00000133", "DPC_WATCHDOG_VIOLATION", "DPC 或中断运行时间过长, 看门狗触发", C_OS, "Windows 7/8/10/11",
  "1) 存储/网卡驱动 DPC 超时 2) SSD 固件缺陷 3) 驱动与系统不匹配",
  "1) 更新 SSD 固件与存储驱动 2) 更新网卡/无线驱动 3) 关闭快速启动",
  "SSD 固件与存储驱动是首要嫌疑",
  "无", "现场可修", "MS", "Bug Check 0x133",
  "0x133 dpc_watchdog_violation ssd cunchu qudong", "BSOD", "Microsoft",
  "1) 参数 1 为 0 表示单次 DPC 超时, 1 表示累计超时 2) 是否在大量磁盘 I/O 时出现 3) 更新 SSD 固件后复测" },

{ "0x00000139", "KERNEL_SECURITY_CHECK_FAILURE", "内核检测到关键数据结构被破坏", C_OS, "Windows 7/8/10/11",
  "1) 驱动越界写内核结构 2) 内存故障 3) 恶意软件篡改",
  "1) 卸载最近驱动/安全软件 2) 内存测试 3) sfc /scannow + 杀毒",
  "与 109 类似, 都指向内核结构完整性",
  "无", "现场可修", "MS", "Bug Check 0x139",
  "0x139 kernel_security_check_failure neihie jiegou", "BSOD", "Microsoft",
  "1) 参数 2 指出被破坏的结构 2) 是否伴随 0x3B/0x1E 反复出现 ⇒ 同一驱动 3) 干净启动后不复现 ⇒ 第三方驱动" },

{ "0x0000013A", "KERNEL_MODE_HEAP_CORRUPTION", "内核模式堆损坏", C_OS, "Windows 7/8/10/11",
  "1) 驱动堆操作错误 2) 内存条故障 3) 驱动版本不兼容",
  "1) 更新/卸载最近驱动 2) 内存测试 3) 驱动验证器定位",
  "仍是驱动写坏内存的路数",
  "无", "现场可修", "MS", "Bug Check 0x13A",
  "0x13a kernel_mode_heap_corruption dui sunhuai", "BSOD", "Microsoft",
  "1) 蓝屏参数 1 为堆损坏子类型 2) 是否在特定操作后出现 3) verifier 针对可疑驱动复现" },

{ "0x00000154", "UNEXPECTED_STORE_EXCEPTION", "内存压缩存储子系统发生意外异常", C_RAM, "Windows 7/8/10/11",
  "1) 内存条故障 2) 存储驱动/SSD 异常 3) 内存压缩功能与驱动冲突",
  "1) 内存测试 2) 更新 SSD 固件与存储驱动 3) 关闭内存压缩(Disable-MMAgent -mc)验证",
  "先测内存, 再查 SSD",
  "中: 可能伴随数据损坏", "需备件", "MS", "Bug Check 0x154",
  "0x154 unexpected_store_exception neicun yasuo", "BSOD", "Microsoft",
  "1) 是否与大批量文件读写同时出现 2) 关闭内存压缩后是否消失 3) SMART 与内存测试结果" },

{ "0x000001CA", "SYNTHETIC_WATCHDOG_TIMEOUT - A system wide watchdog has expired. This indicates that the system is hung and not processing timer ticks", "看门狗超时: 系统整体挂死, 不再处理时钟节拍 (timer tick)", C_MB, "Windows 10/11 (虚拟机/笔记本低功耗场景多见)",
  "1) 系统被长时间挂起后恢复 (虚拟机快照/暂停/迁移, 笔记本深度睡眠) 2) 存储或芯片组驱动响应超时 3) CPU 深度节能状态 (C-State) 与固件不兼容 4) 硬件长时间无响应: 盘掉线、PCIe 设备挂死",
  "1) 回忆蓝屏前是否做过挂起/恢复/快照/迁移, 先排除这条 2) 事件查看器 → Windows 日志 → 系统, 查 Kernel-Power 41 与 WHEA-Logger 3) @powercfg /energy 4) 更新芯片组与存储驱动, BIOS 里关掉激进的节能/深度睡眠后复测 5) 虚拟机换宿主机、或关闭快照对比是否复现",
  "先怀疑「系统被挂起过」, 再查驱动与节能设置; 别一上来就判主板坏",
  "中: 蓝屏瞬间未落盘的写入可能丢", "现场可修", "MS", "与联想 POST 码 01CA 不是同一个码: 带 0x 的 0x000001CA 是蓝屏, 01CA 是固件自检",
  "0x1ca 1ca synthetic watchdog timeout kanguan", "BSOD", "Microsoft",
  "1) 复现前后 Kernel-Power 41 / WHEA-Logger 事件是否消失 2) 连续「挂起-恢复」5 次不再蓝屏 3) 关掉节能仍蓝屏 ⇒ 按驱动或硬件方向继续查, 不能再归因于挂起" },

{ "0xC000021A", "STATUS_SYSTEM_PROCESS_TERMINATED", "关键用户态进程(winlogon/csrss)终止, 系统无法继续", C_OS, "Windows 7/8/10/11",
  "1) 系统文件被替换/损坏 2) 第三方软件篡改系统组件 3) 更新安装失败",
  "1) 启动修复 / 系统还原 2) sfc /scannow 与 DISM /RestoreHealth 3) 卸载最近更新或软件",
  "多半是系统文件被破坏, 不是硬件",
  "中: 可能无法正常进系统", "现场可修", "MS", "Bug Check 0xC000021A",
  "0xc000021a status_system_process_terminated winlogon csrss", "BSOD", "Microsoft",
  "1) 能否进安全模式: 能 ⇒ 软件/更新导致 2) WinRE 下 sfc /scannow /offbootdir 3) 卸载最近补丁后是否恢复" },

-- ========== DEVMGR · 设备管理器代码 (Microsoft CM_PROB_* 官方错误消息) ==========

{ "Code 1", "CM_PROB_NOT_CONFIGURED - This device is not configured correctly. (Code 1)", "该设备的驱动程序未安装或未正确配置 (代码 1)", C_DEV, "Windows 7/8/10/11",
  "1) 设备已被识别, 但系统找不到匹配的驱动程序 2) 驱动包不含该硬件 ID 3) 安装被组策略/安全软件拦截",
  "1) 设备管理器右键设备 → 更新驱动程序 → 自动搜索 2) 无效则去厂商官网按机型下载驱动, 手动指定 INF 3) 仍失败: 属性「详细信息」选硬件 ID, 记录 VEN_/DEV_ 后按 ID 查型号",
  "先补驱动, 99% 是缺驱动; 补完仍不行再怀疑硬件",
  "无", "现场可修", "MS", "CM_PROB_NOT_CONFIGURED",
  "code1 cm_prob_not_configured driver missing qudong weianzhuang", "DEVMGR", "Microsoft",
  "1) 装完后看黄色感叹号是否消失、设备状态是否显示「这个设备运转正常」 2) 拔插设备看是否重新识别 3) 仍未识别用硬件 ID 反查, 确认不是插错槽/型号不符" },

{ "Code 3", "CM_PROB_OUT_OF_MEMORY - The driver for this device might be corrupted, or your system may be running low on memory or other resources. (Code 3)", "驱动可能已损坏, 或系统内存/资源不足 (代码 3)", C_DEV, "Windows 7/8/10/11",
  "1) 驱动文件损坏 2) 系统内存或 GDI/句柄资源耗尽 3) 32 位系统内核地址空间不足",
  "1) 重启后先看是否必现 2) 卸载设备并勾选删除驱动, 再重装官方驱动 3) 关掉占用内存大的程序; 32 位系统优先换 64 位",
  "先卸载重装驱动, 再看内存占用; 反复出现查内存条",
  "无", "现场可修", "MS", "CM_PROB_OUT_OF_MEMORY",
  "code3 cm_prob_out_of_memory neicun buzu qudong sunhuai", "DEVMGR", "Microsoft",
  "1) 任务管理器看提交内存与句柄数是否接近上限 2) 只留该设备驱动重装后重启, 观察 24 小时 3) 多条设备同时报 3 ⇒ 系统级资源问题(内存/重装系统), 不是单个设备" },

{ "Code 9", "CM_PROB_INVALID_DATA - Windows cannot identify this hardware because it does not have a valid hardware identification number. (Code 9)", "Windows 无法识别该硬件, 硬件 ID 无效 (代码 9)", C_DEV, "Windows 7/8/10/11",
  "1) 设备上报的硬件 ID 非法或为空 2) 固件/BIOS 枚举异常 3) 设备本身故障或为山寨/改装卡",
  "1) 完全关机断电, 重新插拔设备(台式机换 PCIe 槽) 2) 进 BIOS 恢复默认, 确认对应接口未被禁用 3) 换一台机器插同一设备, 判断是设备还是主板",
  "先换槽换机验证设备本身; 别急着装驱动",
  "无", "需备件", "MS", "CM_PROB_INVALID_DATA",
  "code9 cm_prob_invalid_data hardware id wufa shibie", "DEVMGR", "Microsoft",
  "1) 属性→详细信息→硬件 ID 是否为空或乱码 2) 同一设备插别的机器是否正常: 正常 ⇒ 本机接口/BIOS; 不正常 ⇒ 设备坏 3) 换槽后 ID 正常即接触/槽位问题" },

{ "Code 10", "CM_PROB_FAILED_START - This device cannot start. (Code 10)", "该设备无法启动 (代码 10)", C_DEV, "Windows 7/8/10/11",
  "1) 驱动加载失败或版本不匹配 2) 设备固件/硬件故障 3) 电源管理或资源冲突",
  "1) 先看「事件」页有无该设备的报错 2) 卸载设备→扫描检测硬件改动让它重装 3) 换驱动版本(回滚或升到厂商最新); 再不行换槽/换机验证硬件",
  "最常见的「万金油」码, 先驱动后硬件, 顺序别反",
  "无", "现场可修", "MS", "CM_PROB_FAILED_START",
  "code10 cm_prob_failed_start shebei wufa qidong driver", "DEVMGR", "Microsoft",
  "1) 卸载后重启, 让系统重新枚举: 能启动 ⇒ 驱动/注册表; 仍 10 ⇒ 硬件或固件 2) 同一设备换机测试 3) 笔记本内置设备报 10 且重装无效, 多为排线/供电, 转硬件检测" },

{ "Code 12", "CM_PROB_NORMAL_CONFLICT - This device cannot find enough free resources that it can use. (Code 12)", "该设备找不到足够可用的资源 (代码 12)", C_DEV, "Windows 7/8/10/11",
  "1) 中断/内存/IO 资源被其它设备占用 2) BIOS 资源分配表异常 3) 老式非即插即用设备占用固定资源",
  "1) 进 BIOS 载入默认并保存, 重启 2) 设备管理器「查看→依类型排序资源」找冲突设备 3) 关掉不用的板载设备(串口/并口/老式声卡)释放资源",
  "现代主板少见; 出现优先查 BIOS 资源与老设备",
  "无", "现场可修", "MS", "CM_PROB_NORMAL_CONFLICT",
  "code12 cm_prob_normal_conflict ziyuan chongtu irq", "DEVMGR", "Microsoft",
  "1) 资源选项卡看「冲突设备列表」是否非空 2) BIOS 关掉 Legacy/串并口后复测 3) 拔掉所有扩展卡只留该设备, 能起来 ⇒ 资源争用; 仍 12 ⇒ 主板资源表异常" },

{ "Code 14", "CM_PROB_NEED_RESTART - This device cannot work properly until you restart your computer. (Code 14)", "需要重启计算机该设备才能工作 (代码 14)", C_DEV, "Windows 7/8/10/11",
  "1) 驱动已安装但需重启才生效 2) 驱动更新中途被打断 3) 快速启动导致驱动未真正加载",
  "1) 正常重启(不是关机再开; 关掉「快速启动」) 2) 重启后仍 14: 卸载设备并删除驱动再装 3) 关闭快速启动后复测",
  "先真重启一次; 关掉快速启动是为了排除假重启",
  "无", "现场可修", "MS", "CM_PROB_NEED_RESTART",
  "code14 cm_prob_need_restart chongqi kuaishu qidong", "DEVMGR", "Microsoft",
  "1) 重启后代码是否变成正常或变成别的码 2) 用 shutdown /r /t 0 强制真重启, 排除快速启动 3) 每次重启都回到 14 ⇒ 驱动安装流程被拦截, 查事件日志" },

{ "Code 16", "CM_PROB_PARTIAL_LOG_CONF - Windows cannot identify all the resources this device uses. (Code 16)", "Windows 无法识别该设备使用的全部资源 (代码 16)", C_DEV, "Windows 7/8/10/11",
  "1) 设备资源描述不完整或损坏 2) 驱动/固件上报异常 3) 老设备兼容性",
  "1) 卸载设备并重启让其重新枚举 2) 更新主板芯片组与设备驱动 3) 老设备考虑换新型号",
  "少见码, 多为设备/固件上报缺陷",
  "无", "需备件", "MS", "CM_PROB_PARTIAL_LOG_CONF",
  "code16 cm_prob_partial_log_conf ziyuan shibie", "DEVMGR", "Microsoft",
  "1) 属性→资源 页看是否只列出部分资源 2) 换机测试同设备是否也 16 3) 同设备在别的机器正常 ⇒ 本机 BIOS/芯片组驱动" },

{ "Code 18", "CM_PROB_REINSTALL - Reinstall the drivers for this device. (Code 18)", "请重新安装该设备的驱动程序 (代码 18)", C_DEV, "Windows 7/8/10/11",
  "1) 驱动文件缺失或被清理工具删除 2) 驱动安装记录损坏 3) 系统还原/更新后驱动注册失效",
  "1) 卸载设备并勾选「删除此设备的驱动程序软件」 2) 重启后重装厂商官方驱动 3) 无效则用系统还原点回到正常时点",
  "直接重装驱动即可, 别在注册表里折腾",
  "无", "现场可修", "MS", "CM_PROB_REINSTALL",
  "code18 cm_prob_reinstall chongzhuang qudong", "DEVMGR", "Microsoft",
  "1) 重装后设备状态是否恢复正常 2) 设备管理器「查看→显示隐藏的设备」确认没有残留同名条目 3) 反复 18 ⇒ 驱动包本身不全, 换完整版驱动包" },

{ "Code 19", "CM_PROB_REGISTRY - Windows cannot start this hardware device because its configuration information (in the registry) is incomplete or damaged. (Code 19)", "注册表中的设备配置信息不完整或已损坏 (代码 19)", C_DEV, "Windows 7/8/10/11",
  "1) 注册表设备键损坏 2) 异常断电/蓝屏导致注册表写入中断 3) 优化软件误删注册表项",
  "1) 卸载设备并重启 2) 无效: 用系统还原或「最后一次正确配置」 3) 仍无效: 从同版本系统导出该设备注册表项对照修复, 或重装系统",
  "先卸载重启, 再系统还原; 手改注册表放最后",
  "中: 改注册表前先导出备份", "现场可修", "MS", "CM_PROB_REGISTRY",
  "code19 cm_prob_registry zhucebiao sunhuai", "DEVMGR", "Microsoft",
  "1) 卸载重启后是否重建成功 2) 事件查看器看系统日志有无注册表损坏记录 3) 多个设备同时 19 ⇒ 整机注册表损坏, 直接考虑还原/重装" },

{ "Code 21", "CM_PROB_WILL_BE_REMOVED - Windows is removing this device. (Code 21)", "Windows 正在删除该设备 (代码 21)", C_DEV, "Windows 7/8/10/11",
  "1) 设备正在被卸载, 属中间状态 2) 卸载流程卡住 3) 热插拔设备被拔出时残留",
  "1) 等 10 秒后刷新设备管理器 2) 仍停留: 重启 3) 重启后条目仍在 ⇒ 按对应真实错误码处理",
  "多数是瞬时状态, 不用处理",
  "无", "现场可修", "MS", "CM_PROB_WILL_BE_REMOVED",
  "code21 cm_prob_will_be_removed shanchu zhong", "DEVMGR", "Microsoft",
  "1) 刷新或重启后条目是否消失 2) 若变成 22/28 等其它码, 按新码处理 3) 反复出现 21 且设备不可用 ⇒ 卸载流程被驱动阻塞, 安全模式下卸载" },

{ "Code 22", "CM_PROB_DISABLED - This device is disabled. (Code 22)", "该设备已被禁用 (代码 22)", C_DEV, "Windows 7/8/10/11",
  "1) 被人为禁用 2) 厂商工具/省电策略禁用 3) 部署脚本误操作",
  "1) 右键设备 → 启用设备 2) 检查 BIOS 中对应接口是否 Enabled 3) 检查厂商电源管理工具是否禁用该设备",
  "最常见是人为禁用, 一步就能恢复",
  "无", "现场可修", "MS", "CM_PROB_DISABLED",
  "code22 cm_prob_disabled jinyong", "DEVMGR", "Microsoft",
  "1) 启用后设备是否可用 2) 重启后是否又变 22 ⇒ 有策略/工具在禁用, 查组策略与厂商软件 3) BIOS 里对应设备是否为 Disabled" },

{ "Code 24", "CM_PROB_DEVICE_NOT_THERE - This device is not present, is not working properly, or does not have all its drivers installed. (Code 24)", "设备不存在、工作不正常或驱动未装全 (代码 24)", C_DEV, "Windows 7/8/10/11",
  "1) 设备已拔出但条目残留 2) 设备接触不良 3) 驱动装了一半",
  "1) 设备管理器「查看→显示隐藏的设备」, 卸载残留条目 2) 重新插拔设备并扫描硬件改动 3) 装全驱动",
  "先分清是「设备不在」还是「驱动没装全」",
  "无", "现场可修", "MS", "CM_PROB_DEVICE_NOT_THERE",
  "code24 cm_prob_device_not_there shebei bu cunzai", "DEVMGR", "Microsoft",
  "1) 显示隐藏设备后该条目是否变灰(说明设备确实不在) 2) 插上设备后码是否消失 3) 插上也 24 ⇒ 接口/排线/供电问题" },

{ "Code 28", "CM_PROB_FAILED_INSTALL - The drivers for this device are not installed. (Code 28)", "未安装该设备的驱动程序 (代码 28)", C_DEV, "Windows 7/8/10/11",
  "1) 系统没有内置该设备驱动 2) 重装系统后未装驱动 3) 驱动包与该系统版本不匹配",
  "1) 联网让 Windows 更新自动装 2) 按机型到厂商官网下载对应系统版本驱动 3) 无官方驱动时查芯片型号找通用驱动",
  "出现率最高的码之一, 就是缺驱动",
  "无", "现场可修", "MS", "CM_PROB_FAILED_INSTALL",
  "code28 cm_prob_failed_install qudong weianzhuang driver", "DEVMGR", "Microsoft",
  "1) 装完看感叹号是否消失 2) 设备状态是否显示正常 3) 装完变 Code 10 ⇒ 驱动版本不对, 换版本而不是继续重装" },

{ "Code 29", "CM_PROB_HARDWARE_DISABLED - This device is disabled because the firmware of the device did not give it the required resources. (Code 29)", "设备固件未提供所需资源, 设备被禁用 (代码 29)", C_DEV, "Windows 7/8/10/11",
  "1) 设备固件未正确上报资源需求 2) BIOS 中该接口资源未分配 3) 固件版本过旧",
  "1) 进 BIOS 载入默认, 确认接口 Enabled 2) 升级主板 BIOS 与设备固件 3) 换槽/换机验证",
  "偏固件层, 先升 BIOS 再谈换件",
  "无", "需备件", "MS", "CM_PROB_HARDWARE_DISABLED",
  "code29 cm_prob_hardware_disabled gujian ziyuan", "DEVMGR", "Microsoft",
  "1) 换槽后是否恢复 2) 同设备换机是否 29 3) 升级 BIOS 后复测, 仍 29 多为设备固件损坏" },

{ "Code 31", "CM_PROB_FAILED_ADD - This device is not working properly because Windows cannot load the drivers required for this device. (Code 31)", "Windows 无法加载该设备所需驱动程序 (代码 31)", C_DEV, "Windows 7/8/10/11",
  "1) 驱动依赖的另一驱动/服务未启动 2) 驱动文件缺失 3) 驱动签名或兼容性问题",
  "1) 卸载设备并重启重装 2) 检查依赖服务(如 Plug and Play、相关总线驱动)是否运行 3) 换官方稳定版驱动",
  "典型是驱动依赖链断了, 不是设备坏",
  "无", "现场可修", "MS", "CM_PROB_FAILED_ADD",
  "code31 cm_prob_failed_add qudong jiazai shibai", "DEVMGR", "Microsoft",
  "1) 事件查看器筛选该设备相关错误, 看是哪个 .sys 加载失败 2) 重装后是否恢复 3) 安全模式下正常、正常模式 31 ⇒ 第三方软件拦截驱动加载" },

{ "Code 32", "CM_PROB_DISABLED_SERVICE - A driver (service) for this device has been disabled. (Code 32)", "该设备的驱动程序(服务)已被禁用 (代码 32)", C_DEV, "Windows 7/8/10/11",
  "1) 服务被手动禁用 2) 优化/清理工具禁用了驱动服务 3) 驱动安装未完成",
  "1) services.msc 找到对应驱动服务, 启动类型改「手动」或「自动」并启动 2) 重启 3) 无效则卸载重装驱动",
  "先看服务启动类型, 别急着重装",
  "无", "现场可修", "MS", "CM_PROB_DISABLED_SERVICE",
  "code32 cm_prob_disabled_service fuwu jinyong", "DEVMGR", "Microsoft",
  "1) 服务状态是否为「正在运行」 2) 启动后设备是否恢复 3) 重启又变禁用 ⇒ 有程序在改, 查启动项与优化软件" },

{ "Code 33", "CM_PROB_TRANSLATION_FAILED - Windows cannot determine which resources are required for this device. (Code 33)", "Windows 无法确定该设备需要哪些资源 (代码 33)", C_DEV, "Windows 7/8/10/11",
  "1) 设备资源需求描述损坏 2) BIOS 资源翻译表异常 3) 设备固件缺陷",
  "1) 卸载设备并重启重新枚举 2) 更新主板 BIOS 3) 换槽/换机验证",
  "少见, 先重新枚举",
  "无", "需备件", "MS", "CM_PROB_TRANSLATION_FAILED",
  "code33 cm_prob_translation_failed ziyuan queding", "DEVMGR", "Microsoft",
  "1) 重启后是否恢复 2) 换槽后是否复现 3) 同设备换机正常 ⇒ 本机 BIOS 资源表, 升 BIOS" },

{ "Code 34", "CM_PROB_NO_SOFTCONFIG - Windows cannot determine the settings for this device. (Code 34)", "Windows 无法确定该设备的设置 (代码 34)", C_DEV, "Windows 7/8/10/11",
  "1) 设备需手工配置资源而系统无法自动决定 2) 跳线/开关设置与驱动不符 3) 老式设备无即插即用",
  "1) 查设备手册确认跳线/DIP 设置 2) 在 BIOS 中为其预留固定资源 3) 换即插即用型号设备",
  "老设备专属码, 现代硬件基本见不到",
  "无", "需备件", "MS", "CM_PROB_NO_SOFTCONFIG",
  "code34 cm_prob_no_softconfig shezhi tiaoxian", "DEVMGR", "Microsoft",
  "1) 核对设备跳线与手册 2) 改跳线后是否识别 3) 仍 34 ⇒ 设备不支持 PnP, 建议更换" },

{ "Code 35", "CM_PROB_BIOS_TABLE - Your computer's system firmware does not include enough information to properly configure and use this device. (Code 35)", "系统固件(BIOS)信息不足, 无法正确配置该设备 (代码 35)", C_DEV, "Windows 7/8/10/11",
  "1) BIOS 版本过旧, 缺少该设备配置表 2) BIOS 设置被改 3) 设备超出主板支持范围",
  "1) 升级主板 BIOS 到最新 2) 载入 BIOS 默认设置 3) 查主板 QVL 确认设备是否受支持",
  "升级 BIOS 是主要手段",
  "中: 刷 BIOS 期间不可断电", "现场可修", "MS", "CM_PROB_BIOS_TABLE",
  "code35 cm_prob_bios_table bios gujian", "DEVMGR", "Microsoft",
  "1) 升 BIOS 后是否识别 2) BIOS 里对应接口是否 Enabled 3) 换到同型号另一台机器正常 ⇒ 本机 BIOS 版本问题" },

{ "Code 36", "CM_PROB_IRQ_TRANSLATION_FAILED - This device is requesting a PCI interrupt but is configured for an ISA interrupt (or vice versa). (Code 36)", "设备请求 PCI 中断却被配置为 ISA 中断(或相反) (代码 36)", C_DEV, "Windows 7/8/10/11",
  "1) BIOS 中断映射配置错误 2) 老式设备与 PCI 总线中断方式冲突 3) 主板 BIOS bug",
  "1) 进 BIOS 载入默认, 打开 PCI 即插即用相关选项 2) 升级主板 BIOS 3) 移除老式 ISA 设备或改用 PCI 版本",
  "现代平台几乎只在老设备上出现",
  "无", "现场可修", "MS", "CM_PROB_IRQ_TRANSLATION_FAILED",
  "code36 cm_prob_irq_translation_failed zhongduan isa pci", "DEVMGR", "Microsoft",
  "1) BIOS 默认后是否恢复 2) 移除其它扩展卡后复测 3) 仅该设备报 36 且换机正常 ⇒ 主板 BIOS 中断映射" },

{ "Code 37", "CM_PROB_FAILED_DRIVER_ENTRY - Windows cannot initialize the device driver for this hardware. (Code 37)", "Windows 无法初始化该硬件的设备驱动程序 (代码 37)", C_DEV, "Windows 7/8/10/11",
  "1) 驱动初始化例程失败 2) 驱动与系统版本不兼容 3) 驱动文件损坏或被替换",
  "1) 卸载设备并删除驱动, 重启重装官方版本 2) 确认驱动位数/系统版本匹配 3) 查事件日志定位失败的 .sys",
  "先换驱动版本, 再查文件是否被替换",
  "无", "现场可修", "MS", "CM_PROB_FAILED_DRIVER_ENTRY",
  "code37 cm_prob_failed_driver_entry chushihua shibai", "DEVMGR", "Microsoft",
  "1) 重装后是否恢复 2) 事件日志该 .sys 是否仍在报错 3) 安全模式下正常 ⇒ 第三方驱动/杀软冲突" },

{ "Code 38", "CM_PROB_DRIVER_FAILED_PRIOR_UNLOAD - Windows cannot load the device driver for this hardware because a previous instance of the device driver is still in memory. (Code 38)", "驱动上一实例仍在内存, 无法重新加载 (代码 38)", C_DEV, "Windows 7/8/10/11",
  "1) 驱动卸载不干净, 旧实例残留 2) 驱动自身有卸载缺陷 3) 热插拔时驱动未释放",
  "1) 重启计算机(必须重启, 不能只注销) 2) 仍 38: 卸载设备并删除驱动包 3) 更新到修复该缺陷的新版驱动",
  "重启就是标准解法",
  "无", "现场可修", "MS", "CM_PROB_DRIVER_FAILED_PRIOR_UNLOAD",
  "code38 cm_prob_driver_failed_prior_unload qudong canliu", "DEVMGR", "Microsoft",
  "1) 重启后是否恢复 2) 每次热插拔都 38 ⇒ 驱动缺陷, 升级版本 3) 换官方旧版驱动做对比验证" },

{ "Code 39", "CM_PROB_DRIVER_FAILED_LOAD - Windows cannot load the device driver for this hardware. The driver may be corrupted or missing. (Code 39)", "驱动可能已损坏或缺失, 无法加载 (代码 39)", C_DEV, "Windows 7/8/10/11",
  "1) 驱动文件缺失/损坏 2) 磁盘坏道导致文件读不出 3) 杀软隔离了驱动文件",
  "1) 卸载设备并删除驱动后重装 2) 跑 sfc /scannow 与 chkdsk 3) 检查杀软隔离区恢复被隔离文件",
  "文件层面问题, 先查文件在不在",
  "无", "现场可修", "MS", "CM_PROB_DRIVER_FAILED_LOAD",
  "code39 cm_prob_driver_failed_load qudong sunhuai queshi", "DEVMGR", "Microsoft",
  "1) 重装后是否恢复 2) 对应 .sys 文件是否存在于 System32\\drivers 3) sfc 报文件损坏 ⇒ 磁盘或杀软问题, 不是驱动包问题" },

{ "Code 40", "CM_PROB_DRIVER_SERVICE_KEY_INVALID - Windows cannot access this hardware because its service key information in the registry is missing or recorded incorrectly. (Code 40)", "注册表中该设备的服务键缺失或记录错误 (代码 40)", C_DEV, "Windows 7/8/10/11",
  "1) 驱动服务键被删或写坏 2) 清理工具误删 3) 驱动安装被中断",
  "1) 卸载设备并删除驱动, 重启重装 2) 无效则系统还原 3) 最后手段: 用同版本系统导出的服务键对照修复",
  "先重装驱动, 它会把服务键写回来",
  "中: 改注册表前导出备份", "现场可修", "MS", "CM_PROB_DRIVER_SERVICE_KEY_INVALID",
  "code40 cm_prob_driver_service_key_invalid fuwu jian zhucebiao", "DEVMGR", "Microsoft",
  "1) 重装后是否恢复 2) 注册表 HKLM\\SYSTEM\\CurrentControlSet\\Services 下对应项是否存在 3) 多个设备同时 40 ⇒ 注册表整体损坏, 还原/重装系统" },

{ "Code 41", "CM_PROB_LEGACY_SERVICE_NO_DEVICES - Windows successfully loaded the device driver for this hardware but cannot find the hardware device. (Code 41)", "驱动已加载但找不到对应硬件设备 (代码 41)", C_DEV, "Windows 7/8/10/11",
  "1) 设备已移除但驱动服务仍在 2) 设备未上电/未连接 3) 驱动对应的是已换掉的旧硬件",
  "1) 显示隐藏设备, 卸载残留条目 2) 确认设备实际在位(供电/线缆/插槽) 3) 不需要该设备则删除其驱动服务",
  "本质是「幽灵设备」, 清理残留即可",
  "无", "现场可修", "MS", "CM_PROB_LEGACY_SERVICE_NO_DEVICES",
  "code41 cm_prob_legacy_service_no_devices youling shebei", "DEVMGR", "Microsoft",
  "1) 显示隐藏设备后条目是否变灰 2) 卸载残留后是否消失 3) 设备实际在位仍 41 ⇒ 供电/排线/接口未识别" },

{ "Code 42", "CM_PROB_DUPLICATE_DEVICE - Windows cannot load the device driver for this hardware because there is a duplicate device already running in the system. (Code 42)", "系统中已有重复设备在运行, 无法再加载 (代码 42)", C_DEV, "Windows 7/8/10/11",
  "1) 同一设备被枚举两次 2) 设备克隆/复制导致的重复实例 3) 驱动枚举缺陷",
  "1) 重启进入安全模式, 卸载重复条目 2) 只保留物理在位的那一个 3) 更新芯片组与设备驱动",
  "先找出哪一条是「真的」",
  "无", "现场可修", "MS", "CM_PROB_DUPLICATE_DEVICE",
  "code42 cm_prob_duplicate_device chongfu shebei", "DEVMGR", "Microsoft",
  "1) 设备管理器是否出现两个同名设备 2) 拔掉设备看哪个条目消失(留下的就是幽灵) 3) 卸载幽灵后重启复测" },

{ "Code 43", "CM_PROB_FAILED_POST_START - Windows has stopped this device because it has reported problems. (Code 43)", "Windows 已停止该设备, 因为设备报告了问题 (代码 43)", C_DEV, "Windows 7/8/10/11",
  "1) 设备自身固件报告故障 2) 驱动在该设备上返回失败 3) USB 供电不足或线缆质量差",
  "1) 拔下设备换口/换线, 完全断电后重插 2) 卸载设备并删除驱动, 重启重装 3) 笔记本先拔掉所有外设只留该设备, 排除供电争用",
  "USB 设备报 43 九成是供电或线缆",
  "无", "现场可修", "MS", "CM_PROB_FAILED_POST_START",
  "code43 cm_prob_failed_post_start usb gongdian xianlan", "DEVMGR", "Microsoft",
  "1) 属性→事件 看设备报告的具体错误 2) 换到后置/独立供电口是否恢复 3) 同设备换机正常 ⇒ 本机供电/接口; 换机也 43 ⇒ 设备固件损坏" },

{ "Code 44", "CM_PROB_HALTED - An application or service has shut down this hardware device. (Code 44)", "应用程序或服务已关闭该硬件设备 (代码 44)", C_DEV, "Windows 7/8/10/11",
  "1) 某软件主动停止了设备 2) 虚拟化/沙箱/驱动级工具接管设备 3) 电源管理策略关闭设备",
  "1) 找到并退出接管该设备的软件(虚拟机、模拟器、外设工具) 2) 重启 3) 检查厂商电源管理是否设置「允许关闭此设备以省电」",
  "查是谁把它关了, 不是设备坏了",
  "无", "现场可修", "MS", "CM_PROB_HALTED",
  "code44 cm_prob_halted chengxu guanbi shebei", "DEVMGR", "Microsoft",
  "1) 退出可疑软件后码是否消失 2) 干净启动(msconfig 禁用非微软服务)复测 3) 干净启动正常 ⇒ 逐个恢复服务定位元凶" },

{ "Code 45", "CM_PROB_PHANTOM - Currently, this hardware device is not connected to the computer. (Code 45)", "该硬件设备当前未连接到计算机 (代码 45)", C_DEV, "Windows 7/8/10/11",
  "1) 设备已拔出 2) 隐藏设备残留 3) 接口/线缆断连",
  "1) 插回设备并扫描硬件改动 2) 不需要该设备则卸载条目 3) 插上仍 45 检查线缆与接口",
  "本质是「设备不在」, 不需要修",
  "无", "现场可修", "MS", "CM_PROB_PHANTOM",
  "code45 cm_prob_phantom wei lianjie", "DEVMGR", "Microsoft",
  "1) 插上设备后码是否消失 2) 设备管理器是否仍显示为隐藏设备 3) 插上无反应 ⇒ 接口/线缆/供电, 不是驱动" },

{ "Code 46", "CM_PROB_SYSTEM_SHUTDOWN - Windows cannot gain access to this hardware device because the operating system is in the process of shutting down. (Code 46)", "操作系统正在关闭, 无法访问该设备 (代码 46)", C_DEV, "Windows 7/8/10/11",
  "1) 关机过程中访问设备 2) 关机卡住 3) 驱动未响应关机通知",
  "1) 等关机完成或强制断电重启 2) 反复卡住: 查是否有程序阻止关机 3) 更新相关驱动",
  "关机瞬间的瞬时码, 无需处理",
  "无", "现场可修", "MS", "CM_PROB_SYSTEM_SHUTDOWN",
  "code46 cm_prob_system_shutdown guanji", "DEVMGR", "Microsoft",
  "1) 重启后是否消失 2) 若每次关机都出现且卡住, 查事件日志「关机被阻止」 3) 干净启动复测判断是否第三方软件" },

{ "Code 47", "CM_PROB_HELD_FOR_EJECT - Windows cannot use this hardware device because it has been prepared for safe removal, but it has not been removed from the computer. (Code 47)", "设备已准备安全删除但未拔出 (代码 47)", C_DEV, "Windows 7/8/10/11",
  "1) 点了「安全删除硬件」但没拔 2) 热插拔设备处于待拔出状态",
  "1) 直接拔出设备 2) 或重启让状态复位 3) 之后要重新使用需重新插入",
  "点过安全删除就会这样, 插拔一次即可",
  "中: 拔出前确认没有正在写入的数据", "现场可修", "MS", "CM_PROB_HELD_FOR_EJECT",
  "code47 cm_prob_held_for_eject anquan shanchu", "DEVMGR", "Microsoft",
  "1) 重新插拔后是否恢复 2) 设备是否重新出现在资源管理器 3) 反复 47 且未点过安全删除 ⇒ 查厂商热插拔工具" },

{ "Code 48", "CM_PROB_DRIVER_BLOCKED - The software for this device has been blocked from starting because it is known to have problems with Windows. (Code 48)", "该设备的软件因已知兼容问题被阻止启动 (代码 48)", C_DEV, "Windows 7/8/10/11",
  "1) 微软兼容性阻止列表命中 2) 驱动版本过旧且有已知崩溃 3) 系统升级后旧驱动不兼容",
  "1) 到厂商官网换最新驱动 2) 若已无更新, 换硬件型号 3) 不要用「禁用驱动签名」绕过",
  "是被判定为不兼容, 不是随机故障",
  "无", "需备件", "MS", "CM_PROB_DRIVER_BLOCKED",
  "code48 cm_prob_driver_blocked jianrong xing bei zuzhi", "DEVMGR", "Microsoft",
  "1) 事件日志是否记录该驱动被阻止及原因 2) 换新版驱动后是否恢复 3) 厂商已停止支持 ⇒ 只能换硬件" },

{ "Code 49", "CM_PROB_REGISTRY_TOO_LARGE - Windows cannot start new hardware devices because the system hive is too large (exceeds the Registry Size Limit). (Code 49)", "系统注册表配置单元过大, 无法启动新设备 (代码 49)", C_DEV, "Windows 7/8/10/11",
  "1) 注册表长期膨胀 2) 大量残留设备键 3) 软件反复写入注册表",
  "1) 清理隐藏/残留设备条目 2) 卸载长期不用的软件 3) 严重时重装系统(注册表无法有效压缩)",
  "注册表体积问题, 清理优于修复",
  "中: 清理注册表前先备份", "现场可修", "MS", "CM_PROB_REGISTRY_TOO_LARGE",
  "code49 cm_prob_registry_too_large zhucebiao guoda", "DEVMGR", "Microsoft",
  "1) 注册表编辑器查看 HKLM\\SYSTEM 与 HKLM\\SOFTWARE 体积 2) 清理残留设备后新设备能否启动 3) 持续增长 ⇒ 某软件在写注册表, 用进程监视器定位" },

{ "Code 50", "CM_PROB_SETPROPERTIES_FAILED - Windows cannot apply all of the properties for this device. (Code 50)", "Windows 无法应用该设备的全部属性 (代码 50)", C_DEV, "Windows 7/8/10/11",
  "1) 设备属性写入失败 2) 注册表权限异常 3) 驱动未正确实现属性处理",
  "1) 卸载设备并重启重装 2) 检查当前账户是否有管理员权限 3) 组策略是否限制设备安装",
  "少见, 多为权限或策略",
  "无", "现场可修", "MS", "CM_PROB_SETPROPERTIES_FAILED",
  "code50 cm_prob_setproperties_failed shuxing shibai", "DEVMGR", "Microsoft",
  "1) 换管理员账户复测是否恢复 2) 事件日志看属性写入失败的具体属性 3) 组策略「设备安装限制」是否启用" },

{ "Code 51", "CM_PROB_WAITING_ON_DEPENDENCY - This device is currently waiting on another device or set of devices to start. (Code 51)", "该设备正在等待其它设备启动 (代码 51)", C_DEV, "Windows 7/8/10/11",
  "1) 依赖的上级设备未启动 2) 设备树顺序问题 3) 固件枚举延迟",
  "1) 等 30 秒后刷新 2) 重启 3) 若依赖设备本身有故障, 先修那个设备",
  "先看设备树里它的父设备是否正常",
  "无", "现场可修", "MS", "CM_PROB_WAITING_ON_DEPENDENCY",
  "code51 cm_prob_waiting_on_dependency dengdai yilai", "DEVMGR", "Microsoft",
  "1) 设备管理器「依连接排序设备」看父设备是否报错 2) 父设备修好后该码是否消失 3) 重启后仍 51 ⇒ 上游设备/总线故障" },

{ "Code 52", "CM_PROB_UNSIGNED_DRIVER - Windows cannot verify the digital signature for the drivers required for this device. (Code 52)", "无法验证该设备所需驱动程序的数字签名 (代码 52)", C_DEV, "Windows 7/8/10/11",
  "1) 驱动无签名或签名已失效 2) 系统时间错误导致证书被判过期 3) 驱动被篡改",
  "1) 先校准系统时间 2) 换厂商官方签名驱动 3) 老设备无签名驱动只能换硬件或换支持的系统版本",
  "别关驱动签名强制, 现场机器不该这么干",
  "无", "需备件", "MS", "CM_PROB_UNSIGNED_DRIVER",
  "code52 cm_prob_unsigned_driver qianming shuzi", "DEVMGR", "Microsoft",
  "1) 驱动文件属性→数字签名 是否有效 2) 校准时间后是否恢复 3) 官方驱动也 52 ⇒ 系统证书库损坏, 修复系统" },

{ "Code 53", "CM_PROB_USED_BY_DEBUGGER - This device's driver is being used by the debugger. (Code 53)", "该设备的驱动程序正被调试器占用 (代码 53)", C_DEV, "Windows 7/8/10/11",
  "1) 系统启用内核调试 2) 调试器占用该设备 3) 开发机遗留调试配置",
  "1) 关闭内核调试: bcdedit /debug off 2) 重启 3) 移除调试器连接",
  "只在开过内核调试的机器上出现",
  "无", "现场可修", "MS", "CM_PROB_USED_BY_DEBUGGER",
  "code53 cm_prob_used_by_debugger tiaoshiqi", "DEVMGR", "Microsoft",
  "1) bcdedit 查看 debug 是否为 Yes 2) 关闭后重启是否恢复 3) 生产机不应开启内核调试, 顺手确认关闭" },

{ "Code 54", "CM_PROB_DEVICE_RESET - The device has been reset and is waiting to be reinitialized. (Code 54)", "设备已重置, 正在等待重新初始化 (代码 54)", C_DEV, "Windows 7/8/10/11",
  "1) 设备发生异常后被重置 2) 固件崩溃恢复中 3) 供电瞬断",
  "1) 等 10 秒或刷新 2) 仍 54 则禁用再启用设备 3) 反复出现查供电与固件版本",
  "多见于 SSD/网卡固件自恢复",
  "无", "现场可修", "MS", "CM_PROB_DEVICE_RESET",
  "code54 cm_prob_device_reset chongzhi", "DEVMGR", "Microsoft",
  "1) 禁用/启用后是否恢复 2) 设备 SMART 或固件日志是否记录复位原因 3) 频繁复位 ⇒ 供电不稳或固件缺陷, 升级固件或换件" },

{ "Code 55", "CM_PROB_CONSOLE_LOCKED - The device cannot be used because the console is locked. (Code 55)", "控制台已锁定, 该设备无法使用 (代码 55)", C_DEV, "Windows 7/8/10/11",
  "1) 系统处于锁定/受限控制台状态 2) 远程会话锁屏 3) 特殊安全策略",
  "1) 解锁控制台并重启 2) 检查是否处于远程桌面/锁定会话 3) 退出安全策略限制模式",
  "罕见码, 基本都是会话状态问题",
  "无", "现场可修", "MS", "CM_PROB_CONSOLE_LOCKED",
  "code55 cm_prob_console_locked kongzhitai suoding", "DEVMGR", "Microsoft",
  "1) 本地登录(非远程)后是否恢复 2) 重启后是否消失 3) 仍 55 ⇒ 查安全策略与第三方锁屏软件" },

{ "Code 56", "CM_PROB_NEED_CLASS_CONFIG - Windows is still setting up the class configuration for this device. (Code 56)", "Windows 仍在设置该设备的类配置 (代码 56)", C_DEV, "Windows 7/8/10/11",
  "1) 设备类配置尚未完成 2) 网卡类设备(如 WLAN)配置指令异常 3) 系统网络/设备状态损坏",
  "1) 等待 1 分钟并重启(该码常为瞬时) 2) 网卡类: 设置 → 网络 → 网络重置 3) 仍 56 则卸载设备并删除驱动后重装",
  "官方明确说明此码通常是瞬时的, 别急着换硬件",
  "无", "现场可修", "MS", "CM_PROB_NEED_CLASS_CONFIG",
  "code56 cm_prob_need_class_config leipeizhi wangka", "DEVMGR", "Microsoft",
  "1) 重启后是否自动消失 2) 设备状态是否恢复「运转正常」 3) 网卡反复 56 ⇒ 执行网络重置; 仍无效卸载驱动重装" },

{ "Code 57", "CM_PROB_GUEST_ASSIGNMENT_FAILED - The device cannot be assigned to the virtual machine. (Code 57)", "该设备无法分配给虚拟机 (代码 57)", C_DEV, "Windows 7/8/10/11",
  "1) 设备不支持直通(DDA) 2) 宿主机未开启虚拟化/IOMMU 3) 设备已被其它虚拟机占用",
  "1) 确认 CPU 虚拟化与 VT-d/IOMMU 已在 BIOS 开启 2) 关闭占用该设备的虚拟机 3) 检查设备是否在支持直通列表内",
  "只在做设备直通的虚拟化环境出现",
  "无", "现场可修", "MS", "CM_PROB_GUEST_ASSIGNMENT_FAILED",
  "code57 cm_prob_guest_assignment_failed xuniji zhitong", "DEVMGR", "Microsoft",
  "1) 宿主机设备管理器该设备是否黄叹号 2) 卸载后重新分配是否成功 3) 仍失败查 BIOS 的 VT-d/IOMMU 与设备直通兼容性" },

-- ========== SYS · 系统与应用错误码 (Microsoft Win32 / HRESULT) ==========

{ "0x80004005", "E_FAIL - Unspecified error", "未指定的错误, 万能失败码, 需结合场景定位", C_OS, "Windows 7/8/10/11",
  "1) 组件注册损坏 2) 网络/共享访问失败 3) 依赖服务未启动",
  "1) 先看操作场景(安装/共享/更新)缩小范围 2) sfc /scannow 与 DISM 修复 3) 重新注册相关组件",
  "它本身没有信息量, 必须结合「在做什么」判断",
  "无", "现场可修", "MS", "HRESULT 0x80004005",
  "0x80004005 e_fail weizhi cuowu", "SYS", "Microsoft",
  "1) 记录失败时正在执行的操作与路径 2) 事件查看器看同一时刻的详细错误 3) 换用户/换机器复现缩小范围" },

{ "0x80070002", "ERROR_FILE_NOT_FOUND - The system cannot find the file specified", "系统找不到指定的文件", C_OS, "Windows 7/8/10/11",
  "1) 依赖文件被删或路径变更 2) 更新缓存损坏 3) 环境变量指向错误",
  "1) 确认报错中提到的文件是否存在 2) 重置更新缓存(停 wuauserv 后清 SoftwareDistribution) 3) 修复安装对应组件",
  "先按报错里的路径找文件, 十有八九真没了",
  "无", "现场可修", "MS", "HRESULT 0x80070002",
  "0x80070002 file_not_found zhaobudao wenjian", "SYS", "Microsoft",
  "1) 报错路径下文件是否存在 2) 清缓存后是否恢复 3) 多组件同时报 ⇒ 系统文件损坏, 跑 sfc/DISM" },

{ "0x80070003", "ERROR_PATH_NOT_FOUND - The system cannot find the path specified", "系统找不到指定路径 (更新或安装解包失败)", C_OS, "Windows 7/8/10/11",
  "1) 更新缓存中的临时目录被清理或权限异常 2) 系统临时目录被改到了不存在的位置 3) 环境变量 TEMP/TMP 指向无效盘符",
  "1) 检查 TEMP/TMP: @echo %TEMP% 2) 改回默认值 C:\\Windows\\Temp 与 %USERPROFILE%\\AppData\\Local\\Temp 3) 手动创建缺失目录并给 SYSTEM 完全控制 4) 重置更新组件后重试",
  "多半是临时目录被清理软件搬走或删掉了, 恢复默认路径即可",
  "无: 只改环境变量与临时目录", "现场可修", "MS", "该码本身是通用路径错误, 出现在更新场景要按临时目录查",
  "0x80070003 80070003 path not found lujing linshi", "SYS", "Microsoft",
  "1) echo %TEMP% 指向已存在的可写目录 2) 更新不再在此码处中断 3) 安装程序日志中无「找不到路径」记录" },

{ "0x80070005", "E_ACCESSDENIED - Access is denied", "访问被拒绝, 权限或策略不足", C_OS, "Windows 7/8/10/11",
  "1) 当前账户权限不足或未提权 2) 文件/注册表 ACL 被改 3) 组策略或安全软件拦截",
  "1) 以管理员身份运行 2) 检查目标文件/注册表项的安全权限 3) 临时关闭安全软件复测",
  "先提权, 再看 ACL, 最后看策略",
  "无", "现场可修", "MS", "HRESULT 0x80070005",
  "0x80070005 accessdenied quanxian jujue", "SYS", "Microsoft",
  "1) 换管理员账户是否复现 2) 用 icacls 查看目标对象权限 3) 事件日志是否记录策略拒绝" },

{ "0x8007000D", "ERROR_INVALID_DATA - The data is invalid", "更新元数据损坏 (数据无效)", C_OS, "Windows 7/8/10/11",
  "1) Windows 更新元数据文件损坏 2) 更新缓存被写坏 3) 磁盘或内存不稳定导致数据落盘错误",
  "1) 重置更新组件: 停 wuauserv 与 bits 服务, 重命名 SoftwareDistribution 目录 2) @net stop wuauserv 3) @ren C:\\Windows\\SoftwareDistribution SoftwareDistribution.old 4) @net start wuauserv 5) 再执行 @DISM /Online /Cleanup-Image /RestoreHealth",
  "先重置更新组件让它重下元数据; 反复出现要查内存与磁盘",
  "无: 只清更新缓存", "现场可修", "MS", "MS 归类为「元数据损坏」, 首选 WU 重置再 CBS 修复",
  "0x8007000d 8007000d invalid data yuanshuju gengxin", "SYS", "Microsoft",
  "1) 重新检查更新能下载到元数据 2) 目标更新安装完成 3) 若隔几天又报同一码 ⇒ 跑内存检测与 chkdsk" },

{ "0x8007000E", "E_OUTOFMEMORY - Not enough memory resources are available", "内存资源不足", C_OS, "Windows 7/8/10/11",
  "1) 物理内存或提交内存耗尽 2) 32 位进程地址空间不足 3) 内存泄漏",
  "1) 关闭占内存进程后重试 2) 增大页面文件 3) 32 位程序换 64 位版本",
  "先看提交内存, 不要一味加内存条",
  "无", "现场可修", "MS", "HRESULT 0x8007000E",
  "0x8007000e outofmemory neicun buzu", "SYS", "Microsoft",
  "1) 任务管理器看提交内存/句柄是否接近上限 2) 重启后是否立刻复现 3) 持续增长 ⇒ 内存泄漏, 定位进程" },

{ "0x80070017", "ERROR_CRC - Data error (cyclic redundancy check)", "数据错误 (循环冗余检查), 读取的数据校验不通过", C_OS, "Windows 7/8/10/11",
  "1) 光驱/安装介质物理损坏 2) 硬盘坏道 3) 数据线接触不良或 U 盘主控异常",
  "1) 换介质或换 U 盘重试 2) @chkdsk C: /f /r3) 换数据线/换接口复测 4) 同一位置反复 CRC ⇒ 盘体问题",
  "CRC 是校验失败, 说明数据真的没读对, 不要当成软件 bug",
  "高: 盘可能有坏道, 先备份", "需备件", "MS", "安装场景常见于刻录盘与劣质 U 盘",
  "0x80070017 80070017 crc shuju cuowu", "SYS", "Microsoft",
  "1) chkdsk 无坏扇区 2) 换介质后读取正常 3) 事件日志无 153/7 类磁盘错误" },

{ "0x8007001F", "ERROR_GEN_FAILURE - A device attached to the system is not functioning", "挂载的设备工作不正常 (更新/驱动场景)", C_OS, "Windows 7/8/10/11",
  "1) 某个外设或驱动在更新期间无响应 2) USB/存储设备掉线, 更新枚举设备失败 3) 驱动与当前系统版本不匹配",
  "1) 拔掉非必要外设 (U 盘/扩展坞/打印机) 后重试 2) 设备管理器看有无黄色感叹号设备 3) 更新对应驱动 (芯片组/存储/USB) 4) 仍失败看 setupact.log 定位是哪个设备",
  "这个码是「有设备不正常」的统称, 必须结合 setupact.log 才定得下来",
  "无", "现场可修", "MS", "通用设备错误码, 单独出现不能判定具体硬件",
  "0x8007001f 8007001f gen failure shebei qudong", "SYS", "Microsoft",
  "1) 设备管理器无异常设备 2) 更新/安装能越过设备枚举阶段 3) 同一外设反复触发 ⇒ 换线换口或换驱动版本复测" },

{ "0x80070020", "ERROR_SHARING_VIOLATION - The process cannot access the file because it is being used by another process", "文件被其他进程占用", C_OS, "Windows 7/8/10/11",
  "1) 杀软/备份软件正在扫描目标文件 2) 上一次安装进程没退干净 3) 索引服务或同步盘占着文件",
  "1) 关掉杀软实时防护后重试 2) 任务管理器结束残留安装进程 3) 用资源监视器 (resmon) 查「关联的句柄」定位占用者 4) 重启后立即重试",
  "用 resmon 的句柄搜索能直接点出占用进程, 比猜快",
  "无", "现场可修", "MS", "安装/更新场景最常见, 不是文件损坏",
  "0x80070020 80070020 sharing violation zanyong", "SYS", "Microsoft",
  "1) resmon 中该文件无占用句柄 2) 安装/复制操作顺利完成 3) 重启后问题不复现" },

{ "0x80070057", "E_INVALIDARG - The parameter is incorrect", "参数错误", C_OS, "Windows 7/8/10/11",
  "1) 输入参数/路径格式不合法 2) 区域与语言设置异常 3) 组件版本不匹配",
  "1) 检查输入内容与路径是否含非法字符 2) 确认系统区域设置为中文(中国) 3) 更新对应组件",
  "装机/部署场景常见, 先查区域设置",
  "无", "现场可修", "MS", "HRESULT 0x80070057",
  "0x80070057 invalidarg canshu cuowu", "SYS", "Microsoft",
  "1) 换一个简单路径/参数是否成功 2) 区域格式是否为中文(中国) 3) 事件日志的具体参数提示" },

{ "0x8007007E", "ERROR_MOD_NOT_FOUND - The specified module could not be found", "找不到指定的模块 (DLL 缺失导致安装起步失败)", C_OS, "Windows 7/8/10/11",
  "1) 安装器依赖的运行库 (VC++/.NET) 缺失 2) 系统目录中的 DLL 被删除或改名 3) 64/32 位版本装错",
  "1) 事件查看器确认是哪个 DLL/EXE 2) 补装对应运行库 (VC++ 2015-2022 x86 与 x64 都装) 3) 修系统文件: @sfc /scannow 4) 仍缺模块则用官方安装包重装该程序",
  "先确认缺哪个模块再补, 盲目重装系统代价太大",
  "无", "现场可修", "MS", "错误消息里的模块名就是线索, 别忽略",
  "0x8007007e 8007007e mod not found dll mocuai", "SYS", "Microsoft",
  "1) 对应程序能正常启动 2) 事件日志不再报该模块缺失 3) 依赖检查工具 (Dependencies) 无红色缺失项" },

{ "0x8007025D", "ERROR_BAD_COMPRESSION_BUFFER - The specified buffer contains ill-formed data", "安装时弹出「Windows 无法安装所需文件」, 底层是缓冲区/数据格式异常 (安装介质或内存问题)", C_OS, "Windows 7/8/10/11 安装与功能更新",
  "1) 安装 U 盘/ISO 损坏或制作方式不对 2) 内存不稳定, 解压 install.wim 时出错 3) 目标分区空间不足或有坏道",
  "1) 用官方 Media Creation Tool 重做 U 盘 2) 换 USB 口 (优先主板后置口) 重试 3) 跑内存检测: @mdsched.exe 4) 目标盘先 @chkdsk C: /f /r",
  "这个码八成是介质或内存, 不是硬盘容量问题",
  "中: 重装前备份数据", "现场可修", "MS", "MS 安装错误文档把该码指向介质与内存",
  "0x8007025d 8007025d bad compression buffer anzhuang jiezhi", "SYS", "Microsoft",
  "1) 换介质与内存后安装通过 2) 安装日志中没有 install.wim 解压失败 3) 同一 U 盘在别的机器上也能装 (反向确认介质)" },

{ "0x8007045D", "ERROR_IO_DEVICE - The request could not be performed because of an I/O device error", "I/O 设备错误 (磁盘读写失败)", C_OS, "Windows 7/8/10/11",
  "1) 硬盘/SSD 出现坏道或掉盘 2) SATA/NVMe 线缆、接口接触不良 3) 存储驱动异常或掉电保护触发",
  "1) 看 SMART: @wmic diskdrive get model,status2) 查线缆与接口, 换口换线复测 3) @chkdsk C: /scan4) 事件日志查 disk 类错误 (Event ID 7/11/51/153)",
  "这是磁盘层错误, 优先保数据再谈修复",
  "高: 盘可能随时掉线, 先备份", "需备件", "MS", "Event ID 153 表示存储设备请求超时, 常与掉盘同时出现",
  "0x8007045d 8007045d io device cipan diaopan", "SYS", "Microsoft",
  "1) 事件日志不再出现 disk 超时/重试 2) 大文件读写压测无报错 3) SMART 健康状态正常" },

{ "0x80070490", "ERROR_NOT_FOUND - Element not found", "找不到元素: 组件或系统文件缺失 (CBS 场景常见)", C_OS, "Windows 8/10/11",
  "1) 组件存储中缺少某个 manifest 或文件 2) 系统文件被第三方清理工具删除 3) 更新所需组件被策略移除",
  "1) 修组件存储: @DISM /Online /Cleanup-Image /RestoreHealth 2) 再跑 @sfc /scannow 3) 仍报则从同版本 ISO 挂载源修复: @DISM /Online /Cleanup-Image /RestoreHealth /Source:wim:D:\\sources\\install.wim:1 4) 检查 CBS.log 中具体缺的组件名",
  "这个码的关键是「缺什么」, 用 /Source 指到同版本镜像最有效",
  "无: 只读修复", "现场可修", "MS", "常与 0x80073712 同时出现",
  "0x80070490 80070490 not found cbs quexi", "SYS", "Microsoft",
  "1) DISM 用 /Source 后返回成功 2) 目标功能/更新安装完成 3) CBS.log 不再报同一组件缺失" },

{ "0x80070570", "ERROR_FILE_CORRUPT - The file or directory is corrupted and unreadable", "文件或目录已损坏且无法读取", C_OS, "Windows 7/8/10/11",
  "1) 磁盘坏道或 SSD 掉块 2) 安装介质 (ISO/U 盘) 损坏 3) 断电/强关导致文件系统不一致",
  "1) 先查盘: @chkdsk C: /f /r 2) 安装场景换官方介质重新制作 U 盘 (用 Media Creation Tool) 3) 修系统文件: @sfc /scannow 4) 反复损坏同一区域 ⇒ 盘要换",
  "安装时报这个码, 先怀疑 U 盘/ISO, 再怀疑硬盘",
  "高: chkdsk /f 会改文件系统, 先备份重要数据", "需备件", "MS", "SMART 有重映射/待映射扇区时直接换盘, 不要反复修",
  "0x80070570 80070570 file corrupt sunhuai cipan", "SYS", "Microsoft",
  "1) chkdsk 无坏扇区报告 2) 换介质后安装通过 3) 同一文件不再报损坏" },

{ "0x800705B4", "ERROR_TIMEOUT - This operation returned because the timeout period expired", "操作超时", C_OS, "Windows 7/8/10/11",
  "1) 磁盘响应慢 (坏道/叠瓦盘/后台扫描) 2) 服务无响应 3) 网络或远程资源超时",
  "1) 看磁盘队列与响应时间 (资源监视器) 2) 查 SMART 与事件日志 disk 错误 3) 关闭占用磁盘的扫描/备份任务后重试 4) 服务场景则重启对应服务",
  "超时本身不是根因, 顺藤摸到慢的那一环才算定位",
  "无", "现场可修", "MS", "更新场景出现时多为磁盘慢或服务卡住",
  "0x800705b4 800705b4 timeout chaoshi", "SYS", "Microsoft",
  "1) 磁盘平均响应时间恢复正常 2) 原操作在超时时间内完成 3) 无新的 disk 超时事件" },

{ "0x800706BA", "RPC_S_SERVER_UNAVAILABLE - The RPC server is unavailable", "RPC 服务器不可用 (服务没起来或被防火墙拦)", C_OS, "Windows 7/8/10/11",
  "1) RPC 相关服务未启动或被禁用 2) 防火墙/安全软件拦截 135 端口 3) 域环境 DNS 解析不到目标机器",
  "1) 确认服务状态: @sc query rpcss2) 启动服务: @net start rpcss3) 检查防火墙是否放行 RPC4) 域内确认 DNS 与网络连通: @ping 目标机",
  "先看 rpcss 服务是否在跑, 再看 135 端口通不通",
  "无", "现场可修", "MS", "常出现在远程管理、打印、WSUS 场景",
  "0x800706ba 800706ba rpc server unavailable fuwu", "SYS", "Microsoft",
  "1) sc query rpcss 显示 RUNNING 2) 远程管理/打印功能恢复 3) 事件日志无 RPC 连接失败记录" },

{ "0x800706BE", "RPC_S_CALL_FAILED - The remote procedure call failed", "远程过程调用失败 (调用链中途断开)", C_OS, "Windows 7/8/10/11",
  "1) 目标服务崩溃或重启 2) 调用超时 (默认 60 秒左右) 3) 网络抖动或防火墙中断连接",
  "1) 确认目标服务状态与最近重启时间 2) 事件日志查对应服务的崩溃记录 3) 关掉第三方防护后重试 4) 反复失败则修复该组件 (如打印组件/更新代理)",
  "和 0x800706BA 的区别: BA 是叫不到服务器, BE 是叫到了但调用失败",
  "无", "现场可修", "MS", "常见于打印后台、更新代理、WMI 调用",
  "0x800706be 800706be rpc call failed diaoyong", "SYS", "Microsoft",
  "1) 相关服务连续运行无崩溃 2) 同一操作不再返回 0x800706BE 3) 事件日志中 RPC 错误成对消失" },

{ "0x80073712", "ERROR_SXS_COMPONENT_STORE_CORRUPT - The component store is corrupted", "组件存储损坏, 更新或功能安装失败", C_OS, "Windows 8/10/11, Server 2012 及以上",
  "1) WinSxS 组件存储被损坏 (断电、强制关机、磁盘坏道) 2) 之前某次更新安装中断, 留下半成品 3) 第三方「精简/优化」工具删过组件",
  "1) 先跑组件存储修复 2) @DISM /Online /Cleanup-Image /RestoreHealth 3) @sfc /scannow 4) 仍失败看日志 C:\\Windows\\Logs\\CBS\\CBS.log 里第一条 FAIL 5) 反复损坏要查盘: @chkdsk C: /scan",
  "先 DISM 修组件存储再 sfc; 修完再做更新, 别直接重装",
  "无: 只读修复, 不改用户数据", "现场可修", "MS", "MS 更新错误码表把该码归为组件存储损坏, 首选动作是 DISM 修复",
  "0x80073712 80073712 sxs component store corrupt cbs dism gengxin", "SYS", "Microsoft",
  "1) DISM 返回「操作成功完成」 2) 重新执行原更新能装完 3) CBS.log 不再出现同一处 ERROR_SXS_COMPONENT_STORE_CORRUPT" },

{ "0x800F081F", "CBS_E_SOURCE_MISSING - The source files could not be found", "找不到源文件, 常见于 .NET 3.5 或组件安装", C_OS, "Windows 7/8/10/11",
  "1) 组件存储缺少对应源 2) 未指定安装源路径 3) 系统组件库损坏",
  "1) 挂载同版本 ISO, 用 /Source:WIM:\\sources\\install.wim:1 指定源 2) 先修组件库 DISM /RestoreHealth 3) 检查组策略是否禁用了 Windows 更新作为源",
  "这条码的关键是「给对源」, 与网络无关",
  "无", "现场可修", "MS", "CBS_E_SOURCE_MISSING",
  "0x800f081f source_missing yuan wenjian dotnet", "SYS", "Microsoft",
  "1) DISM 日志是否明确缺哪个包 2) 指定同版本源后是否成功 3) 组策略「指定 Windows 更新的位置」是否被改" },

{ "0x800F0830", "CBS_E_IMAGE_UNSERVICEABLE - The image can no longer be serviced", "系统映像已不可维护, 常规修复无效", C_OS, "Windows 10/11, Server 2016 及以上",
  "1) 组件存储严重损坏, DISM 已无法自愈 2) 系统被第三方大幅精简 3) 磁盘出现坏道导致系统文件反复损坏",
  "1) 先确认不是盘的问题: @chkdsk C: /scan 2) 试一次 @DISM /Online /Cleanup-Image /RestoreHealth 3) 报此码时 MS 建议用官方介质就地升级重装系统组件",
  "这个码意味着映像本身不可服务, 别在 DISM 上反复耗时间",
  "高: 就地升级/重装前必须备份", "需备件", "MS", "MS 列为需要就地升级 (IPU) 的错误",
  "0x800f0830 800f0830 cbs image unserviceable yingxiang", "SYS", "Microsoft",
  "1) 就地升级完成后 DISM /ScanHealth 无损坏 2) 后续更新可安装 3) 若换盘后仍出现 ⇒ 排查系统精简或镜像来源" },

{ "0x800F0831", "CBS_E_STORE_CORRUPTION - The CBS store is corrupted", "组件存储 (CBS) 损坏, 更新装不上", C_OS, "Windows 10/11, Server 2016 及以上",
  "1) 更新包下载不完整或校验失败 2) 组件存储本身已有损坏 (与 0x80073712 常同时出现) 3) 磁盘写入错误导致包体残缺",
  "1) 清空更新缓存后重新下载 2) @net stop wuauserv 3) @ren C:\\Windows\\SoftwareDistribution SoftwareDistribution.old 4) @net start wuauserv 5) 再跑 @DISM /Online /Cleanup-Image /RestoreHealth",
  "先清缓存重下, 再 DISM; 只清缓存不修存储往往下次还报",
  "无: 只删更新缓存, 不动个人文件", "现场可修", "MS", "更新缓存目录被占用时先停 wuauserv 与 bits 服务",
  "0x800f0831 800f0831 cbs store corruption gengxinbao sunhuai", "SYS", "Microsoft",
  "1) 重新下载的包能安装完成 2) CBS.log 中 CBS_E_STORE_CORRUPTION 不再出现 3) 再次检查更新不再卡在同一 KB 号" },

{ "0x800F0922", "CBS_E_INSTALLERS_FAILED - Processing advanced installers and generic commands failed", "更新安装器执行失败, 常见于系统保留分区空间不足", C_OS, "Windows 8/10/11",
  "1) 系统保留分区 (EFI/恢复分区) 剩余空间不足, 放不下新组件 2) .NET 或服务堆栈安装器执行失败 3) 安全软件拦截了安装器写系统目录",
  "1) 查各分区剩余空间, 特别是 EFI 与恢复分区 2) 临时关闭第三方杀软/防护后重试 3) 跑 @DISM /Online /Cleanup-Image /RestoreHealth 4) 功能更新反复失败时, 用官方介质做就地升级",
  "功能更新报这个码, 先看 EFI/恢复分区是不是被塞满 (常见 100MB 分区不够)",
  "中: 调整分区有风险, 先备份", "需备件", "MS", "MS 更新错误码表归类为「高级安装器失败」, 首选动作是重置更新组件",
  "0x800f0922 800f0922 cbs installers failed baoliu fenqu kongjian", "SYS", "Microsoft",
  "1) EFI/恢复分区可用空间满足更新要求 2) 重试安装能过 99% 之后的安装阶段 3) 事件日志中不再出现同一安装器的失败记录" },

{ "0x800F0982", "PSFX_E_MATCHING_COMPONENT_NOT_FOUND - Matching component not found for delta update", "增量更新找不到匹配组件", C_OS, "Windows 10 1903 及以上 / 11",
  "1) 组件存储里缺少增量更新需要的基线组件 2) 之前用第三方工具删过 WinSxS 内容 3) 长期不更新, 版本跨度过大",
  "1) 先修组件存储: @DISM /Online /Cleanup-Image /RestoreHealth 2) 再跑 @sfc /scannow 3) 仍失败则用同版本官方 ISO 做就地升级 (保留文件与应用)",
  "跨版本太久导致的增量不匹配, DISM 修不好就只能就地升级",
  "无: 就地升级保留个人文件, 但建议先备份", "现场可修", "MS", "MS 列为累积更新常见失败码",
  "0x800f0982 800f0982 psfx matching component delta zengliang", "SYS", "Microsoft",
  "1) 就地升级后版本号正确 2) 后续累积更新能正常安装 3) CBS.log 不再出现 PSFX_E_MATCHING_COMPONENT_NOT_FOUND" },

{ "0x800F0988", "PSFX_E_INVALID_DELTA_COMBINATION - Delta combination is invalid", "增量补丁组合非法 (补丁分支不匹配)", C_OS, "Windows 10 1903 及以上 / 11",
  "1) 系统装过非本通道的补丁 (测试版/第三方包) 2) 组件存储中基线版本与本次增量不匹配 3) 更新中途断电留下不一致状态",
  "1) 确认系统版本与通道: @winver 2) 卸载最近的可疑补丁: @wusa /uninstall /kb:编号 3) 修组件存储: @DISM /Online /Cleanup-Image /RestoreHealth 4) 仍失败按官方 ISO 就地升级",
  "补丁组合非法多数是装错了通道包, 先回退可疑补丁再修",
  "中: 卸载补丁可能回退安全修复", "现场可修", "MS", "与 0x800F0982 的区别: 0982 是缺组件, 0988 是组合非法",
  "0x800f0988 800f0988 psfx invalid delta combination buding", "SYS", "Microsoft",
  "1) winver 显示版本与目标补丁通道一致 2) 累积更新安装成功 3) 事件日志不再记录同一 KB 的失败" },

{ "0x8024401C", "WU_E_PT_HTTP_STATUS_REQUEST_TIMEOUT - The HTTP request timed out", "更新请求超时 (连上了但服务器/网络太慢)", C_OS, "Windows 7/8/10/11",
  "1) 网络质量差或带宽被占满 2) 代理/网关响应超时 3) WSUS 或更新服务器负载过高",
  "1) 换网络或错峰重试 2) 关掉代理直连: @netsh winhttp reset proxy3) 清理更新缓存后重试 4) 域环境确认 WSUS 地址可访问: @wuauclt /detectnow 后看 WindowsUpdate.log",
  "先归因网络, 再归因缓存; 不要按系统损坏处理",
  "无", "现场可修", "MS", "与 0x8024402C 的区别: 2C 是解析不到名字, 1C 是连上了但超时",
  "0x8024401c 8024401c wu timeout chaoshi wangluo", "SYS", "Microsoft",
  "1) 换网络后不再超时 2) 更新能下载到 100% 3) Get-WindowsUpdateLog 中无连续超时记录" },

{ "0x8024402C", "WU_E_PT_WINHTTP_NAME_NOT_RESOLVED - Proxy or server name could not be resolved", "更新服务器或代理名称解析失败", C_OS, "Windows 7/8/10/11",
  "1) DNS 解析异常, 解析不到更新服务器 2) 代理配置错误或代理不可达 3) hosts 文件被改写了微软更新域名 4) 网络策略/防火墙拦截",
  "1) 先测域名解析: @nslookup download.windowsupdate.com 2) 检查代理: @netsh winhttp show proxy 3) 清掉错误代理: @netsh winhttp reset proxy 4) 检查 hosts 是否屏蔽了 microsoft/windowsupdate 域名 5) 换网络 (手机热点) 复测, 判断是本机还是网络侧",
  "这是网络层错误, 不是系统损坏; 换热点一试就能定性",
  "无: 只改网络配置", "现场可修", "MS", "WU_E_PT_* 前缀表示「平台/传输层」错误, 与组件存储无关",
  "0x8024402c 8024402c wu proxy dns jiexi wangluo", "SYS", "Microsoft",
  "1) nslookup 能解析更新域名 2) netsh winhttp show proxy 显示「直接访问」或正确代理 3) 检查更新能连上服务器 (不再报 8024 系列)" },

{ "0xC000007B", "STATUS_INVALID_IMAGE_FORMAT - The application was unable to start correctly", "应用程序无法正常启动, 多为 32/64 位运行库混装", C_OS, "Windows 7/8/10/11",
  "1) 应用位数与系统/依赖库不匹配 2) DirectX 或 VC++ 运行库损坏 3) 依赖 DLL 被替换成错误版本",
  "1) 装齐 VC++ 运行库(x86 与 x64 都要) 2) 重装 DirectX 运行库 3) 用 Dependency Walker 查缺失/位数不符的 DLL",
  "典型是 x86/x64 混装, 两种运行库都装",
  "无", "现场可修", "MS", "STATUS_INVALID_IMAGE_FORMAT",
  "0xc000007b invalid_image_format yunxingku dll", "SYS", "Microsoft",
  "1) 该程序是 32 位还是 64 位, 对应运行库是否装全 2) 换同版本另一台机器是否正常 3) 用 Depends 查红色标记的 DLL" },

{ "0xC01A001D", "STATUS_LOG_FULL - The transaction log is full due to insufficient disk space", "磁盘空间耗尽, 事务日志写满 (安装/更新中断)", C_OS, "Windows 10/11, Server 2016 及以上",
  "1) 系统盘剩余空间不足 2) 更新缓存与日志占满分区 3) 卷影副本/还原点占用过多",
  "1) 查剩余空间: @dir C:\\ /-c2) 清更新缓存与临时文件 3) 限制还原点占用: @vssadmin list shadowstorage4) 留出至少 20GB 再重试更新",
  "先把系统盘腾到 20GB 以上, 再谈其它修复",
  "无: 删缓存不动个人文件, 但别删 Windows 目录下的未知文件", "现场可修", "MS", "MS 更新错误表归为「磁盘空间耗尽」",
  "0xc01a001d c01a001d status log full cipan kongjian", "SYS", "Microsoft",
  "1) 系统盘剩余空间大于 20GB 2) 原安装/更新能继续 3) 事件日志无 STATUS_LOG_FULL" },

-- ========== SELF · 自检与系统文字提示 (屏幕原话, 不是数字码) ==========

{ "Battery not detected", "Battery not detected / 未检测到电池", "系统检测不到电池", C_PWR, "小新 / IdeaPad / ThinkBook",
  "1) 电池排线松动 2) 电池保护板锁死 3) 主板电池接口故障 4) 长期插电导致电池深度放电",
  "1) 关机断电, 可拆机型重插电池排线 2) 长按电源放电后开机 3) 用官方工具读电池状态 4) 不可拆机型送修",
  "排线松动最常见; 保护板锁死需服务站解锁",
  "无", "需备件", "SUP", "部分机型需在 BIOS 里做电池校准",
  "battery dianchi wei jiance", "SELF", "联想",
  "1) BIOS 中是否识别电池 2) 重插电池接口或换电池 3) 只用适配器时是否正常" },

{ "BitLocker 恢复", "BitLocker Recovery - Enter recovery key", "BitLocker 恢复模式, 需输入恢复密钥", C_SEC, "全系 (预装 Win10/11 专业版机型)",
  "1) 改过 BIOS/安全启动/TPM 设置 2) 主板或硬盘被更换 3) 系统更新异常 4) 多次输错 PIN",
  "1) 登录微软账户查看该设备的恢复密钥 2) 或用单位 AD/AAD 账户查 3) 输入 48 位密钥解锁 4) 解锁后备份密钥并考虑暂停 BitLocker 再改硬件",
  "没有密钥无法解密, 不要尝试格式化以外的野路子; 数据重要先找密钥",
  "极高: 无密钥 = 数据无法恢复, 切勿清盘", "现场可修", "SUP", "预装 Office/专业版机型默认开启, 装机前建议先暂停",
  "bitlocker recovery key 48 bitlocker huifu miyao", "SELF", "通用",
  "1) 是否改过 BIOS/TPM/主板 2) 输入恢复密钥能否进入 3) 进系统后 manage-bde -status 查保护状态" },

{ "Fan error", "Fan error", "风扇错误", C_FAN, "ThinkPad T/X/P/L/E / ThinkBook",
  "1) 风扇积灰卡死或转速过低 2) 风扇轴承磨损 3) 散热鳍片被堵 4) 风扇排线接触不良",
  "1) 关机断电, 从出风口看风扇是否转 2) 清灰 (压缩空气/软刷) 3) 听是否有异响 4) 拆机重插风扇排线或换风扇",
  "先清灰; 清灰后仍报错多为风扇本体故障, 换风扇总成",
  "无", "需备件", "HMM", "开机自检时风扇转速不达标即报, 长期带故障会过热降频",
  "fan error fengshan sanre", "SELF", "通用",
  "1) 开机风扇是否转、有无异响 2) 清灰换硅脂后是否消失 3) 风扇转但仍报 ⇒ 测速线或风扇本体" },

{ "Thermal Shutdown", "Thermal Shutdown / 温度过高自动关机", "温度过高触发保护关机", C_FAN, "拯救者 Legion / 游戏本",
  "1) 风扇积灰 2) 硅脂干涸 3) 出风口被堵 4) 高性能模式下长时间满载 5) 温控传感器异常",
  "1) 清灰并检查风扇转速 2) 换硅脂 (导热硅脂/液金按机型) 3) 垫高机身保证进风 4) 用官方工具看温度曲线",
  "先清灰换硅脂; 温度正常仍关机查传感器",
  "无", "需备件", "经验", "游戏本高负载属正常温升, 关键看是否异常关机",
  "thermal shutdown guore wendu", "SELF", "通用",
  "1) 出风口温度与风扇转速 2) 清灰换硅脂 3) 仍过热 ⇒ 热管或风扇" },

{ "Thermal sensing error", "Thermal sensing error", "温度传感器错误", C_FAN, "ThinkPad T/X/P/L/E",
  "1) 温度传感器或其排线故障 2) 主板温控电路异常 3) 散热模块未压紧",
  "1) 关机断电静置后复测 2) 检查散热模块螺丝是否均匀压紧 3) 该码多为硬件级, 记录报错送检",
  "官方处置为更换系统主板; 不要靠软件关温控绕过",
  "无", "必须返厂", "HMM", "与 Fan error 区别: 这是传感器本身读不到",
  "thermal sensing wendu chuanganqi", "SELF", "通用",
  "1) 进 BIOS 看 CPU 温度是否合理 2) 摸散热器是否发烫 3) 读数异常(0/-1/255) ⇒ 温度传感器故障" },

{ "UEFI 变量存储将满", "The System UEFI Variable Storage is nearly full", "非易失 UEFI 变量存储即将用尽", C_MB, "ThinkPad 新机型 / ThinkBook",
  "1) 系统或程序在 UEFI 变量区写入过多数据 2) 反复休眠唤醒累积无效变量 3) 第三方工具写变量",
  "1) 开机报错后按 F1 进 UEFI BIOS 2) 弹窗提示清理时选 Yes 3) 重启后确认报错消失 4) 反复出现查最近装过的软件",
  "选 Yes 会清掉系统/程序写入的变量 (保留 UEFI 规范全局变量), 双系统引导项可能需重建",
  "中: 可能丢失自定义启动项, 清理前记下引导顺序", "现场可修", "X390", "官方说明: POST 所需数据在独立变量区, 不受影响",
  "uefi nvram uefi bianliang", "SELF", "联想",
  "1) 是否装过大量驱动或安全启动项 2) BIOS 中清理无效启动项后是否消失 3) 反复报满 ⇒ 需清 NVRAM" },

{ "一体机无显示", "AiO powers on, fans spin, no display", "一体机开机风扇转但无显示", C_DSP, "一体机 (IdeaCentre AIO)",
  "1) 内存接触不良 2) 屏线/背光故障 3) 主板显卡输出异常 4) 电源供电不足",
  "1) 断电长按电源 15 秒放电 2) 外接显示器验证 3) 可拆机型重插内存 4) 仍无显示送修",
  "一体机拆装难度高, 保修期内直接报修",
  "无", "必须返厂", "KB", "外接显示器有画面说明屏或屏线问题",
  "yitiji wu xianshi", "SELF", "通用",
  "1) 外接显示器测试 2) 内存重插 3) 用手电照屏看是否有暗影" },

{ "开机密码提示", "Power-on password / Supervisor password prompt", "开机密码 / 超级用户密码提示", C_SEC, "ThinkPad 全系 / 商用台式",
  "1) 设置了开机密码 (Power-on) 2) 设置了超级用户密码 (Supervisor) 3) 企业统一策略下发",
  "1) 输入正确密码 2) 忘记开机密码可用超级用户密码进入后清除 3) 两个都忘需服务站凭购机凭证处理",
  "还有一次机会时别乱试 (连错 3 次报 0199 锁死)",
  "无", "现场可修", "HMM", "企业机器联系 IT 管理员",
  "supervisor power-on mima password", "SELF", "联想",
  "1) 分清是开机密码还是 supervisor 密码 2) 输入正确密码能否进入 3) 忘记需服务站处理, 不要反复试(会锁定)" },

{ "开机直接进 BIOS", "Boots directly into BIOS Setup / 卡在 BIOS 界面", "开机直接进 BIOS 或卡在 BIOS 界面", C_MB, "小新 / IdeaPad / 拯救者 / ThinkBook",
  "1) 硬盘未被识别或被设为非启动 2) F2 键卡住 (进 BIOS 的键被压住) 3) BIOS 设置被改 4) 主板 CMOS 异常",
  "1) 检查 F2/F1/Del 键是否卡住 2) 进 BIOS 看是否识别硬盘 3) 载入默认设置 F9+F10 4) 仍不认盘查硬盘",
  "先排除按键卡住; 再查硬盘识别",
  "无", "现场可修", "KB", "轻薄本 Fn 锁定也会导致开机直入 BIOS",
  "f2 jin bios", "SELF", "通用",
  "1) 是否识别到启动盘 2) 启动顺序与 CSM 设置 3) 硬盘是否在位" },

{ "开机黑屏仅电源灯亮", "Black screen, power LED on, no POST", "开机黑屏, 只有电源灯亮", C_DSP, "全系",
  "1) 内存接触不良 2) 屏线松动 3) 主板供电异常 4) 静电积累 5) 外接显示器可验证是否屏的问题",
  "1) 长按电源 15 秒放电, 再开机 2) 接外接显示器看是否有画面 3) 有画面 = 屏/屏线问题 4) 无画面 = 内存/主板, 重插内存 (可拆机型)",
  "先放电再外接验证, 能区分屏故障与主板故障",
  "无", "需备件", "HMM", "官方 no-beep 症状表: 无鸣叫 + 灯亮 + 黑屏 = 先查连接再查主板",
  "heiping hei ping", "SELF", "通用",
  "1) 外接显示器是否出图 2) 内存重插/单条测试 3) 听风扇与硬盘是否有动作" },

{ "未知适配器", "Unknown adapter is connected. System is going to shutdown", "检测到未知适配器, 系统将关机", C_PWR, "ThinkPad 全系",
  "1) 适配器非联想原装且无识别芯片 2) 适配器识别针脚接触不良 3) 充电口/线材损坏 4) 主板充电电路故障",
  "1) 换原装适配器测试 2) 检查充电头有无异物/歪针 3) 换线材 4) 原装也报则查主板充电电路",
  "先换原装适配器; 原装仍报需检测主板供电",
  "无", "需备件", "HMM", "与「功率不足」不同, 这个直接关机",
  "unknown adapter shipeiqi", "SELF", "联想",
  "1) 是否使用非原装或转接头 2) 换原装适配器是否消失 3) 原装也报 ⇒ 充电口或主板检测电路" },

{ "硬盘密码提示", "Hard disk password prompt", "硬盘密码提示", C_SEC, "ThinkPad 全系",
  "1) 硬盘设置了 HDD 密码 2) 硬盘从别的机器拆过来 3) 企业安全策略",
  "1) 输入硬盘密码 2) 忘密码无法通过换机绕过 (密码存在盘上) 3) 服务站也无法解密, 只能换盘",
  "硬盘密码遗忘基本等于数据报废, 设置前务必记录",
  "极高: 密码丢失无法读取盘内数据", "必须返厂", "HMM", "与开机密码不同, 这个锁在硬盘上",
  "hdd password yingpan mima hdd password", "SELF", "联想",
  "1) 密码是否用户自己设过 2) 输入后能否进入 3) 忘记则数据基本无法恢复, 需专业服务" },

{ "适配器功率不足", "The connected AC adapter has a lower wattage than the recommended model", "电源适配器功率低于机型要求", C_PWR, "ThinkPad 全系 / ThinkBook / 拯救者",
  "1) 用了小功率适配器 (如 45W 带独显机型) 2) 用了非原装/劣质适配器 3) 扩展坞 PD 供电不足 4) 适配器老化",
  "1) 核对机型要求的功率 (机身底部或说明书) 2) 换原装同功率适配器 3) 走扩展坞时直连机身测试 4) 观察是否只在满载时报",
  "换足够功率的原装适配器即可; 长期欠功率会边用边掉电",
  "无", "现场可修", "HMM", "按 Esc 可继续启动, 但电池可能充不满",
  "adapter watt shipeiqi gonglv", "SELF", "联想",
  "1) 适配器铭牌功率与机型要求是否一致 2) 换原装适配器是否消失 3) 接扩展坞时出现 ⇒ 坞供电不足" },

{ "键盘无响应", "Keyboard not responding / backlight off", "键盘无响应或背光不亮", C_KBD, "全系",
  "1) 键盘排线松动 2) 键盘进液 3) 驱动异常 4) 背光被 Fn 快捷键关闭 5) 主板键盘控制器故障",
  "1) 试 Fn+空格/F11 开背光 2) 外接键盘测试 3) 设备管理器看是否有黄色感叹号 4) 进液立刻断电送修",
  "先试外接键盘区分软硬件",
  "无", "需备件", "HMM", "进液机型不要反复通电",
  "jianpan wu xiangying", "SELF", "通用",
  "1) 外接键盘是否正常 2) BIOS 中内置键盘是否可用 3) 重插排线或换键盘" },

{ "非原装电池不支持充电", "The battery installed is not supported by this system and will not charge", "当前电池不受支持, 不会充电", C_PWR, "ThinkPad 全系",
  "1) 装了非原装/兼容电池 2) 电池通信芯片异常 3) 电池排线接触不良 4) 主板电池识别电路故障",
  "1) 换回原装电池测试 2) 重插电池接口 3) 用 Lenovo 官方工具读电池信息 4) 仍报错查主板",
  "非原装电池不充电属官方限制, 换原装电池解决",
  "无", "现场可修", "HMM", "按 Esc 可继续开机, 但电池不充电",
  "battery dianchi chongdian", "SELF", "联想",
  "1) 电池是否原装(查 FRU) 2) 换原装电池是否消失 3) 原装也报 ⇒ 电池通信或主板" },

{ "非正品电池警告", "This system does not support batteries that are not genuine Lenovo-made", "使用非正品电池警告 (仍可开机)", C_PWR, "ThinkPad 全系 / IdeaPad",
  "1) 电池非联想正品 2) 电池被第三方更换过电芯 3) 电池固件被改写",
  "1) 确认电池来源 2) 换原装电池 3) 若为二手购入, 检查是否被翻新换芯",
  "官方声明非正品电池不受保修与安全责任, 建议换原装",
  "无", "现场可修", "HMM", "官方原文: 系统会继续启动, 但可能不给非授权电池充电",
  "genuine lenovo dianchi", "SELF", "联想",
  "1) BIOS 中能否读出电池序列号 2) 换原装电池复测 3) 原装也报 ⇒ 主板电池接口" },

}
-- ------------------------------- 状态 ------------------------------------
local S = {
    page = "home",          -- home | list | detail | search
    hsel = 0, lsel = 1, top = 1, htop = 1,   -- 首页游标(0=不选中)/列表游标/列表首行/框内首条
    rects = {},             -- 首页命中区 (与 sel 一一对应)
    list = {}, list_title = "", lfrom = "home",
    e = nil, dtitle = "", lines = {}, ltop = 0, dfrom = "home",
    q = "", found = {}, kb = false, kbx = 4, kby = 0, kidx = -1,
    rec = {},
    err = nil,
}

-- ------------------------------- 索引 ------------------------------------
local IDX  = {}     -- 全部条目
local LOWER = {}    -- 与 IDX 同下标的检索串 (小写)
local BYCODE = {}   -- 归一化码 → 条目
local NORM = {}     -- 与 IDX 同下标的命名空间归一化码
local LC   = {}     -- 每层条数

-- 码归一化: 带命名空间, 避免跨体系误命中
--   dev:56   ← Code 56            (设备管理器)
--   hex:1ca  ← 0x000001CA / 0x1CA (蓝屏 STOP)
--   raw:1ca  ← 01CA / 1CA         (固件自检 POST)
--   txt:...  ← 屏幕文字           (BitLocker 恢复)
-- ★ 关键: 以前不带命名空间, 0x1CA(蓝屏) 与 01CA(固件自检) 会互相覆盖。
local function norm_code(s)
    local t = string.lower(s or "")
    t = t:gsub("^[ \t\r\n]+", ""):gsub("[ \t\r\n]+$", "")
    if t:sub(1, 4) == "code" then
        t = t:sub(5):gsub("^[ \t\r\n]+", ""):gsub("^0+", "")
        if t == "" then t = "0" end
        return "dev:" .. t
    end
    if t:sub(1, 2) == "0x" then
        local u = t:sub(3)
        if u:match("^[0-9a-f]+$") then u = u:gsub("^0+", "") end
        if u == "" then u = "0" end
        return "hex:" .. u
    end
    if t:match("^[0-9a-f]+$") then
        local u = t:gsub("^0+", "")
        if u == "" then u = "0" end
        return "raw:" .. u
    end
    return "txt:" .. t
end

-- 去掉命名空间: 只用于「同数字不同体系」的次优命中 (排在精确命中之后)
local function bare_key(k)
    return (k:gsub("^%a+:", ""))
end

-- 类别拼音 (系统键盘只有 ASCII, 打不出汉字; 让 "neicun"/"nc" 也能命中)
local PY = {
    "qidongyindao qdyd yindao boot",
    "yingpancunchu ypcc yingpan cunchu ssd hdd",
    "neicun nc ram memory",
    "zhuban zb bios motherboard",
    "dianyuan dianchi dy dc battery",
    "sanrefengshan srfs fengshan sanre",
    "xianshipingmu xspm xianshi pingmu screen",
    "jianpanshuru jpsr jianpan keyboard",
    "anquanmima aqmm mima password",
    "wangluowuxian wlwx wangka wuxian wangluo wifi",
    "fengmingbaojing fmbj fengming beep beeping",
    -- v2: 设备管理器 / 系统与蓝屏
    "shebeiguanliqi sbglq devmgr device driver qudong",
    "xitonglanping xtlp bsod stop lanping xitong yindao boot",
}

local function build_index()
    IDX, LOWER, BYCODE, NORM, LC = {}, {}, {}, {}, {}
    for i = 1, #DB do
        local e = DB[i]
        local lay = e[14]
        LC[lay] = (LC[lay] or 0) + 1
        NORM[i] = norm_code(e[1])
        -- 检索串: 码 + 英文 + 检索词 + 类别拼音 + 中文类别名 + 层名/层同义词 + 厂商 + 适用
        -- ★ 不能写回 e[14]: 14=layer 15=vendor 16=verify
        LOWER[i] = string.lower(e[1] .. " " .. e[2] .. " " .. e[13] .. " "
            .. (PY[e[4]] or "") .. " " .. (CATS_CN[e[4]] or "") .. " "
            .. (LAYER_NAME[lay] or "") .. " " .. (LAYER_ALIAS[lay] or "") .. " "
            .. (e[15] or "") .. " " .. (e[5] or ""))
        IDX[i] = e
        local k = NORM[i]
        if not BYCODE[k] then BYCODE[k] = e end
        local k0 = bare_key(k)
        if not BYCODE[k0] then BYCODE[k0] = e end
        local kl = string.lower(e[1])
        if not BYCODE[kl] then BYCODE[kl] = e end
    end
end

-- ------------------------------- 文本 ------------------------------------
local function charw(step) return (step > 1) and 16 or 8 end

local function utf8_step(b)
    if b >= 0xF0 then return 4 elseif b >= 0xE0 then return 3
    elseif b >= 0xC0 then return 2 else return 1 end
end

-- 按像素宽折行 (汉字 16 / ASCII 8, 与 app.mix 一致)
-- ★ 2026-11-23: 有空格时优先在空格处断行 (英文原名/命令不再被拦腰截断成 "compon|ent")
local function wrap(s, maxw)
    local out = {}
    local i, n = 1, #s
    while i <= n do
        local w, j, lastsp = 0, i, nil
        while j <= n do
            local step = utf8_step(s:byte(j))
            local ch = s:sub(j, j + step - 1)
            local adv = charw(step)
            if w + adv > maxw then break end
            if ch == " " then lastsp = j end
            w = w + adv
            j = j + step
        end
        if j > n then
            out[#out + 1] = s:sub(i, n)
            break
        end
        local cut = j
        if lastsp and lastsp > i + 8 then cut = lastsp end   -- 行尾有空格 → 在空格断
        if cut <= i then
            cut = i + utf8_step(s:byte(i))                   -- 单字超宽: 至少吃掉一个字
        end
        out[#out + 1] = (s:sub(i, cut - 1)):gsub("[ \t\r\n]+$", "")
        i = cut
        while i <= n and s:sub(i, i) == " " do i = i + 1 end
    end
    if #out == 0 then out[1] = "" end
    return out
end

-- 单行省略 (超宽截断 + ..)
local function ell(s, maxw)
    if app.mixw(s) <= maxw then return s end
    local out, w = {}, 0
    local i, n = 1, #s
    while i <= n do
        local step = utf8_step(s:byte(i))
        local adv = charw(step)
        if w + adv > maxw - 16 then break end
        out[#out + 1] = s:sub(i, i + step - 1)
        w = w + adv
        i = i + step
    end
    return table.concat(out) .. ".."
end

-- ------------------------------- 小工具 ----------------------------------
local function clamp(v, lo, hi)
    if v < lo then return lo elseif v > hi then return hi else return v end
end

local function draw_top(title, right)
    app.fill(0, 0, W - 1, SB_H - 1, WH)
    app.mix(PAD, 4, ell(title, 250), false)
    if right and #right > 0 then
        local tw = app.mixw(right)
        app.mix(W - PAD - tw, 4, right, false)
    end
    app.hline(0, W - 1, SB_H - 1, BK)
end

-- ============ 底部「台阶」软键栏 (复刻网络检测页 page_netdect.c nd_draw_nav) ============
-- 几何 (与网络检测页逐值一致): 谷线贴屏幕底 6px, 盖子线在谷线上方 21px,
--   左右文字距屏幕边框 10px, 文字顶 = 盖子线 + 4 (16px 字正好落在盖子线与谷线之间)。
--   盖子线从屏幕边框画到"文字右缘 + 10px", 然后 45° 斜坡下到谷线, 中间留凹口。
-- ★ 2026-11-23 linit: ① 下方不再写"上下滚动 左右翻页 返回"那种提示行;
--   ② 左软键=返回, 右软键=搜索, 中间凹口=输入框 (搜索页) 或信息 (其他页);
--   ③ 键盘弹出时整条栏**上浮到键盘顶部**, 不再被键盘压住。
local NAV_LX    = 10                -- 左文字左缘 (距屏幕左边框 10px)
local NAV_RX    = W - 10            -- 右文字右缘 (距屏幕右边框 10px)
local NAV_SLOPE = 21                -- 盖子线 → 谷线 的垂直距离

-- 键盘: 只画前 4 排 (第 5 排 Fn/Ctrl/中/空格/←↓→ 不要),
--       且第 4 排 M 右边的 ↑ 也隐藏 (linit 要求, 见 kbd_std: M(273,32) → ↑(305,45))
local KB_ROWS  = 4
local KB_ROW   = (type(app.kbd.ROW) == "number") and app.kbd.ROW or 29
local KB_H     = KB_ROWS * KB_ROW

-- ★ 2026-11-23 linit: 第 4 排重排 —— kbd_std 里 M 右边是 ↑(305,45), 再往右是回车
--   (Enter 350..399, **跨 2 排高**, 下半截正好落在本排)。原来"白盖 x>=305..屏幕右缘"
--   把回车下半截一起涂掉了 ⇒ 回车看着被挡了一半。改成: 只在本排内自绘, z..m 七键
--   拉宽补位, 与回车之间留 2 个字符宽 (16px), 回车本身保持原样。
local KB_LETTERS = { "Z", "X", "C", "V", "B", "N", "M" }
local KB_OLD_X   = { 70, 104, 138, 172, 206, 240, 273 }    -- kbd_std 原左缘
local KB_OLD_W   = { 34, 34, 34, 34, 34, 33, 32 }          -- kbd_std 原宽度
local KB_R4_X0, KB_R4_X1 = 70, 334      -- 本排新左端 / 右端 (回车 350 起, 中间留 16px)
local KB_ENTER_X = 350                  -- 回车左缘

local function kb_top()   return H - KB_H end
-- 键盘只属于搜索页: S.kb 从搜索页带进列表/详情时不能把台阶栏也顶上去
local function kb_shown() return S.kb and S.page == "search" end
-- 谷线 y: 没键盘时固定贴屏幕底 6px (与网络检测等页一致);
--   有键盘时整条栏浮在键盘顶上, KB_NAV_GAP = 0 ⇒ 谷线正好落在键盘第一排上沿
local KB_NAV_GAP = 6            -- ★ 键盘页台阶栏相对键盘顶再上移 (3 → 6, linit 2026-11-23)
local function nav_valley()
    return kb_shown() and (kb_top() - KB_NAV_GAP) or (H - 6)
end

-- 台阶两侧的起点: 左 = 左文字右缘 + 10; 右 = 右文字左缘 - 10
local function nav_geom(lbl, rlbl)
    return NAV_LX + app.mixw(lbl) + 10, NAV_RX - app.mixw(rlbl) + 1 - 10
end

local function kb_row4_y() return kb_top() + 3 * KB_ROW end

-- 本排第 i 个字母键的左右缘 (7 等分 [KB_R4_X0, KB_R4_X1])
local function kb_row4_span(i)
    local n, total = #KB_LETTERS, KB_R4_X1 - KB_R4_X0 + 1
    local a = KB_R4_X0 + math.floor((i - 1) * total / n)
    local b = KB_R4_X0 + math.floor(i * total / n) - 1
    return a, b
end

-- 本排命中: 新列 → 用原几何中心反查 kbd 索引 (press/char 仍走原生逻辑, 不改输入模型)
local function kb_row4_hit(x, y)
    if x < KB_R4_X0 or x > KB_R4_X1 then
        if x > KB_R4_X1 and x < KB_ENTER_X then return -1 end   -- 与回车之间的留白
        return app.kbd.hit(x, y)                                -- Shift / 回车下半
    end
    local n, total = #KB_LETTERS, KB_R4_X1 - KB_R4_X0 + 1
    local i = math.floor((x - KB_R4_X0) * n / total) + 1
    if i < 1 then i = 1 elseif i > n then i = n end
    local ox = KB_OLD_X[i] + math.floor(KB_OLD_W[i] / 2)
    return app.kbd.hit(ox, y)
end

-- 重绘第 4 排: 清掉原 ↑ 与旧字母键, 再按新宽度画 7 键 (回车左侧一个像素都不碰)
local function draw_kb_row4()
    local y = kb_row4_y()
    app.fill(KB_R4_X0, y, KB_ENTER_X - 1, y + KB_ROW - 1, WH)
    for i = 1, #KB_LETTERS do
        local a, b = kb_row4_span(i)
        app.hline(a, b, y, BK)
        app.hline(a, b, y + KB_ROW - 1, BK)
        app.vline(a, y, y + KB_ROW - 1, BK)
        app.vline(b, y, y + KB_ROW - 1, BK)
        app.mix(a + math.floor((b - a + 1 - 8) / 2),
                y + math.floor((KB_ROW - 16) / 2), KB_LETTERS[i], false)
    end
end

-- 键盘几何命中 (第 4 排走自绘映射, 其余照原生)
local function kb_hit_vis(x, y)
    local y4 = kb_row4_y()
    if y >= y4 and y < y4 + KB_ROW then return kb_row4_hit(x, y) end
    return app.kbd.hit(x, y)
end

-- mid_kind: "input" 画输入框(带光标) | "text" 只画一行居中文字 | nil 空
local function draw_nav(lbl, rlbl, mid, mid_kind)
    local valley = nav_valley()
    local lid = valley - NAV_SLOPE
    local ty  = lid + 4
    local lend, rstart = nav_geom(lbl, rlbl)
    app.mix(NAV_LX, ty, lbl, false)
    app.mix(NAV_RX - app.mixw(rlbl) + 1, ty, rlbl, false)
    app.hline(0, lend, lid, BK)                    -- 左横杠: 贴屏幕左边框
    app.hline(rstart, W - 1, lid, BK)              -- 右横杠: 贴屏幕右边框
    local vl, vr = lend + NAV_SLOPE, rstart - NAV_SLOPE
    if vl < vr then app.hline(vl, vr, valley, BK) end   -- 谷线
    for d = 0, NAV_SLOPE do                        -- 两侧 45° 斜坡
        app.px(lend + d, lid + d, BK)
        app.px(rstart - d, lid + d, BK)
    end
    if vl >= vr then return end
    if mid_kind == "input" then
        -- ★ 2026-11-23 linit: 中间不画输入框描边, 只显示当前输入 (空时显示灰色占位) + 光标;
        --   linit 再要求: 内容在凹口里**居中对齐** (原来左对齐贴着左斜坡)。
        local raw   = (S.q == "") and "输入代码/英文/拼音" or S.q
        local shown = ell(raw, vr - vl - 12)
        local tx    = (vl + vr) // 2 - app.mixw(shown) // 2
        if tx < vl + 4 then tx = vl + 4 end
        -- ★ 必须是黑字: app.mix 的 inv=true 只把前景画成白色、不填背景,
        --   台阶栏是白底 ⇒ 占位提示会变成"白底白字"完全看不见 (真机踩过)
        app.mix(tx, ty, shown, false)
        if S.q ~= "" then
            local cx = tx + app.mixw(S.q)
            if cx > vr - 6 then cx = vr - 6 end
            app.fill(cx, ty + 1, cx + 1, ty + 12, BK)     -- 光标
        end
    elseif mid and mid ~= "" then
        local mw = app.mixw(mid)
        local mx = (vl + vr) // 2 - mw // 2
        if mx < vl + 2 then mx = vl + 2 end
        if mx + mw > vr - 2 then mx = vr - 2 - mw end
        if mx >= vl then app.mix(mx, ty, mid, false) end
    end
end

local function rowfill(x, y, w, h, on, text, inv)
    if on then
        app.fill(x, y - 1, x + w - 1, y + h - 1, BK)
        app.mix(x + 4, y, text, true)
    else
        app.mix(x + 4, y, text, false)
    end
end

-- 1px 圆角矩形描边 (半径 r)。直边用 hline/vline, 四角按圆方程逐行取 1 个像素。
-- 只描边不填充: 首页大搜索框要的是「细线框」而不是实心块。
local function rrect1(x0, y0, x1, y1, r)
    if r < 1 then
        app.rect(x0, y0, x1, y1, BK)
        return
    end
    app.hline(x0 + r, x1 - r, y0, BK)
    app.hline(x0 + r, x1 - r, y1, BK)
    app.vline(x0, y0 + r, y1 - r, BK)
    app.vline(x1, y0 + r, y1 - r, BK)
    for i = 0, r do
        local k = math.sqrt(r * r - (r - i) * (r - i))
        local dx = r - math.floor(k + 0.5)
        app.fill(x0 + dx, y0 + i, x0 + dx, y0 + i, BK)   -- 左上
        app.fill(x1 - dx, y0 + i, x1 - dx, y0 + i, BK)   -- 右上
        app.fill(x0 + dx, y1 - i, x0 + dx, y1 - i, BK)   -- 左下
        app.fill(x1 - dx, y1 - i, x1 - dx, y1 - i, BK)   -- 右下
    end
end

-- ------------------------------- 首页命中区 ------------------------------
-- 顺序: 整个大框 (= 底部台阶输入口) → 最近查询条目
-- ★ 框内数据只做「默认可见的便览」, 不单独设命中区: 点框任意位置等价于点
--   台阶栏中间的「输入代码/英文/拼音」, 直接进输入, 而不是打开某一条详情。
local function layout_home()
    local R = {}
    local b = HOME.box
    R[#R + 1] = { x = b.x, y = b.y, w = b.w, h = b.h, kind = "search" }
    for i = 1, math.min(HOME.rec.n, #S.rec) do
        R[#R + 1] = { x = HOME.rec.x, y = HOME.rec.y + (i - 1) * HOME.rec.rh,
                      w = HOME.rec.w, h = HOME.rec.rh - 1, kind = "code", arg = S.rec[i] }
    end
    S.rects = R
    if S.hsel > #R then S.hsel = #R end
    if S.hsel < 0 then S.hsel = 0 end      -- 0 = 不选中任何项 (首页默认态)
    S.nlayer = 0
end

local function save_rec()
    if not app.nvs.set_str("lf_rc", table.concat(S.rec, "|")) then
        S.err = "最近查询未能保存"
    end
end

local function push_rec(code)
    for i = #S.rec, 1, -1 do
        if S.rec[i] == code then table.remove(S.rec, i) end
    end
    table.insert(S.rec, 1, code)
    while #S.rec > 5 do table.remove(S.rec) end
    save_rec()
    layout_home()
end

-- ------------------------------- 搜索 ------------------------------------
-- 打分: 精确码 > 同数字异体系 > 前缀 > 码包含 > 归一化包含 > 文本命中
--   中文分类词 (蓝屏 / 设备管理器 / 引导 / 风扇) 走文本命中, 命中整层但排在码命中之后
local function do_search(q)
    local res = {}
    q = string.lower(q or "")
    q = q:gsub("^[ \t\r\n]+", ""):gsub("[ \t\r\n]+$", "")
    if q == "" then return res end
    local qn = norm_code(q)
    local q0 = bare_key(qn)
    local scored = {}
    for i = 1, #IDX do
        local e = IDX[i]
        local code = string.lower(e[1])
        local score = 0
        if NORM[i] == qn then
            score = 1000                       -- 同体系精确: 0x1ca→蓝屏, 01ca→固件
        elseif bare_key(NORM[i]) == q0 then
            score = 985                        -- 同数字不同体系: 仍然给出, 但排在后面
        elseif code == q then
            score = 980
        elseif #q <= #code and code:sub(1, #q) == q then
            score = 900                        -- 前缀: 7b / 0xc0000
        elseif code:find(q, 1, true) then
            score = 820                        -- 码里包含
        elseif NORM[i]:find(qn, 1, true) then
            score = 700
        elseif LOWER[i]:find(q, 1, true) then
            score = 600                        -- 英文 / 拼音 / 中文类别 / 层名
        end
        if score > 0 then scored[#scored + 1] = { e = e, s = score } end
    end
    table.sort(scored, function(a, b)
        if a.s ~= b.s then return a.s > b.s end
        return a.e[1] < b.e[1]
    end)
    for i = 1, math.min(#scored, 60) do res[i] = scored[i].e end
    return res
end

-- ------------------------------- 详情内容 --------------------------------
-- 六段固定框架: 错误信息 → 可能原因 → 处理方法 → 验证方法 → 注意事项 → 来源与适用
-- 规则: 空字段整段不显示; 不显示数据库内部字段名; 命令单独成行 (数据里以 @ 开头)。
local SOURCE_CN = {
    HMM = "联想 HMM 硬件维护手册", KB = "联想知识库", SUP = "联想支持文档",
    X390 = "联想 X390 错误消息页", MS = "Microsoft Learn", OEM = "厂商官方文档",
    ["经验"] = "社区经验 (未逐条核实)",
}

-- "1) … 2) …" 拆成条目; 没有编号就整段算一条
-- ★ 2026-11-23: 裁剪一律用 ASCII 空白类, 不用 %s —— 本机 Lua 里 %s 会匹配 UTF-8
--   尾字节 0xA0 (如「池」E6 B1 A0), gsub("%s+$","") 会把汉字截掉半个字节。
local function split_items(s)
    local out = {}
    local SEP = "\1"
    local t = " " .. (s or "") .. " "
    t = t:gsub(" (%d)%) ", SEP .. "%1) ")
    for seg in t:gmatch("[^" .. SEP .. "]+") do
        local one = seg:gsub("^[ \t\r\n]+", ""):gsub("[ \t\r\n]+$", "")
        if one ~= "" then out[#out + 1] = one end
    end
    return out
end

local function build_detail(e)
    local L = {}
    local function push(txt, kind)
        if not txt or txt == "" then return end
        local w = TXT_W - ((kind == "cmd") and 14 or 0)
        local ls = wrap(txt, w)
        for i = 1, #ls do
            L[#L + 1] = { t = ls[i], kind = kind, head = (i == 1) }
        end
    end
    local function para(val, kind)
        if not val or val == "" then return end
        for _, seg in ipairs(split_items(val)) do
            local num, cmd = seg:match("^(%d%))%s*@(.*)$")
            if num then
                push(num .. " > " .. cmd, "cmd")    -- 命令: 编号 + > 提示符, 单独成行
            elseif seg:find("@", 1, true) and kind == "step" then
                push((seg:gsub("@", "> ")), "cmd")
            else
                push(seg, kind)
            end
        end
    end
    local function sec(title, val, kind)
        if not val or val == "" then return end
        push(title, "sec")
        para(val, kind)
    end

    -- 1) 错误信息: 码 → 英文原名 → 中文含义
    push(e[1], "code")
    push(e[2], "en")
    push(e[3], "zh")

    -- 2~4) 原因 / 处理 / 验证
    sec("可能原因", e[6], "item")
    sec("处理方法", e[7], "step")
    sec("验证方法", e[16], "item")

    -- 5) 注意事项: 风险 / 处置等级 / 建议 / 备注 (全部为空则整段不显示)
    local notes = {}
    if e[9] and e[9] ~= "" and not e[9]:match("^无") then notes[#notes + 1] = "风险 " .. e[9] end
    if e[10] and e[10] ~= "" and e[10] ~= "现场可修" then notes[#notes + 1] = "处置 " .. e[10] end
    if e[8] and e[8] ~= "" then notes[#notes + 1] = "建议 " .. e[8] end
    if e[12] and e[12] ~= "" then notes[#notes + 1] = "备注 " .. e[12] end
    if #notes > 0 then
        push("注意事项", "sec")
        for _, s in ipairs(notes) do para(s, "note") end
    end

    -- 6) 来源及适用 (放最底部)
    push("来源与适用", "sec")
    local src = SOURCE_CN[e[11]] or e[11]
    if e[15] and e[15] ~= "" then src = src .. " · " .. e[15] end
    push("来源 " .. src, "note")
    if e[5] and e[5] ~= "" then push("适用 " .. e[5], "note") end

    S.lines = L
    S.ltop = 0
end

local ABOUT = {
    "本应用数据全部离线内置, 不联网、不上传、不采集任何信息。",
    "共 237 条, 按「码出现在哪个界面」分七层: 固件自检 41 / 蜂鸣 36 / 引导 25 / 蓝屏 49 / 设备管理器 41 / 系统错误 29 / 开机提示 16。",
    "官方来源: 1) 联想 ThinkPad 硬件维护手册 (HMM) 错误码表 2) 联想知识库与支持文档 3) Microsoft Learn: 蓝屏 Bug Check 参考、设备管理器错误消息、NTSTATUS (MS-ERREF)、Windows 更新错误码表、Win32 系统错误消息。",
    "来源标记: HMM / KB / SUP / X390 为联想官方; MS 为 Microsoft Learn; OEM 为其它厂商官方文档; 「经验」为社区经验, 未逐条核实, 仅作参考。",
    "详情统一六段: 错误信息 → 可能原因 → 处理方法 → 验证方法 → 注意事项 → 来源与适用。所有处置都要求「能复验」: 照验证方法走一遍再下结论, 不要只凭一条码换件。",
    "适用: ThinkPad (T/X/P/L/E)、ThinkBook、小新·IdeaPad、拯救者 Legion、ThinkCentre、IdeaCentre·天逸 的固件码; 蓝屏 / 引导 / 设备管理器 / 系统错误码适用于 Windows 7/8/10/11。",
    "未收录: 1) 各机型 FRU 料号与拆装步骤 2) 服务器 ThinkSystem 与 ThinkStation 专有码 3) 主板指示灯闪烁序列 4) SmartBeep 旋律码 5) 无稳定错误码的厂商码 (Dell/HP/ASUS 等, 资料未经证实不写)。",
    "免责: 本应用是查询工具, 不替代官方诊断。涉及拆机、刷 BIOS、改分区、清盘的操作请先备份数据; 保修期内优先联系服务站。",
}

-- 详情页的滚动上限必须按「页」对齐:
--   页码总数 = ceil(#lines / DET_ROWS); 若上限仍取 #lines - DET_ROWS,
--   末页永远翻不到 (屏上停在「y-1/y 页」, 再拖也不动, 内容看着没到底)。
local function det_maxtop()
    local total = math.max(1, math.ceil(#S.lines / DET_ROWS))
    return (total - 1) * DET_ROWS
end

local function open_about()
    S.e = nil
    S.dtitle = "数据来源与免责"
    S.lines = {}
    for i = 1, #ABOUT do
        local ls = wrap(ABOUT[i], TXT_W)
        for k = 1, #ls do S.lines[#S.lines + 1] = { t = ls[k], head = (k == 1) } end
    end
    S.ltop = 0
    S.dfrom = S.page
    S.page = "detail"
end

-- ------------------------------- 页面切换 --------------------------------
local function open_detail(e, from)
    if not e then return end
    S.e = e
    S.dtitle = e[1] .. "  " .. e[3]
    build_detail(e)
    S.dfrom = from or S.page
    S.page = "detail"
    push_rec(e[1])
end

local function open_detail_code(code, from)
    local e = BYCODE[string.lower(code)]
    if e then
        open_detail(e, from)
    else
        app.dlg.toast("没有收录 " .. code, 1200)
    end
end

local function open_list(items, title, from)
    S.list, S.list_title, S.lfrom = items, title, from or "home"
    S.lsel, S.top = 1, 1
    S.page = "list"
end

local function open_layer(lay)
    local items = {}
    for i = 1, #DB do
        if DB[i][14] == lay then items[#items + 1] = DB[i] end
    end
    open_list(items, (LAYER_NAME[lay] or lay) .. " · " .. #items .. " 条", "home")
end

local function open_hot()
    local items = {}
    for i = 1, #HOT do
        local e = BYCODE[string.lower(HOT[i])]
        if e then items[#items + 1] = e end
    end
    open_list(items, "常用故障码", "home")
end

local function enter_search()
    S.page = "search"
    S.kb = true
    S.found = do_search(S.q)
    -- ★ 4 排 (去掉 Fn/Ctrl/中/空格/←↓→ 那排), 键盘贴屏幕底
    app.kbd.use("rows", KB_ROWS, kb_top())
    S.kbx, S.kby = 4, kb_top() + math.floor(KB_ROW / 2)
    S.kidx = -1                    -- 初始不高亮任何键, 避免一进来就误输入
end

-- ------------------------------- 绘制: 首页 ------------------------------
-- 布局 (400×300): 半屏高大框 (框内直接铺数据) / 最近查询 (下半) / 台阶栏
-- ★ 首页精简后不再有标题排、搜索提示排、7 层入口、常用栏与底部说明行。
local function draw_home()
    app.clear(WH)

    -- 1) 大框: R5 圆角 + 1px 描边; 框内直接铺数据 (只便览), 默认从第 1 条起, 不留空白
    local b = HOME.box
    rrect1(b.x, b.y, b.x + b.w - 1, b.y + b.h - 1, HOME_R)
    local n = #DB
    for i = 1, HOME_ROWS do
        local idx = S.htop + i - 1
        if idx > n then break end
        local e = DB[idx]
        local y = b.y + 10 + (i - 1) * HOME_RH
        app.mix(b.x + 8, y, ell(e[1], DATA_CODE_W), false)
        app.mix(b.x + 8 + DATA_CODE_W, y, ell(e[3], b.w - 16 - DATA_CODE_W), false)
    end

    -- 2) 最近查询 (紧贴大框下方)
    app.mix(HOME.rec.x + 4, HOME.rec.y - 20, "最近查询", false)
    if #S.rec == 0 then
        app.mix(HOME.rec.x + 4, HOME.rec.y, "还没有记录", false)
    else
        for i = 1, math.min(HOME.rec.n, #S.rec) do
            local y = HOME.rec.y + (i - 1) * HOME.rec.rh
            local e = BYCODE[string.lower(S.rec[i])]
            local t = e and (e[1] .. " " .. e[3]) or S.rec[i]
            rowfill(HOME.rec.x, y, HOME.rec.w, HOME.rec.rh - 1, S.hsel == 1 + i,
                    ell(t, HOME.rec.w - 8))
        end
    end

    -- 3) 台阶栏 (左=返回退出 右/中间=搜索)
    draw_nav("返回", "搜索", nil, "input")
end

-- ------------------------------- 绘制: 列表 ------------------------------
-- 一行三段: 码 (定宽, 不截断) | 中文含义 | 层标签 (右对齐)
-- ★ 码单独占列, 解决「长码 + 长中文一起被 ell 截掉, 认不出是哪个码」的问题
local LIST_CODE_W = 104
local function draw_list()
    app.clear(WH)

    -- 顶部标题行: 分类名 / 搜索词 + 位置
    local n = #S.list
    local title = S.list_title or ""
    app.mix(PAD, 4, ell(title, W - PAD * 2 - 96), false)
    if n > 0 then
        local pos = S.lsel .. "/" .. n
        app.mix(W - PAD - app.mixw(pos), 4, pos, false)
    end

    local y0 = 26                                  -- 标题行下方
    local y1 = nav_valley() - NAV_SLOPE - 8        -- 让出底部台阶栏
    for i = 0, ROWS - 1 do
        local idx = S.top + i
        local e = S.list[idx]
        if not e then break end
        local y = y0 + i * ROW_H
        if y >= y1 then break end
        local h = ROW_H - 2
        if y + h > y1 then h = y1 - y end
        if h < 6 then break end
        local tag  = LAYER_SHORT[e[14]] or ""
        local code = ell(e[1], LIST_CODE_W - 8)
        local mean = ell(e[3], W - PAD * 2 - LIST_CODE_W - app.mixw(tag) - 10)
        local on   = (idx == S.lsel)
        if on then
            app.fill(PAD - 4, y - 2, W - PAD + 2, y + h, BK)
        end
        app.mix(PAD, y, code, on)
        app.mix(PAD + LIST_CODE_W, y, mean, on)
        app.mix(W - PAD - app.mixw(tag), y, tag, on)
    end

    if n == 0 then
        app.mix(PAD, y0 + 20, "这一类暂时没有条目", false)
    end

    -- 滚动条
    local vis = ROWS
    if n > vis then
        local tx = W - 5
        local th = math.max(10, math.floor((y1 - y0) * vis / n))
        local ty = y0 + math.floor((y1 - y0 - th) * (S.top - 1) / math.max(1, n - vis))
        app.fill(tx, ty, tx + 2, ty + th, BK)
    end

    draw_nav("返回", "搜索", nil, "text")
end

-- ------------------------------- 绘制: 详情 ------------------------------
local function draw_detail()
    app.clear(WH)
    local total = math.max(1, math.ceil(#S.lines / DET_ROWS))
    local cur = math.floor(S.ltop / DET_ROWS) + 1

    local y = 4
    for i = 0, DET_ROWS - 1 do
        local ln = S.lines[S.ltop + i + 1]
        if not ln then break end
        local k = ln.kind
        if k == "code" then
            -- 错误码: 反白条, 一眼能看到 (不靠颜色, 靠反白)
            app.fill(PAD - 4, y - 2, W - PAD + 2, y + 16, BK)
            app.mix(PAD, y, ln.t, true)
        elseif k == "sec" then
            -- 段标题: 小方块 + 上下短横线
            app.hline(PAD, W - PAD, y - 2, BK)
            app.fill(PAD, y + 6, PAD + 5, y + 11, BK)
            app.mix(PAD + 9, y + 1, ln.t, false)
        elseif k == "cmd" then
            app.mix(PAD + 12, y, ln.t, false)          -- 命令单独缩进, 不与解释混排
        elseif ln.head then
            app.mix(PAD, y, ln.t, false)
        else
            app.mix(PAD + 10, y, ln.t, false)          -- 折行续行缩进
        end
        y = y + LINE_H
    end

    draw_nav("返回", "搜索", "第 " .. cur .. "/" .. total .. " 页", "text")
end

-- ------------------------------- 绘制: 搜索 ------------------------------
local function draw_search()
    app.clear(WH)

    -- ★ 2026-11-23 linit: 顶部与首页同一套 R5 大框, 框内默认就铺数据 ——
    --   未输入时给全库前几条 (与首页一致, 不留空白、不写提示语),
    --   有输入时换成搜索结果; 顶部那条输入框仍在底部台阶栏中间凹口。
    local list = (#S.q > 0) and S.found or DB
    local b = HOME.box
    rrect1(b.x, b.y, b.x + b.w - 1, b.y + b.h - 1, HOME_R)
    if #S.q > 0 and #list == 0 then
        app.mix(b.x + 8, b.y + 10, "没有匹配, 换个关键词试试", false)
    else
        for i = 1, HOME_ROWS do
            if i > #list then break end
            local e = list[i]
            local y = b.y + 10 + (i - 1) * HOME_RH
            app.mix(b.x + 8, y, ell(e[1], DATA_CODE_W), false)
            app.mix(b.x + 8 + DATA_CODE_W, y, ell(e[3], b.w - 16 - DATA_CODE_W), false)
        end
    end

    if kb_shown() then
        app.kbd.draw(S.kidx)
        -- 第 4 排: 去掉 ↑, z..m 拉宽补位 (不动 M 右边的回车)
        draw_kb_row4()
    end

    draw_nav("返回", "搜索", nil, "input")
end

-- ------------------------------- 触摸 ------------------------------------
local T = { down = false, x = 0, y = 0, x0 = 0, y0 = 0, ly = 0, t0 = 0, moved = false }

local function scroll_by(d)
    if S.page == "list" then
        S.lsel = clamp(S.lsel + d, 1, math.max(1, #S.list))
        -- 拖动路径原来只挪游标不挪视窗, 游标会跑出屏幕; 这里补上"保持选中项可见"
        if S.lsel < S.top then S.top = S.lsel end
        if S.lsel > S.top + ROWS - 1 then S.top = S.lsel - ROWS + 1 end
        if S.top < 1 then S.top = 1 end
    elseif S.page == "detail" then
        S.ltop = clamp(S.ltop + d, 0, det_maxtop())
    elseif S.page == "home" then
        S.htop = clamp(S.htop + d, 1, math.max(1, #DB - HOME_ROWS + 1))
        layout_home()                        -- 框内首条变了, 重建命中区
    end
end

local function tap_home(x, y)
    -- 底部台阶栏: 左软键=返回(走统一退出确认), 右软键/中间凹口=进搜索(带键盘)
    local lid = nav_valley() - NAV_SLOPE
    if y >= lid - 2 then
        local lend = nav_geom("返回", "搜索")
        if x <= lend then app.exit() else enter_search() end
        return
    end
    for i = 1, #S.rects do
        local r = S.rects[i]
        if x >= r.x and x < r.x + r.w and y >= r.y and y < r.y + r.h then
            S.hsel = i
            if r.kind == "data" then open_detail(DB[r.arg], "home")
            elseif r.kind == "layer" then open_layer(r.arg)
            elseif r.kind == "code" then open_detail_code(r.arg, "home")
            elseif r.kind == "search" then enter_search()
            else open_about() end
            return
        end
    end
end

-- 台阶栏点击: 左软键=返回上一页, 右软键=进搜索 (与按键语义一致)
-- 返回 true 表示这一下已经被台阶栏消费掉
local function tap_nav(x, y, from)
    local lid = nav_valley() - NAV_SLOPE
    if y < lid - 2 then return false end
    local lend, rstart = nav_geom("返回", "搜索")
    if x <= lend then
        S.page = from or "home"
        if S.page == "search" then S.kb = false end
        if S.page == "home" then layout_home() end
    elseif x >= rstart then
        enter_search()
    end
    return true
end

local function tap_list(x, y)
    if tap_nav(x, y, S.lfrom) then return end
    local y0 = SB_H + 6
    local y1 = nav_valley() - NAV_SLOPE - 8
    if y < y0 or y >= y1 then return end
    local i = math.floor((y - y0) / ROW_H)
    local idx = S.top + i
    if S.list[idx] then
        S.lsel = idx
        open_detail(S.list[idx], "list")
    end
end

local function tap_detail(x, y)
    if tap_nav(x, y, S.dfrom) then return end
    -- 下半屏=下一页, 上半屏=上一页 (与左右键一致)
    if y > H / 2 then
        S.ltop = clamp(S.ltop + DET_ROWS, 0, det_maxtop())
    else
        S.ltop = clamp(S.ltop - DET_ROWS, 0, det_maxtop())
    end
end

local function tap_search(x, y)
    local kt = kb_top()
    -- 键盘区
    if S.kb and y >= kt then
        -- 第 4 排已自绘重排: 认自绘键位 + 回车, 与回车之间的留白当点空白
        local i, y4 = nil, kb_row4_y()
        if y >= y4 and y < y4 + KB_ROW then
            i = kb_row4_hit(x, y)
            if i and i < 0 then S.kb = false; return end
        else
            i = app.kbd.hit(x, y)
        end
        if i and i >= 0 then
            S.kbx, S.kby, S.kidx = x, y, i
            S.press = i               -- 统一到 poll 里执行 (press 有副作用)
        end
        return
    end
    -- 底部台阶栏 (键盘弹出时它浮在键盘顶, 所以要在键盘分支之后再判 y)
    local lid = nav_valley() - NAV_SLOPE
    if y >= lid - 2 then
        local lend, rstart = nav_geom("返回", "搜索")
        if x <= lend then                          -- 左软键: 返回首页
            S.page = "home"
            S.kb = false
            layout_home()
        elseif x >= rstart then                    -- 右软键: 执行搜索 → 结果列表
            if #S.found > 0 then open_list(S.found, "搜索: " .. S.q, "search")
            else app.dlg.toast("没有匹配结果", 1200) end
        else                                       -- 中间凹口: 输入框
            S.kb = true
        end
        return
    end
    -- 顶部 R5 大框内的数据行 (排版与 draw_search 一致: y = box.y + 10 + (i-1)*HOME_RH)
    local bx = HOME.box
    local rel = y - (bx.y + 10)
    if rel >= -4 and x >= bx.x + 4 and x <= bx.x + bx.w - 5 then
        local i = math.floor((rel + 2) / HOME_RH) + 1
        local list = (#S.q > 0) and S.found or DB
        if i >= 1 and i <= #list and i <= HOME_ROWS then
            open_detail(list[i], "search")
            return
        end
    end
    -- 其他空白处: 收起键盘 (linit: 点别的地方就收)
    S.kb = false
end

local function handle_tap(x, y)
    if S.page == "home" then tap_home(x, y)
    elseif S.page == "list" then tap_list(x, y)
    elseif S.page == "detail" then tap_detail(x, y)
    elseif S.page == "search" then tap_search(x, y) end
end

local function poll_touch()
    local x, y = app.touch()
    local now = app.millis() or 0
    if type(x) == "number" then
        if not T.down then
            T.down, T.x, T.y, T.x0, T.y0, T.ly, T.t0, T.moved = true, x, y, x, y, y, now, false
        else
            if math.abs(x - T.x0) > 12 or math.abs(y - T.y0) > 12 then T.moved = true end
            if T.moved then
                local dy = y - T.ly
                -- ★ 2026-11-23 linit「菜单方向反了」: 方向按页面语义分两类
                --   详情页 = 文档滚动: 手指下拖 → 内容整体下移 → 露出更靠前的行 → 索引 -1
                --   首页/列表 = 游标菜单: 手指下拖 → 游标往下走        → 索引 +1
                local step = (S.page == "detail") and -1 or 1
                if dy >= 18 then scroll_by(step); T.ly = y
                elseif dy <= -18 then scroll_by(-step); T.ly = y end
            end
            T.x, T.y = x, y
        end
    else
        if T.down then
            T.down = false
            if not T.moved and (now - T.t0) < 900 then handle_tap(T.x, T.y) end
        end
    end
end

-- ------------------------------- 按键动作 --------------------------------
local function key_search(a)
    -- ⚠ 不能在这里重调 app.kbd.use: 它会把 s_kbd 复位成 kbd_std, Caps/Shift 状态丢失。
    if S.kb then
        local moved = false
        if a == K_LEFT then
            local nx = S.kbx - 48
            if nx < 0 then nx = W - 6 end
            if kb_hit_vis(nx, S.kby) >= 0 then S.kbx = nx; moved = true end
        elseif a == K_RIGHT then
            local nx = S.kbx + 48
            if nx > W - 1 then nx = 4 end
            if kb_hit_vis(nx, S.kby) >= 0 then S.kbx = nx; moved = true end
        elseif a == K_UP then
            local ny = S.kby - KB_ROW
            if kb_hit_vis(S.kbx, ny) >= 0 then S.kby = ny; moved = true end
        elseif a == K_DOWN then
            local ny = S.kby + KB_ROW
            if kb_hit_vis(S.kbx, ny) >= 0 then S.kby = ny; moved = true end
        end
        if moved then S.kidx = kb_hit_vis(S.kbx, S.kby); return true end

        if a == K_OK then
            if S.kidx and S.kidx >= 0 then
                local act = app.kbd.press(S.kidx)
                local ch = app.kbd.char(S.kidx)
                if act == app.kbd.CHAR and ch then
                    if #S.q < 18 then
                        S.q = S.q .. ch
                        S.found = do_search(S.q)
                    end
                elseif act == app.kbd.BKSP then
                    S.q = S.q:sub(1, -2)
                    S.found = do_search(S.q)
                elseif act == app.kbd.ENTER or act == app.kbd.TAB then
                    if #S.found > 0 then open_list(S.found, "搜索: " .. S.q, "search") end
                elseif act == app.kbd.ESC then
                    S.kb = false
                end
            end
            return true
        end
        -- ★ 2026-11-23 linit: 按返回直接回首页, 不停在「只剩顶部框、中间空白」的
        --   中间态 (原先是 S.kb = false 收键盘后留在搜索页)
        if a == K_BACK then
            S.page, S.kb = "home", false
            layout_home()
            return true
        end
        return true
    end

    -- 键盘收起态: 返回同样直接回首页
    if a == K_BACK then
        S.page, S.kb = "home", false
        layout_home()
        return true
    end
    if a == K_OK then
        if #S.found > 0 then open_list(S.found, "搜索: " .. S.q, "search") end
        return true
    end
    return true
end

-- ------------------------------- 五契约 ----------------------------------
function on_enter()
    build_index()
    S.rec = {}
    local s = app.nvs.get_str("lf_rc")
    if s and #s > 0 then
        for c in s:gmatch("[^|]+") do
            if BYCODE[string.lower(c)] then S.rec[#S.rec + 1] = c end
        end
    end
    S.page, S.lsel, S.top = "home", 1, 1
    S.q, S.found, S.kb = "", {}, false
    layout_home()
end

function render()
    if S.page == "list" then draw_list()
    elseif S.page == "detail" then draw_detail()
    elseif S.page == "search" then draw_search()
    else draw_home() end
end

function action(a)
    -- 搜索页: 含键盘导航
    if S.page == "search" then
        return key_search(a)
    end

    if S.page == "detail" then
        local maxTop = det_maxtop()
        if a == K_UP then
            S.ltop = clamp(S.ltop - 1, 0, maxTop)
        elseif a == K_DOWN then
            S.ltop = clamp(S.ltop + 1, 0, maxTop)
        elseif a == K_LEFT then
            S.ltop = clamp(S.ltop - DET_ROWS, 0, maxTop)
        elseif a == K_RIGHT then
            S.ltop = clamp(S.ltop + DET_ROWS, 0, maxTop)
        elseif a == K_BACK then
            S.page = S.dfrom or "home"
            if S.page == "search" then S.kb = false end
            if S.page == "home" then layout_home() end
        else
            return true
        end
        return true
    end

    if S.page == "list" then
        if a == K_UP then
            S.lsel = clamp(S.lsel - 1, 1, math.max(1, #S.list))
        elseif a == K_DOWN then
            S.lsel = clamp(S.lsel + 1, 1, math.max(1, #S.list))
        elseif a == K_LEFT then
            S.lsel = clamp(S.lsel - ROWS, 1, math.max(1, #S.list))
        elseif a == K_RIGHT then
            S.lsel = clamp(S.lsel + ROWS, 1, math.max(1, #S.list))
        elseif a == K_OK then
            open_detail(S.list[S.lsel], "list")
            return true
        elseif a == K_BACK then
            S.page = S.lfrom or "home"
            if S.page == "home" then layout_home() end
            return true
        else
            return true
        end
        -- 保持选中项在可视区
        if S.lsel < S.top then S.top = S.lsel end
        if S.lsel > S.top + ROWS - 1 then S.top = S.lsel - ROWS + 1 end
        if S.top < 1 then S.top = 1 end
        return true
    end

    -- 首页: 命中区 = 大搜索框 + 最近查询条目; 0 表示不选中 (默认态)
    if a == K_UP or a == K_LEFT then
        S.hsel = S.hsel - 1
        if S.hsel < 0 then S.hsel = #S.rects end
    elseif a == K_DOWN or a == K_RIGHT then
        S.hsel = S.hsel + 1
        if S.hsel > #S.rects then S.hsel = 0 end
    elseif a == K_OK then
        local r = S.rects[S.hsel]
        if not r then enter_search(); return true end   -- 未选中任何项时 OK = 直接进搜索
        if r.kind == "data" then open_detail(DB[r.arg], "home")
        elseif r.kind == "layer" then open_layer(r.arg)
        elseif r.kind == "code" then open_detail_code(r.arg, "home")
        elseif r.kind == "search" then enter_search()
        else open_about() end
        return true
    elseif a == K_BACK then
        return false        -- 顶层退出确认
    else
        return true
    end
    return true
end

function poll()
    poll_touch()

    -- 触摸键盘按下: 统一在这里执行一次 (press 有副作用, 不能重复调)
    if S.press and S.press >= 0 and S.page == "search" then
        local i = S.press
        S.press = nil
        local act = app.kbd.press(i)
        local ch = app.kbd.char(i)
        if act == app.kbd.CHAR and ch then
            if #S.q < 18 then
                S.q = S.q .. ch
                S.found = do_search(S.q)
            end
        elseif act == app.kbd.BKSP then
            S.q = S.q:sub(1, -2)
            S.found = do_search(S.q)
        elseif act == app.kbd.ENTER or act == app.kbd.TAB then
            if #S.found > 0 then open_list(S.found, "搜索: " .. S.q, "search") end
        elseif act == app.kbd.ESC then
            S.kb = false
        end
    end
end

function on_exit()
    app.kbd.reset()
end
