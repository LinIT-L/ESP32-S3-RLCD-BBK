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
--   item.bmp 道具图标 64 个 @32px
--   tile.bmp 地形 8 格 @20px = grass dirt tree water wall floor sand road
return {
  dir = "/store/minidayz/",
  spr_px = 32, spr_n = 180,
  item_px = 32, item_n = 64,
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
}
