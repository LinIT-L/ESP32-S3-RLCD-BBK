<?php
/**
 * lp-device-admin.php — 掌机项目 · 后台页面（总览 / 设备 / 详情 / 统计 / 设置）
 *
 * 复用 WP 自带样式（wrap / widefat / nav-tab / button），不引入外部 JS 库；
 * 图表用服务端生成的 SVG（数据全部来自数据库，没有一处假数据）。
 * 所有写操作：capability + nonce + 二次确认(前端 confirm) + 审计日志。
 */
if (!defined('ABSPATH')) { exit; }

define('LP_CAP', 'manage_options');   /* 设备档案含 MAC/IP：只给管理员 */

/* ============================================================ 小工具 */

function lp_e($s) { return esc_html((string) $s); }

function lp_human_time($mysql_dt) {
    if (empty($mysql_dt)) { return '—'; }
    $ts = strtotime($mysql_dt);
    if (!$ts) { return '—'; }
    return date('Y-m-d H:i', $ts);
}

function lp_ago($mysql_dt) {
    if (empty($mysql_dt)) { return '—'; }
    /* 用本地时间 epoch 做基准，与 current_time('mysql') 写入的值同源 */
    $d = lp_now_ts() - strtotime($mysql_dt);
    if ($d < 0) { return '刚刚'; }
    if ($d < 60) { return $d . ' 秒前'; }
    if ($d < 3600) { return floor($d / 60) . ' 分钟前'; }
    if ($d < 86400) { return floor($d / 3600) . ' 小时前'; }
    return floor($d / 86400) . ' 天前';
}

function lp_is_online($row) {
    if (empty($row['last_hb'])) { return false; }
    return strtotime((string) $row['last_hb']) >= lp_now_ts() - (int) lp_setting('online_timeout', 900);
}

/** 极简柱状图（SVG，纯服务端生成） */
function lp_bar_chart(array $rows, $label = 'k', $value = 'n', $w = 720, $h = 160) {
    if (!$rows) { return '<p style="color:#888">暂无数据</p>'; }
    $max = 1;
    foreach ($rows as $r) { $max = max($max, (int) $r[$value]); }
    $n = count($rows);
    $bw = max(6, (int) floor(($w - 20) / max(1, $n)) - 6);
    $out = '<svg width="100%" viewBox="0 0 ' . $w . ' ' . ($h + 34) . '" style="max-width:' . $w . 'px">';
    $i = 0;
    foreach ($rows as $r) {
        $v = (int) $r[$value];
        $bh = (int) round(($h - 10) * ($v / $max));
        $x = 10 + $i * ($bw + 6);
        $y = $h - $bh;
        $out .= '<rect x="' . $x . '" y="' . $y . '" width="' . $bw . '" height="' . $bh . '" fill="#2271b1" rx="2"></rect>';
        $out .= '<text x="' . ($x + $bw / 2) . '" y="' . ($y - 4) . '" font-size="10" text-anchor="middle" fill="#555">' . $v . '</text>';
        $lab = (string) $r[$label];
        $out .= '<text x="' . ($x + $bw / 2) . '" y="' . ($h + 14) . '" font-size="9" text-anchor="middle" fill="#888">'
              . lp_e(substr($lab, -6)) . '</text>';
        $i++;
    }
    $out .= '<line x1="0" y1="' . $h . '" x2="' . $w . '" y2="' . $h . '" stroke="#ddd"></line></svg>';
    return $out;
}

/** 极简折线图 */
function lp_line_chart(array $rows, $label = 'k', $value = 'n', $w = 720, $h = 160) {
    if (!$rows) { return '<p style="color:#888">暂无数据</p>'; }
    $max = 1;
    foreach ($rows as $r) { $max = max($max, (int) $r[$value]); }
    $n = count($rows);
    $dx = $n > 1 ? ($w - 20) / ($n - 1) : 0;
    $pts = array();
    $i = 0;
    foreach ($rows as $r) {
        $v = (int) $r[$value];
        $x = 10 + $i * $dx;
        $y = $h - (($h - 10) * ($v / $max));
        $pts[] = round($x, 1) . ',' . round($y, 1);
        $i++;
    }
    $out = '<svg width="100%" viewBox="0 0 ' . $w . ' ' . ($h + 34) . '" style="max-width:' . $w . 'px">';
    $out .= '<polyline fill="none" stroke="#2271b1" stroke-width="2" points="' . implode(' ', $pts) . '"></polyline>';
    foreach ($pts as $idx => $p) {
        list($x, $y) = explode(',', $p);
        $out .= '<circle cx="' . $x . '" cy="' . $y . '" r="2.5" fill="#2271b1"></circle>';
    }
    $out .= '<line x1="0" y1="' . $h . '" x2="' . $w . '" y2="' . $h . '" stroke="#ddd"></line>';
    $lab = array();
    foreach ($rows as $r) { $lab[] = (string) $r[$label]; }
    if ($lab) {
        $out .= '<text x="10" y="' . ($h + 16) . '" font-size="9" fill="#888">' . lp_e($lab[0]) . '</text>';
        if (count($lab) > 1) {
            $out .= '<text x="' . ($w - 10) . '" y="' . ($h + 16) . '" font-size="9" text-anchor="end" fill="#888">'
                  . lp_e(end($lab)) . '</text>';
        }
    }
    $out .= '</svg>';
    return $out;
}

function lp_kpi($num, $label, $hint = '', $tone = '') {
    $style = $tone === 'warn' ? 'border-left:3px solid #dba617' : ($tone === 'ok' ? 'border-left:3px solid #00a32a' : '');
    return '<div style="background:#fff;border:1px solid #dcdcde;border-radius:6px;padding:12px 16px;min-width:132px;' . $style . '">'
         . '<div style="font-size:24px;font-weight:600;line-height:1.2">' . lp_e($num) . '</div>'
         . '<div style="color:#646970;font-size:12px;margin-top:2px">' . lp_e($label) . '</div>'
         . ($hint !== '' ? '<div style="color:#999;font-size:11px;margin-top:4px">' . lp_e($hint) . '</div>' : '')
         . '</div>';
}

/** 没数据时的显式提示（要求：不能显示虚构在线数） */
function lp_empty_notice(array $cards) {
    if ((int) $cards['total'] > 0) { return ''; }
    return '<div class="notice notice-warning" style="margin:12px 0"><p><strong>尚未接入设备数据。</strong>'
         . '下表的 0 是真实值——目前没有任何设备向服务器上报过心跳。'
         . '设备端需要先实现注册/心跳（协议见 <code>docs/规范/设备接入协议.md</code>），'
         . '或在「设置 → 设备接入」里配置产品密钥后按协议上报。</p></div>';
}

/* ============================================================ POST 处理 */

function lp_admin_handle_post() {
    if (empty($_POST['lp_action'])) { return null; }
    if (!current_user_can(LP_CAP)) { return array('err', '没有权限'); }
    $act = sanitize_key($_POST['lp_action']);
    check_admin_referer('lp_' . $act);

    switch ($act) {
        case 'settings':
            lp_save_settings($_POST);
            return array('ok', '设置已保存（立即生效）');

        case 'device_update':
            $dev = sanitize_text_field($_POST['dev_id'] ?? '');
            global $wpdb;
            $t = lp_tbl('devices');
            $row = lp_dev_get($dev);
            if (!$row) { return array('err', '设备不存在'); }
            $upd = array(
                'name'     => substr(sanitize_text_field($_POST['name'] ?? ''), 0, 64),
                'note'     => substr(sanitize_text_field($_POST['note'] ?? ''), 0, 255),
                'disabled' => empty($_POST['disabled']) ? 0 : 1,
            );
            $wpdb->update($t, $upd, array('dev_id' => $dev));
            lp_audit('device.update', $dev, 'name=' . $upd['name'] . ' disabled=' . $upd['disabled']);
            return array('ok', '设备档案已更新');

        case 'device_delete':
            $dev = sanitize_text_field($_POST['dev_id'] ?? '');
            if (!lp_dev_get($dev)) { return array('err', '设备不存在'); }
            global $wpdb;
            $wpdb->update(lp_tbl('devices'),
                array('deleted_at' => current_time('mysql')), array('dev_id' => $dev));
            lp_audit('device.delete', $dev, '逻辑删除（可恢复）');
            return array('ok', '已逻辑删除（历史记录保留，可在清理时物理删除）');

        case 'device_restore':
            $dev = sanitize_text_field($_POST['dev_id'] ?? '');
            global $wpdb;
            $wpdb->update(lp_tbl('devices'), array('deleted_at' => null), array('dev_id' => $dev));
            lp_audit('device.restore', $dev, '');
            return array('ok', '已恢复');

        case 'cleanup':
            $r = lp_cleanup();
            return array('ok', '清理完成：事件 ' . $r['events'] . ' 条 / nonce ' . $r['nonces'] . ' 条 / 设备 ' . $r['devices'] . ' 台');

        case 'fw_add':
            global $wpdb;
            $v = substr(sanitize_text_field($_POST['version'] ?? ''), 0, 32);
            if ($v === '') { return array('err', '版本号不能为空'); }
            $wpdb->replace(lp_tbl('fw'), array(
                'version' => $v, 'released' => current_time('mysql'),
                'notes' => substr(sanitize_textarea_field($_POST['notes'] ?? ''), 0, 2000),
                'min_compat' => substr(sanitize_text_field($_POST['min_compat'] ?? ''), 0, 32),
            ));
            lp_audit('fw.add', $v, '');
            return array('ok', '固件版本 ' . $v . ' 已登记');
    }
    return array('err', '未知操作');
}

/* ---------- 一次性提示（PRG 用 transient，不把文案塞进 URL） ---------- */
function lp_flash_set($type, $text) {
    set_transient('lp_flash_' . get_current_user_id(), array($type, $text), 60);
}
function lp_flash_get() {
    $k = 'lp_flash_' . get_current_user_id();
    $v = get_transient($k);
    if ($v) { delete_transient($k); }
    return $v;
}

/** 每个页面开头调：处理 POST → 跳回当前页（PRG）。返回 flash 给渲染用。 */
function lp_maybe_handle_post() {
    $r = lp_admin_handle_post();
    if ($r === null) { return lp_flash_get(); }
    lp_flash_set($r[0], $r[1]);
    $url = remove_query_arg('lp_msg');
    wp_safe_redirect($url);
    exit;
}

function lp_render_flash($f) {
    if (!$f) { return; }
    $cls = $f[0] === 'ok' ? 'notice-success' : 'notice-error';
    echo '<div class="notice ' . $cls . ' is-dismissible"><p>' . lp_e($f[1]) . '</p></div>';
}

/* ============================================================ 页面：总览 */

function lp_page_overview() {
    if (!current_user_can(LP_CAP)) { wp_die('没有权限'); }
    $c = lp_stats_cards();
    $s = lp_settings();
    $fw = lp_dist('fw', 8);
    $board = lp_dist('board', 8);
    ?>
    <div class="wrap">
      <h1>掌机项目 · 设备总览</h1>
      <?php echo lp_empty_notice($c); ?>
      <p style="color:#646970;margin:6px 0 16px">
        在线判定：最后心跳在 <strong><?php echo (int) $s['online_timeout']; ?> 秒</strong>内算在线
        （可在「设置」里改）。设备曾经注册过不等于在线。
      </p>

      <div style="display:flex;gap:12px;flex-wrap:wrap;margin-bottom:20px">
        <?php
        echo lp_kpi($c['online'], '当前在线', '', 'ok');
        echo lp_kpi($c['total'], '累计登记设备');
        echo lp_kpi($c['today'], '今日新增');
        echo lp_kpi($c['active7'], '近 7 天活跃');
        echo lp_kpi($c['active30'], '近 30 天活跃');
        echo lp_kpi($c['offline'], '当前离线');
        echo lp_kpi($c['fw_count'], '不同固件版本');
        ?>
      </div>

      <table class="widefat striped" style="max-width:820px">
        <tbody>
          <tr><td style="width:200px">最近一次设备上报</td>
              <td><?php echo $c['last_report'] ? lp_e(lp_human_time($c['last_report'])) . '（' . lp_e(lp_ago($c['last_report'])) . '）' : '<span style="color:#b32d2e">从未收到</span>'; ?></td></tr>
          <tr><td>长期离线（超 <?php echo (int) $s['offline_grace']; ?> 秒）</td><td><?php echo (int) $c['long_offline']; ?> 台</td></tr>
          <tr><td>注册接口</td><td><code><?php echo lp_e(rest_url('lp/v1/register')); ?></code>
              <?php echo $s['allow_register'] && $s['product_key'] !== '' ? '<span style="color:#00a32a">开放</span>' : '<span style="color:#b32d2e">关闭</span>'; ?></td></tr>
          <tr><td>心跳接口</td><td><code><?php echo lp_e(rest_url('lp/v1/heartbeat')); ?></code></td></tr>
          <tr><td>协议版本</td><td>v<?php echo (int) $s['proto']; ?></td></tr>
        </tbody>
      </table>

      <h2 class="title" style="margin-top:26px">固件版本分布</h2>
      <?php if ($fw): ?>
        <table class="widefat striped" style="max-width:520px">
          <tbody><?php foreach ($fw as $r): ?>
            <tr><td><?php echo lp_e($r['k']); ?></td><td style="width:80px"><?php echo (int) $r['n']; ?> 台</td></tr>
          <?php endforeach; ?></tbody>
        </table>
      <?php else: ?><p style="color:#888">暂无数据（设备尚未上报固件版本）。</p><?php endif; ?>

      <h2 class="title" style="margin-top:26px">硬件型号分布</h2>
      <?php if ($board): ?>
        <table class="widefat striped" style="max-width:520px">
          <tbody><?php foreach ($board as $r): ?>
            <tr><td><?php echo lp_e($r['k']); ?></td><td style="width:80px"><?php echo (int) $r['n']; ?> 台</td></tr>
          <?php endforeach; ?></tbody>
        </table>
      <?php else: ?><p style="color:#888">暂无数据。</p><?php endif; ?>
    </div>
    <?php
}

/* ============================================================ 页面：设备列表 */

function lp_page_devices() {
    if (!current_user_can(LP_CAP)) { wp_die('没有权限'); }
    $flash = lp_maybe_handle_post();

    $args = array(
        'q'     => sanitize_text_field($_GET['q'] ?? ''),
        'state' => sanitize_key($_GET['state'] ?? 'all'),
        'fw'    => sanitize_text_field($_GET['fw'] ?? ''),
        'board' => sanitize_text_field($_GET['board'] ?? ''),
        'order' => sanitize_key($_GET['order'] ?? 'last'),
        'paged' => max(1, (int) ($_GET['paged'] ?? 1)),
        'per'   => 20,
    );
    $res = lp_dev_list($args);
    $s = lp_settings();
    $fw_all = lp_dist('fw', 50);
    $bd_all = lp_dist('board', 50);
    ?>
    <div class="wrap">
      <h1>掌机项目 · 设备管理</h1>
      <?php lp_render_flash($flash); ?>

      <form method="get" style="margin:14px 0;display:flex;gap:8px;flex-wrap:wrap;align-items:center">
        <input type="hidden" name="page" value="lintos-devices">
        <input type="search" name="q" value="<?php echo lp_e($args['q']); ?>" placeholder="MAC / 设备 ID / 名称">
        <select name="state">
          <option value="all" <?php selected($args['state'], 'all'); ?>>全部状态</option>
          <option value="online" <?php selected($args['state'], 'online'); ?>>在线</option>
          <option value="offline" <?php selected($args['state'], 'offline'); ?>>离线</option>
        </select>
        <select name="fw">
          <option value="">全部固件</option>
          <?php foreach ($fw_all as $r): ?>
            <option value="<?php echo lp_e($r['k']); ?>" <?php selected($args['fw'], $r['k']); ?>><?php echo lp_e($r['k']); ?></option>
          <?php endforeach; ?>
        </select>
        <select name="board">
          <option value="">全部硬件</option>
          <?php foreach ($bd_all as $r): ?>
            <option value="<?php echo lp_e($r['k']); ?>" <?php selected($args['board'], $r['k']); ?>><?php echo lp_e($r['k']); ?></option>
          <?php endforeach; ?>
        </select>
        <select name="order">
          <option value="last" <?php selected($args['order'], 'last'); ?>>按最近活跃</option>
          <option value="first" <?php selected($args['order'], 'first'); ?>>按首次联网</option>
        </select>
        <button class="button">筛选</button>
        <a class="button" href="<?php echo esc_url(admin_url('admin.php?page=lintos-devices')); ?>">重置</a>
        <span style="color:#646970">共 <?php echo (int) $res['total']; ?> 台</span>
      </form>

      <table class="widefat striped">
        <thead><tr>
          <th>设备</th><th style="width:150px">MAC</th><th style="width:96px">状态</th>
          <th style="width:90px">固件</th><th style="width:100px">硬件</th>
          <th style="width:120px">首次联网</th><th style="width:120px">最近活跃</th>
          <th style="width:80px">上报</th>
        </tr></thead>
        <tbody>
        <?php if (!$res['rows']): ?>
          <tr><td colspan="8" style="color:#888">没有设备。<?php echo (int) $res['total'] === 0 ? '（设备端还没接入）' : ''; ?></td></tr>
        <?php else: foreach ($res['rows'] as $r):
            $on = lp_is_online($r);
            $del = !empty($r['deleted_at']);
        ?>
          <tr <?php echo $del ? 'style="opacity:.55"' : ''; ?>>
            <td>
              <a href="<?php echo esc_url(admin_url('admin.php?page=lintos-device&dev=' . urlencode($r['dev_id']))); ?>">
                <strong><?php echo lp_e($r['name'] ?: $r['dev_id']); ?></strong></a>
              <?php if ($del): ?><span style="color:#b32d2e">（已删除）</span><?php endif; ?>
              <?php if ((int) $r['disabled']): ?><span style="color:#b32d2e">（已禁用）</span><?php endif; ?>
              <div style="color:#888;font-size:11px">
                ID <code class="lp-copy" data-copy="<?php echo lp_e($r['dev_id']); ?>" style="cursor:pointer" title="点击复制"><?php echo lp_e($r['dev_id']); ?></code>
              </div>
            </td>
            <td><code class="lp-copy" data-copy="<?php echo lp_e($r['mac']); ?>" style="cursor:pointer" title="点击复制"><?php echo lp_e($r['mac']); ?></code></td>
            <td><?php echo $on ? '<span style="color:#00a32a">● 在线</span>' : '<span style="color:#b32d2e">● 离线</span>'; ?>
                <div style="color:#888;font-size:11px"><?php echo lp_e(lp_ago($r['last_hb'])); ?></div></td>
            <td><?php echo lp_e($r['fw'] ?: '—'); ?></td>
            <td><?php echo lp_e($r['board'] ?: '—'); ?></td>
            <td><?php echo lp_e(lp_human_time($r['first_seen'])); ?></td>
            <td><?php echo lp_e(lp_human_time($r['last_seen'])); ?></td>
            <td><?php echo (int) $r['hits']; ?></td>
          </tr>
        <?php endforeach; endif; ?>
        </tbody>
      </table>

      <?php if ($res['pages'] > 1): $base = admin_url('admin.php'); ?>
      <div class="tablenav"><div class="tablenav-pages">
        <?php
        $mk = function ($p) use ($base, $args) {
            $qs = array('page' => 'lintos-devices', 'paged' => $p, 'q' => $args['q'],
                        'state' => $args['state'], 'fw' => $args['fw'],
                        'board' => $args['board'], 'order' => $args['order']);
            return $base . '?' . http_build_query(array_filter($qs, function ($v) { return $v !== ''; }));
        };
        ?>
        <?php if ($res['page'] > 1): ?><a class="button" href="<?php echo esc_url($mk($res['page'] - 1)); ?>">‹ 上一页</a><?php endif; ?>
        <span style="margin:0 10px;color:#646970">第 <?php echo (int) $res['page']; ?> / <?php echo (int) $res['pages']; ?> 页</span>
        <?php if ($res['page'] < $res['pages']): ?><a class="button" href="<?php echo esc_url($mk($res['page'] + 1)); ?>">下一页 ›</a><?php endif; ?>
      </div></div>
      <?php endif; ?>

      <p style="color:#888;font-size:12px;margin-top:12px">
        在线阈值 <?php echo (int) $s['online_timeout']; ?> 秒；MAC/IP 属于可识别设备的标识信息，仅管理员可见，
        采集目的仅用于设备联网统计与故障排查。
      </p>
    </div>
    <script>
    document.querySelectorAll('.lp-copy').forEach(function (el) {
      el.addEventListener('click', function () {
        var t = el.getAttribute('data-copy');
        if (navigator.clipboard) { navigator.clipboard.writeText(t); }
        var o = el.textContent; el.textContent = '已复制';
        setTimeout(function () { el.textContent = o; }, 900);
      });
    });
    </script>
    <?php
}

/* ============================================================ 页面：设备详情 */

function lp_page_device() {
    if (!current_user_can(LP_CAP)) { wp_die('没有权限'); }
    $flash = lp_maybe_handle_post();
    $dev = sanitize_text_field($_GET['dev'] ?? '');
    $r = lp_dev_get($dev);
    if (!$r) { echo '<div class="wrap"><h1>设备不存在</h1></div>'; return; }

    $ev = lp_dev_events($dev, 60);
    $fw = lp_dev_fw_history($dev);
    $on = lp_is_online($r);
    $s = lp_settings();
    ?>
    <div class="wrap">
      <h1><?php echo lp_e($r['name'] ?: $r['dev_id']); ?>
        <span style="font-size:14px;color:#646970">（<?php echo $on ? '在线' : '离线'; ?>）</span></h1>
      <?php lp_render_flash($flash); ?>
      <p><a href="<?php echo esc_url(admin_url('admin.php?page=lintos-devices')); ?>">← 返回设备列表</a></p>

      <table class="widefat striped" style="max-width:820px">
        <tbody>
          <tr><td style="width:180px">设备 ID</td><td><code><?php echo lp_e($r['dev_id']); ?></code></td></tr>
          <tr><td>MAC</td><td><code><?php echo lp_e($r['mac']); ?></code></td></tr>
          <tr><td>硬件型号</td><td><?php echo lp_e($r['board'] ?: '—'); ?></td></tr>
          <tr><td>芯片型号</td><td><?php echo lp_e($r['chip'] ?: '—'); ?></td></tr>
          <tr><td>当前固件</td><td><?php echo lp_e($r['fw'] ?: '—'); ?></td></tr>
          <tr><td>首次联网</td><td><strong><?php echo lp_e(lp_human_time($r['first_seen'])); ?></strong>
              <span style="color:#888">（永久保留，心跳不会覆盖）</span></td></tr>
          <tr><td>最近联网</td><td><?php echo lp_e(lp_human_time($r['last_seen'])); ?>（<?php echo lp_e(lp_ago($r['last_seen'])); ?>）</td></tr>
          <tr><td>最后心跳</td><td><?php echo lp_e(lp_human_time($r['last_hb'])); ?>（<?php echo lp_e(lp_ago($r['last_hb'])); ?>）</td></tr>
          <tr><td>最近 IP</td><td><?php echo lp_e($r['last_ip'] ?: '—'); ?></td></tr>
          <tr><td>累计上报</td><td><?php echo (int) $r['hits']; ?> 次</td></tr>
          <tr><td>注册来源</td><td><?php echo lp_e($r['source']); ?></td></tr>
          <tr><td>协议版本</td><td>v<?php echo (int) $r['proto']; ?></td></tr>
        </tbody>
      </table>

      <?php echo lp_admin_post_form_start('device_update', '保存修改'); ?>
        <input type="hidden" name="dev_id" value="<?php echo lp_e($r['dev_id']); ?>">
        <table class="form-table" style="max-width:820px">
          <tr><th>设备名称</th><td><input type="text" name="name" class="regular-text"
              value="<?php echo lp_e($r['name']); ?>" placeholder="例如 客厅那台"></td></tr>
          <tr><th>备注</th><td><input type="text" name="note" class="regular-text"
              value="<?php echo lp_e($r['note']); ?>" placeholder="换过屏 / 测试机 …"></td></tr>
          <tr><th>禁用</th><td><label><input type="checkbox" name="disabled" value="1"
              <?php checked((int) $r['disabled'], 1); ?>> 禁止该设备继续心跳（下架/退货机器用）</label></td></tr>
        </table>
      <?php echo lp_admin_post_form_end(); ?>

      <?php if (empty($r['deleted_at'])): ?>
      <form method="post" style="margin-top:10px"
            onsubmit="return confirm('确认删除设备 <?php echo lp_e($r['dev_id']); ?>？\n采用逻辑删除，历史记录保留，可在数据清理时物理删除。')">
        <?php wp_nonce_field('lp_device_delete'); ?>
        <input type="hidden" name="lp_action" value="device_delete">
        <input type="hidden" name="dev_id" value="<?php echo lp_e($r['dev_id']); ?>">
        <button class="button button-link-delete">删除设备</button>
      </form>
      <?php else: ?>
      <form method="post" style="margin-top:10px">
        <?php wp_nonce_field('lp_device_restore'); ?>
        <input type="hidden" name="lp_action" value="device_restore">
        <input type="hidden" name="dev_id" value="<?php echo lp_e($r['dev_id']); ?>">
        <button class="button">恢复设备</button>
        <span style="color:#b32d2e">该设备已于 <?php echo lp_e(lp_human_time($r['deleted_at'])); ?> 删除</span>
      </form>
      <?php endif; ?>

      <h2 class="title" style="margin-top:26px">固件变更记录（<?php echo count($fw); ?>）</h2>
      <?php if (!$fw): ?><p style="color:#888">没有固件变更记录。</p><?php else: ?>
      <table class="widefat striped" style="max-width:620px">
        <thead><tr><th style="width:150px">时间</th><th style="width:120px">固件</th><th>说明</th></tr></thead>
        <tbody><?php foreach ($fw as $x): ?>
          <tr><td><?php echo lp_e(lp_human_time($x['ts'])); ?></td>
              <td><?php echo lp_e($x['fw']); ?></td>
              <td style="color:#646970"><?php echo lp_e($x['detail']); ?></td></tr>
        <?php endforeach; ?></tbody>
      </table>
      <?php endif; ?>

      <h2 class="title" style="margin-top:26px">联网历史（最近 <?php echo count($ev); ?> 条）</h2>
      <?php if (!$ev): ?><p style="color:#888">还没有心跳记录。</p><?php else: ?>
      <table class="widefat striped" style="max-width:820px">
        <thead><tr><th style="width:150px">时间</th><th style="width:110px">类型</th>
          <th style="width:90px">固件</th><th style="width:130px">IP</th><th>说明</th></tr></thead>
        <tbody><?php foreach ($ev as $x):
          $map = array('register' => '首次注册', 'hb' => '心跳', 'fw' => '固件升级',
                       'online' => '重新联网', 'error' => '异常上报');
          $txt = isset($map[$x['type']]) ? $map[$x['type']] : $x['type'];
          $color = $x['type'] === 'fw' ? '#996800' : ($x['type'] === 'error' ? '#b32d2e' : '#2271b1');
        ?>
          <tr><td><?php echo lp_e(lp_human_time($x['ts'])); ?></td>
              <td><span style="color:<?php echo $color; ?>"><?php echo lp_e($txt); ?></span></td>
              <td><?php echo lp_e($x['fw']); ?></td>
              <td><?php echo lp_e($x['ip']); ?></td>
              <td style="color:#646970"><?php echo lp_e($x['detail']); ?></td></tr>
        <?php endforeach; ?></tbody>
      </table>
      <?php endif; ?>
      <p style="color:#888;font-size:12px;margin-top:14px">
        说明：正常心跳按 <?php echo (int) $s['hb_interval']; ?> 秒一次，为避免刷屏不逐条入库，
        只在「首次注册 / 固件变更 / 从离线恢复 / 异常」时写事件；最近心跳时间见上表「最后心跳」。
      </p>
    </div>
    <?php
}

/* ============================================================ 页面：统计 */

function lp_page_stats() {
    if (!current_user_can(LP_CAP)) { wp_die('没有权限'); }
    $gran = sanitize_key($_GET['gran'] ?? 'day');
    if (!in_array($gran, array('day', 'week', 'month'), true)) { $gran = 'day'; }
    $points = array('day' => 14, 'week' => 12, 'month' => 12)[$gran];

    $new    = lp_new_series($gran, $points);
    $online = lp_online_series(14);
    $fwd    = lp_dist('fw', 10);
    $bdd    = lp_dist('board', 10);
    $top    = lp_top_active(10);
    $c      = lp_stats_cards();
    ?>
    <div class="wrap">
      <h1>掌机项目 · 数据统计</h1>
      <?php echo lp_empty_notice($c); ?>

      <h2 class="title">新增设备趋势</h2>
      <p class="nav-tab-wrapper" style="margin-bottom:8px">
        <?php foreach (array('day' => '按日', 'week' => '按周', 'month' => '按月') as $k => $v): ?>
          <a class="nav-tab <?php echo $gran === $k ? 'nav-tab-active' : ''; ?>"
             href="<?php echo esc_url(admin_url('admin.php?page=lintos-stats&gran=' . $k)); ?>"><?php echo $v; ?></a>
        <?php endforeach; ?>
      </p>
      <?php echo lp_bar_chart($new, 'k', 'n'); ?>

      <h2 class="title" style="margin-top:26px">每日活跃设备（按最后心跳日聚合）</h2>
      <?php echo lp_line_chart($online, 'k', 'n'); ?>
      <p style="color:#888;font-size:12px">
        口径：把每台设备的「最后心跳时间」归到日期上并去重计数。没有历史快照表，
        因此这是"当天有过心跳的设备数"，不是"当天某一时刻的在线数"。
      </p>

      <h2 class="title" style="margin-top:26px">固件版本分布</h2>
      <?php echo lp_bar_chart($fwd, 'k', 'n'); ?>

      <h2 class="title" style="margin-top:26px">硬件型号分布</h2>
      <?php echo lp_bar_chart($bdd, 'k', 'n'); ?>

      <h2 class="title" style="margin-top:26px">最近活跃排行（按累计上报次数）</h2>
      <table class="widefat striped" style="max-width:720px">
        <thead><tr><th>设备</th><th style="width:110px">固件</th><th style="width:90px">上报</th><th style="width:140px">最后心跳</th></tr></thead>
        <tbody>
        <?php if (!$top): ?><tr><td colspan="4" style="color:#888">暂无数据。</td></tr>
        <?php else: foreach ($top as $r): ?>
          <tr><td><a href="<?php echo esc_url(admin_url('admin.php?page=lintos-device&dev=' . urlencode($r['dev_id']))); ?>">
                <?php echo lp_e($r['nm']); ?></a></td>
              <td><?php echo lp_e($r['fw']); ?></td>
              <td><?php echo (int) $r['hits']; ?></td>
              <td><?php echo lp_e(lp_human_time($r['last_hb'])); ?></td></tr>
        <?php endforeach; endif; ?>
        </tbody>
      </table>

      <h2 class="title" style="margin-top:26px">长期离线</h2>
      <p>超过 <strong><?php echo (int) lp_setting('offline_grace', 1800); ?> 秒</strong>没有心跳的设备：
         <strong><?php echo (int) $c['long_offline']; ?></strong> 台（占累计 <?php echo (int) $c['total']; ?> 台的
         <?php echo (int) $c['total'] > 0 ? round($c['long_offline'] * 100 / $c['total'], 1) : 0; ?>%）。</p>
    </div>
    <?php
}

/* ============================================================ 页面：设置 */

function lp_admin_post_form_start($action, $label) {
    $html = '<form method="post" style="margin-top:16px">';
    $html .= wp_nonce_field('lp_' . $action, '_wpnonce', true, false);
    $html .= '<input type="hidden" name="lp_action" value="' . lp_e($action) . '">';
    return $html;
}
function lp_admin_post_form_end() {
    return '<p><button class="button button-primary">保存</button></p></form>';
}

function lp_page_settings() {
    if (!current_user_can(LP_CAP)) { wp_die('没有权限'); }
    $flash = lp_maybe_handle_post();
    $s = lp_settings();
    $c = lp_stats_cards();
    global $wpdb;
    $fw_t = lp_tbl('fw');
    $fw_rows = $wpdb->get_results("SELECT * FROM $fw_t ORDER BY released DESC LIMIT 20", ARRAY_A);
    ?>
    <div class="wrap">
      <h1>掌机项目 · 项目设置</h1>
      <?php lp_render_flash($flash); ?>

      <form method="post">
        <?php wp_nonce_field('lp_settings'); ?>
        <input type="hidden" name="lp_action" value="settings">

        <h2 class="title">设备接入</h2>
        <table class="form-table" style="max-width:860px">
          <tr><th>允许新设备注册</th>
              <td><label><input type="checkbox" name="allow_register" value="1" <?php checked((int) $s['allow_register'], 1); ?>>
                  关闭后 /lp/v1/register 直接返回 REGISTER_CLOSED（已注册设备心跳不受影响）</label></td></tr>
          <tr><th>注册产品密钥</th>
              <td><input type="text" name="product_key" class="regular-text code" value="<?php echo lp_e($s['product_key']); ?>"
                    placeholder="留空 = 禁止注册">
                  <p class="description">设备端用它对注册请求做 HMAC-SHA256。这是**产品级**密钥（可烧进固件），
                  不是后台管理员密码；后台登录仍走 WP 账号。</p></td></tr>
          <tr><th>注册接口</th><td><code><?php echo lp_e(rest_url('lp/v1/register')); ?></code>（POST）</td></tr>
          <tr><th>心跳接口</th><td><code><?php echo lp_e(rest_url('lp/v1/heartbeat')); ?></code>（POST）</td></tr>
          <tr><th>接口版本</th><td><input type="number" name="proto" value="<?php echo (int) $s['proto']; ?>" min="1" max="99" style="width:80px"></td></tr>
          <tr><th>最近一次设备上报</th><td><?php echo $c['last_report'] ? lp_e(lp_human_time($c['last_report'])) : '<span style="color:#b32d2e">从未收到</span>'; ?></td></tr>
        </table>

        <h2 class="title">在线判定</h2>
        <table class="form-table" style="max-width:860px">
          <tr><th>心跳间隔（秒）</th>
              <td><input type="number" name="hb_interval" value="<?php echo (int) $s['hb_interval']; ?>" min="60" max="86400" style="width:110px">
                  <p class="description">注册/心跳的应答里会把这个值下发，设备按它定时上报。</p></td></tr>
          <tr><th>在线超时（秒）</th>
              <td><input type="number" name="online_timeout" value="<?php echo (int) $s['online_timeout']; ?>" min="120" max="86400" style="width:110px">
                  <p class="description">最后心跳早于「现在 − 这个值」即判离线。建议 ≥ 心跳间隔 × 1.5。</p></td></tr>
          <tr><th>长期离线（秒）</th>
              <td><input type="number" name="offline_grace" value="<?php echo (int) $s['offline_grace']; ?>" min="300" max="2592000" style="width:110px"></td></tr>
          <tr><th>异常上报</th>
              <td><select name="abnormal_action">
                    <option value="log" <?php selected($s['abnormal_action'], 'log'); ?>>只记录（推荐）</option>
                    <option value="reject" <?php selected($s['abnormal_action'], 'reject'); ?>>拒绝并记录</option>
                  </select></td></tr>
        </table>

        <h2 class="title">数据管理</h2>
        <table class="form-table" style="max-width:860px">
          <tr><th>设备数据保留（天）</th>
              <td><input type="number" name="retain_days" value="<?php echo (int) $s['retain_days']; ?>" min="1" max="3650" style="width:110px">
                  <p class="description">只对**已逻辑删除**的设备生效：超过这个天数才物理清除。</p></td></tr>
          <tr><th>联网历史保留（天）</th>
              <td><input type="number" name="retain_events" value="<?php echo (int) $s['retain_events']; ?>" min="1" max="3650" style="width:110px"></td></tr>
          <tr><th>导出</th>
              <td><a class="button" href="<?php echo esc_url(wp_nonce_url(admin_url('admin-post.php?action=lp_export'), 'lp_export')); ?>">导出设备 CSV</a>
                  <p class="description">包含设备 ID / MAC / 名称 / 硬件 / 固件 / 首末时间 / 上报次数，含 MAC 属敏感数据，勿外传。</p></td></tr>
          <tr><th>立即清理</th><td>
              <form method="post" style="display:inline" onsubmit="return confirm('按保留期清理事件与非活跃设备？此操作不可撤销。')">
                <?php wp_nonce_field('lp_cleanup'); ?>
                <input type="hidden" name="lp_action" value="cleanup">
                <button class="button">执行清理</button>
              </form>
              <span class="description">每天也会由 WP-Cron 自动跑一次。</span></td></tr>
        </table>

        <h2 class="title">固件信息</h2>
        <table class="form-table" style="max-width:860px">
          <tr><th>当前最新版本</th>
              <td><input type="text" name="latest_fw" value="<?php echo lp_e($s['latest_fw']); ?>" class="regular-text code" placeholder="1.4.3"></td></tr>
          <tr><th>最低兼容版本</th>
              <td><input type="text" name="min_compat" value="<?php echo lp_e($s['min_compat']); ?>" class="regular-text code" placeholder="1.3.0"></td></tr>
          <tr><th>更新说明</th>
              <td><textarea name="fw_notes" rows="3" class="large-text"><?php echo lp_e($s['fw_notes']); ?></textarea></td></tr>
        </table>

        <p><button class="button button-primary">保存设置</button></p>
      </form>

      <h2 class="title">固件版本库</h2>
      <form method="post" style="margin-bottom:10px">
        <?php wp_nonce_field('lp_fw_add'); ?>
        <input type="hidden" name="lp_action" value="fw_add">
        <input type="text" name="version" placeholder="版本号 1.4.3" class="code" style="width:130px">
        <input type="text" name="min_compat" placeholder="最低兼容" class="code" style="width:110px">
        <input type="text" name="notes" placeholder="一句话说明" style="width:300px">
        <button class="button">登记版本</button>
      </form>
      <table class="widefat striped" style="max-width:760px">
        <thead><tr><th style="width:130px">版本</th><th style="width:150px">登记时间</th><th style="width:120px">最低兼容</th><th>说明</th></tr></thead>
        <tbody>
        <?php if (!$fw_rows): ?><tr><td colspan="4" style="color:#888">还没有登记任何固件版本。</td></tr>
        <?php else: foreach ($fw_rows as $f): ?>
          <tr><td><code><?php echo lp_e($f['version']); ?></code></td>
              <td><?php echo lp_e(lp_human_time($f['released'])); ?></td>
              <td><?php echo lp_e($f['min_compat']); ?></td>
              <td><?php echo lp_e($f['notes']); ?></td></tr>
        <?php endforeach; endif; ?>
        </tbody>
      </table>

      <h2 class="title">最近操作日志</h2>
      <table class="widefat striped" style="max-width:900px">
        <thead><tr><th style="width:150px">时间</th><th style="width:110px">操作者</th>
          <th style="width:130px">动作</th><th style="width:170px">对象</th><th>说明</th></tr></thead>
        <tbody>
        <?php $logs = lp_recent_audit(30); if (!$logs): ?>
          <tr><td colspan="5" style="color:#888">暂无。</td></tr>
        <?php else: foreach ($logs as $l): ?>
          <tr><td><?php echo lp_e(lp_human_time($l['ts'])); ?></td>
              <td><?php echo lp_e($l['actor']); ?></td>
              <td><code><?php echo lp_e($l['action']); ?></code></td>
              <td><?php echo lp_e($l['target']); ?></td>
              <td style="color:#646970"><?php echo lp_e($l['detail']); ?></td></tr>
        <?php endforeach; endif; ?>
        </tbody>
      </table>
    </div>
    <?php
}

/* ============================================================ CSV 导出 */

add_action('admin_post_lp_export', function () {
    if (!current_user_can(LP_CAP)) { wp_die('没有权限'); }
    check_admin_referer('lp_export');
    global $wpdb;
    $t = lp_tbl('devices');
    $rows = $wpdb->get_results("SELECT dev_id,mac,name,board,chip,fw,proto,first_seen,last_seen,last_hb,last_ip,hits,source,note
                                FROM $t WHERE deleted_at IS NULL ORDER BY id ASC", ARRAY_A);
    lp_audit('device.export', '-', 'rows=' . count($rows));
    header('Content-Type: text/csv; charset=utf-8');
    header('Content-Disposition: attachment; filename=lintos-devices-' . date('Ymd-His') . '.csv');
    $out = fopen('php://output', 'w');
    fwrite($out, "\xEF\xBB\xBF");                    /* BOM: Excel 中文不乱码 */
    fputcsv($out, array('dev_id', 'mac', 'name', 'board', 'chip', 'fw', 'proto',
                        'first_seen', 'last_seen', 'last_hb', 'last_ip', 'hits', 'source', 'note'));
    foreach ($rows as $r) { fputcsv($out, $r); }
    fclose($out);
    exit;
});

/* ============================================================ 菜单 */

add_action('admin_menu', function () {
    add_submenu_page('lintos-project', '设备总览', '设备总览', LP_CAP, 'lintos-devices-overview', 'lp_page_overview');
    add_submenu_page('lintos-project', '设备管理', '设备管理', LP_CAP, 'lintos-devices', 'lp_page_devices');
    add_submenu_page('lintos-project', '设备详情', '设备详情', LP_CAP, 'lintos-device', 'lp_page_device');
    add_submenu_page('lintos-project', '数据统计', '数据统计', LP_CAP, 'lintos-stats', 'lp_page_stats');
    add_submenu_page('lintos-project', '项目设置', '项目设置', LP_CAP, 'lintos-settings', 'lp_page_settings');
}, 20);
