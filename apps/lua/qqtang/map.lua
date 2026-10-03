-- qqtang/map.lua — 《QQ堂》冒险模式资源元信息 (由 tools/qqt_pack.py 生成, 不要手改)
-- 设备端路径前缀 /store/qqtang/ ; 位图 = app.img.load(path) 后 blit
--   tile.bmp  13 格 @20px  = 0floor 1rubble 2rock 3treasure 4grass 5water 6exit 7floor_pebble 8floor_tuft 9floor_crack 10floor_slab 11rock_b 12rock_c
--   face.bmp  16 表情 @48px  0 是默认正脸
--   item.bmp  16 道具 @24px
--   wicon.bmp 12 世界图标(程序化线描符号) @40x36
--   equip.bmp 4 装备槽 @32px  0头 1身 2翅膀 3泡泡
--   sppr.bmp = 人物/怪物精灵; bg 墨量 0.07 (地板衬底, 很淡; 大地图下已基本不用)
return {
  dir = "/store/qqtang/",
  tile = 20,
  bg_w = 400, bg_h = 300,
  face_px = 48, item_px = 24, equip_px = 32, wicon_w = 40, wicon_h = 36,
  spr_px = 24, spr_hero = 8, spr_mon = 4,
  view_cols = 20, view_rows = 15,
  worlds = {
    { id=1, name="魔法王国1", bg="bg1.bmp", stages=6, icon="pve" },
    { id=2, name="魔法王国2", bg="bg2.bmp", stages=5, icon="sculpture" },
    { id=3, name="机械王国", bg="bg3.bmp", stages=4, icon="machine" },
    { id=4, name="黑龙王国", bg="bg4.bmp", stages=7, icon="tank" },
    { id=5, name="迷之森林", bg="bg5.bmp", stages=5, icon="field" },
    { id=6, name="迷之沙漠", bg="bg6.bmp", stages=5, icon="desert" },
    { id=7, name="迷之底", bg="bg7.bmp", stages=4, icon="mine" },
    { id=8, name="风暴海湾", bg="bg8.bmp", stages=3, icon="water" },
    { id=9, name="极昼平原", bg="bg9.bmp", stages=3, icon="town" },
    { id=10, name="怒风峡谷", bg="bg10.bmp", stages=3, icon="bomb" },
    { id=11, name="守望山麓", bg="bg11.bmp", stages=3, icon="pig" },
    { id=12, name="永冬之颠", bg="bg12.bmp", stages=2, icon="snow" },
  },
}
