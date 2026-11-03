<?php
/**
 * Plugin Name: LinTOS 产品页
 * Description: 为 /lintos/ 提供产品展示页短代码 [lintos_home]。与 linit-project.php 并存，互不依赖。
 * Version: 1.0
 * Author: LinIT
 *
 * 为什么单独一个文件（2026-10-31）
 * --------------------------------
 * linit-project.php 正被另一路任务改造（新增 lp-device 设备统计模块），
 * 直接在它里面加页面代码会和那边的改动撞车。这里用独立文件 + 独立短代码 +
 * 独立函数前缀（lx_），两边的改动可以各自上线、各自回滚。
 *
 * 数据原则
 * --------
 * · 数字全部现场读真实数据源：/fw 目录、/bbk/index.json、/bbk/maps.json 的**聚合值**；
 * · 地图只公开「区域名 / 区域数 / 合计体积 / 更新时间」，不输出文件名、路径、内部 ID；
 * · 不写死版本号、大小、时间，避免过期信息。
 */
if (!defined('ABSPATH')) { exit; }

if (!defined('LX_ROOT'))   { define('LX_ROOT', '/www/wwwroot/bbk.linit.cn'); }
if (!defined('LX_BASE'))   { define('LX_BASE', 'https://bbk.linit.cn'); }
if (!defined('LX_UPLOAD')) { define('LX_UPLOAD', 'https://bbk.linit.cn/upload/'); }
if (!defined('LX_REPO'))   { define('LX_REPO', 'https://github.com/LinIT-L/ESP32-S3-RLCD-BBK'); }

function lx_read_json($rel) {
    $f = LX_ROOT . $rel;
    if (!is_file($f)) { return array(); }
    $d = json_decode((string) file_get_contents($f), true);
    return is_array($d) ? $d : array();
}

function lx_firmwares() {
    $out = array();
    foreach ((array) glob(LX_ROOT . '/fw/LinTOS-*.bin') as $f) {
        $out[] = array(
            'file' => basename($f),
            'size' => (int) filesize($f),
            'time' => (int) filemtime($f),
            'url'  => LX_BASE . '/fw/' . rawurlencode(basename($f)),
        );
    }
    usort($out, function ($a, $b) { return strcmp($b['file'], $a['file']); });
    return $out;
}

function lx_hsize($b) {
    $b = (int) $b;
    if ($b >= 1048576) { return round($b / 1048576, 1) . ' MB'; }
    if ($b >= 1024)    { return round($b / 1024) . ' KB'; }
    return $b . ' B';
}

/* 内联 SVG 图标（不引外部依赖、不用图标字体） */
function lx_icon($k) {
    static $m = array(
        'lua'   => '<path d="M4 5h16v14H4z"/><path d="M8 10l2 2-2 2M12 14h4"/>',
        'game'  => '<rect x="3" y="7" width="18" height="10" rx="4"/><path d="M8 10v4M6 12h4M16 11h.01M18 13h.01"/>',
        'book'  => '<path d="M5 4h9a3 3 0 013 3v13H8a3 3 0 00-3 3z"/><path d="M5 4v16"/>',
        'store' => '<path d="M4 9h16l-1-4H5z"/><path d="M5 9v10h14V9"/><path d="M9 19v-6h6v6"/>',
        'map'   => '<path d="M9 4L4 6v14l5-2 6 2 5-2V4l-5 2-6-2z"/><path d="M9 4v14M15 6v14"/>',
        'code'  => '<path d="M9 8l-4 4 4 4M15 8l4 4-4 4"/>',
        'down'  => '<path d="M12 4v10M8 10l4 4 4-4M5 19h14"/>',
        'chip'  => '<rect x="7" y="7" width="10" height="10" rx="2"/><path d="M10 3v4M14 3v4M10 17v4M14 17v4M3 10h4M3 14h4M17 10h4M17 14h4"/>',
    );
    $d = isset($m[$k]) ? $m[$k] : $m['chip'];
    return '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" '
         . 'stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' . $d . '</svg>';
}

/* ---------------- 前台 ---------------- */
function lx_lintos_home() {
    $idx  = lx_read_json('/bbk/index.json');
    $maps = lx_read_json('/bbk/maps.json');
    $fw   = lx_firmwares();
    $pkgs = isset($idx['pkgs']) ? (array) $idx['pkgs'] : array();
    $packs = isset($maps['packs']) ? (array) $maps['packs'] : array();
    $mfiles = isset($maps['files']) ? (array) $maps['files'] : array();

    /* 按分类分组，保留 index.json 里首次出现的顺序 */
    $groups = array();
    foreach ($pkgs as $p) {
        $c = (isset($p['cat']) && $p['cat'] !== '') ? $p['cat'] : '其他';
        $groups[$c][] = $p;
    }
    $store_ver = isset($idx['ver']) ? (int) $idx['ver'] : 0;

    /* 地图：只做聚合，绝不输出文件名/路径 */
    $size_of = array();
    foreach ($mfiles as $f) { if (isset($f['path'])) { $size_of[$f['path']] = (int) $f['size']; } }
    $regions = array();
    $map_bytes = 0;
    foreach ($packs as $pk) {
        if (!empty($pk['label'])) { $regions[] = $pk['label']; }
        foreach ((array) $pk['files'] as $fp) { $map_bytes += isset($size_of[$fp]) ? $size_of[$fp] : 0; }
    }
    $map_updated = isset($maps['updated']) ? $maps['updated'] : '';

    ob_start(); ?>
<style>
.lx{--lx-bd:rgba(0,0,0,.1);--lx-bd2:rgba(0,0,0,.06);--lx-mut:#6b7280;--lx-acc:#2563eb;--lx-bg2:#f7f8f9}
@media(prefers-color-scheme:dark){.lx{--lx-bd:rgba(255,255,255,.16);--lx-bd2:rgba(255,255,255,.09);--lx-mut:#9ca3af;--lx-acc:#60a5fa;--lx-bg2:rgba(255,255,255,.03)}}
.lx{font-size:15px;line-height:1.7}
.lx *{box-sizing:border-box}
.lx h1{margin:0 0 8px;font-size:34px;line-height:1.15;letter-spacing:-.02em}
.lx h2{font-size:20px;margin:38px 0 4px;padding-bottom:9px;border-bottom:1px solid var(--lx-bd)}
.lx h2 .lx-n{float:right;font-size:13.5px;font-weight:400;color:var(--lx-mut);line-height:2}
.lx .lx-slogan{margin:0 0 10px;font-size:17px;font-weight:600;color:var(--lx-mut)}
.lx .lx-intro{margin:0;color:var(--lx-mut);max-width:62ch}
.lx .lx-hero{padding:2px 0 22px;border-bottom:1px solid var(--lx-bd)}
.lx .lx-sub{color:var(--lx-mut);font-size:13.5px;margin:0 0 14px}
.lx .lx-cta{display:flex;flex-wrap:wrap;gap:10px;margin-top:18px}
.lx .lx-btn{display:inline-flex;align-items:center;gap:7px;padding:11px 20px;border-radius:10px;background:var(--lx-acc);color:#fff!important;text-decoration:none!important;font-weight:600;font-size:14.5px;border:1px solid transparent;transition:opacity .15s}
.lx .lx-btn:hover{opacity:.88}
.lx .lx-btn.ghost{background:transparent;border-color:var(--lx-bd);color:inherit!important}
.lx .lx-btn svg{width:17px;height:17px;flex:none}
.lx .lx-card{border:1px solid var(--lx-bd);border-radius:12px;padding:16px 18px;background:var(--lx-bg2)}
.lx .lx-fw{display:flex;flex-wrap:wrap;gap:14px;align-items:center;justify-content:space-between}
.lx .lx-fw .lx-v{font-size:19px;font-weight:700}
.lx .lx-meta{color:var(--lx-mut);font-size:13px;margin-top:3px}
.lx .lx-grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fill,minmax(232px,1fr));margin-top:14px}
.lx .lx-feat{border:1px solid var(--lx-bd);border-radius:12px;padding:15px 16px}
.lx .lx-feat svg{width:21px;height:21px;color:var(--lx-acc)}
.lx .lx-feat b{display:block;margin:9px 0 4px;font-size:14.5px}
.lx .lx-feat span{display:block;color:var(--lx-mut);font-size:13px;line-height:1.6}
.lx .lx-app{border:1px solid var(--lx-bd);border-radius:12px;padding:14px 15px;display:flex;flex-direction:column;gap:6px}
.lx .lx-app b{font-size:14.5px}
.lx .lx-app .lx-desc{color:var(--lx-mut);font-size:13px;line-height:1.55;flex:1}
.lx .lx-app .lx-foot{display:flex;justify-content:space-between;align-items:center;font-size:12.5px;color:var(--lx-mut);padding-top:6px;border-top:1px solid var(--lx-bd2)}
.lx .lx-app a{text-decoration:none}
.lx .lx-app a:hover{color:var(--lx-acc)}
.lx .lx-cat{font-size:14px;font-weight:600;margin:22px 0 0;color:var(--lx-mut)}
.lx .lx-map .lx-card b{display:block;margin:8px 0 4px;font-size:14.5px}
.lx .lx-map .lx-card span{color:var(--lx-mut);font-size:13px;line-height:1.6}
.lx .lx-map .lx-card svg{width:21px;height:21px;color:var(--lx-acc)}
.lx .lx-kv{display:flex;flex-wrap:wrap;gap:8px 22px;margin-top:14px;font-size:13.5px;color:var(--lx-mut)}
.lx .lx-kv b{color:inherit;font-weight:600}
.lx .lx-note{color:var(--lx-mut);font-size:13px;margin-top:12px}
.lx .lx-dev{border:1px solid var(--lx-bd);border-radius:12px;padding:20px 22px;margin-top:14px;background:var(--lx-bg2)}
.lx .lx-dev>p{margin:6px 0 0;color:var(--lx-mut);font-size:13.5px;max-width:60ch}
@media(max-width:600px){
  .lx h1{font-size:27px}.lx .lx-slogan{font-size:15px}
  .lx .lx-fw{flex-direction:column;align-items:flex-start}
  .lx .lx-cta .lx-btn{flex:1 1 100%;justify-content:center}
  .lx h2{margin-top:30px}
}
</style>
<div class="lx">

  <div class="lx-hero">
    <h1>LinTOS</h1>
    <p class="lx-slogan">让经典电子词典，拥有更多可能。</p>
    <p class="lx-intro">LinTOS 是面向 ESP32-S3 掌机的开源系统，提供经典游戏模拟、Lua 可扩展应用、阅读器、字体资源、离线地图等功能，让传统电子词典拥有更多玩法和扩展能力。</p>
    <div class="lx-cta">
      <a class="lx-btn" href="#lx-fw"><?php echo lx_icon('down'); ?>固件下载</a>
      <a class="lx-btn ghost" href="#lx-store"><?php echo lx_icon('store'); ?>Lua 应用商店</a>
      <a class="lx-btn ghost" href="<?php echo esc_url(LX_UPLOAD); ?>"><?php echo lx_icon('lua'); ?>Lua 上传</a>
    </div>
  </div>

<?php if ($fw): $cur = $fw[0];
      $ver = str_replace(array('LinTOS-', '-16MB.bin', '.bin'), '', $cur['file']); ?>
  <h2 id="lx-fw">固件下载</h2>
  <p class="lx-sub">一套固件适配小智一体版 / 微雪原版 / 标准扩展版，整片 16MB 写入。</p>
  <div class="lx-card lx-fw">
    <div>
      <div class="lx-v">LinTOS <?php echo esc_html($ver); ?></div>
      <div class="lx-meta"><?php echo lx_hsize($cur['size']); ?> ·
        更新于 <?php echo date('Y-m-d', $cur['time']); ?> ·
        ESP32-S3 / 16MB Flash</div>
    </div>
    <div class="lx-cta" style="margin:0">
      <a class="lx-btn" href="<?php echo esc_url($cur['url']); ?>"><?php echo lx_icon('down'); ?>下载固件</a>
      <a class="lx-btn ghost" href="<?php echo esc_url(LX_REPO . '/releases'); ?>" target="_blank" rel="noopener nofollow">更新说明 ↗</a>
    </div>
  </div>
  <div class="lx-note">刷机：Chrome / Edge + USB 线连接设备，选串口后一键写入。刷机有风险，动手前先备份原机固件。</div>
<?php endif; ?>

  <h2>系统特色</h2>
  <p class="lx-sub">以下均为当前版本已实现的功能。</p>
  <div class="lx-grid">
    <div class="lx-feat"><?php echo lx_icon('lua'); ?><b>可扩展 Lua 应用</b><span>单文件 Lua 应用，装上即用；开发者可不断投稿扩展。</span></div>
    <div class="lx-feat"><?php echo lx_icon('game'); ?><b>经典游戏模拟</b><span>步步高词典机、NES、GBC、GB、Arduboy 等多套模拟核心。</span></div>
    <div class="lx-feat"><?php echo lx_icon('book'); ?><b>阅读器</b><span>TXT / EPUB / FB2，支持 TF 卡字体与内置点阵字库。</span></div>
    <div class="lx-feat"><?php echo lx_icon('store'); ?><b>应用商店</b><span>设备联网后在线获取应用、游戏与字体，自动检查更新。</span></div>
    <div class="lx-feat"><?php echo lx_icon('map'); ?><b>离线地图</b><span>矢量地图包，道路与地标分层，下载后可完全离线浏览定位。</span></div>
    <div class="lx-feat"><?php echo lx_icon('code'); ?><b>开源系统</b><span>基于 ESP-IDF，源码公开、持续更新，欢迎社区参与。</span></div>
  </div>

  <h2 id="lx-store">Lua 应用生态 <span class="lx-n"><?php echo count($pkgs); ?> 个应用</span></h2>
  <p class="lx-sub">与设备端商店同源（索引 ver <?php echo $store_ver; ?>），新应用会自动出现在这里。</p>
<?php foreach ($groups as $cat => $list): ?>
  <div class="lx-cat"><?php echo esc_html($cat); ?> · <?php echo count($list); ?></div>
  <div class="lx-grid">
  <?php foreach ($list as $p): ?>
    <div class="lx-app">
      <b><?php echo esc_html($p['label']); ?></b>
      <div class="lx-desc"><?php echo esc_html($p['desc']); ?></div>
      <div class="lx-foot">
        <span><?php echo lx_hsize(isset($p['size']) ? $p['size'] : 0); ?></span>
        <a href="<?php echo esc_url(LX_BASE . '/bbk/' . $p['path']); ?>">下载 →</a>
      </div>
    </div>
  <?php endforeach; ?>
  </div>
<?php endforeach; ?>

  <h2>离线地图</h2>
  <p class="lx-sub">地图数据由服务端统一生成、统一发布，设备按需下载后离线使用。</p>
  <div class="lx-grid lx-map">
    <div class="lx-card"><?php echo lx_icon('map'); ?><b>统一生成</b><span>按预设区域与精度规则批量生成，不靠人工逐个上传。</span></div>
    <div class="lx-card"><?php echo lx_icon('chip'); ?><b>矢量压缩</b><span>只存折线不存图片，任意缩放都清晰，体积远小于栅格瓦片。</span></div>
    <div class="lx-card"><?php echo lx_icon('down'); ?><b>下载即用</b><span>设备内一键下载到 TF 卡，之后完全离线浏览与定位。</span></div>
  </div>
  <div class="lx-kv">
    <?php if ($regions): ?><div>当前覆盖：<b><?php echo esc_html(implode('、', $regions)); ?></b></div><?php endif; ?>
    <?php if ($map_bytes): ?><div>数据合计：<b><?php echo lx_hsize($map_bytes); ?></b></div><?php endif; ?>
    <?php if ($map_updated): ?><div>最近更新：<b><?php echo esc_html($map_updated); ?></b></div><?php endif; ?>
  </div>
  <div class="lx-note">获取方式：掌机「设置 → 应用商店」中按区域下载；新区域会在设备下次检查更新时出现。</div>

  <h2>一起扩展 LinTOS</h2>
  <div class="lx-dev">
    <b style="font-size:16px">欢迎开发者参与</b>
    <p>LinTOS 支持 Lua 应用扩展。欢迎制作工具、游戏和实用应用，通过投稿平台分享——审核通过后进入商店，被所有设备下载。</p>
    <div class="lx-cta">
      <a class="lx-btn" href="<?php echo esc_url(LX_UPLOAD); ?>"><?php echo lx_icon('lua'); ?>Lua 上传</a>
      <a class="lx-btn ghost" href="<?php echo esc_url(LX_BASE . '/category/' . rawurlencode('技术交流') . '/'); ?>">技术交流</a>
    </div>
  </div>

</div>
<?php
    return ob_get_clean();
}
add_shortcode('lintos_home', 'lx_lintos_home');
