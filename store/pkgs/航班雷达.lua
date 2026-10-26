--@lapp v1
--@app name=航班雷达
--@app cat=运维
--@app desc=自动定位当前位置, 扫描附近空域航班: 距离/方位/高度/地速/呼号
--@app need=wifi
--@app version=1.0
--@app icon=AAAAAAAAAAAAAAAf/gAAAAAAAf9/wAAAAAAP/3/4AAAAADf/f/YAAAAA+///74AAAAH74APvwAAAB/8AgH/wAAAP+ACAD/gAAB/hAIAD/AAAP8CAAAH+AABvAA/4AHsAAPYAcAcAN4AB/AOAAOAfwAH6BgAAMA/AA/EYAAAMB+AH4DAAAAYD8AfgYAAAAwPwD8DAAIABgfgPgYABwADA+BOBAAPgAEDkHIIAA+AAIJwfRgAD4AAwfD8kAAPgABB+PgQAA+AAED4+CAAD4AAIPj4IAAfwAAg+fAgAH/wACB98EAD//4AEH3wQA///4AQffBAP///4BB98EB////wEH0XQHwfwfAQRfBAMB/AYBB98EAAH8AAEH3wQAAPwAAQffBAAA+AABB98CAAD4AAIHz4IAAPgAAg+PggAAcAACD4+BAABwAAQPj8EAAPgABB+HwYAD/gAMHwcggAf/AAgnBOBAB48AEDkD4GAAAAAwPgPwMAAAAGB+AfgYAAAAwPwB+AwAAAGA/AD8BgAAAwH4AH4BgAAMA/AAfwDgADgH8AA9gBwBwA3gABvAA/4AHsAAD/AAAAB/gAAH+AAAAP8AAAP+AAAD/gAAAf/AAB/8AAAAfvgA+/AAAAA+///74AAAAA3/3/2AAAAAA//f/gAAAAAAf9/wAAAAAAAH/4AAAA=
-- 图标自绘: blit_app_icon(x, y[, inv=true])
-- ── LAPP 图标自绘前奏 (打包器注入; 读内嵌 base64, 不碰文件系统) ──
local _APPICON = "AAAAAAAAAAAAAAAf/gAAAAAAAf9/wAAAAAAP/3/4AAAAADf/f/YAAAAA+///74AAAAH74APvwAAAB/8AgH/wAAAP+ACAD/gAAB/hAIAD/AAAP8CAAAH+AABvAA/4AHsAAPYAcAcAN4AB/AOAAOAfwAH6BgAAMA/AA/EYAAAMB+AH4DAAAAYD8AfgYAAAAwPwD8DAAIABgfgPgYABwADA+BOBAAPgAEDkHIIAA+AAIJwfRgAD4AAwfD8kAAPgABB+PgQAA+AAED4+CAAD4AAIPj4IAAfwAAg+fAgAH/wACB98EAD//4AEH3wQA///4AQffBAP///4BB98EB////wEH0XQHwfwfAQRfBAMB/AYBB98EAAH8AAEH3wQAAPwAAQffBAAA+AABB98CAAD4AAIHz4IAAPgAAg+PggAAcAACD4+BAABwAAQPj8EAAPgABB+HwYAD/gAMHwcggAf/AAgnBOBAB48AEDkD4GAAAAAwPgPwMAAAAGB+AfgYAAAAwPwB+AwAAAGA/AD8BgAAAwH4AH4BgAAMA/AAfwDgADgH8AA9gBwBwA3gABvAA/4AHsAAD/AAAAB/gAAH+AAAAP8AAAP+AAAD/gAAAf/AAB/8AAAAfvgA+/AAAAA+///74AAAAA3/3/2AAAAAA//f/gAAAAAAf9/wAAAAAAAH/4AAAA="
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
-- ============================================================================
-- planeradar.lua — 附近航班雷达 (Lua 单文件应用)
--
-- 数据: adsb.fi 开放数据 API (免 key, 公开接口限流 1 次/秒)
--       一次返回以站点为中心 N 海里内的全部 ADS-B 目标, 且**已带 dst(距离 NM)
--       / dir(方位角)**, 不需要自己算球面距离。
--
-- 位置来源 (三层, 2026-10-02 linit 要求"根据 Wi-Fi 获取当前位置"):
--   ① 自动定位  —— 联网后用 IP 定位接口拿经纬度 (连的就是 Wi-Fi, 所以这就是"从网络定位")
--   ② 预设城市  —— IP 定位只能到城市级, 且常偏到运营商出口城市 → 可就近手选修正
--   ③ 经纬度微调 —— 完全手动: 上下调纬度 / 左右调经度 / 确认切步长
--
-- 为什么不用自己收: ESP32-S3 的 2.4GHz 射频收不到 1090MHz ADS-B 原始广播
-- (要外接专用接收机), 所以取"社区接收网络已汇总的数据"。
--
-- 契约: on_enter / render / poll / action / on_exit
-- 版面 (2026-10-15 linit): 无顶栏 · 底部软键栏(左=设置 右=扫描) · 雷达大球居中 ·
--   左面板=位置/数据状态/选中航班详情 · 右面板=附近航班列表 · **全 16px 统一字体**
-- 交互: **设置**(底栏左软键) = 系统配置(网络设置交给系统网络模块 / 默认距离档);
--   **左上位置名** = 位置调整(选省 → 选市, 标准二级弹窗); 状态/进度只在**底栏中间凹口**;
--   按键 上/下=选航班 左/右=距离档 确认=详情(长按=设置) 返回=退出;
--   触摸 底栏 左=设置 右=扫描, 雷达/右列表=选航班看详情, 左上位置名=选省选市
--
-- 2026-10-15 修 (参考 github.com/emwstudio/esp32-radar 的功能集, 按 linit 要求裁剪):
--   ① 定位失败自愈: 原来 fail 后死等用户长按重选, 现在每 10s 自动重试, 最多 3 轮
--   ② 距离环海里标注 (学它的 40/80/120NM 环标): 4 环 = 档位 25/50/75/100%
--   ③ 顶栏数据新鲜度 "Ns" (它的四角要素"时间"位)
--   ④ 特急应答机 7700/7600/7500 整行反白
--   不采用的: 语音播报链路 (MiniCPM5+VoxCPM2, linit 明确不要) / GPS 坐标 (用网络定位)
-- ============================================================================

local W = app.W or 400
local H = app.H or 300

-- 动作常量带数值兜底 (桥常量万一取不到, 应用照常能用)
local A_UP    = (type(app.UP)      == "number") and app.UP      or 1
local A_DOWN  = (type(app.DOWN)    == "number") and app.DOWN    or 2
local A_LEFT  = (type(app.LEFT)    == "number") and app.LEFT    or 3
local A_RIGHT = (type(app.RIGHT)   == "number") and app.RIGHT   or 4
local A_OK    = (type(app.CONFIRM) == "number") and app.CONFIRM or 5
local A_BACK  = (type(app.BACK)    == "number") and app.BACK    or 6
local BK, WH  = 0, 1

-- 省 → 市/区 位置库: 阿里云 DataV 行政区划中心点 (生成, 勿手改)
local POS = {
  { "北京", 39.905, 116.405, {
    {"东城区",39.918,116.419},{"西城区",39.915,116.367},{"朝阳区",39.921,116.486},{"丰台区",39.864,116.287},
    {"石景山区",39.915,116.195},{"海淀区",39.956,116.310},{"门头沟区",39.937,116.105},
    {"房山区",39.736,116.139},{"通州区",39.902,116.659},{"顺义区",40.129,116.654},{"昌平区",40.218,116.236},
    {"大兴区",39.729,116.338},{"怀柔区",40.324,116.637},{"平谷区",40.145,117.112},{"密云区",40.377,116.843},
    {"延庆区",40.465,115.985},
  }},
  { "天津", 39.126, 117.190, {
    {"和平区",39.118,117.196},{"河东区",39.122,117.227},{"河西区",39.102,117.218},{"南开区",39.120,117.164},
    {"河北区",39.157,117.202},{"红桥区",39.175,117.163},{"东丽区",39.088,117.314},{"西青区",39.139,117.012},
    {"津南区",38.990,117.383},{"北辰区",39.226,117.135},{"武清区",39.377,117.058},{"宝坻区",39.717,117.308},
    {"滨海新区",39.033,117.654},{"宁河区",39.329,117.828},{"静海区",38.936,116.925},
    {"蓟州区",40.045,117.407},
  }},
  { "河北省", 38.045, 114.502, {
    {"石家庄",38.045,114.502},{"唐山",39.635,118.175},{"秦皇岛",39.943,119.587},{"邯郸",36.612,114.491},
    {"邢台",37.068,114.509},{"保定",38.868,115.482},{"张家口",40.812,114.884},{"承德",40.976,117.939},
    {"沧州",38.311,116.857},{"廊坊",39.524,116.704},{"衡水",37.735,115.666},
  }},
  { "山西省", 37.857, 112.549, {
    {"太原",37.857,112.549},{"大同",40.090,113.295},{"阳泉",37.861,113.583},{"长治",36.191,113.114},
    {"晋城",35.498,112.851},{"朔州",39.331,112.433},{"晋中",37.696,112.736},{"运城",35.023,111.004},
    {"忻州",38.418,112.734},{"临汾",36.084,111.518},{"吕梁",37.524,111.134},
  }},
  { "内蒙古", 40.818, 111.671, {
    {"呼和浩特",40.818,111.671},{"包头",40.658,109.840},{"乌海",39.674,106.826},{"赤峰",42.275,118.957},
    {"通辽",43.617,122.263},{"鄂尔多斯",39.817,109.990},{"呼伦贝尔",49.215,119.758},
    {"巴彦淖尔",40.757,107.417},{"乌兰察布",41.034,113.115},{"兴安",46.076,122.070},
    {"锡林郭勒",43.944,116.091},{"阿拉善",38.845,105.706},
  }},
  { "辽宁省", 41.797, 123.429, {
    {"沈阳",41.797,123.429},{"大连",38.915,121.619},{"鞍山",41.111,122.996},{"抚顺",41.876,123.921},
    {"本溪",41.298,123.771},{"丹东",40.124,124.383},{"锦州",41.119,121.136},{"营口",40.667,122.235},
    {"阜新",42.012,121.649},{"辽阳",41.269,123.182},{"盘锦",41.124,122.070},{"铁岭",42.291,123.844},
    {"朝阳",41.577,120.451},{"葫芦岛",40.756,120.856},
  }},
  { "吉林省", 43.887, 125.325, {
    {"长春",43.887,125.325},{"吉林",43.844,126.553},{"四平",43.170,124.371},{"辽源",42.903,125.145},
    {"通化",41.721,125.937},{"白山",41.943,126.428},{"松原",45.118,124.824},{"白城",45.619,122.841},
    {"延边朝鲜族",42.905,129.513},
  }},
  { "黑龙江省", 45.757, 126.642, {
    {"哈尔滨",45.757,126.642},{"齐齐哈尔",47.342,123.958},{"鸡西",45.300,130.976},{"鹤岗",47.332,130.277},
    {"双鸭山",46.643,131.157},{"大庆",46.591,125.113},{"伊春",47.725,128.899},{"佳木斯",46.810,130.362},
    {"七台河",45.771,131.016},{"牡丹江",44.583,129.619},{"黑河",50.250,127.499},{"绥化",46.637,126.993},
    {"大兴安岭",52.335,124.712},
  }},
  { "上海", 31.232, 121.473, {
    {"黄浦区",31.223,121.490},{"徐汇区",31.180,121.438},{"长宁区",31.218,121.422},{"静安区",31.229,121.448},
    {"普陀区",31.242,121.392},{"虹口区",31.261,121.492},{"杨浦区",31.271,121.523},{"闵行区",31.112,121.376},
    {"宝山区",31.399,121.490},{"嘉定区",31.384,121.250},{"浦东新区",31.246,121.568},
    {"金山区",30.725,121.331},{"松江区",31.030,121.224},{"青浦区",31.151,121.113},{"奉贤区",30.912,121.458},
    {"崇明区",31.627,121.398},
  }},
  { "江苏省", 32.042, 118.767, {
    {"南京",32.042,118.767},{"无锡",31.575,120.302},{"徐州",34.262,117.185},{"常州",31.773,119.947},
    {"苏州",31.299,120.620},{"南通",32.016,120.865},{"连云港",34.600,119.179},{"淮安",33.598,119.021},
    {"盐城",33.378,120.140},{"扬州",32.393,119.421},{"镇江",32.204,119.453},{"泰州",32.485,119.915},
    {"宿迁",33.963,118.275},
  }},
  { "浙江省", 30.287, 120.154, {
    {"杭州",30.287,120.154},{"宁波",29.868,121.550},{"温州",28.001,120.672},{"嘉兴",30.763,120.751},
    {"湖州",30.867,120.102},{"绍兴",29.997,120.582},{"金华",29.090,119.650},{"衢州",28.942,118.873},
    {"舟山",30.016,122.107},{"台州",28.661,121.429},{"丽水",28.452,119.922},
  }},
  { "安徽省", 31.861, 117.283, {
    {"合肥",31.861,117.283},{"芜湖",31.326,118.376},{"蚌埠",32.940,117.363},{"淮南",32.648,117.018},
    {"马鞍山",31.689,118.508},{"淮北",33.972,116.795},{"铜陵",30.930,117.817},{"安庆",30.509,117.044},
    {"黄山",29.709,118.317},{"滁州",32.304,118.316},{"阜阳",32.897,115.820},{"宿州",33.634,116.984},
    {"六安",31.753,116.508},{"亳州",33.869,115.783},{"池州",30.656,117.489},{"宣城",30.946,118.758},
  }},
  { "示例省", 39.904, 116.407, {
    {"北京",39.904,116.407},{"上海",31.230,121.474},{"广州",23.129,113.264},{"成都",30.573,104.066},
    {"泉州",24.909,118.589},{"漳州",24.511,117.662},{"南平",26.636,118.178},{"龙岩",25.092,117.030},
    {"宁德",26.659,119.527},
  }},
  { "江西省", 28.676, 115.892, {
    {"南昌",28.676,115.892},{"景德镇",29.293,117.215},{"萍乡",27.623,113.852},{"九江",29.712,115.993},
    {"新余",27.811,114.931},{"鹰潭",28.239,117.034},{"赣州",25.851,114.940},{"吉安",27.112,114.986},
    {"宜春",27.804,114.391},{"抚州",27.984,116.358},{"上饶",28.444,117.971},
  }},
  { "山东省", 36.676, 117.001, {
    {"济南",36.676,117.001},{"青岛",36.083,120.355},{"淄博",36.815,118.048},{"枣庄",34.856,117.558},
    {"东营",37.435,118.665},{"烟台",37.539,121.391},{"潍坊",36.709,119.107},{"济宁",35.415,116.587},
    {"泰安",36.195,117.129},{"威海",37.510,122.116},{"日照",35.429,119.461},{"临沂",35.065,118.326},
    {"德州",37.454,116.307},{"聊城",36.456,115.980},{"滨州",37.384,118.017},{"菏泽",35.247,115.469},
  }},
  { "河南省", 34.758, 113.665, {
    {"郑州",34.758,113.665},{"开封",34.797,114.341},{"洛阳",34.663,112.434},{"平顶山",33.735,113.308},
    {"安阳",36.103,114.352},{"鹤壁",35.748,114.295},{"新乡",35.303,113.884},{"焦作",35.239,113.238},
    {"濮阳",35.768,115.041},{"许昌",34.023,113.826},{"漯河",33.576,114.026},{"三门峡",34.777,111.194},
    {"南阳",32.999,112.541},{"商丘",34.437,115.650},{"信阳",32.123,114.075},{"周口",33.620,114.650},
    {"驻马店",32.980,114.025},{"济源",35.090,112.590},
  }},
  { "湖北省", 30.584, 114.299, {
    {"武汉",30.584,114.299},{"黄石",30.220,115.077},{"十堰",32.647,110.788},{"宜昌",30.703,111.291},
    {"襄阳",32.042,112.144},{"鄂州",30.397,114.891},{"荆门",31.035,112.204},{"孝感",30.926,113.927},
    {"荆州",30.327,112.238},{"黄冈",30.448,114.879},{"咸宁",29.833,114.329},{"随州",31.717,113.374},
    {"恩施土家族苗族",30.283,109.487},{"仙桃",30.365,113.454},{"潜江",30.421,112.897},
    {"天门",30.653,113.166},{"神农架林区",31.744,110.672},
  }},
  { "湖南省", 28.194, 112.982, {
    {"长沙",28.194,112.982},{"株洲",27.836,113.152},{"湘潭",27.830,112.944},{"衡阳",26.900,112.608},
    {"邵阳",27.238,111.469},{"岳阳",29.370,113.133},{"常德",29.040,111.691},{"张家界",29.127,110.480},
    {"益阳",28.570,112.355},{"郴州",25.794,113.032},{"永州",26.435,111.608},{"怀化",27.550,109.978},
    {"娄底",27.728,112.008},{"湘西土家族苗族",28.314,109.740},
  }},
  { "广东省", 23.125, 113.281, {
    {"广州",23.125,113.281},{"韶关",24.801,113.592},{"深圳",22.547,114.086},{"珠海",22.225,113.554},
    {"汕头",23.371,116.708},{"佛山",23.029,113.123},{"江门",22.590,113.095},{"湛江",21.275,110.365},
    {"茂名",21.660,110.919},{"肇庆",23.052,112.473},{"惠州",23.079,114.413},{"梅州",24.299,116.118},
    {"汕尾",22.774,115.364},{"河源",23.746,114.698},{"阳江",21.859,111.975},{"清远",23.685,113.051},
    {"东莞",23.046,113.746},{"中山",22.521,113.382},{"潮州",23.662,116.632},{"揭阳",23.544,116.356},
    {"云浮",22.930,112.044},
  }},
  { "广西壮族", 22.824, 108.320, {
    {"南宁",22.824,108.320},{"柳州",24.315,109.412},{"桂林",25.274,110.299},{"梧州",23.475,111.298},
    {"北海",21.473,109.119},{"防城港",21.615,108.345},{"钦州",21.967,108.624},{"贵港",23.094,109.602},
    {"玉林",22.631,110.154},{"百色",23.898,106.616},{"贺州",24.414,111.552},{"河池",24.696,108.062},
    {"来宾",23.734,109.230},{"崇左",22.404,107.354},
  }},
  { "海南省", 20.032, 110.331, {
    {"海口",20.032,110.331},{"三亚",18.248,109.508},{"三沙",16.831,112.349},{"儋州",19.517,109.577},
    {"五指山",18.777,109.517},{"琼海",19.246,110.467},{"文昌",19.613,110.754},{"万宁",18.796,110.389},
    {"东方",19.102,108.654},{"定安县",19.685,110.349},{"屯昌县",19.363,110.103},{"澄迈县",19.737,110.007},
    {"临高县",19.908,109.688},{"白沙黎族自治县",19.225,109.453},{"昌江黎族自治县",19.261,109.053},
    {"乐东黎族自治县",18.748,109.175},{"陵水黎族自治县",18.505,110.037},{"保亭黎族苗族自治县",18.636,109.702},
    {"琼中黎族苗族自治县",19.036,109.840},
  }},
  { "重庆", 29.533, 106.505, {
    {"万州区",30.808,108.380},{"涪陵区",29.704,107.395},{"渝中区",29.557,106.563},
    {"大渡口区",29.481,106.486},{"江北区",29.575,106.533},{"沙坪坝区",29.541,106.454},
    {"九龙坡区",29.523,106.481},{"南岸区",29.524,106.561},{"北碚区",29.825,106.438},
    {"綦江区",29.028,106.651},{"大足区",29.700,105.715},{"渝北区",29.601,106.513},{"巴南区",29.382,106.519},
    {"黔江区",29.528,108.783},{"长寿区",29.834,107.075},{"江津区",29.283,106.253},{"合川区",29.991,106.266},
    {"永川区",29.349,105.895},{"南川区",29.157,107.098},{"璧山区",29.594,106.231},{"铜梁区",29.840,106.055},
    {"潼南区",30.190,105.842},{"荣昌区",29.404,105.594},{"开州区",31.168,108.413},{"梁平区",30.672,107.800},
    {"武隆区",29.324,107.757},{"城口县",31.946,108.665},{"丰都县",29.866,107.732},{"垫江县",30.330,107.349},
    {"忠县",30.292,108.038},{"云阳县",30.931,108.698},{"奉节县",31.020,109.466},{"巫山县",31.075,109.879},
    {"巫溪县",31.397,109.629},{"石柱土家族自治县",29.999,108.112},{"秀山土家族苗族自治县",28.445,108.996},
    {"酉阳土家族苗族自治县",28.840,108.767},{"彭水苗族土家族自治县",29.294,108.167},
  }},
  { "四川省", 30.659, 104.066, {
    {"成都",30.659,104.066},{"自贡",29.353,104.773},{"攀枝花",26.580,101.716},{"泸州",28.889,105.443},
    {"德阳",31.128,104.399},{"绵阳",31.464,104.742},{"广元",32.434,105.830},{"遂宁",30.513,105.571},
    {"内江",29.587,105.066},{"乐山",29.582,103.761},{"南充",30.795,106.083},{"眉山",30.048,103.832},
    {"宜宾",28.760,104.631},{"广安",30.456,106.633},{"达州",31.209,107.502},{"雅安",29.988,103.001},
    {"巴中",31.859,106.754},{"资阳",30.122,104.642},{"阿坝藏族羌族",31.900,102.221},
    {"甘孜藏族",30.051,101.964},{"凉山彝族",27.887,102.259},
  }},
  { "贵州省", 26.578, 106.713, {
    {"贵阳",26.578,106.713},{"六盘水",26.585,104.847},{"遵义",27.707,106.937},{"安顺",26.246,105.932},
    {"毕节",27.302,105.285},{"铜仁",27.718,109.192},{"黔西南布依族苗族",25.088,104.898},
    {"黔东南苗族侗族",26.583,107.977},{"黔南布依族苗族",26.258,107.517},
  }},
  { "云南省", 25.041, 102.712, {
    {"昆明",25.041,102.712},{"曲靖",25.502,103.798},{"玉溪",24.350,102.544},{"保山",25.112,99.167},
    {"昭通",27.337,103.717},{"丽江",26.872,100.233},{"普洱",22.777,100.972},{"临沧",23.887,100.087},
    {"楚雄彝族",25.042,101.546},{"红河哈尼族彝族",23.367,103.384},{"文山壮族苗族",23.370,104.244},
    {"西双版纳傣族",22.002,100.798},{"大理白族",25.589,100.226},{"德宏傣族景颇族",24.437,98.578},
    {"怒江傈僳族",25.851,98.854},{"迪庆藏族",27.827,99.706},
  }},
  { "西藏", 29.660, 91.132, {
    {"拉萨",29.660,91.132},{"日喀则",29.268,88.885},{"昌都",31.137,97.178},{"林芝",29.655,94.362},
    {"山南",29.236,91.767},{"那曲",31.476,92.060},{"阿里",32.503,80.105},
  }},
  { "陕西省", 34.263, 108.948, {
    {"西安",34.263,108.948},{"铜川",34.917,108.980},{"宝鸡",34.369,107.145},{"咸阳",34.333,108.705},
    {"渭南",34.499,109.503},{"延安",36.597,109.491},{"汉中",33.078,107.029},{"榆林",38.290,109.741},
    {"安康",32.690,109.029},{"商洛",33.868,109.940},
  }},
  { "甘肃省", 36.058, 103.824, {
    {"兰州",36.058,103.824},{"嘉峪关",39.787,98.277},{"金昌",38.514,102.188},{"白银",36.546,104.174},
    {"天水",34.579,105.725},{"武威",37.930,102.635},{"张掖",38.933,100.455},{"平凉",35.543,106.685},
    {"酒泉",39.744,98.511},{"庆阳",35.734,107.638},{"定西",35.580,104.626},{"陇南",33.389,104.929},
    {"临夏回族",35.599,103.212},{"甘南藏族",34.986,102.911},
  }},
  { "青海省", 36.623, 101.779, {
    {"西宁",36.623,101.779},{"海东",36.503,102.103},{"海北藏族",36.959,100.901},{"黄南藏族",35.518,102.020},
    {"海南藏族",36.280,100.620},{"果洛藏族",34.474,100.242},{"玉树藏族",33.004,97.009},
    {"海西蒙古族藏族",37.375,97.371},
  }},
  { "宁夏回族", 38.466, 106.278, {
    {"银川",38.466,106.278},{"石嘴山",39.013,106.376},{"吴忠",37.986,106.199},{"固原",36.005,106.285},
    {"中卫",37.515,105.190},
  }},
  { "新疆维吾尔", 43.793, 87.618, {
    {"乌鲁木齐",43.793,87.618},{"克拉玛依",45.596,84.874},{"吐鲁番",42.948,89.184},{"哈密",42.833,93.513},
    {"昌吉回族",44.015,87.304},{"博尔塔拉蒙古",44.903,82.075},{"巴音郭楞蒙古",41.769,86.151},
    {"阿克苏",41.171,80.265},{"克孜勒苏柯尔克孜",39.713,76.173},{"喀什",39.468,75.989},{"和田",37.111,79.925},
    {"伊犁哈萨克",43.922,81.318},{"塔城",46.746,82.986},{"阿勒泰",47.848,88.140},{"石河子",44.306,86.041},
    {"阿拉尔",40.542,81.286},{"图木舒克",39.867,79.078},{"五家渠",44.167,87.527},{"北屯",47.353,87.825},
    {"铁门关",41.827,85.501},{"双河",44.841,82.354},{"可克达拉",43.683,80.636},{"昆玉",37.208,79.287},
    {"胡杨河",44.693,84.828},
  }},
  { "台湾省", 25.044, 121.509, {
    {"台湾省",25.044,121.509},
  }},
  { "香港", 22.320, 114.173, {
    {"中西区",22.282,114.154},{"湾仔区",22.276,114.183},{"东区",22.280,114.226},{"南区",22.246,114.160},
    {"油尖旺区",22.312,114.173},{"九龙",22.334,114.163},{"九龙城区",22.313,114.193},
    {"黄大仙区",22.336,114.204},{"观塘区",22.321,114.214},{"荃湾区",22.368,114.121},
    {"屯门区",22.394,113.977},{"元朗区",22.441,114.032},{"北区",22.496,114.147},{"大埔区",22.446,114.172},
    {"西贡区",22.314,114.265},{"沙田区",22.380,114.195},{"葵青区",22.364,114.139},{"离岛区",22.286,113.946},
  }},
  { "澳门", 22.199, 113.549, {
    {"花地玛堂区",22.208,113.553},{"花王堂区",22.199,113.549},{"望德堂区",22.194,113.550},
    {"大堂区",22.189,113.554},{"风顺堂区",22.187,113.542},{"嘉模堂区",22.154,113.559},
    {"路凼填海区",22.137,113.570},{"圣方济各堂区",22.123,113.560},
  }},
}

-- 位置来源: 1 = 自动定位(IP)   2 = 选定的省/市 (坐标落在 G.lat/lon/place 并写 NVS)
local SRC_AUTO   = 1
local SRC_PICK   = 2
local SRC_COUNT  = 2

-- 兜底位置 (定位全失败时用): 可自行指定的默认坐标
local FALLBACK = { name = "默认位置", lat = 39.9042, lon = 116.4074 }

local RANGES = { 25, 50, 100, 200 }        -- 海里
local REFRESH_MS = 8000                    -- 航班自动刷新间隔 (接口限流 1/s, 8s 很宽裕)
local MAX_AC     = 160
local FIELDS = "hex,flight,r,t,lat,lon,alt_baro,alt_geom,gs,track,baro_rate,"
            .. "squawk,dst,dir,rssi,ownOp,emergency"

-- IP 定位接口 (实测国内均可达)。**首选 ip-api**:
--   ① 带 lang=zh-CN → 直接返回中文城市名 (ip.sb 只给英文 "Putian")
--   ② 走 **HTTP** → 不需要 TLS 握手, 既不占那 16KB 内部连续块, 也避开证书/时间问题
--   (只查"我在哪个城市", 不含任何隐私数据)
-- 兜底 ip.sb (HTTPS), 给 latitude/longitude 字段。
local GEO_URLS = {
    "http://ip-api.com/json/?lang=zh-CN",
    "https://api.ip.sb/geoip",
}

-- ---- 版面 (2026-10-15 linit: 无顶栏 + 底部软键栏 + 大球居中 + 全 16px + 不留空隙) ----
-- 底栏几何与 sdrv_draw_softbar 完全一致 (盖子线 273 + 左右斜坡 + 谷线 294, 软键 16px@277)
local SOFT_LID, SOFT_VALLEY, SOFT_KEYY, SOFT_MARGIN = 273, 294, 277, 16
local CX, CY    = 200, 136          -- 雷达中心 = 屏幕居中
local CR        = 132               -- 大球半径 (右侧改成两排后能再放大)
local LP_X      = 2                 -- 左上: 位置 + 机型统计
local RP_X      = 336               -- 右侧: 选中航班详情 (名称/内容 上下两排, 更窄)

-- ---- 状态 ----
local G = {
    st     = "idle",     -- idle | loading | ok | err
    ac     = {},
    sel    = 1,
    src    = SRC_AUTO,   -- 位置来源
    lat    = FALLBACK.lat,
    lon    = FALLBACK.lon,
    place  = FALLBACK.name,
    rng    = 3,
    t0     = 0,
    err    = "",
    detail = false,
    dlg    = nil,        -- 打开的设置弹窗 (app.dlg.list 轮询式契约)
    tune   = false,      -- 经纬度微调
    step   = 2,          -- 微调步长索引: 1=0.01 2=0.1 3=1.0
    geo    = "idle",     -- idle | run | done | fail
    geoi   = 1,          -- 正在试的定位接口下标
    geoerr = "",
    ok_t   = 0,          -- 确认键按下时刻 (判长按)
    ok_pend= false,
    ok_long= false,
    dirty  = true,       -- 需要重绘 (没变化就整帧跳过绘制)
    upd    = 0,          -- 最近一次成功取数的时刻 (顶栏显示 "Ns 前" = 数据新鲜度)
    geotry = 0,          -- 定位失败后的自动重试轮数 (最多 3 轮, 2026-10-15 修: 原来死等用户重选)
    geot   = 0,          -- 上一轮定位失败的时刻 (10s 后再试一轮)
}
-- 位置来源编号见上方 SRC_* (1=自动 2=选定省市)

-- ============================ 小工具 ============================
-- ⚠ Lua 5.3+ 的 string.format("%d", x) 遇到**非整数浮点**会直接抛错 → 一律先取整
local function i0(v)
    v = tonumber(v)
    if not v then return 0 end
    return math.floor(v + 0.5)
end
local function trim(s)
    s = tostring(s or "")
    s = s:gsub("^[%s]+", "")
    s = s:gsub("[%s]+$", "")
    return s
end
local function pad_l(s, n)
    s = tostring(s)
    if #s >= n then return s:sub(1, n) end
    return string.rep(" ", n - #s) .. s
end
local function pad_r(s, n)
    s = tostring(s)
    if #s >= n then return s:sub(1, n) end
    return s .. string.rep(" ", n - #s)
end
local function alt_txt(a)
    local v = a.alt_baro
    if v == "ground" then return "GND" end
    if type(v) ~= "number" then v = a.alt_geom end
    if type(v) ~= "number" then return "--" end
    return string.format("%d", i0(v))
end
local function alt_short(a)
    local v = a.alt_baro
    if v == "ground" then return "GND" end
    if type(v) ~= "number" then v = a.alt_geom end
    if type(v) ~= "number" then return "  -" end
    local f = i0(v) / 100
    if f < 0 then f = 0 end
    if f > 999 then f = 999 end
    return string.format("%3d", f)
end
local function fnum(v, fmt, suf)
    if type(v) ~= "number" then return "--" end
    return string.format(fmt, v) .. (suf or "")
end

-- ============================ 雷达基准图 ============================
local RINGPTS = {}
local function build_rings()
    if RINGPTS[1] then return end
    local radii = { CR * 0.25, CR * 0.5, CR * 0.75, CR }
    for ri = 1, #radii do
        local r = radii[ri]
        local n = math.max(32, math.floor(r * 4))
        local t = {}
        for i = 0, n - 1 do
            -- 虚线环: 每 4 段画 2 段 (2 实 2 虚)
            if (i % 4) < 2 then
                local a = i * 2 * math.pi / n
                t[#t + 1] = math.floor(CX + r * math.sin(a) + 0.5)
                t[#t + 1] = math.floor(CY - r * math.cos(a) + 0.5)
            end
        end
        RINGPTS[ri] = t
    end
end
local function blip_xy(a, range)
    local d = a.dst
    if type(d) ~= "number" or type(a.dir) ~= "number" then return nil end
    local r = d / range * CR
    if r > CR then r = CR end
    local th = a.dir * math.pi / 180
    return math.floor(CX + r * math.sin(th) + 0.5), math.floor(CY - r * math.cos(th) + 0.5)
end

-- ============================ 位置 (NVS + IP 定位) ============================
local function place_of_src()
    if G.src == SRC_AUTO then
        return (G.place ~= "") and G.place or "自动"
    end
    return (G.place ~= "" and G.place ~= "自动") and G.place or "自定义"
end
local function pos_save()
    app.nvs.set_int("pr_ver", 2)                     -- 位置模型版本 (省/市两级)
    app.nvs.set_int("pr_src", G.src)
    app.nvs.set_int("pr_lat", i0(G.lat * 10000))     -- 32 位整数存 ×10000, 精度 0.0001 度
    app.nvs.set_int("pr_lon", i0(G.lon * 10000))
    app.nvs.set_int("pr_rng", G.rng)
    app.nvs.set_str("pr_place", G.place)
end
local function pos_load()
    local s  = app.nvs.get_int("pr_src")
    local la = app.nvs.get_int("pr_lat")
    local lo = app.nvs.get_int("pr_lon")
    local nm = app.nvs.get_str("pr_place")
    local r  = app.nvs.get_int("pr_rng")
    -- 老版本 src 编号(预设城市 2..9)统一归到 SRC_PICK —— 坐标/名字本来就在 NVS 里, 不丢
    if type(s) == "number" and s >= 1 then
        G.src = (s == SRC_AUTO) and SRC_AUTO or SRC_PICK
    end
    if type(la) == "number" and la ~= 0 then G.lat = la / 10000 end
    if type(lo) == "number" and lo ~= 0 then G.lon = lo / 10000 end
    if type(nm) == "string" and nm ~= "" then G.place = trim(nm) end
    if type(r) == "number" and r >= 1 and r <= #RANGES then G.rng = r end
    -- ★★★ 位置模型换成"省/市两级"后, 老存档(预设城市那套)一律回到**自动定位** ——
    --   用户要的是"按网络位置显示"。之后选了什么就记什么 (pr_ver=2 打标, 不再重置)。
    if app.nvs.get_int("pr_ver") ~= 2 then
        G.src = SRC_AUTO
        G.place = ""
        app.nvs.set_int("pr_ver", 2)
    end
end
-- 选定位置: 坐标已在 G.lat/lon/place, 只需落盘
local function pos_apply_src()
    pos_save()
end

-- ============================ 取数 ============================
local function flights_url()
    return string.format("https://opendata.adsb.fi/api/v2/lat/%.4f/lon/%.4f/dist/%d",
                         G.lat, G.lon, RANGES[G.rng])
end
local function flights_start()
    if G.st == "loading" then return end
    if not app.net.is_connected() then
        G.st = "err"
        G.err = "未联网"
        return
    end
    G.st = "loading"
    G.t0 = app.millis()
    if not app.net.fetch_start(flights_url(), 9000) then
        G.st = "err"
        G.err = "请求忙"
    end
end
-- 依次试 GEO_URLS (app.net 同时只允许一个请求, 所以串行)
local function geo_start()
    if G.geo == "run" then return end
    if not app.net.is_connected() then
        G.geo = "fail"; G.geot = app.millis()
        G.geoerr = "未联网"
        return
    end
    G.geo = "run"
    -- 自愈: 上次哪个接口成功过就从它开始 (ip-api 偶发失败, 不要每次都白等一轮)
    local gi = app.nvs.get_int("pr_geoi")
    if type(gi) ~= "number" or gi < 1 or gi > #GEO_URLS then gi = 1 end
    G.geoi = gi
    if not app.net.fetch_start(GEO_URLS[G.geoi], 8000) then
        G.geo = "fail"; G.geot = app.millis()
        G.geoerr = "请求忙"
    end
end
local function geo_parse(body)
    -- 首选接口: latitude/longitude; 兜底接口: lat/lon。用 -999 区分"缺失"与"真的 0"
    local la = app.json.num(body, "latitude", -999)
    local lo = app.json.num(body, "longitude", -999)
    if la == -999 then la = app.json.num(body, "lat", -999) end
    if lo == -999 then lo = app.json.num(body, "lon", -999) end
    if la == -999 or lo == -999 or la == 0 or lo == 0 then return nil end
    local nm = app.json.str(body, "city")
    if not nm or nm == "" then nm = app.json.str(body, "regionName") end
    return la, lo, trim(nm)
end

local function apply_rows(rows)
    local list = {}
    for i = 1, #rows do
        local a = rows[i]
        if type(a.dst) == "number" then
            a.flight = trim(a.flight)
            a.ownOp  = trim(a.ownOp)
            a.r      = trim(a.r)
            a.t      = trim(a.t)
            a.hex    = trim(a.hex)
            a.squawk = trim(a.squawk)
            if a.flight == "" then
                a.flight = (a.r ~= "") and a.r or ((a.hex ~= "") and a.hex or "?")
            end
            list[#list + 1] = a
        end
    end
    table.sort(list, function(x, y) return x.dst < y.dst end)
    G.ac = list
    -- 机型统计 (左上显示): 按架数降序, 同数按型号名字典序 (table.sort 不稳定, 必须给全序)
    local cnt, order = {}, {}
    for i = 1, #list do
        local ty = list[i].t
        if ty and ty ~= "" then
            if cnt[ty] == nil then cnt[ty] = 0; order[#order + 1] = ty end
            cnt[ty] = cnt[ty] + 1
        end
    end
    table.sort(order, function(x, y)
        if cnt[x] ~= cnt[y] then return cnt[x] > cnt[y] end
        return x < y
    end)
    local ts = {}
    for i = 1, math.min(9, #order) do ts[i] = { order[i], cnt[order[i]] } end
    G.types = ts
    if G.sel > #list then G.sel = #list end
    if G.sel < 1 then G.sel = 1 end
end

-- ============================ 绘制 ============================
-- ---- 底部软键栏 (与频谱仪/网络检测同款: 盖子线 + 左右斜坡 + 谷线, 几何照 sdrv_draw_softbar) ----
local function draw_softbar()
    local left, right = "设置", "扫描"
    local wl = app.mixw(left)
    local wr = app.mixw(right)
    app.mix(SOFT_MARGIN, SOFT_KEYY, left)
    app.mix(W - SOFT_MARGIN - wr, SOFT_KEYY, right)
    local Lend = SOFT_MARGIN + wl + 12
    local Rstart = W - SOFT_MARGIN - wr - 12
    app.hline(0, Lend, SOFT_LID, BK)
    app.hline(Rstart, W - 1, SOFT_LID, BK)
    local slope = SOFT_VALLEY - SOFT_LID
    if Lend + slope < Rstart - slope then
        app.hline(Lend + slope, Rstart - slope, SOFT_VALLEY, BK)
    end
    for d = 0, slope do
        if d < 40 then
            app.px(Lend + d, SOFT_LID + d, BK)
            app.px(Rstart - d, SOFT_LID + d, BK)
        end
    end
    -- 凹口 = **进度条**: 未联网 → 连接中 → 定位中 → 取数中 → 完成(架数) / 失败(原因)
    local mid
    if not app.net.is_connected() then
        mid = app.wifi.connecting() and "连接中" or "未联网"
    elseif G.geo == "run" then mid = "定位中"
    elseif G.st == "loading" then mid = "取数中"
    elseif G.st == "err" then mid = "失败:" .. tostring(G.err or ""):sub(1, 18)
    else mid = string.format("%d架 %dNM", #G.ac, RANGES[G.rng]) end
    local wm = app.mixw(mid)
    if wm < (Rstart - 12) - (Lend + 12) then
        app.mix((W - wm) / 2, SOFT_KEYY, mid)
    end
end

-- ---- 小飞机图标 (按航向旋转的线框): 机身 + 主翼 + 尾翼 ----
-- 航向 0=正北(屏幕向上), 顺时针; 局部坐标 -y=机头方向, +x=右翼
local function pline(x0, y0, x1, y1)
    local dx, dy = x1 - x0, y1 - y0
    local n = math.max(math.abs(dx), math.abs(dy))
    if n == 0 then app.px(x0, y0, BK); return end
    for i = 0, n do
        app.px(math.floor(x0 + dx * i / n + 0.5), math.floor(y0 + dy * i / n + 0.5), BK)
    end
end
-- 2px 粗线: 原线 + 对角偏移再画一遍 (飞机图标放大后需要, 否则斜线段有洞)
local function pline2(x0, y0, x1, y1)
    pline(x0, y0, x1, y1)
    pline(x0 + 1, y0 + 1, x1 + 1, y1 + 1)
end
-- 选中圈: 以小圆环代替方框
local function draw_ring(cx, cy, r)
    local n = math.max(24, math.floor(r * 4))
    for i = 0, n - 1 do
        local a = i * 2 * math.pi / n
        app.px(math.floor(cx + r * math.sin(a) + 0.5), math.floor(cy - r * math.cos(a) + 0.5), BK)
    end
end

local function draw_plane(x, y, hdg)
    local t = (tonumber(hdg) or 0) * math.pi / 180
    local s, c = math.sin(t), math.cos(t)
    local function seg(ax, ay, bx, by)
        pline2(math.floor(x + ax * c + ay * s + 0.5),
               math.floor(y - ax * s + ay * c + 0.5),
               math.floor(x + bx * c + by * s + 0.5),
               math.floor(y - bx * s + by * c + 0.5))
    end
    -- 放大 1.5 倍 (在上一版基础上): 机身 24px, 翼展 18px
    seg(0, -12, 0, 9)     -- 机身 (机头 → 机尾)
    seg(-9, 0, 9, 0)      -- 主翼
    seg(-6, 9, 6, 9)      -- 尾翼
end

-- ---- 尾迹虚线: 从机尾沿**反航向**向后画虚线 (数据只有当前点, 按航向反推"来路") ----
-- 长度随地速 (快的飞机尾巴长一点), 2 实 2 虚, 让"往哪飞"一眼看出
local function draw_trail(x, y, hdg, len)
    local t = (tonumber(hdg) or 0) * math.pi / 180
    local dx, dy = -math.sin(t), math.cos(t)      -- 反航向 (屏幕坐标)
    local i = 5                                   -- 从机身外开始
    while i <= len do
        for k = 0, 1 do
            local j = i + k
            if j <= len then
                app.px(math.floor(x + dx * j + 0.5), math.floor(y + dy * j + 0.5), BK)
            end
        end
        i = i + 4                                 -- 2 画 2 空 = 虚线
    end
end

local function draw_radar(range)
    for ri = 1, 4 do
        local t = RINGPTS[ri]
        for i = 1, #t, 2 do app.px(t[i], t[i + 1], BK) end
    end
    for k = 0, 7 do
        -- 八方辐条也走虚线 (2 实 2 虚), 与外环虚线风格一致
        local a = k * math.pi / 4
        local s, c = math.sin(a), math.cos(a)
        local f = 0.80
        local n = 0
        while f <= 1.001 do
            if n % 4 < 2 then
                app.px(math.floor(CX + CR * f * s + 0.5), math.floor(CY - CR * f * c + 0.5), BK)
            end
            n = n + 1
            f = f + 0.016
        end
    end
    -- 距离环不放数字 (linit: 圆盘上不要 25/50/75/100 标注)
    app.mix(CX - 4, CY - CR - 2, "N")
    app.mix(CX + CR - 8, CY - 6, "E")
    app.mix(CX - 4, CY + CR - 12, "S")
    app.mix(CX - CR + 4, CY - 6, "W")
    app.fill(CX - 2, CY - 2, CX + 2, CY + 2, BK)

    for i = 1, #G.ac do
        local a = G.ac[i]
        local x, y = blip_xy(a, range)
        if x then
            local hdg = (type(a.track) == "number") and a.track or a.dir
            if a.alt_baro ~= "ground" then
                -- 尾迹虚线 (先画, 压在飞机下面): 起点让开放大后的机身, 长度 16~26px 随地速
                local gs = tonumber(a.gs) or 0
                draw_trail(x, y, hdg, 16 + math.min(10, i0(gs / 60)))
            end
            if i == G.sel then
                -- 选中: 飞机 + **外接圆圈** + 航向延长线 + 呼号
                draw_plane(x, y, hdg)
                draw_ring(x, y, 15)
                local t = hdg * math.pi / 180
                local f = 16
                while f <= 24 do
                    app.px(math.floor(x + f * math.sin(t) + 0.5),
                           math.floor(y - f * math.cos(t) + 0.5), BK)
                    f = f + 1
                end
                local lx = x + 15
                if lx + #a.flight * 8 > CX + CR - 2 then lx = x - 15 - #a.flight * 8 end
                if lx < 2 then lx = 2 end
                local ly = (y - 26 >= 2) and (y - 26) or (y + 12)
                app.mix(lx, ly, a.flight)
            elseif a.alt_baro == "ground" then
                -- 地面目标: 空心块 (不画飞机, 一眼区分)
                app.rect(x - 2, y - 2, x + 2, y + 2, BK)
            else
                -- 空中目标: **按航向旋转的小飞机线框** (不再是圆点)
                draw_plane(x, y, hdg)
            end
        end
    end
end

-- ---- 右侧: 选中航班详情 (每项**名称一行 / 内容一行**, 面板更窄) ----
local function draw_right_panel()
    local a = G.ac[G.sel]
    if not a then return end
    local x = RP_X
    local fields = {
        { "呼号", a.flight },
        { "距离", string.format("%.1fNM", a.dst) },
        { "方位", tostring(i0(a.dir)) },
        { "高度", alt_txt(a) .. "ft" },
        { "速度", fnum(a.gs, "%.0f", "") .. "kt" },
        { "机型", (a.t ~= "" and a.t or "--") },
    }
    local y = 4
    for i = 1, #fields do
        app.mix(x, y, fields[i][1])            -- 名称
        app.mix(x, y + 20, fields[i][2])       -- 内容
        y = y + 44
    end
end

-- ---- 左上: 位置 + 机型统计 (点击位置名改位置; 不显示航班列表) ----
local function draw_left_panel()
    app.mix(LP_X, 4, place_of_src())
    local ty = G.types
    if not ty then return end
    local y = 30
    for i = 1, #ty do
        app.mix(LP_X, y, string.format("%s %d", ty[i][1]:sub(1, 4), ty[i][2]))
        y = y + 20
    end
end

-- ---- 航班详情弹窗 (标准二级弹窗; 双击/长按/点已选中的飞机都能打开) ----
local function open_detail()
    local a = G.ac[G.sel]
    if not a then return end
    local op = a.ownOp
    if op == "" then op = "--" end
    if #op > 14 then op = op:sub(1, 14) end
    local items = {
        string.format("呼号 %s   %s", a.flight, (a.t ~= "" and a.t or "--")),
        string.format("注册 %s   %s", (a.r ~= "" and a.r or "--"), (a.hex ~= "" and a.hex or "--")),
        string.format("高度 %sft  升降 %s", alt_txt(a), fnum(a.baro_rate, "%.0f", "")),
        string.format("地速 %skt  航向 %d", fnum(a.gs, "%.0f", ""), i0(a.track)),
        string.format("应答机 %s  %s", (a.squawk ~= "" and a.squawk or "--"),
                      (a.emergency and a.emergency ~= "none") and a.emergency or "无紧急"),
        string.format("距 %.1fNM  方位 %d", a.dst, i0(a.dir)),
        string.format("%s %.3f/%.3f", place_of_src(), G.lat, G.lon),
        "运营 " .. op,
    }
    G.dlg = "detail"
    app.dlg.list(a.flight, items, 0)
end

-- 命中最近的飞机 (24px 半径); 没有 → nil
local function hit_plane(x, y)
    if (x - CX) * (x - CX) + (y - CY) * (y - CY) > (CR + 8) * (CR + 8) then return nil end
    local range, best, bd = RANGES[G.rng], nil, 24 * 24
    for i = 1, #G.ac do
        local bx, by = blip_xy(G.ac[i], range)
        if bx then
            local dd = (bx - x) * (bx - x) + (by - y) * (by - y)
            if dd < bd then best, bd = i, dd end
        end
    end
    return best
end

-- ---- 位置选择: 左上位置名 → 选省 → 选市 (标准二级弹窗) ----
local function open_city(pi)
    G.dlg = "city"
    G.sel_prov = pi
    local cs = POS[pi][4]
    local t = {}
    for i = 1, #cs do t[#t + 1] = cs[i][1] end
    app.dlg.list(POS[pi][1], t, G.sel_city or 0)
end
local function open_province()
    G.dlg = "prov"
    local t = { "自动定位" }
    for i = 1, #POS do t[#t + 1] = POS[i][1] end
    app.dlg.list("选择省份", t, G.sel_prov or 0)
end

-- ---- 设置: 各种配置 (网络交给系统网络模块; 其余本应用自己管) ----
local function open_range()
    G.dlg = "range"
    app.dlg.list("默认距离档", { "25 海里", "50 海里", "100 海里", "200 海里" }, G.rng - 1)
end
local function open_settings()
    G.dlg = "set"
    app.dlg.list("设置", { "网络设置", "默认距离档" }, 0)
end

function render()
    -- ★ 只在"有变化"时重绘: 框架在 RUN 态强制每帧 needs_redraw, render() 每帧都被调用;
    -- 但本应用是"数据 8s 一次 + 按键驱动", 没必要逐帧重画 ~3000 个点。
    -- 更关键: mbedtls 工作内存改到 PSRAM 后, TLS 握手期间 PSRAM 带宽被抢,
    -- 逐帧重绘会慢两个数量级(实测 836ms/帧) → 画面像卡死。跳过绘制后,
    -- 帧缓冲保留上一帧, flush 照旧(≈0.8ms), CPU 与带宽都省下来。
    if not G.dirty then return end
    G.dirty = false
    build_rings()
    app.clear(WH)
    draw_softbar()
    draw_radar(RANGES[G.rng])
    draw_left_panel()
    draw_right_panel()
end

-- ============================ 交互 ============================
local function move_sel(d)
    local n = #G.ac
    if n == 0 then return end
    G.sel = G.sel + d
    if G.sel < 1 then G.sel = n elseif G.sel > n then G.sel = 1 end
end

local function set_rng(d)
    local r = G.rng + d
    if r < 1 then r = #RANGES elseif r > #RANGES then r = 1 end
    if r ~= G.rng then
        G.rng = r
        pos_save()
        G.ac = {}
        G.sel = 1
        flights_start()
    end
end

-- 切来源: 自动 → 重新定位; 预设/自定义 → 直接用坐标
local function src_apply()
    if G.src == SRC_AUTO then
        G.geo = "idle"
        geo_start()
    else
        pos_apply_src()
        G.ac = {}
        G.sel = 1
        flights_start()
    end
end

function action(a)
    G.dirty = true                     -- 任何按键都可能改画面
    if type(a) ~= "number" or a < 1 or a > 7 then return true end

    -- 弹窗开着时按键归弹窗 (内核把动作发给弹窗层), 这里不处理

    if a == A_UP then        move_sel(-1)
    elseif a == A_DOWN then  move_sel(1)
    elseif a == A_LEFT then  set_rng(-1)
    elseif a == A_RIGHT then set_rng(1)
    elseif a == A_OK then
        -- 只记时刻: 短按=航班详情, 长按=设置 (在 poll 里判, 因为 action 只给按下沿)
        G.ok_t = app.millis()
        G.ok_pend = true
    elseif a == A_BACK then  app.exit()
    end
    return true
end

-- ---- 触摸 (当前板子没有触摸芯片; 逻辑保留, 换触屏板即可用) ----
local t_was = false
local function poll_touch()
    local x, y = app.touch()
    -- ★ 长按飞机 (≥600ms, 只触发一次) → 详情弹窗
    if x then
        if app.touch_hold() >= 600 and not G.lp_fired then
            local bp = hit_plane(x, y)
            if bp then
                G.lp_fired = true
                G.sel = bp
                open_detail()
            end
        end
    else
        G.lp_fired = false
    end
    if not x then
        t_was = false
        -- 抬指了: 这时才开详情弹窗 (按下沿开会被松手那一下点掉)
        if G.pend_detail then G.pend_detail = false; open_detail() end
        return
    end
    if t_was then return end
    t_was = true
    G.dirty = true
    -- 左上"位置"区: 点位置名 = 选省 → 选市 (位置调整走这里, 与"设置"分开)
    if y < 30 and x < 92 then
        open_province()
        return
    end
    -- 底部软键栏 (与频谱仪同款触摸区): 左=设置 右=扫描
    if y >= SOFT_LID then
        if x < 90 then
            open_settings()
        elseif x > 310 then
            if G.st == "loading" then
                app.dlg.toast("刷新中")
            elseif not app.net.is_connected() then
                app.dlg.toast("未联网")
            else
                flights_start()
            end
        end
        return
    end
    -- 雷达区: 点飞机 = 选中; 点**已选中的那架** 或 **双击同一架** → 详情
    -- ⚠ 弹窗**必须等抬指再开**: 按下沿就 push 的话, 松手那一下的 CONFIRM+tap 坐标会落到
    --   刚打开的弹窗上 → 直接点中某行 → 弹窗"打开就自动关闭" (实机踩到)。
    local bp = hit_plane(x, y)
    if bp then
        local now = app.millis()
        local was_sel = (bp == G.sel)
        local dbl = (G.tap_i == bp) and (now - G.tap_t <= 450)
        G.sel = bp
        if was_sel or dbl then G.pend_detail = true end
        G.tap_i, G.tap_t = bp, now
    end
end

function poll()
    build_rings()
    local now = app.millis()

    -- 确认键: 短按=详情, 长按 0.6s=设置 (标准二级弹窗)
    if G.ok_pend then
        if app.key_down(A_OK) then
            if now - G.ok_t >= 600 then
                G.ok_pend = false
                G.ok_long = true
                open_settings()
            end
        else
            G.ok_pend = false
            -- 短按 = 刷新取数 (详情常驻右侧, 不再弹窗)
            if G.st ~= "loading" then flights_start() end
        end
    elseif G.ok_long and not app.key_down(A_OK) then
        G.ok_long = false
    end

    -- 弹窗结果 (轮询式: 弹窗关闭后本页 poll 才跑; r = C 侧回调值, **0 基**, -1=取消/返回)
    if G.dlg then
        local r = app.dlg.result()
        if r ~= nil then
            local what = G.dlg
            G.dlg = nil
            if what == "prov" and r >= 0 then
                if r == 0 then
                    G.src = SRC_AUTO
                    G.place = ""
                    src_apply()                    -- 自动定位(IP)
                else
                    G.sel_city = 0
                    open_city(r)                   -- r = 省下标 (0 基, 与 POS 序号差 1)
                end
            elseif what == "city" and r >= 0 then
                local c = POS[G.sel_prov][4][r + 1]
                if c then
                    G.lat, G.lon, G.place = c[2], c[3], c[1]
                    G.src = SRC_PICK
                    G.sel_city = r
                    pos_save()
                    G.ac = {}
                    G.sel = 1
                    flights_start()
                end
            elseif what == "set" and r >= 0 then
                if r == 0 then app.wifi.setup()     -- 网络设置: 系统网络模块
                else open_range() end               -- 默认距离档
            elseif what == "range" and r >= 0 then
                G.rng = r + 1
                pos_save()
                G.ac = {}
                G.sel = 1
                flights_start()
            end
            -- what == "detail": 任何结果都只是关闭
            G.dirty = true
        end
        poll_touch()
        return
    end

    if G.geo == "run" then
        G.dirty = true
        local st, body, code, nm = app.net.fetch_poll()
        if st == "ok" then
            local la, lo, nm2 = geo_parse(body)
            if la then
                G.lat, G.lon = la, lo
                if nm2 and nm2 ~= "" then G.place = nm2 end
                G.geo = "done"
                G.geoerr = ""
                G.geotry = 0
                G.dirty = true
                app.nvs.set_int("pr_geoi", G.geoi)   -- 记住这个接口好用
                pos_save()
                G.ac = {}
                G.sel = 1
                flights_start()
            elseif G.geoi < #GEO_URLS then    -- 换下一个定位接口
                G.geoi = G.geoi + 1
                if not app.net.fetch_start(GEO_URLS[G.geoi], 8000) then
                    G.geo = "fail"; G.geot = app.millis(); G.geoerr = "请求忙"
                end
            else
                G.geo = "fail"; G.geot = app.millis(); G.geoerr = "无坐标"
            end
        elseif st == "err" then
            if G.geoi < #GEO_URLS then
                G.geoi = G.geoi + 1
                if not app.net.fetch_start(GEO_URLS[G.geoi], 8000) then
                    G.geo = "fail"; G.geot = app.millis(); G.geoerr = tostring(code)
                end
            else
                G.geo = "fail"; G.geot = app.millis()
                G.geoerr = nm or tostring(code)
            end
        end
        poll_touch()
        return                               -- 定位期间独占网络, 不与航班请求并发
    end

    if G.st == "loading" then
        G.dirty = true
        local st, body, code, nm = app.net.fetch_poll()
        G.dirty = true
        if st == "ok" then
            local rows = app.json.pick(body, "aircraft", FIELDS, MAX_AC)
            if type(rows) == "table" and #rows > 0 then apply_rows(rows) end
            G.st = "ok"
            G.err = ""
            G.upd = now                       -- 数据新鲜度基准
            G.t0 = now                       -- 刷新节拍从"收完"起算, 不是"发起"起算
        elseif st == "err" then
            G.st = "err"
            G.err = nm or tostring(code)
        end
    elseif G.geo == "idle" and G.src == SRC_AUTO then
        geo_start()                          -- 首次进来先定位, 定完再取航班
    elseif G.geo == "fail" and G.src == SRC_AUTO and G.geotry < 3
           and now - G.geot >= 10000 then
        -- 定位失败自愈 (2026-10-15 修): 原来 fail 后死等用户长按确认键重选来源,
        -- 网络抖一下就得手动干预; 现在每 10s 自动重试, 最多 3 轮。
        G.geotry = G.geotry + 1
        geo_start()
    elseif now - G.t0 >= REFRESH_MS then
        flights_start()
    end
    poll_touch()
end

function on_enter()
    pos_load()
    build_rings()
    G.st = "idle"
    G.geo = "idle"
    G.geoi = 1
    G.geoerr = ""
    G.ac = {}
    G.types = nil
    G.sel = 1
    G.dlg = nil
    G.sel_prov = 0
    G.sel_city = 0
    G.tap_i = nil
    G.tap_t = 0
    G.lp_fired = false
    G.pend_detail = false
    G.ok_pend = false
    G.ok_long = false
    G.t0 = 0
    G.upd = 0
    G.geotry = 0
    G.geot = 0
    G.dirty = true
    if G.src == SRC_AUTO then
        geo_start()                          -- 自动定位: 定完位再取航班
    else
        pos_apply_src()
        flights_start()
    end
end

function on_exit()
    G.dlg = nil
    G.ac = {}
end
