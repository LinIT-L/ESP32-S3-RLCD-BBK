#!/usr/bin/env python3
"""把 LBF + 许可文本 打成 stored ZIP（固件 am_asset 优先处理 method=0，零新依赖）。"""
import os
import zipfile

SRC = '/Users/linit/AI/LinTOS1.0/dist/fonts'
LIC = os.path.expanduser('~/Desktop/字体/LinTOS_字体整理_20261030/02_授权文件')
OUT = os.path.expanduser('~/Desktop/字体/LinTOS_字体整理_20261030/09_资源包')

# id: (lbf 文件名, 显示名, 原生字号, 许可文件, 许可简称, 上游)
FONTS = {
    'fusion8':    ('fusion8.lbf',    '缝合像素8',  8,  'OFL_FusionPixel.txt',          'OFL-1.1', 'TakWolf/fusion-pixel-font'),
    'fusion10':   ('fusion10.lbf',   '缝合像素10', 10, 'OFL_FusionPixel.txt',          'OFL-1.1', 'TakWolf/fusion-pixel-font'),
    'fusion12':   ('fusion12.lbf',   '缝合像素12', 12, 'OFL_FusionPixel.txt',          'OFL-1.1', 'TakWolf/fusion-pixel-font'),
    'ark12':      ('ark12.lbf',      '方舟像素12', 12, 'OFL_ArkPixel.txt',             'OFL-1.1', 'TakWolf/ark-pixel-font'),
    'ark12_otf':  ('ark12_otf.lbf',  '方舟像素12(OTF)', 12, 'OFL_ArkPixel.txt',        'OFL-1.1', 'TakWolf/ark-pixel-font'),
    'wqy12':      ('wqy12.lbf',      '文泉驿点阵宋12', 12, 'LICENSE_WenQuanYi.txt',    'GPL-2.0+字体嵌入例外', 'AmusementClub/WenQuanYi-Bitmap-Song-TTF'),
    'wqy14':      ('wqy14.lbf',      '文泉驿点阵宋14', 14, 'LICENSE_WenQuanYi.txt',    'GPL-2.0+字体嵌入例外', 'AmusementClub/WenQuanYi-Bitmap-Song-TTF'),
    'wqy16':      ('wqy16.lbf',      '文泉驿点阵宋16', 16, 'LICENSE_WenQuanYi.txt',    'GPL-2.0+字体嵌入例外', 'AmusementClub/WenQuanYi-Bitmap-Song-TTF'),
    'unifont16':  ('unifont16.lbf',  'GNU Unifont16', 16, 'OFL_Unifont.txt',           'OFL-1.1/GPL-2.0', 'GNU Unifont'),
    'chill7':     ('chill7.lbf',     '寒蝉点阵体7', 7,   'OFL_ChillBitmap.txt',         'OFL-1.1', 'Warren2060/ChillBitmap'),
    'qiuye16':    ('qiuye16.lbf',    '秋叶圆体16', 16,   'OFL_QiuYeYuanTi-16.txt',      'OFL-1.1', 'yzdnn/QiuYeYuanTi-16'),
    'zhengge16':  ('zhengge16.lbf',  '正格点黑16', 16,   'OFL_ZhengGeDianHei-16.txt',   'OFL-1.1', 'yzdnn/ZhengGeDianHei-16'),
    'changban16': ('changban16.lbf', '长坂点宋16', 16,   'OFL_ChangBanDianSong-16.txt', 'OFL-1.1', 'yzdnn/ChangBanDianSong-16'),
}


def main():
    os.makedirs(OUT, exist_ok=True)
    for fid, (lbf, disp, px, lic, licname, upstream) in FONTS.items():
        src = os.path.join(SRC, lbf)
        if not os.path.exists(src):
            print('SKIP 缺文件', src)
            continue
        zp = os.path.join(OUT, 'font_%s.zip' % fid)
        with zipfile.ZipFile(zp, 'w', zipfile.ZIP_STORED) as z:
            z.write(src, lbf)
            lp = os.path.join(LIC, lic)
            if os.path.exists(lp):
                z.write(lp, '%s.LICENSE.txt' % fid)
            else:
                print('  ! 许可文件缺失', lic)
            readme = (
                '%s\n'
                '====================\n'
                '名称: %s\n'
                '字号: %dpx\n'
                '格式: LBF v1（LinTOS 阅读器字体）\n'
                '许可: %s\n'
                '上游: %s\n'
                '\n'
                '安装: 解压到 TF 卡 /sdcard/fonts/ 后，在阅读器菜单 → 字体 中选择。\n'
                '字体为点阵字模，未做二次修改；许可全文见同包 %s.LICENSE.txt。\n'
            ) % (disp, disp, px, licname, upstream, fid)
            z.writestr('%s.README.txt' % fid, readme.encode('utf-8'))
        print('%-12s %8d  %s' % (fid, os.path.getsize(zp), os.path.basename(zp)))


if __name__ == '__main__':
    main()
