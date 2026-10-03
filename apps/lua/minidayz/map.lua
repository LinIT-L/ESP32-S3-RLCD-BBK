-- minidayz/map.lua — Mini DayZ 掌机版资源元信息 (由 tools/mdz_pack.py 生成)
-- 素材来源: ~/Desktop/minidayz-master/asset/images (Godot 复刻工程, 粉丝作品)
--   spr.bmp  人物/僵尸/动物 帧横排, 每帧 32px; 顺序:
--     player       帧 0..11
--     bandit       帧 12..23
--     tutor        帧 24..35
--     z_normal     帧 36..47
--     z_fast       帧 48..59
--     z_army       帧 60..71
--     z_shooter    帧 72..83
--     z_scream     帧 84..95
--     z_buried     帧 96..107
--     z_tank       帧 108..119
--     dog          帧 120..131
--     deer         帧 132..143
--     wolf         帧 144..155
--     rabbit       帧 156..167
--     crow         帧 168..179
--   item.bmp 道具图标 37 个 @32px
--   tile.bmp 地形 28 格 @20px = grass dirt tree water wall floor sand road
return {
  dir = "/store/minidayz/",
  spr_px = 32, spr_n = 180,
  item_px = 32, item_n = 37,
  tile = 20,
  groups = {
    { name="player", first=0, count=12 },
    { name="bandit", first=12, count=12 },
    { name="tutor", first=24, count=12 },
    { name="z_normal", first=36, count=12 },
    { name="z_fast", first=48, count=12 },
    { name="z_army", first=60, count=12 },
    { name="z_shooter", first=72, count=12 },
    { name="z_scream", first=84, count=12 },
    { name="z_buried", first=96, count=12 },
    { name="z_tank", first=108, count=12 },
    { name="dog", first=120, count=12 },
    { name="deer", first=132, count=12 },
    { name="wolf", first=144, count=12 },
    { name="rabbit", first=156, count=12 },
    { name="crow", first=168, count=12 },
  },
  items = {
    { name="军刀", icon=0, kind="melee", val=3 },
    { name="猎刀", icon=1, kind="melee", val=4 },
    { name="屠刀", icon=2, kind="melee", val=4 },
    { name="铁镐", icon=3, kind="melee", val=5 },
    { name="消防斧", icon=4, kind="melee", val=7 },
    { name="手枪", icon=5, kind="gun", val=8 },
    { name="手枪弹", icon=6, kind="tool", val=1 },
    { name="步枪弹", icon=7, kind="tool", val=1 },
    { name="苹果", icon=8, kind="food", val=8 },
    { name="橘子", icon=9, kind="food", val=7 },
    { name="香蕉", icon=10, kind="food", val=8 },
    { name="豆罐头", icon=11, kind="food", val=18 },
    { name="鱼罐头", icon=12, kind="food", val=20 },
    { name="鲱鱼", icon=13, kind="food", val=12 },
    { name="培根", icon=14, kind="food", val=22 },
    { name="军用口粮", icon=15, kind="food", val=35 },
    { name="熟肉排", icon=16, kind="food", val=28 },
    { name="军用水壶", icon=17, kind="drink", val=30 },
    { name="矿泉水", icon=18, kind="drink", val=25 },
    { name="可乐", icon=19, kind="drink", val=12 },
    { name="格瓦斯", icon=20, kind="drink", val=10 },
    { name="能量饮料", icon=21, kind="drink", val=15 },
    { name="绷带", icon=22, kind="med", val=12 },
    { name="生理盐水", icon=23, kind="med", val=25 },
    { name="血袋", icon=24, kind="med", val=40 },
    { name="吗啡", icon=25, kind="med", val=20 },
    { name="四环素", icon=26, kind="med", val=15 },
    { name="维生素", icon=27, kind="med", val=8 },
    { name="暖贴", icon=28, kind="med", val=6 },
    { name="火柴", icon=29, kind="tool", val=0 },
    { name="电池", icon=30, kind="tool", val=0 },
    { name="绳索", icon=31, kind="tool", val=0 },
    { name="胶带", icon=32, kind="tool", val=0 },
    { name="收音机", icon=33, kind="tool", val=0 },
    { name="工具箱", icon=34, kind="tool", val=0 },
    { name="清洁套件", icon=35, kind="tool", val=0 },
    { name="麻袋", icon=36, kind="tool", val=0 },
  },
  -- disp[地形id+1] = tile.bmp 里的图集序号(变体); 地形: grass dirt tree water wall floor sand road
  disp = {
    { 0, 1, 2, 3 },
    { 4 },
    { 5, 6, 7 },
    { 8 },
    { 9 },
    { 10, 11 },
    { 12 },
    { 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 },
  },
  -- 路面: 位置组首号 (m=路中 l=左缘 r=右缘 u=上缘 d=下缘); 组内 +0..2 是颗粒变体
  road_disp = { m = 13, l = 16, r = 19, u = 22, d = 25 },
  tile_n = 28,
}
