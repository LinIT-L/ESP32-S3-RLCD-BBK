<?php
/**
 * Plugin Name: LinTOS 项目中心
 * Description: 掌机项目：前台短代码 [lintos_resources] 渲染资源页；后台「掌机项目」管理资源与设备（联网统计/档案/固件）
 * Version: 1.1
 * Author: LinIT
 */
if (!defined('ABSPATH')) { exit; }

define('LP_ROOT', '/www/wwwroot/bbk.linit.cn');
define('LP_BASE', 'https://bbk.linit.cn');

/* ---- 设备模块（2026-10-31）: 数据层 + 设备端 REST + 后台页面 ---- */
define('LP_DEV_DIR', __DIR__ . '/lp-device');
require_once LP_DEV_DIR . '/lp-device-core.php';      /* 表/设置/统计/REST */
if (is_admin()) {
    require_once LP_DEV_DIR . '/lp-device-admin.php';  /* 后台页面（只在 admin 载入，前台零开销） */
}

/* 首次启用 / 版本升级时建表（幂等；不做破坏性变更） */
add_action('plugins_loaded', function () {
    if (get_option('lp_db_version') !== LP_DB_VERSION) { lp_install(); }
});

function lp_json($rel) {
    $f = LP_ROOT . $rel;
    if (!is_file($f)) { return array(); }
    $d = json_decode(file_get_contents($f), true);
    return is_array($d) ? $d : array();
}

function lp_firmware() {
    $out = array();
    foreach ((array) glob(LP_ROOT . '/fw/LinTOS-*.bin') as $f) {
        $out[] = array(
            'file' => basename($f),
            'size' => filesize($f),
            'time' => filemtime($f),
            'url'  => LP_BASE . '/fw/' . basename($f),
        );
    }
    usort($out, function ($a, $b) { return strcmp($b['file'], $a['file']); });
    return $out;
}

function lp_size($b) {
    if ($b >= 1048576) { return round($b / 1048576, 1) . ' MB'; }
    if ($b >= 1024) { return round($b / 1024) . ' KB'; }
    return $b . ' B';
}

/* ---------------- 前台短代码 ---------------- */
function lp_shortcode() {
    $idx   = lp_json('/bbk/index.json');
    $maps  = lp_json('/bbk/maps.json');
    $fw    = lp_firmware();
    $pkgs  = isset($idx['pkgs']) ? $idx['pkgs'] : array();
    $packs = isset($maps['packs']) ? $maps['packs'] : array();
    $files = isset($maps['files']) ? $maps['files'] : array();

    $fsize = array();
    foreach ($files as $f) { $fsize[$f['path']] = isset($f['size']) ? (int) $f['size'] : 0; }

    ob_start(); ?>
<style>
.lp{--lp-bd:rgba(0,0,0,.1);--lp-mut:#6b7280;--lp-acc:#2563eb}
@media(prefers-color-scheme:dark){.lp{--lp-bd:rgba(255,255,255,.14);--lp-mut:#9ca3af;--lp-acc:#60a5fa}}
.lp h2{font-size:19px;margin:34px 0 6px;padding-bottom:8px;border-bottom:1px solid var(--lp-bd)}
.lp h2:first-child{margin-top:0}
.lp .lp-sub{color:var(--lp-mut);font-size:13.5px;margin:0 0 16px}
.lp .lp-hero{border:1px solid var(--lp-bd);border-radius:12px;padding:18px 20px;margin:0 0 8px}
.lp .lp-hero b{font-size:15px}
.lp .lp-btn{display:inline-block;margin-top:12px;padding:10px 20px;border-radius:9px;background:var(--lp-acc);color:#fff!important;text-decoration:none;font-weight:600;font-size:14px}
.lp .lp-btn.ghost{background:transparent;border:1px solid var(--lp-bd);color:inherit!important;margin-left:8px}
.lp table{width:100%;border-collapse:collapse;font-size:14px}
.lp td,.lp th{border-top:1px solid var(--lp-bd);padding:9px 6px;text-align:left;vertical-align:top}
.lp th{border-top:0;color:var(--lp-mut);font-weight:500;font-size:12.5px;text-transform:uppercase;letter-spacing:.03em}
.lp td.n{white-space:nowrap;color:var(--lp-mut);font-size:13px;text-align:right}
.lp a.lk{text-decoration:none;border-bottom:1px solid transparent}
.lp a.lk:hover{border-bottom-color:var(--lp-acc)}
.lp .lp-tag{display:inline-block;font-size:11.5px;padding:1px 7px;border-radius:20px;background:rgba(37,99,235,.1);color:var(--lp-acc);margin-left:6px}
.lp .lp-note{color:var(--lp-mut);font-size:13px;margin-top:10px}
</style>
<div class="lp">

<?php if ($fw): $cur = $fw[0]; ?>
<h2>固件</h2>
<p class="lp-sub">一套固件适配多种扩展板，刷整片 16MB。</p>
<div class="lp-hero">
  <b><?php echo esc_html(str_replace('.bin','',$cur['file'])); ?></b>
  <span class="lp-tag">最新</span><br>
  <span style="color:var(--lp-mut);font-size:13.5px"><?php echo lp_size($cur['size']); ?> · 更新于 <?php echo date('Y-m-d', $cur['time']); ?></span><br>
  <a class="lp-btn" href="<?php echo esc_url($cur['url']); ?>">下载固件</a>
  <a class="lp-btn ghost" href="https://linit-l.github.io/ESP32-S3-RLCD-BBK/flash/" target="_blank" rel="noopener nofollow">在线刷机 ↗</a>
</div>
<div class="lp-note">刷机需 Chrome / Edge，用 USB 线连接设备，选择串口后一键写入。</div>
<?php endif; ?>

<h2>应用商店 <span style="color:var(--lp-mut);font-weight:400;font-size:14px"><?php echo count($pkgs); ?> 个</span></h2>
<p class="lp-sub">设备联网后自动获取；也可手动下载放入 <code>/sdcard</code>。</p>
<table>
<tr><th style="width:34%">名称</th><th>说明</th><th style="width:78px;text-align:right">大小</th></tr>
<?php foreach ($pkgs as $p): ?>
<tr>
  <td><a class="lk" href="<?php echo esc_url(LP_BASE . '/bbk/' . $p['path']); ?>"><?php echo esc_html($p['label']); ?></a></td>
  <td style="color:var(--lp-mut)"><?php echo esc_html($p['desc']); ?></td>
  <td class="n"><?php echo lp_size((int) $p['size']); ?></td>
</tr>
<?php endforeach; ?>
</table>

<?php if ($packs): ?>
<h2>离线地图</h2>
<p class="lp-sub">矢量地图包，支持离线浏览与定位。</p>
<table>
<tr><th style="width:34%">区域</th><th>包含文件</th><th style="width:78px;text-align:right">合计</th></tr>
<?php foreach ($packs as $pk):
    $sum = 0; $names = array();
    foreach ($pk['files'] as $fp) { $sum += isset($fsize[$fp]) ? $fsize[$fp] : 0;
        $names[] = '<a class="lk" href="' . esc_url(LP_BASE . '/bbk/' . $fp) . '">' . esc_html(basename($fp)) . '</a>'; }
?>
<tr>
  <td><?php echo esc_html($pk['label']); ?></td>
  <td style="color:var(--lp-mut)"><?php echo implode(' · ', $names); ?></td>
  <td class="n"><?php echo lp_size($sum); ?></td>
</tr>
<?php endforeach; ?>
</table>
<?php endif; ?>

<h2>设备端地址</h2>
<p class="lp-sub">在掌机「设置 → 应用商店」里填入以下地址即可自动拉取。</p>
<table>
<tr><td style="width:34%">应用商店</td><td><code><?php echo esc_html(LP_BASE . '/bbk/index.json'); ?></code></td></tr>
<tr><td>地图包</td><td><code><?php echo esc_html(LP_BASE . '/bbk/maps.json'); ?></code></td></tr>
</table>

</div>
<?php
    return ob_get_clean();
}
add_shortcode('lintos_resources', 'lp_shortcode');

/* ---------------- 后台管理页 ---------------- */
add_action('admin_menu', function () {
    add_menu_page('掌机项目', '掌机项目', 'manage_options', 'lintos-project', 'lp_admin_page', 'dashicons-smartphone', 58);
});

function lp_admin_page() {
    if (!current_user_can('manage_options')) { return; }
    $idx   = lp_json('/bbk/index.json');
    $maps  = lp_json('/bbk/maps.json');
    $fw    = lp_firmware();
    $pkgs  = isset($idx['pkgs']) ? $idx['pkgs'] : array();
    $packs = isset($maps['packs']) ? $maps['packs'] : array();
    $files = isset($maps['files']) ? $maps['files'] : array();
    $fwsize = 0; foreach ($fw as $f) { $fwsize += $f['size']; }
    ?>
    <div class="wrap">
      <h1>掌机项目资源</h1>
      <p style="color:#666">数据直接读自 <code><?php echo esc_html(LP_ROOT); ?></code>，与设备端同源。</p>

      <h2 class="title">概览</h2>
      <table class="widefat striped" style="max-width:760px">
        <tbody>
          <tr><td style="width:200px">固件文件</td><td><?php echo count($fw); ?> 个 · <?php echo lp_size($fwsize); ?></td></tr>
          <tr><td>应用包</td><td><?php echo count($pkgs); ?> 个 · <?php echo lp_size(array_sum(array_map(function($p){return (int)$p['size'];}, $pkgs))); ?></td></tr>
          <tr><td>地图包</td><td><?php echo count($packs); ?> 个区域 · <?php echo count($files); ?> 个文件</td></tr>
          <tr><td>清单更新时间</td><td><?php echo esc_html(isset($idx['updated']) ? $idx['updated'] : '—'); ?></td></tr>
          <tr><td>资源页地址</td><td><a href="<?php echo esc_url(LP_BASE . '/lintos/'); ?>" target="_blank"><?php echo esc_html(LP_BASE . '/lintos/'); ?></a></td></tr>
        </tbody>
      </table>

      <?php
      /* ---- 设备概览（真实数据；没数据就明确说没数据）---- */
      $dc = function_exists('lp_stats_cards') ? lp_stats_cards() : null;
      ?>
      <h2 class="title" style="margin-top:26px">设备</h2>
      <?php if ($dc === null): ?>
        <p style="color:#b32d2e">设备模块未加载（lp-device 目录缺失）。</p>
      <?php elseif ((int) $dc['total'] === 0): ?>
        <div class="notice notice-warning inline"><p><strong>尚未接入设备数据。</strong>
          目前没有任何设备上报过心跳，在线数无法统计。</p></div>
        <p><a class="button" href="<?php echo esc_url(admin_url('admin.php?page=lintos-devices-overview')); ?>">设备总览</a>
           <a class="button" href="<?php echo esc_url(admin_url('admin.php?page=lintos-settings')); ?>">设备接入设置</a></p>
      <?php else: ?>
        <table class="widefat striped" style="max-width:760px">
          <tbody>
            <tr><td style="width:200px">当前在线</td><td><b><?php echo (int) $dc['online']; ?></b> 台</td></tr>
            <tr><td>累计登记</td><td><?php echo (int) $dc['total']; ?> 台</td></tr>
            <tr><td>今日新增</td><td><?php echo (int) $dc['today']; ?> 台</td></tr>
            <tr><td>近 7 天活跃</td><td><?php echo (int) $dc['active7']; ?> 台</td></tr>
            <tr><td>最近一次上报</td><td><?php echo $dc['last_report'] ? esc_html($dc['last_report']) : '—'; ?></td></tr>
          </tbody>
        </table>
        <p><a class="button" href="<?php echo esc_url(admin_url('admin.php?page=lintos-devices')); ?>">设备管理</a>
           <a class="button" href="<?php echo esc_url(admin_url('admin.php?page=lintos-stats')); ?>">数据统计</a>
           <a class="button" href="<?php echo esc_url(admin_url('admin.php?page=lintos-settings')); ?>">项目设置</a></p>
      <?php endif; ?>

      <h2 class="title" style="margin-top:26px">固件</h2>
      <table class="widefat striped" style="max-width:760px">
        <thead><tr><th>文件</th><th style="width:110px">大小</th><th style="width:130px">更新时间</th></tr></thead>
        <tbody>
        <?php foreach ($fw as $f): ?>
          <tr><td><a href="<?php echo esc_url($f['url']); ?>" target="_blank"><?php echo esc_html($f['file']); ?></a></td>
              <td><?php echo lp_size($f['size']); ?></td>
              <td><?php echo date('Y-m-d H:i', $f['time']); ?></td></tr>
        <?php endforeach; ?>
        </tbody>
      </table>

      <h2 class="title" style="margin-top:26px">应用包</h2>
      <table class="widefat striped" style="max-width:760px">
        <thead><tr><th>名称</th><th style="width:90px">分类</th><th style="width:100px">大小</th><th style="width:100px">CRC32</th></tr></thead>
        <tbody>
        <?php foreach ($pkgs as $p): ?>
          <tr><td><a href="<?php echo esc_url(LP_BASE . '/bbk/' . $p['path']); ?>" target="_blank"><?php echo esc_html($p['label']); ?></a></td>
              <td><?php echo esc_html(isset($p['cat']) ? $p['cat'] : '—'); ?></td>
              <td><?php echo lp_size((int) $p['size']); ?></td>
              <td><code style="font-size:11px"><?php echo esc_html(isset($p['crc32']) ? $p['crc32'] : '—'); ?></code></td></tr>
        <?php endforeach; ?>
        </tbody>
      </table>

      <h2 class="title" style="margin-top:26px">地图包</h2>
      <table class="widefat striped" style="max-width:760px">
        <thead><tr><th>区域</th><th>文件</th></tr></thead>
        <tbody>
        <?php foreach ($packs as $pk): ?>
          <tr><td><?php echo esc_html($pk['label']); ?></td>
              <td><?php echo esc_html(implode(', ', $pk['files'])); ?></td></tr>
        <?php endforeach; ?>
        </tbody>
      </table>
    </div>
    <?php
}
