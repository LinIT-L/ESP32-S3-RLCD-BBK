--@lapp v1
--@app name=人格诊断
--@app cat=学习
--@app desc=40 题四维度偏好测评, 16 型图鉴与历史记录 (离线, 非心理诊断)
--@app version=1
--@app build=202611072303
--@app icon=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAf+AAAAAAAAf//gAAAAAAH4AfwAAAAAB4AAHwAAAAAeH/+HgAAAADh/D/HgAAAA4+AAfHAAAAHHgAAPOAAAA54AAAecAAAHOAAAAc4AAA5wAAAA5wAADOAAAAAzgAAZwAAAABmAADOAAAAAHMAAMwAAAAAO4ABmB/wD/gZgAG4H/AP+AzAAzAf8A/4DMADMB/wD/gG4AZgH/AP+AZgBmAf8A/4B2AGwB/wD/gDYAbAH/AP+AMwDMAf8A/4AzAMwAAAAAADMAzAAAAAAAGwDIAAAAAAAbAMgAAAAAABsAyAAAAAAAGwDIAAAAAAAbAMwAAAAAABsAzAAAAAAAEwDMAf8A/4AzAGwB/wD/gDMAbAH/AP+ANgBmAf8A/4A2AGYB/wD/gGYANgH/AP+AZgAzAf8A/4DMADuB/wD/gMwAGYH/AP+BmAAdwAAAAAOYAAzgAAAAAzAABmAAAAAGcAAHMAAAAAzgAAOcAAAAHMAAAc4AAABxgAAA5wAAAOMAAABzwAADzgAAADj4AA8cAAAAHj+A/DgAAAAHD//w8AAAAAPgfgPAAAAAAPwAPwAAAAAAP//8AAAAAAAD/8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
-- 图标自绘: blit_app_icon(x, y[, inv=true])
-- ── LAPP 图标自绘前奏 (打包器注入; 读内嵌 base64, 不碰文件系统) ──
local _APPICON = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAf+AAAAAAAAf//gAAAAAAH4AfwAAAAAB4AAHwAAAAAeH/+HgAAAADh/D/HgAAAA4+AAfHAAAAHHgAAPOAAAA54AAAecAAAHOAAAAc4AAA5wAAAA5wAADOAAAAAzgAAZwAAAABmAADOAAAAAHMAAMwAAAAAO4ABmB/wD/gZgAG4H/AP+AzAAzAf8A/4DMADMB/wD/gG4AZgH/AP+AZgBmAf8A/4B2AGwB/wD/gDYAbAH/AP+AMwDMAf8A/4AzAMwAAAAAADMAzAAAAAAAGwDIAAAAAAAbAMgAAAAAABsAyAAAAAAAGwDIAAAAAAAbAMwAAAAAABsAzAAAAAAAEwDMAf8A/4AzAGwB/wD/gDMAbAH/AP+ANgBmAf8A/4A2AGYB/wD/gGYANgH/AP+AZgAzAf8A/4DMADuB/wD/gMwAGYH/AP+BmAAdwAAAAAOYAAzgAAAAAzAABmAAAAAGcAAHMAAAAAzgAAOcAAAAHMAAAc4AAABxgAAA5wAAAOMAAABzwAADzgAAADj4AA8cAAAAHj+A/DgAAAAHD//w8AAAAAPgfgPAAAAAAPwAPwAAAAAAP//8AAAAAAAD/8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
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
-- 人格诊断 (Personality 16) — LinTOS Lua 单文件应用
--
-- 定位: 人格**偏好**探索工具, 不是心理测验量表, 也不构成任何诊断。
--   题目、计分、16 型文案与图标全部为本项目原创; 未复用任何第三方素材
--   （参考的三个开源仓库均无 LICENSE 文件, 默认保留所有权利, 不能拷贝）。
--
-- 结构:
--   Q[40]      题目: {题干, 维度(1..4), 方向(+1/-1)}
--                维度 1=E/I 2=S/N 3=T/F 4=J/P; 方向 -1 = 反向计分题
--   TYPE[code] 16 型资料: 名称/类别/一句话/概述/特点/优势/注意/沟通/工作/人际/成长
--   计分: 每题 5 档 -> -2..+2, 维度求和 [-20,+20] -> 倾向百分比 50±sum*2.5
--
-- 存储 (NVS, 与应用同生命周期):
--   p16_prog  40 位答题串, '0'=未答, '1'..'5'=所选档
--   p16_cur   当前题号
--   p16_hist  历史记录, '\n' 分隔: epoch|CODE|p1,p2,p3,p4
--   p16_last  最近一次结果 (首页展示用)
--
-- 屏幕: 400x300 1bpp, 顶栏 24px 归应用自己用 (Lua 页是全屏页)。
-- ============================================================================

local W, H = app.W, app.H
local BK, WH = app.BLACK, app.WHITE

local TOP     = 24          -- 顶栏下沿
local FOOT    = 22          -- 底部提示行高
local LINE_H  = 20          -- 16px 正文行高
local OPT_H   = 26          -- 选项行高
local OPT_Y   = 118         -- 第一项选项 y
local OPT_GAP = 29          -- 选项行距

-- ---------------------------------------------------------------- 数据: 题目
-- dir=-1 的题是反向计分: 同一档回答在维度上加相反方向的分。
local Q = {
  -- 维度 1: E / I
  { "聚会结束后，你通常还觉得有精神，想再聊一会儿", 1,  1 },
  { "一个人待了一整天，你会觉得有点闷，想找人说说话", 1,  1 },
  { "遇到烦心事，你更愿意先找人聊聊，而不是自己消化", 1,  1 },
  { "到了新环境，你常常是主动开口介绍自己的那个人", 1,  1 },
  { "周末没有安排时，你会主动约人出去", 1,  1 },
  { "长时间和人相处后，你需要独处一阵才能恢复", 1, -1 },
  { "讨论时你更习惯先听完再发言，不太抢话", 1, -1 },
  { "电话响起时，你有时会先看一眼是谁再接", 1, -1 },
  { "热闹的场合你也喜欢，但常常想早点离开", 1, -1 },
  { "复杂的想法，你更愿意用文字说，而不是当面讲", 1, -1 },

  -- 维度 2: S / N
  { "你更相信亲眼看到、亲手做过的事", 2,  1 },
  { "学新东西时，你喜欢先看具体例子，再自己总结", 2,  1 },
  { "别人换了发型、房间挪了家具，你很快就会注意到", 2,  1 },
  { "做事时你更关心眼前这一步怎么做好", 2,  1 },
  { "你更喜欢明确、能照着做的指令，而不是开放的方向", 2,  1 },
  { "做事时你常一边做一边想，如果换成另一种会怎样", 2, -1 },
  { "你对抽象的概念、比喻和可能性很感兴趣", 2, -1 },
  { "别人说过你想得太多，或者容易跑题", 2, -1 },
  { "事情还没发生，你就已经在脑子里预演各种可能", 2, -1 },
  { "比起细节，你更容易记住整件事给人的感觉", 2, -1 },

  -- 维度 3: T / F
  { "做决定时，你先把利弊列清楚，再考虑感受", 3,  1 },
  { "朋友向你倾诉，你容易先给出解决办法", 3,  1 },
  { "别人指出你的方案有问题，你更在意他说得对不对", 3,  1 },
  { "你觉得规则应该对所有人一样，例外越少越好", 3,  1 },
  { "争论时你能保持冷静，把观点和人分开", 3,  1 },
  { "看到别人难过，你会先被情绪带动，再想原因", 3, -1 },
  { "你很难对别人说不，即使自己已经很累", 3, -1 },
  { "评价一件事时，你更看重它有没有照顾到人", 3, -1 },
  { "团队气氛不对，你会先想办法让大家舒服一点", 3, -1 },
  { "为了顾及对方感受，你常把想说的话咽回去", 3, -1 },

  -- 维度 4: J / P
  { "出门前你习惯把行程和时间安排好", 4,  1 },
  { "事情没做完，你心里会一直惦记着", 4,  1 },
  { "你喜欢提前到，不喜欢踩着点", 4,  1 },
  { "你的东西通常有固定的位置", 4,  1 },
  { "清单和计划让你觉得安心", 4,  1 },
  { "你常临时改计划，觉得这样更有意思", 4, -1 },
  { "你习惯把事情拖到最后一刻再做", 4, -1 },
  { "房间乱一点你也能忍，只要找得到东西", 4, -1 },
  { "你更喜欢到时候再看，而不是现在就定死", 4, -1 },
  { "计划被打乱时，你反而觉得轻松", 4, -1 },
}

local OPT_TEXT = { "非常符合", "比较符合", "不确定", "不太符合", "完全不符合" }

local DIM_L = { "E", "S", "T", "J" }
local DIM_R = { "I", "N", "F", "P" }
local DIM_NAME = { "外向 / 内向", "实感 / 直觉", "思考 / 情感", "判断 / 感知" }

-- ---------------------------------------------------------------- 数据: 16 型
-- cat: 1 分析家 2 外交家 3 守护者 4 探险家
local TYPE = {
  INTJ = { name = "建筑师", cat = 1,
    tag  = "先想清楚，再动手",
    over = "习惯先在脑子里把事情推演一遍，再决定要不要动手。对目标和效率敏感，不太在意场面上的热络。",
    tr   = "独立 / 长期视角 / 对低效没耐心",
    str  = "能把复杂问题拆成可执行的步骤；做决定前会自己先验证；长期目标不容易被短期诱惑带走。",
    weak = "容易把还没想清楚当成不沟通的理由；对别人的情绪需求反应偏慢。",
    comm = "说结论、给依据，最怕绕圈子。希望对方把问题说具体，而不是只说感觉不对。",
    work = "适合需要独立推进、目标明确、允许自己定方法的活。反复被打断和无准备的会议最消耗。",
    rel  = "关系里重承诺而不是重热闹。不常说，但会用帮你把事情解决掉来表达在意。",
    grow = "把先说出来再修正练成习惯；重要的人需要的是回应，不是最优解。" },

  INTP = { name = "逻辑学家", cat = 1,
    tag  = "先把道理讲通",
    over = "对为什么的兴趣大于怎么办。喜欢把概念拆开、找出矛盾，再拼回一个更顺的解释。",
    tr   = "好奇 / 爱较真 / 想法多落地少",
    str  = "能发现别人忽略的前提问题；学新领域快；不容易被权威说法唬住。",
    weak = "想得比做得多；对截止日期和流程细节天然不敏感。",
    comm = "喜欢精确表达，会顺手纠正不严谨的说法，容易被误解成挑刺。",
    work = "适合研究、设计、排查类工作，需要有整块不被打扰的时间。",
    rel  = "用分享想法来靠近别人，不太擅长主动关心日常琐事。",
    grow = "给想法设一个必须先做出来的最小版本；纠正别人前先确认对方要不要听。" },

  ENTJ = { name = "指挥官", cat = 1,
    tag  = "定方向，扛结果",
    over = "看到目标就先想路径和资源，习惯把人和事排到合适的位置上。",
    tr   = "目标导向 / 决断快 / 不怕冲突",
    str  = "能在混乱里迅速定方向；敢做不受欢迎但必要的决定；推动力强。",
    weak = "节奏太快，容易替别人做决定；对慢的容忍度低。",
    comm = "直接、给结论、要时间点。不喜欢含糊的再看看。",
    work = "适合带项目、定规则、扛结果的岗位；重复且没有决策空间的工作会很快厌倦。",
    rel  = "用解决问题和安排妥当来表达关心，偶尔需要提醒自己先听。",
    grow = "决策前多问一句你怎么看；把我说了算换成我们定个标准。" },

  ENTP = { name = "辩论家", cat = 1,
    tag  = "换个角度看问题",
    over = "点子多，喜欢把一个问题的前提翻过来看，享受讨论本身。",
    tr   = "反应快 / 爱试探边界 / 兴趣转移快",
    str  = "能快速找到新角度；擅长说服和破局；不怕被质疑。",
    weak = "开很多头，收尾少；有时为了讨论有趣而抬杠。",
    comm = "喜欢来回抛观点，语速快、跳跃大。对方认真时容易觉得被冒犯。",
    work = "适合需要创意、谈判、开新方向的场景；长期维护型工作容易失去兴趣。",
    rel  = "用玩笑和新鲜感维持关系，冲突后不太记仇。",
    grow = "每次只留一个想法做到能交付；先确认对方在讨论还是在求助。" },

  INFJ = { name = "提倡者", cat = 2,
    tag  = "看见别人没说出口的",
    over = "对人的状态很敏感，常在别人开口前就察觉不对。心里有一套长期坚持的价值标准。",
    tr   = "洞察 / 长期主义 / 边界感弱",
    str  = "能听懂话外之意；愿意为在乎的事长期投入；给人稳定感。",
    weak = "容易过度承担别人的情绪；不擅长在被消耗时及时说不。",
    comm = "委婉、留余地，希望对方自己领会。遇到直来直去的人会退回去。",
    work = "适合有意义感、能影响具体人的工作；纯指标驱动且冷漠的环境消耗很大。",
    rel  = "少数深关系，投入很深。容易先照顾对方，再发现自己委屈。",
    grow = "把我需要什么说出口；帮忙之前先确认自己还有余量。" },

  INFP = { name = "调停者", cat = 2,
    tag  = "心里有一把自己的尺",
    over = "内在价值感很强，表面随和，碰到原则问题会异常坚持。",
    tr   = "共情 / 理想主义 / 内耗多",
    str  = "能理解复杂的情绪；写和表达有个人风格；对不公敏感。",
    weak = "容易把批评当成否定；想很多但迟迟不行动。",
    comm = "温和、不爱争，习惯用文字表达；不满时常先沉默。",
    work = "适合能自主、有价值认同的创作或助人型工作；强竞争和硬指标会耗掉热情。",
    rel  = "忠诚、专一，期待被理解而不是被改造。",
    grow = "把我不够好换成这件事还能改哪里；给情绪一个出口，别只在心里演。" },

  ENFJ = { name = "主人公", cat = 2,
    tag  = "把大家拢到一起",
    over = "天然关注别人的成长和气氛，习惯把大家拢到一起。",
    tr   = "热心 / 会带动 / 容易过度付出",
    str  = "能让团队愿意往前走；善于发现别人的长处；沟通有温度。",
    weak = "太在意评价；把自己的需求排到最后。",
    comm = "主动、热情、会给反馈。被冷淡回应时容易反复揣测。",
    work = "适合带人、协调、对外沟通的角色；长期做纯幕后会缺成就感。",
    rel  = "投入、会照顾细节，也容易替对方规划。",
    grow = "允许自己这次不帮忙；把注意力从别人怎么看我挪回我要什么。" },

  ENFP = { name = "竞选者", cat = 2,
    tag  = "被可能性点燃",
    over = "对新鲜的可能很敏感，容易被一个想法点燃，也容易同时点燃好几个。",
    tr   = "热情 / 想象力强 / 收尾弱",
    str  = "能快速建立连接；给团队带来活力；善于看到人的潜力。",
    weak = "兴趣转移快；承诺给得太快，兑现压力大。",
    comm = "表达丰富、爱分享，情绪来得快去得也快。",
    work = "适合需要创意、变化和人的场景；重复流程会迅速消耗。",
    rel  = "关系里主动、热烈，也需要空间和新鲜感。",
    grow = "答应前先看日程；挑一件正在做的事，把它做完再开新的。" },

  ISTJ = { name = "物流师", cat = 3,
    tag  = "答应的事就做完",
    over = "相信秩序和可验证的事实，答应的事会按部就班做完。",
    tr   = "可靠 / 重细节 / 不喜欢临时变动",
    str  = "执行稳定；记得住流程和细节；承诺兑现率高。",
    weak = "对变化和模糊容忍度低；容易把以前都这样当理由。",
    comm = "说事实、给步骤，不喜欢夸张和空话。",
    work = "适合需要准确、流程清晰、可追责的工作。",
    rel  = "用做事和守约表达在意，不太说感受。",
    grow = "变化来临时先问要保住的核心是什么；把关心说出口一次。" },

  ISFJ = { name = "守卫者", cat = 3,
    tag  = "默默把事做好",
    over = "注意别人的具体需要，习惯默默把事做好，不主动邀功。",
    tr   = "细致 / 体贴 / 不擅拒绝",
    str  = "能记住别人的偏好；做事稳；在团队里是黏合剂。",
    weak = "容易过度承担；被忽视时不说，攒到最后一起失望。",
    comm = "温和、委婉，倾向先顺着对方。",
    work = "适合服务、支持、需要耐心和准确的工作。",
    rel  = "长情、照顾细节，期待被看见而不是被要求。",
    grow = "练习这次我做不到；把付出说出来，别等对方猜。" },

  ESTJ = { name = "总经理", cat = 3,
    tag  = "把规则和进度摆平",
    over = "看重秩序和效率，习惯把规则讲清楚、把责任落到人。",
    tr   = "直接 / 讲效率 / 重规则",
    str  = "能迅速组织资源；判断果断；说到做到。",
    weak = "容易显得强硬；对不同的做事方式耐心不足。",
    comm = "直给结论和要求，不喜欢铺垫。",
    work = "适合管理、运营、需要拍板和推进的岗位。",
    rel  = "用承担和安排表达责任，柔软的话比较少。",
    grow = "下结论前多听三十秒；把应该换成你更希望怎样。" },

  ESFJ = { name = "执政官", cat = 3,
    tag  = "让大家处得舒服",
    over = "在意关系和氛围，愿意花力气让大家都舒服。",
    tr   = "热心 / 重和谐 / 在意评价",
    str  = "会照顾人；能维系关系网；执行力不差。",
    weak = "容易为了和气委屈自己；对负面评价敏感。",
    comm = "主动、体贴、会给回应。",
    work = "适合需要与人打交道、维护关系的岗位。",
    rel  = "投入、会记挂细节，也期待对等回应。",
    grow = "允许关系里出现分歧；把别人会怎么想放低一点。" },

  ISTP = { name = "鉴赏家", cat = 4,
    tag  = "先上手再说",
    over = "遇事冷静，喜欢上手拆解，靠手感解决问题。",
    tr   = "冷静 / 动手强 / 不爱解释",
    str  = "现场解决问题快；对工具有直觉；压力下不慌。",
    weak = "不爱提前沟通；对长期承诺谨慎到回避。",
    comm = "话少、说重点，不喜欢情绪化的长谈。",
    work = "适合技术、维修、现场处置类工作。",
    rel  = "用行动表达，不太说；需要自己的空间。",
    grow = "提前说一句计划，让别人不必猜；把没事换成具体状态。" },

  ISFP = { name = "探险家", cat = 4,
    tag  = "按自己的节奏来",
    over = "对当下的感受和美感敏感，喜欢按自己的节奏生活。",
    tr   = "温和 / 审美强 / 回避冲突",
    str  = "能感知细微气氛；做事有自己的风格；对人友善。",
    weak = "不擅长争取；冲突面前容易退。",
    comm = "温和、少争辩，用行动和作品表达。",
    work = "适合有审美、能自主节奏的工作。",
    rel  = "体贴、不压迫，需要被尊重边界。",
    grow = "练习把需求说清楚；冲突不等于关系破裂。" },

  ESTP = { name = "企业家", cat = 4,
    tag  = "先试了再说",
    over = "反应快，喜欢现场感和即时反馈，敢先试再说。",
    tr   = "行动派 / 敢冒险 / 不耐烦",
    str  = "抓机会快；临场应变强；不怕被拒绝。",
    weak = "容易低估风险；对长期枯燥的事没耐心。",
    comm = "直接、生动，喜欢当场解决。",
    work = "适合销售、谈判、现场运营类工作。",
    rel  = "热情、会制造乐趣，也需要自由。",
    grow = "决定前留十分钟想后果；把一件长期的事拆成短期关卡。" },

  ESFP = { name = "表演者", cat = 4,
    tag  = "把当下过热",
    over = "热情外放，享受当下，也擅长把气氛带起来。",
    tr   = "外向 / 感染力强 / 回避沉重",
    str  = "让人放松；对情绪敏感；行动力不差。",
    weak = "容易回避长期问题；计划性弱。",
    comm = "直接、有情绪、爱互动。",
    work = "适合面对人、需要现场表现的工作。",
    rel  = "热烈、需要陪伴，不喜欢冷处理。",
    grow = "给未来留一件必须做的小事；难受时说清楚，而不是换个话题。" },
}

local CAT_NAME = { "分析家", "外交家", "守护者", "探险家" }
local CAT_CODES = {
  { "INTJ", "INTP", "ENTJ", "ENTP" },
  { "INFJ", "INFP", "ENFJ", "ENFP" },
  { "ISTJ", "ISFJ", "ESTJ", "ESFJ" },
  { "ISTP", "ISFP", "ESTP", "ESFP" },
}

-- ---------------------------------------------------------------- 状态
local S = {
  page   = "home",
  cur    = 1,                 -- 当前第几题 (1..40)
  ans    = {},                -- [1..40] = 0 / 1..5
  result = nil,               -- { code, p={..}, sum={..} }
  hist   = {},                -- { {t=epoch, code=, p={}} ... } 新在前
  read   = nil,               -- 只读查看的历史条目
  gi     = 1,                 -- 图鉴类别 1..4
  gsel   = 0,                 -- 图鉴选中卡片 1..4
  hsel   = 0,                 -- 历史选中行
  dcode  = nil,               -- 详情页看的是哪一型
  dfrom  = "result",          -- 详情页从哪来 (result / gallery)
  dlines = nil,               -- 详情页折行缓存
  dpages = nil,               -- 每页起始行号 (按小标题对齐)
  dpage  = 1,                 -- 当前第几页
  card   = false,             -- 分享卡
  pending = nil,              -- 等弹窗结果的动作
  saved  = false,             -- 本次结果是否已入历史
  t0     = 0,
}

-- ---------------------------------------------------------------- 小工具
local function clamp(v, lo, hi)
  if v < lo then return lo elseif v > hi then return hi else return v end
end

local function inrect(x, y, x0, y0, x1, y1)
  return x >= x0 and x <= x1 and y >= y0 and y <= y1
end

-- 按像素宽折行 (16px 混排)。只在进入页面时算一次, 不放在 render 里。
local function wrap(s, maxw)
  local out, line, lw = {}, {}, 0
  local i, n = 1, #s
  while i <= n do
    local b = s:byte(i)
    local step = 1
    if b >= 0xF0 then step = 4 elseif b >= 0xE0 then step = 3
    elseif b >= 0xC0 then step = 2 end
    local ch = s:sub(i, i + step - 1)
    local cw = app.mixw(ch)
    if lw + cw > maxw and #line > 0 then
      out[#out + 1] = table.concat(line)
      line, lw = {}, 0
    end
    line[#line + 1] = ch
    lw = lw + cw
    i = i + step
  end
  if #line > 0 then out[#out + 1] = table.concat(line) end

  -- 行首禁则: 句号/逗号这类标点不许单独落到下一行行首
  local PUNCT = { ["\xE3\x80\x82"] = true, ["\xEF\xBC\x8C"] = true,   -- 。 ，
                  ["\xE3\x80\x81"] = true, ["\xEF\xBC\x9B"] = true,   -- 、 ；
                  ["\xEF\xBC\x9A"] = true, ["\xEF\xBC\x81"] = true,   -- ： ！
                  ["\xEF\xBC\x9F"] = true, ["\xEF\xBC\x89"] = true,   -- ？ ）
                  ["\xE3\x80\x8B"] = true }                              -- 》
  for i = 1, #out - 1 do
    local nxt = out[i + 1]
    if #nxt >= 3 and PUNCT[nxt:sub(1, 3)] then
      local prev = out[i]
      local k = #prev
      while k > 1 do
        local b = prev:byte(k)
        if b >= 0x80 and b < 0xC0 then k = k - 1 else break end
      end
      if k > 1 then
        out[i]   = prev:sub(1, k - 1)
        out[i+1] = prev:sub(k) .. nxt
      end
    end
  end
  return out
end

local function ellipsis(s, maxw)
  local i, n, w = 1, #s, 0
  while i <= n do
    local b = s:byte(i)
    local step = 1
    if b >= 0xF0 then step = 4 elseif b >= 0xE0 then step = 3
    elseif b >= 0xC0 then step = 2 end
    local ch = s:sub(i, i + step - 1)
    local cw = app.mixw(ch)
    if w + cw > maxw - 16 then return s:sub(1, i - 1) .. ".." end
    w = w + cw
    i = i + step
  end
  return s
end

-- epoch 秒 -> "MM-DD" (纯整数, 不依赖 os.date)
local function fmt_date(ep)
  local t = tonumber(ep) or 0
  if t < 100000 then return "--/--" end
  local days = math.floor(t / 86400)
  local z = days + 719468
  local era = math.floor(z / 146097)
  local doe = z - era * 146097
  local yoe = math.floor((doe - math.floor(doe / 1460) + math.floor(doe / 36524)
                          - math.floor(doe / 146096)) / 365)
  local doy = doe - (365 * yoe + math.floor(yoe / 4) - math.floor(yoe / 100))
  local mp = math.floor((5 * doy + 2) / 153)
  local d = doy - math.floor((153 * mp + 2) / 5) + 1
  local m = mp + ((mp < 10) and 3 or -9)
  return string.format("%02d-%02d", m, d)
end

-- ---------------------------------------------------------------- 存储
local function nvs_set(k, v)
  local ok, r = pcall(function() return app.nvs.set_str(k, v) end)
  return ok and (r ~= false)
end

local function nvs_get(k)
  local ok, v = pcall(function() return app.nvs.get_str(k) end)
  if ok then return v end
  return nil
end

local function save_prog()
  local t = {}
  for i = 1, #Q do t[i] = tostring(S.ans[i] or 0) end
  nvs_set("p16_prog", table.concat(t))
  nvs_set("p16_cur", tostring(S.cur))
end

local function answered_count()
  local n = 0
  for i = 1, #Q do if (S.ans[i] or 0) > 0 then n = n + 1 end end
  return n
end

local function load_hist()
  S.hist = {}
  local s = nvs_get("p16_hist")
  if type(s) ~= "string" or s == "" then return end
  for line in string.gmatch(s, "[^\n]+") do
    local ep, code, p = string.match(line, "^(%d+)|(%u%u%u%u)|([%d,]+)$")
    if ep and code and TYPE[code] then
      local pp = {}
      local k = 1
      for v in string.gmatch(p, "%d+") do pp[k] = tonumber(v); k = k + 1 end
      if #pp == 4 then
        S.hist[#S.hist + 1] = { t = ep, code = code, p = pp }
      end
    end
  end
end

local function push_hist(res)
  local ep = "0"
  local ok, e = pcall(function() return app.util.epoch() end)
  if ok and type(e) == "string" then ep = e end
  table.insert(S.hist, 1, { t = ep, code = res.code,
                            p = { res.p[1], res.p[2], res.p[3], res.p[4] } })
  while #S.hist > 12 do table.remove(S.hist) end
  local rows = {}
  for i = 1, #S.hist do
    local h = S.hist[i]
    rows[#rows + 1] = string.format("%s|%s|%d,%d,%d,%d",
      h.t, h.code, h.p[1], h.p[2], h.p[3], h.p[4])
  end
  nvs_set("p16_hist", table.concat(rows, "\n"))
  nvs_set("p16_last", res.code)
end

local function load_prog()
  local s = nvs_get("p16_prog")
  S.ans = {}
  for i = 1, #Q do S.ans[i] = 0 end
  if type(s) == "string" then
    for i = 1, math.min(#Q, #s) do
      local v = tonumber(string.sub(s, i, i)) or 0
      S.ans[i] = (v >= 1 and v <= 5) and v or 0
    end
  end
  local c = tonumber(nvs_get("p16_cur") or "0") or 0
  S.cur = (c >= 1 and c <= #Q) and c or 1
  -- 定位到第一个未答的题, 方便继续
  if answered_count() > 0 then
    for i = 1, #Q do
      if (S.ans[i] or 0) == 0 then S.cur = i; break end
    end
  end
end

-- ---------------------------------------------------------------- 计分
-- 返回 { code, p={E,S,T,J 百分比}, sum={..}, done }
local function score(ans)
  local sum = { 0, 0, 0, 0 }
  local done = 0
  for i = 1, #Q do
    local a = ans[i] or 0
    if a >= 1 and a <= 5 then
      done = done + 1
      local d, dir = Q[i][2], Q[i][3]
      sum[d] = sum[d] + (3 - a) * dir
    end
  end
  local p = {}
  for d = 1, 4 do
    local v = 50 + sum[d] * 5 / 2
    v = math.floor(v + 0.5)
    p[d] = clamp(v, 0, 100)
  end
  local code = (p[1] >= 50 and "E" or "I")
            .. (p[2] >= 50 and "S" or "N")
            .. (p[3] >= 50 and "T" or "F")
            .. (p[4] >= 50 and "J" or "P")
  return { code = code, p = p, sum = sum, done = done }
end

local function weak_dim(p)
  return math.abs(p - 50) <= 5
end

-- ---------------------------------------------------------------- 绘图原语
local function line(x0, y0, x1, y1)
  local dx, sx = math.abs(x1 - x0), (x0 < x1) and 1 or -1
  local dy, sy = -math.abs(y1 - y0), (y0 < y1) and 1 or -1
  local err = dx + dy
  while true do
    app.px(x0, y0, BK)
    if x0 == x1 and y0 == y1 then break end
    local e2 = 2 * err
    if e2 >= dy then err = err + dy; x0 = x0 + sx end
    if e2 <= dx then err = err + dx; y0 = y0 + sy end
  end
end

local function circle(cx, cy, r)
  local x, y, d = r, 0, 1 - r
  while x >= y do
    app.px(cx + x, cy + y, BK); app.px(cx + y, cy + x, BK)
    app.px(cx - y, cy + x, BK); app.px(cx - x, cy + y, BK)
    app.px(cx - x, cy - y, BK); app.px(cx - y, cy - x, BK)
    app.px(cx + y, cy - x, BK); app.px(cx + x, cy - y, BK)
    y = y + 1
    if d < 0 then d = d + 2 * y + 1
    else x = x - 1; d = d + 2 * (y - x) + 1 end
  end
end

-- 每型一个几何徽章: 类别决定外框形状, 中间放 4 字母代码。
-- 全部用图元现场画, 不依赖任何图片资源 (原创)。
local function emblem(cx, cy, r, cat)
  if cat == 1 then                      -- 分析家: 菱形
    line(cx, cy - r, cx + r, cy); line(cx + r, cy, cx, cy + r)
    line(cx, cy + r, cx - r, cy); line(cx - r, cy, cx, cy - r)
    line(cx, cy - r + 4, cx + r - 4, cy); line(cx + r - 4, cy, cx, cy + r - 4)
    line(cx, cy + r - 4, cx - r + 4, cy); line(cx - r + 4, cy, cx, cy - r + 4)
  elseif cat == 2 then                  -- 外交家: 双圆
    circle(cx, cy, r); circle(cx, cy, r - 4)
  elseif cat == 3 then                  -- 守护者: 双框
    app.rect(cx - r, cy - r, cx + r, cy + r, BK)
    app.rect(cx - r + 4, cy - r + 4, cx + r - 4, cy + r - 4, BK)
  else                                  -- 探险家: 三角
    line(cx, cy - r, cx + r, cy + r); line(cx + r, cy + r, cx - r, cy + r)
    line(cx - r, cy + r, cx, cy - r)
    line(cx, cy - r + 6, cx + r - 5, cy + r - 4)
    line(cx + r - 5, cy + r - 4, cx - r + 5, cy + r - 4)
    line(cx - r + 5, cy + r - 4, cx, cy - r + 6)
  end
  -- 中间 2x2 四点: 代表四个维度, 任何形状里都放得下
  local d  = (r >= 30) and 7 or 4
  local sz = (r >= 30) and 8 or 4
  app.fill(cx - d - sz, cy - d - sz, cx - d - 1,     cy - d - 1,     BK)
  app.fill(cx + d,      cy - d - sz, cx + d + sz - 1, cy - d - 1,     BK)
  app.fill(cx - d - sz, cy + d,      cx - d - 1,     cy + d + sz - 1, BK)
  app.fill(cx + d,      cy + d,      cx + d + sz - 1, cy + d + sz - 1, BK)
end

local function button(x0, y0, x1, y1, label, inv)
  if inv then app.fill(x0, y0, x1, y1, BK) end
  app.rect(x0, y0, x1, y1, BK)
  local tw = app.mixw(label)
  app.mix(x0 + (x1 - x0 + 1 - tw) // 2, y0 + (y1 - y0 + 1 - 16) // 2, label, inv)
end

local function title(txt, right)
  app.fill(0, 0, W - 1, TOP - 1, WH)
  app.mix(10, 4, txt, false)
  if right then
    local tw = app.mixw(right)
    app.mix(W - 10 - tw, 4, right, false)
  end
  app.hline(0, W - 1, TOP - 1, BK)
end

-- 倾向条: 左端为第一个字母, 右端为第二个; 中间刻度 50%
local function dim_bar(y, d, p)
  local l, rr = DIM_L[d], DIM_R[d]
  local p1 = p[d]
  local lab = string.format("%s %d%%", l, p1)
  app.mix(12, y + 2, lab, false)
  local bx0, bx1 = 86, 292
  app.rect(bx0, y + 4, bx1, y + 17, BK)
  local mid = (bx0 + bx1) // 2
  local fillw = (bx1 - bx0) * p1 // 100
  if fillw > 2 then app.fill(bx0 + 1, y + 5, bx0 + fillw - 1, y + 16, BK) end
  -- 中线刻度 (画在填充之后, 保证任何比例都看得见)
  local yy
  for yy = y + 3, y + 18, 2 do app.px(mid, yy, BK) end
  local rl = string.format("%d%% %s", 100 - p1, rr)
  app.mix(bx1 + 8, y + 2, rl, false)
  if weak_dim(p1) then app.mix(W - 24, y + 2, "*", false) end
end

-- ---------------------------------------------------------------- 页面: 首页
local function has_progress()
  local n = answered_count()
  return n > 0 and n < #Q
end

local function home_buttons()
  local b = {}
  local n = answered_count()
  if n > 0 and n < #Q then
    b[#b + 1] = { "继续测评", "quiz" }
  else
    b[#b + 1] = { "开始测评", "start" }
  end
  b[#b + 1] = { "人格图鉴", "gallery" }
  b[#b + 1] = { "历史记录", "history" }
  b[#b + 1] = { "设置", "settings" }
  return b
end

local function draw_home()
  app.clear(WH)
  title("人格诊断", "v1")

  -- 左: 徽章 / 最近结果
  local cx, cy = 66, 96
  local last = nvs_get("p16_last")
  if type(last) == "string" and TYPE[last] then
    emblem(cx, cy - 6, 38, TYPE[last].cat)
    local nm = TYPE[last].name
    local tw = app.mixw(nm)
    app.mix(cx - tw // 2, 148, nm, false)
    local tw2 = app.mixw(last)
    app.mix(cx - tw2 // 2, 168, last, false)
  else
    emblem(cx, cy - 6, 38, 2)
    app.mixc(cx, 148, "16 型 40 题", false)
  end

  -- 右: 按钮
  local bs = home_buttons()
  local y = 40
  for i = 1, #bs do
    button(146, y, 388, y + 32, bs[i][1], false)
    y = y + 42
  end

  app.mix(14, 200, "40 道题，按第一感觉选", false)
  app.mix(14, 224, "不提示对错，也没有标准答案", false)
  app.hline(12, W - 12, 250, BK)
  app.mix(14, 258, "结果只是偏好倾向，不构成心理诊断", false)
  app.mix(14, 278, "也不要把它当成固定命运", false)
end

-- ---------------------------------------------------------------- 页面: 答题
local function draw_quiz()
  app.clear(WH)
  title("第 " .. S.cur .. " / " .. #Q .. " 题", "已答 " .. answered_count())

  -- 进度条
  app.rect(10, 27, W - 11, 33, BK)
  local pw = (W - 22) * (S.cur - 1) // #Q
  if pw > 1 then app.fill(11, 28, 10 + pw, 32, BK) end

  -- 题干 (最多 4 行): 在 40..112 之间垂直居中, 一行题也不会贴着上边
  local lines = wrap(Q[S.cur][1], W - 24)
  local n = math.min(4, #lines)
  local qy = 40 + ((72 - n * LINE_H) // 2)
  if qy < 40 then qy = 40 end
  for i = 1, n do
    app.mix(12, qy + (i - 1) * LINE_H, lines[i], false)
  end

  -- 五档
  for i = 1, 5 do
    local y = OPT_Y + (i - 1) * OPT_GAP
    local sel = (S.ans[S.cur] == i)
    if sel then app.fill(10, y, W - 11, y + OPT_H - 1, BK) end
    app.rect(10, y, W - 11, y + OPT_H - 1, BK)
    app.mix(24, y + 5, OPT_TEXT[i], sel)
  end

  -- 底部: 上一题 / 下一题
  button(10, 268, 118, 292, "上一题", false)
  local done = answered_count()
  local lbl = (done >= #Q and S.cur == #Q) and "生成结果" or "下一题"
  button(282, 268, 390, 292, lbl, done >= #Q)
end

-- ---------------------------------------------------------------- 页面: 结果
local function draw_result()
  app.clear(WH)
  title("测评结果", S.read and fmt_date(S.read.t) or nil)
  local r = S.read and { code = S.read.code, p = S.read.p } or S.result
  if not r then S.page = "home"; return end
  local tp = TYPE[r.code]

  emblem(54, 74, 36, tp.cat)
  app.text(94, 38, r.code, false)
  app.mix(94, 70, tp.name, false)
  app.zhsm(94, 94, CAT_NAME[tp.cat])
  app.mix(94, 92, "", false)

  -- 一句话定位 (完整概述去"完整分析"里看, 这里不挤两行)
  app.mix(12, 116, ellipsis(tp.tag, W - 24), false)

  for d = 1, 4 do dim_bar(142 + (d - 1) * 26, d, r.p) end

  -- 维度落在 45..55 就提示"倾向较弱"; 四个都弱时直接劝重测, 不硬给结论
  local nweak = 0
  for d = 1, 4 do if weak_dim(r.p[d]) then nweak = nweak + 1 end end
  if nweak == 4 then
    app.mix(12, 240, "四个维度都接近中间，建议重测", false)
  elseif nweak > 0 then
    app.mix(12, 240, "带 * 的维度偏向较弱，别当成定论", false)
  end

  if not S.read then
    button(8, 262, 104, 292, "完整分析", false)
    button(110, 262, 200, 292, "换个人格", false)
    button(206, 262, 288, 292, S.saved and "已保存" or "保存", S.saved)
    button(294, 262, 392, 292, "重测", false)
  else
    button(8, 262, 120, 292, "完整分析", false)
    button(126, 262, 238, 292, "回到历史", false)
    button(244, 262, 392, 292, "返回首页", false)
  end
end

-- ---------------------------------------------------------------- 页面: 详情
local DET = {
  { "概述", "over" }, { "特点", "tr" }, { "优势", "str" }, { "需要注意", "weak" },
  { "沟通方式", "comm" }, { "工作偏好", "work" }, { "人际关系", "rel" },
  { "成长建议", "grow" },
}

local DPAGE_ROWS = 12          -- 详情页一页几行 (12 x 20 = 240, 留出顶栏与底栏)

local function build_detail(code)
  local tp = TYPE[code]
  local L = {}
  for i = 1, #DET do
    L[#L + 1] = { h = DET[i][1] }
    local ls = wrap(tp[DET[i][2]], W - 24)
    for k = 1, #ls do L[#L + 1] = { t = ls[k] } end
    L[#L + 1] = { gap = true }
  end
  -- 分页: 尽量从"小标题"开新页, 不让一段话被拦腰切开
  local pages = { 1 }
  local i = 1
  while i <= #L do
    local nxt = i + DPAGE_ROWS
    if nxt > #L then break end
    local j = nxt
    while j > i + 1 and not (L[j] and L[j].h) do j = j - 1 end
    if L[j] and L[j].h then
      pages[#pages + 1] = j
      i = j
    else
      pages[#pages + 1] = nxt
      i = nxt
    end
  end
  return { lines = L, pages = pages }
end

local function draw_detail()
  app.clear(WH)
  local tp = TYPE[S.dcode]
  title(S.dcode .. " " .. tp.name, nil)
  local D = S.dlines or { lines = {}, pages = { 1 } }
  local L, PG = D.lines, D.pages
  local pi = clamp(S.dpage, 1, #PG)
  local from = PG[pi] or 1
  local to   = math.min(#L, from + DPAGE_ROWS - 1)
  if pi < #PG then to = math.min(to, PG[pi + 1] - 1) end

  local pgs = string.format("%d/%d", pi, #PG)
  app.mix(W - 10 - app.mixw(pgs), 4, pgs, false)

  local y = 30
  for i = from, to do
    local it = L[i]
    if it.h then
      app.mix(12, y, it.h, false)
      app.hline(12, 12 + app.mixw(it.h), y + 17, BK)
    elseif it.t then
      app.mix(18, y, it.t, false)
    end
    y = y + LINE_H
  end
  if to < from then app.mix(12, 40, "没有内容", false) end

  app.hline(0, W - 1, H - FOOT - 2, BK)
  app.mix(10, H - FOOT + 2, "左右翻页  返回", false)
end

-- ---------------------------------------------------------------- 页面: 图鉴
local function draw_gallery()
  app.clear(WH)
  title("人格图鉴", "第 " .. S.gi .. "/4 类")

  -- 类别页签
  for i = 1, 4 do
    local x0 = 4 + (i - 1) * 99
    local on = (i == S.gi)
    if on then app.fill(x0, 27, x0 + 94, 47, BK) end
    local tw = app.mixw(CAT_NAME[i])
    app.mix(x0 + (94 - tw) // 2, 29, CAT_NAME[i], on)
  end

  -- 2x2 卡片
  local codes = CAT_CODES[S.gi]
  for i = 1, 4 do
    local cx0 = (i % 2 == 1) and 6 or 206
    local cy0 = (i <= 2) and 54 or 158
    local sel = (S.gsel == i)
    if sel then app.rect(cx0 - 2, cy0 - 2, cx0 + 190, cy0 + 102, BK) end
    app.rect(cx0, cy0, cx0 + 188, cy0 + 100, BK)
    local code = codes[i]
    emblem(cx0 + 34, cy0 + 34, 24, S.gi)
    app.text(cx0 + 70, cy0 + 14, code, false)
    app.mix(cx0 + 70, cy0 + 44, TYPE[code].name, false)
    app.mix(cx0 + 70, cy0 + 66, ellipsis(TYPE[code].tag, 112), false)
  end

  app.hline(0, W - 1, H - FOOT - 2, BK)
  app.mix(10, H - FOOT + 2, "点卡片看详情  左右换类别  返回", false)
end

-- ---------------------------------------------------------------- 页面: 历史
local function draw_history()
  app.clear(WH)
  title("历史记录", #S.hist .. " 条")
  if #S.hist == 0 then
    app.mix(14, 70, "还没有记录", false)
    app.mix(14, 96, "做完一次测评就会出现在这里", false)
    app.hline(0, W - 1, H - FOOT - 2, BK)
    app.mix(10, H - FOOT + 2, "返回", false)
    return
  end

  local y = 30
  for i = 1, math.min(#S.hist, 8) do
    local h = S.hist[i]
    local tp = TYPE[h.code]
    if S.hsel == i then app.fill(2, y - 2, W - 3, y + 26, BK) end
    local inv = (S.hsel == i)
    app.mix(8, y + 4, fmt_date(h.t), inv)
    app.mix(80, y + 4, h.code, inv)
    app.mix(140, y + 4, tp.name, inv)
    app.mix(230, y + 4, string.format("%d/%d/%d/%d", h.p[1], h.p[2], h.p[3], h.p[4]), inv)
    -- 删除叉: 点它只删这一条
    app.mix(W - 20, y + 4, "x", inv)
    y = y + 30
  end

  app.hline(0, W - 1, H - FOOT - 2, BK)
  app.mix(10, H - FOOT + 2, "点按查看  点 x 删除  返回", false)
end

-- ---------------------------------------------------------------- 页面: 设置
local function draw_settings()
  app.clear(WH)
  title("设置", "关于")

  app.mix(14, 36, "题库", false)
  app.mix(90, 36, "40 题 / 四维度各 10 题", false)
  app.mix(14, 60, "量表", false)
  app.mix(90, 60, "五档 非常符合 到 完全不符合", false)
  app.mix(14, 84, "计分", false)
  app.mix(90, 84, "每题加权，维度出百分比", false)
  app.mix(14, 108, "存储", false)
  app.mix(90, 108, "全部在本机，不联网", false)

  app.hline(12, W - 12, 136, BK)
  app.mix(14, 146, "说明", false)
  local note = "这是个自我了解的小工具：题目和文案为本项目原创，"
            .. "结果只是偏好倾向，不构成心理测评或医学诊断，"
            .. "也不能用来评价一个人的能力。"
  local ls = wrap(note, W - 28)
  for i = 1, math.min(4, #ls) do
    app.mix(14, 168 + (i - 1) * LINE_H, ls[i], false)
  end

  button(10, 254, 190, 282, "清空答题进度", false)
  button(200, 254, 390, 282, "清空历史记录", false)
end

-- ---------------------------------------------------------------- 页面: 分享卡
local function draw_card()
  app.clear(WH)
  if not (S.result or S.read) then S.card = false; draw_home(); return end
  app.rect(6, 6, W - 7, H - 7, BK)
  app.rect(9, 9, W - 10, H - 10, BK)
  local r = S.result or (S.read and { code = S.read.code, p = S.read.p })
  if not r then return end
  local tp = TYPE[r.code]
  app.mix(24, 18, "人格诊断", false)
  app.mix(W - 24 - app.mixw("LinTOS"), 18, "LinTOS", false)
  app.hline(18, W - 19, 38, BK)

  emblem(70, 118, 40, tp.cat)
  app.text(122, 66, r.code, false)
  app.mix(122, 96, tp.name, false)
  app.zhsm(122, 118, CAT_NAME[tp.cat])
  app.mix(122, 136, tp.tag, false)

  for d = 1, 4 do
    local y = 158 + (d - 1) * 24
    local p1 = r.p[d]
    app.mix(150, y, string.format("%s %d", DIM_L[d], p1), false)
    app.rect(206, y + 4, 300, y + 15, BK)
    if p1 > 2 then app.fill(207, y + 5, 206 + (94 * p1 // 100), y + 14, BK) end
    app.mix(306, y, string.format("%d %s", 100 - p1, DIM_R[d]), false)
  end

  app.hline(18, W - 19, H - 34, BK)
  app.mix(24, H - 28, "倾向不是标签，只是一次自我观察", false)
end

-- ---------------------------------------------------------------- 渲染
local function draw()
  if S.card then draw_card(); return end
  if S.page == "home" then draw_home()
  elseif S.page == "quiz" then draw_quiz()
  elseif S.page == "result" then draw_result()
  elseif S.page == "detail" then draw_detail()
  elseif S.page == "gallery" then draw_gallery()
  elseif S.page == "history" then draw_history()
  elseif S.page == "settings" then draw_settings()
  else draw_home() end
end

function render()
  draw()
end

-- ---------------------------------------------------------------- 页面切换
local function open_result(r)
  S.result = r
  S.read = nil
  S.saved = false
  S.page = "result"
end

local function open_detail(code, from)
  S.dfrom = from or "result"
  S.dcode = code
  S.dlines = build_detail(code)
  S.dpage = 1
  S.page = "detail"
end

local function goto_page(p)
  if p == "start" then
    -- 开始新测评: 清掉旧答案
    for i = 1, #Q do S.ans[i] = 0 end
    S.cur = 1
    S.saved = false
    S.result = nil
    S.read = nil
    save_prog()
    S.page = "quiz"
  elseif p == "quiz" then
    S.read = nil
    S.page = "quiz"
  elseif p == "gallery" then
    S.page = "gallery"
  elseif p == "history" then
    load_hist()
    S.hsel = 0
    S.page = "history"
  elseif p == "settings" then
    S.page = "settings"
  else
    S.page = "home"
  end
end

-- ---------------------------------------------------------------- 交互
local function finish_quiz()
  local r = score(S.ans)
  if r.done < #Q then
    app.dlg.toast("还有 " .. (#Q - r.done) .. " 题没答", 1200)
    return
  end
  open_result(r)
  push_hist(r)
  S.saved = true
  app.dlg.toast("已生成并保存结果", 1200)
end

local function set_answer(i)
  S.ans[S.cur] = i
  S.cur = S.cur   -- 保持
  save_prog()
  -- 自动前进到下一道未答的题
  if S.cur < #Q then
    local nxt = S.cur + 1
    while nxt <= #Q and (S.ans[nxt] or 0) > 0 do nxt = nxt + 1 end
    if nxt <= #Q then S.cur = nxt end
  end
end

local function ask_delete(idx)
  S.pending = { kind = "del", idx = idx }
  app.dlg.confirm("删除这条记录?")
end

local function on_tap(x, y)
  if S.card then S.card = false; return end

  if S.page == "home" then
    local bs = home_buttons()
    local yy = 40
    for i = 1, #bs do
      if inrect(x, y, 146, yy, 388, yy + 32) then goto_page(bs[i][2]); return end
      yy = yy + 42
    end
    return
  end

  if S.page == "quiz" then
    for i = 1, 5 do
      local yy = OPT_Y + (i - 1) * OPT_GAP
      if inrect(x, y, 10, yy, W - 11, yy + OPT_H - 1) then
        set_answer(i)
        return
      end
    end
    if inrect(x, y, 10, 268, 118, 292) then
      if S.cur > 1 then S.cur = S.cur - 1; save_prog() end
      return
    end
    if inrect(x, y, 282, 268, 390, 292) then
      if S.cur < #Q then
        S.cur = S.cur + 1; save_prog()
      else
        finish_quiz()
      end
      return
    end
    return
  end

  if S.page == "result" then
    if not S.read then
      if inrect(x, y, 8, 262, 104, 292) then open_detail(S.result.code, "result"); return end
      if inrect(x, y, 110, 262, 200, 292) then goto_page("gallery"); return end
      if inrect(x, y, 206, 262, 288, 292) then
        if not S.saved and S.result then push_hist(S.result); S.saved = true
          app.dlg.toast("已保存", 900) else app.dlg.toast("已经在历史里了", 900) end
        return
      end
      if inrect(x, y, 294, 262, 392, 292) then goto_page("start"); return end
    else
      if inrect(x, y, 8, 262, 120, 292) then open_detail(S.read.code, "result"); return end
      if inrect(x, y, 126, 262, 238, 292) then S.read = nil; goto_page("history"); return end
      if inrect(x, y, 244, 262, 392, 292) then S.read = nil; goto_page("home"); return end
    end
    return
  end

  if S.page == "detail" then
    local np = S.dlines and #S.dlines.pages or 1
    if y < 26 then S.dpage = 1; return end        -- 点标题回第一页
    if y > H - FOOT - 2 then
      if S.dpage < np then S.dpage = S.dpage + 1 end
      return
    end
    if S.dpage > 1 then S.dpage = S.dpage - 1
    else
      if S.dfrom == "gallery" then S.page = "gallery"
      elseif S.read then S.page = "history"
      elseif S.result then S.page = "result"
      else S.page = "home" end
    end
    return
  end

  if S.page == "gallery" then
    for i = 1, 4 do
      local x0 = 4 + (i - 1) * 99
      if inrect(x, y, x0, 27, x0 + 94, 47) then S.gi = i; S.gsel = 0; return end
    end
    local codes = CAT_CODES[S.gi]
    for i = 1, 4 do
      local cx0 = (i % 2 == 1) and 6 or 206
      local cy0 = (i <= 2) and 54 or 158
      if inrect(x, y, cx0, cy0, cx0 + 188, cy0 + 100) then
        S.gsel = i
        open_detail(codes[i], "gallery")
        return
      end
    end
    return
  end

  if S.page == "history" then
    for i = 1, math.min(#S.hist, 8) do
      local yy = 30 + (i - 1) * 30
      if inrect(x, y, W - 30, yy - 2, W - 6, yy + 26) then ask_delete(i); return end
      if inrect(x, y, 2, yy - 2, W - 32, yy + 26) then
        local h = S.hist[i]
        S.hsel = i
        S.read = h
        S.result = nil
        S.page = "result"
        return
      end
    end
    return
  end

  if S.page == "settings" then
    if inrect(x, y, 10, 254, 190, 282) then
      S.pending = { kind = "clearp" }
      app.dlg.confirm("清空答题进度?")
      return
    end
    if inrect(x, y, 200, 254, 390, 282) then
      S.pending = { kind = "clearh" }
      app.dlg.confirm("清空全部历史记录?")
      return
    end
    return
  end
end

-- ---------------------------------------------------------------- 触摸轮询
local T = { down = false, x = 0, y = 0, x0 = 0, y0 = 0, t0 = 0, moved = false }

local function poll_touch()
  local x, y = app.touch()
  local now = app.millis()
  if type(x) == "number" then
    if not T.down then
      T.down, T.x, T.y, T.x0, T.y0, T.t0, T.moved = true, x, y, x, y, now, false
    else
      if math.abs(x - T.x0) > 12 or math.abs(y - T.y0) > 12 then T.moved = true end
      T.x, T.y = x, y
    end
  elseif T.down then
    T.down = false
    local dt = now - T.t0
    if (not T.moved) and dt < 900 then on_tap(T.x0, T.y0) end
  end
end

-- ---------------------------------------------------------------- 契约函数
function on_enter()
  S.t0 = app.millis()
  load_prog()
  load_hist()
  S.page = "home"
  S.saved = false
  S.result = nil
  S.read = nil
  S.card = false
end

function poll()
  poll_touch()

  if S.pending then
    local r = app.dlg.result()
    if r ~= nil then
      if r == 0 then
        local k = S.pending.kind
        if k == "del" then
          table.remove(S.hist, S.pending.idx)
          local rows = {}
          for i = 1, #S.hist do
            local h = S.hist[i]
            rows[#rows + 1] = string.format("%s|%s|%d,%d,%d,%d",
              h.t, h.code, h.p[1], h.p[2], h.p[3], h.p[4])
          end
          nvs_set("p16_hist", table.concat(rows, "\n"))
          S.hsel = 0
        elseif k == "clearp" then
          for i = 1, #Q do S.ans[i] = 0 end
          S.cur = 1
          save_prog()
          app.dlg.toast("进度已清空", 900)
        elseif k == "clearh" then
          S.hist = {}
          nvs_set("p16_hist", "")
          nvs_set("p16_last", "")
          app.dlg.toast("历史已清空", 900)
        end
      end
      S.pending = nil
      T.down = false
    end
  end
end

function action(a)
  if S.card then
    S.card = false
    return true
  end

  if a == app.BACK then
    if S.page == "home" then return false end
    if S.page == "detail" then
      if S.dfrom == "gallery" then S.page = "gallery"
      elseif S.read then S.page = "history"
      elseif S.result then S.page = "result"
      else S.page = "home" end
    elseif S.page == "result" then
      if S.read then S.read = nil; goto_page("history") else goto_page("home") end
    else
      goto_page("home")
    end
    return true
  end

  if S.page == "detail" then
    local np = S.dlines and #S.dlines.pages or 1
    if a == app.LEFT or a == app.UP then
      if S.dpage > 1 then S.dpage = S.dpage - 1 end
    elseif a == app.RIGHT or a == app.DOWN then
      if S.dpage < np then S.dpage = S.dpage + 1 end
    end
    return true
  end

  if S.page == "gallery" then
    if a == app.LEFT then S.gi = (S.gi + 2) % 4 + 1
    elseif a == app.RIGHT then S.gi = S.gi % 4 + 1
    elseif a == app.UP or a == app.DOWN then
      local d = (a == app.UP) and -1 or 1
      S.gsel = ((S.gsel - 1 + d) % 4) + 1
    elseif a == app.CONFIRM or a == app.OK then
      if S.gsel == 0 then S.gsel = 1 end
      open_detail(CAT_CODES[S.gi][S.gsel], "gallery")
    end
    return true
  end

  if S.page == "quiz" then
    if a == app.LEFT then if S.cur > 1 then S.cur = S.cur - 1 end
    elseif a == app.RIGHT then
      if S.cur < #Q then S.cur = S.cur + 1 else finish_quiz() end
    elseif a == app.UP then set_answer(math.max(1, (S.ans[S.cur] or 3) - 1))
    elseif a == app.DOWN then set_answer(math.min(5, (S.ans[S.cur] or 3) + 1))
    elseif a == app.CONFIRM or a == app.OK then
      if (S.ans[S.cur] or 0) == 0 then set_answer(3) else
        if S.cur < #Q then S.cur = S.cur + 1 else finish_quiz() end
      end
    end
    return true
  end

  if S.page == "result" then
    if a == app.CONFIRM or a == app.OK then S.card = true end
    return true
  end

  if S.page == "history" then
    if a == app.UP then S.hsel = math.max(1, S.hsel - 1)
    elseif a == app.DOWN then S.hsel = math.min(#S.hist, S.hsel + 1)
    elseif a == app.CONFIRM or a == app.OK then
      if S.hsel >= 1 and S.hist[S.hsel] then
        S.read = S.hist[S.hsel]
        S.result = nil
        S.page = "result"
      end
    end
    return true
  end

  return true
end

function on_exit()
  save_prog()
end
