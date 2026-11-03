<?php
/**
 * t_device.php — 掌机项目设备管理 · 服务器端测试
 *
 *   php /tmp/lp_test/t_device.php
 *
 * 怎么做到"真 MySQL 但不碰生产表":
 *   ① 定义 LP_TBL_PREFIX='tmp_'，插件所有表名 → tmp_lp_*
 *   ② 用 CREATE TEMPORARY TABLE 建表（会话级，连接断开即消失，不写磁盘 DDL）
 *   ③ 全程在 START TRANSACTION 里跑，结束时 ROLLBACK ⇒ wp_options 里的测试写入也不留痕
 * 所以测的是**生产代码 + 生产数据库引擎**，但生产表一个字节都没动。
 */
declare(strict_types=1);

define('LP_TBL_PREFIX', 'tmp_');
$ROOT = getenv('LP_WP') ?: '/www/wwwroot/bbk.linit.cn';
require $ROOT . '/wp-load.php';

$PLUGIN = getenv('LP_PLUGIN') ?: '/tmp/lp_test/plugin';
require_once ABSPATH . 'wp-admin/includes/user.php';   /* wp_delete_user 在 admin 侧 */
require $PLUGIN . '/lp-device/lp-device-core.php';
require $PLUGIN . '/lp-device/lp-device-admin.php';

global $wpdb;
$PASS = 0; $FAIL = 0;
function ok($c, $label, $extra = '') {
    global $PASS, $FAIL;
    if ($c) { $PASS++; echo "  PASS  {$label}\n"; }
    else    { $FAIL++; echo "  FAIL  {$label}" . ($extra !== '' ? "  ({$extra})" : '') . "\n"; }
}
function has($h, $n) { return strpos((string) $h, $n) !== false; }

/* ---------- 建临时表 ---------- */
$sqls = lp_tables_sql('tmp_');
foreach ($sqls as $sql) {
    $sql = str_replace('CREATE TABLE IF NOT EXISTS', 'CREATE TEMPORARY TABLE', $sql);
    if ($wpdb->query($sql) === false) {
        echo "建表失败: " . $wpdb->last_error . "\n"; exit(1);
    }
}
$t = 'tmp_lp_';
echo "=== 掌机项目设备管理 · 测试（临时表 {$t}* + 事务回滚）===\n\n";

/* ---------- 设置：产品密钥（只写 options，最后回滚） ---------- */
$wpdb->query('START TRANSACTION');
lp_save_settings(array(
    'allow_register' => 1, 'product_key' => 'TEST-PRODUCT-KEY-123',
    'hb_interval' => 600, 'online_timeout' => 900, 'offline_grace' => 1800,
    'proto' => 1, 'retain_days' => 365, 'retain_events' => 90,
));
$GLOBALS['lp_test_key'] = 'TEST-PRODUCT-KEY-123';

/* ---------- 工具：造一个合法注册请求 ---------- */
function mk_reg($mac, $board = 'xiaozhi', $chip = 'esp32s3', $fw = '1.4.3', $proto = 1,
               $key = null, $ts = null, $nonce = null, $dev = null) {
    $a = array(
        'mac' => $mac, 'dev_id' => $dev !== null ? $dev : lp_dev_id_from_mac($mac),
        'board' => $board, 'chip' => $chip, 'fw' => $fw, 'proto' => $proto,
        'ts' => $ts !== null ? $ts : time(),
        'nonce' => $nonce !== null ? $nonce : bin2hex(random_bytes(8)),
    );
    $k = $key !== null ? $key : $GLOBALS['lp_test_key'];
    $a['sig'] = hash_hmac('sha256', lp_sig_string_register($a), $k);
    return $a;
}
function do_reg(array $a) {
    $req = new WP_REST_Request('POST', '/lp/v1/register');
    foreach ($a as $k => $v) { $req->set_param($k, $v); }
    return rest_do_request($req);
}
function mk_hb($dev_id, $secret, $fw = '', $ts = null, $nonce = null) {
    $a = array('dev_id' => $dev_id, 'fw' => $fw, 'ts' => $ts !== null ? $ts : time(),
               'nonce' => $nonce !== null ? $nonce : bin2hex(random_bytes(8)));
    $a['sig'] = hash_hmac('sha256', lp_sig_string_hb($a), $secret);
    return $a;
}
function do_hb(array $a) {
    $req = new WP_REST_Request('POST', '/lp/v1/heartbeat');
    foreach ($a as $k => $v) { $req->set_param($k, $v); }
    return rest_do_request($req);
}
function row($dev_id) {
    global $wpdb, $t;
    return $wpdb->get_row($wpdb->prepare("SELECT * FROM {$t}devices WHERE dev_id=%s", $dev_id), ARRAY_A);
}
function cnt($sql) {
    global $wpdb;
    return (int) $wpdb->get_var($sql);
}

/* ============================ T1 首次注册 ============================ */
echo "T1 新设备首次注册\n";
$mac1 = '3C:71:BF:11:22:33';
$dev1 = lp_dev_id_from_mac($mac1);
ok($dev1 === 'LNT3C71BF112233', "dev_id 由 MAC 派生成 " . $dev1);
$r = do_reg(mk_reg($mac1));
ok($r->get_status() === 200, "注册返回 200", 'status=' . $r->get_status());
$d = $r->get_data();
ok(!empty($d['secret']) && strlen($d['secret']) === 48, "下发 48 位设备密钥");
ok((int) $d['hb'] === 600, "下发心跳间隔 600s");
$row1 = row($dev1);
ok($row1 !== null, "设备已入库");
ok($row1 !== null && !empty($row1['first_seen']), "first_seen 有值: " . ($row1['first_seen'] ?? ''));
ok($row1 !== null && (int) $row1['hits'] === 1, "hits=1");
ok($row1 !== null && $row1['mac'] === $mac1, "MAC 原样保存");
ok((int) $row1['disabled'] === 0, "默认未禁用");
printf("\n");

/* ============================ T2 重复注册 ============================ */
echo "T2 同一设备重复注册\n";
$first = $row1['first_seen'];
sleep(1);
$r = do_reg(mk_reg($mac1, 'xiaozhi', 'esp32s3', '1.4.3'));
ok($r->get_status() === 200, "重复注册仍 200");
$row2 = row($dev1);
ok(cnt("SELECT COUNT(*) FROM {$t}devices WHERE dev_id='{$dev1}'") === 1, "没有产生重复设备");
ok($row2['first_seen'] === $first, "first_seen 没被覆盖（{$first}）");
ok($row2['secret'] !== $row1['secret'], "密钥已轮换（设备重刷后可继续用）");
printf("\n");

/* ============================ T3 连续心跳 ============================ */
echo "T3 连续心跳\n";
$secret = $row2['secret'];
$before = $row2['last_hb'];
sleep(1);
$r = do_hb(mk_hb($dev1, $secret));
ok($r->get_status() === 200, "心跳 200");
$row3 = row($dev1);
ok(strtotime($row3['last_hb']) >= strtotime($before), "last_hb 前进");
ok((int) $row3['hits'] === 2, "hits 递增到 2", 'hits=' . $row3['hits']);
ok(lp_is_online($row3), "判定为在线");
ok(lp_stats_cards()['online'] >= 1, "总览在线数 ≥1");
printf("\n");

/* ============================ T4 停止心跳 → 离线 ============================ */
echo "T4 停止心跳 → 自动离线（不靠设备发离线消息）\n";
$wpdb->query("UPDATE {$t}devices SET last_hb = (NOW() - INTERVAL 3600 SECOND) WHERE dev_id='{$dev1}'");
$row4 = row($dev1);
ok(!lp_is_online($row4), "超过在线阈值 → 离线");
$c = lp_stats_cards();
ok($c['online'] === 0, "在线数回到 0", 'online=' . $c['online']);
ok($c['total'] === 1, "累计仍是 1（注册过不等于在线）");
ok($c['offline'] === 1, "离线数 1");
ok($c['long_offline'] === 1, "长期离线 1（超 1800s）");
printf("\n");

/* ============================ T5 固件变更 ============================ */
echo "T5 固件版本变更（保留历史）\n";
$r = do_hb(mk_hb($dev1, $secret, '1.5.0'));
ok($r->get_status() === 200, "带新固件的心跳 200");
$row5 = row($dev1);
ok($row5['fw'] === '1.5.0', "当前固件更新为 1.5.0", 'fw=' . $row5['fw']);
$hist = lp_dev_fw_history($dev1);
ok(count($hist) === 1, "固件变更记录 1 条", 'n=' . count($hist));
ok(has($hist[0]['detail'], '1.4.3'), "记录里保留了旧版本: " . $hist[0]['detail']);
$ev = lp_dev_events($dev1, 20);
$types = array_column($ev, 'type');
ok(in_array('register', $types, true), "事件里有 register");
ok(in_array('fw', $types, true), "事件里有 fw");
printf("\n");

/* ============================ T6 多设备统计 ============================ */
echo "T6 多台不同 MAC → 统计准确\n";
$macs = array('AA:BB:CC:00:00:01', 'AA:BB:CC:00:00:02', 'AA:BB:CC:00:00:03');
foreach ($macs as $i => $m) {
    do_reg(mk_reg($m, $i === 0 ? 'weixue' : 'std', 'esp32s3', $i === 0 ? '1.4.2' : '1.4.3'));
}
$c = lp_stats_cards();
ok($c['total'] === 4, "累计 4 台", 'total=' . $c['total']);
/* T5 又给 dev1 发过心跳，它已恢复到在线 ⇒ 4 台全在线；
 * "离线" 这条路径已在 T4 单独验证过（那里在线数确实回到 0）。 */
ok($c['online'] === 4, '在线 4 台（新注册即心跳，dev1 已被 T5 心跳带回在线）', 'online=' . $c['online']);
ok($c['offline'] === 0, '离线 0 台', 'offline=' . $c['offline']);
ok($c['fw_count'] === 3, "固件版本 3 种（1.4.2/1.4.3/1.5.0）", 'n=' . $c['fw_count']);
$fwd = lp_dist('fw');
$map = array(); foreach ($fwd as $x) { $map[$x['k']] = (int) $x['n']; }
ok(($map['1.4.3'] ?? 0) === 2, "1.4.3 有 2 台", json_encode($map));
ok(($map['1.4.2'] ?? 0) === 1, "1.4.2 有 1 台");
$bdd = lp_dist('board');
$bm = array(); foreach ($bdd as $x) { $bm[$x['k']] = (int) $x['n']; }
ok(($bm['std'] ?? 0) === 2 && ($bm['weixue'] ?? 0) === 1, "硬件分布正确", json_encode($bm));
printf("\n");

/* ============================ T7 异常与攻击 ============================ */
echo "T7 非法输入 / 伪造 / 重放\n";
$r = do_reg(mk_reg('ZZ:ZZ:ZZ:ZZ:ZZ:ZZ'));
ok($r->get_status() === 400, "非法 MAC → 400", 'status=' . $r->get_status());

$r = do_reg(mk_reg($mac1, 'xiaozhi', 'esp32s3', '1.4.3', 1, null, null, null, 'LNT000000000000'));
ok($r->get_status() === 400, "伪造 dev_id（与 MAC 不匹配）→ 400");
ok($r->get_data()['err'] === 'BAD_DEV_ID', "错误码 BAD_DEV_ID");

$a = mk_reg('AA:BB:CC:00:00:09', 'std', 'esp32s3', '1.4.3', 1, 'WRONG-KEY');
$r = do_reg($a);
ok($r->get_status() === 403 && $r->get_data()['err'] === 'BAD_SIG', "错误产品密钥 → 403 BAD_SIG");

$a = mk_reg('AA:BB:CC:00:00:09', 'std', 'esp32s3', '1.4.3', 1, null, time() - 4000);
$r = do_reg($a);
ok($r->get_status() === 400 && $r->get_data()['err'] === 'BAD_TS', "时间戳过期 → 400 BAD_TS");

$n = bin2hex(random_bytes(8));
$a = mk_reg('AA:BB:CC:00:00:08', 'std', 'esp32s3', '1.4.3', 1, null, null, $n);
ok(do_reg($a)->get_status() === 200, "首次 nonce 通过");
$a2 = mk_reg('AA:BB:CC:00:00:08', 'std', 'esp32s3', '1.4.3', 1, null, null, $n);
$r = do_reg($a2);
ok($r->get_status() === 409 && $r->get_data()['err'] === 'REPLAY', "同一 nonce 重放 → 409 REPLAY");
$wpdb->query("DELETE FROM {$t}devices WHERE dev_id='" . lp_dev_id_from_mac('AA:BB:CC:00:00:08') . "'");

$r = do_hb(mk_hb('LNT_UNKNOWN', 'x'));
ok($r->get_status() === 404, "未知设备心跳 → 404");
$r = do_hb(mk_hb($dev1, str_repeat('0', 48)));
ok($r->get_status() === 403 && $r->get_data()['err'] === 'BAD_SIG', "错误设备密钥 → 403");

$row_bad = row($dev1);
$before_hits = (int) $row_bad['hits'];
do_hb(mk_hb($dev1, str_repeat('0', 48)));
ok((int) row($dev1)['hits'] === $before_hits, "签名失败不会累加 hits");

/* 超长/怪字符字段被裁剪 */
$a = mk_reg('AA:BB:CC:00:00:07', str_repeat('X', 200), str_repeat('Y', 200), str_repeat('9', 200));
$r = do_reg($a);
$row_x = row(lp_dev_id_from_mac('AA:BB:CC:00:00:07'));
ok($r->get_status() === 200, "超长字段不报错");
ok(strlen($row_x['board']) <= 32 && strlen($row_x['chip']) <= 32 && strlen($row_x['fw']) <= 32,
   "字段被截到 ≤32 字符", strlen($row_x['board']) . '/' . strlen($row_x['chip']) . '/' . strlen($row_x['fw']));
ok(!has($row_x['board'], ';') && !has($row_x['board'], "'"), "非白名单字符被过滤");
$wpdb->query("DELETE FROM {$t}devices WHERE dev_id='" . lp_dev_id_from_mac('AA:BB:CC:00:00:07') . "'");
printf("\n");

/* ============================ T8 限频 ============================ */
echo "T8 限频与注册开关\n";
lp_save_settings(array_merge(lp_settings(), array('allow_register' => 0)));
$r = do_reg(mk_reg('AA:BB:CC:00:00:0A'));
ok($r->get_status() === 403 && $r->get_data()['err'] === 'REGISTER_CLOSED', "关闭注册 → 403 REGISTER_CLOSED");
lp_save_settings(array_merge(lp_settings(), array('allow_register' => 1)));

/* 心跳限频：同 IP 每分钟 240 次 */
$limited = false;
for ($i = 0; $i < 300; $i++) {
    $r = do_hb(mk_hb($dev1, $secret));
    if ($r->get_status() === 429) { $limited = true; break; }
}
ok($limited, "心跳超过限频后返回 429");
delete_transient('lp_rl_' . md5('hb|' . (isset($_SERVER['REMOTE_ADDR']) ? $_SERVER['REMOTE_ADDR'] : '')));
printf("\n");

/* ============================ T9 列表：搜索/筛选/排序/分页 ============================ */
echo "T9 列表查询\n";
$all = lp_dev_list(array('per' => 2, 'paged' => 1));
ok($all['total'] === 4, "总数 4", 'total=' . $all['total']);
ok(count($all['rows']) === 2, "每页 2 条", 'rows=' . count($all['rows']));
ok($all['pages'] === 2, "共 2 页");
$p2 = lp_dev_list(array('per' => 2, 'paged' => 2));
ok(count($p2['rows']) === 2 && $p2['rows'][0]['dev_id'] !== $all['rows'][0]['dev_id'], "第 2 页不是第 1 页");

$q = lp_dev_list(array('q' => 'AA:BB:CC:00:00:02'));
ok($q['total'] === 1, "按 MAC 搜索命中 1 条", 'total=' . $q['total']);

$q = lp_dev_list(array('q' => 'LNT3C71'));
ok($q['total'] === 1, "按设备 ID 前缀搜索命中");

/* 先把一台调成离线，才能同时验证两种筛选 */
$wpdb->query("UPDATE {$t}devices SET last_hb=(NOW() - INTERVAL 3600 SECOND)
              WHERE dev_id='" . lp_dev_id_from_mac('AA:BB:CC:00:00:02') . "'");
$on = lp_dev_list(array('state' => 'online'));
$off = lp_dev_list(array('state' => 'offline'));
ok($on['total'] === 3 && $off['total'] === 1, "在线/离线筛选（{$on['total']}/{$off['total']}）");

$f = lp_dev_list(array('fw' => '1.4.3'));
ok($f['total'] === 2, "按固件筛选 1.4.3 → 2 台", 'total=' . $f['total']);

$b = lp_dev_list(array('board' => 'weixue'));
ok($b['total'] === 1, "按硬件筛选 weixue → 1 台");

$o1 = lp_dev_list(array('order' => 'first'));
$o2 = lp_dev_list(array('order' => 'last'));
ok($o1['rows'][0]['dev_id'] !== $o2['rows'][0]['dev_id'] || count($o1['rows']) === 1,
   "两种排序结果不同/或只有一台");

/* SQL 注入尝试（应当被 prepare 化，不报错、不返回全表） */
$inj = lp_dev_list(array('q' => "' OR 1=1 -- "));
ok($inj['total'] === 0, "注入串被当普通字符串（0 命中）", 'total=' . $inj['total']);
printf("\n");

/* ============================ T10 禁用 / 逻辑删除 ============================ */
echo "T10 禁用与逻辑删除\n";
$wpdb->update("{$t}devices", array('disabled' => 1), array('dev_id' => $dev1));
$r = do_hb(mk_hb($dev1, $secret));
ok($r->get_status() === 403 && $r->get_data()['err'] === 'DISABLED', "禁用后心跳 403 DISABLED");
$wpdb->update("{$t}devices", array('disabled' => 0), array('dev_id' => $dev1));

$wpdb->update("{$t}devices", array('deleted_at' => current_time('mysql')), array('dev_id' => $dev1));
ok(lp_dev_list(array())['total'] === 3, "逻辑删除后列表少一台");
ok(lp_dev_get($dev1) !== null, "记录仍在（可恢复）");
ok(count(lp_dev_events($dev1, 50)) > 0, "历史记录仍在");
$r = do_hb(mk_hb($dev1, $secret));
ok($r->get_status() === 404, "已删除设备心跳 404（不当成新设备）");
$wpdb->update("{$t}devices", array('deleted_at' => null), array('dev_id' => $dev1));
ok(lp_dev_list(array())['total'] === 4, "恢复后回到 4 台");
printf("\n");

/* ============================ T11 后台页面渲染 ============================ */
echo "T11 后台页面渲染（管理员）\n";
wp_set_current_user(1);                     /* 站点管理员 */
ok(current_user_can('manage_options'), "测试用户是管理员");
ob_start(); lp_page_overview(); $h1 = ob_get_clean();
ok(has($h1, '设备总览') && has($h1, '累计登记设备'), "总览页渲染出卡片");
ok(has($h1, '在线判定'), "总览页写明在线判定口径");
ok(!has($h1, '尚未接入设备数据'), '有数据时不显示"尚未接入"提示');

ob_start(); lp_page_devices(); $h2 = ob_get_clean();
ok(has($h2, '设备管理') && has($h2, $dev1), "设备列表页包含设备 ID");
ok(has($h2, 'lp-copy'), "有复制按钮（复制 MAC / 设备 ID）");
ok(has($h2, '共 4 台'), "显示总数");

ob_start(); lp_page_device(); $h3 = ob_get_clean();  /* 缺 dev 参数 → 设备不存在 */
ok(has($h3, '设备不存在'), "缺参数时安全提示");
$_GET['dev'] = $dev1;
ob_start(); lp_page_device(); $h4 = ob_get_clean();
ok(has($h4, '首次联网') && has($h4, '固件变更记录') && has($h4, '联网历史'), "详情页三块都在");
ok(has($h4, '永久保留'), "详情页说明首次联网永久保留");

ob_start(); lp_page_stats(); $h5 = ob_get_clean();
ok(has($h5, '<svg'), "统计页有 SVG 图表");
ok(has($h5, '新增设备趋势') && has($h5, '长期离线'), "统计页栏目齐全");

ob_start(); lp_page_settings(); $h6 = ob_get_clean();
ok(has($h6, '允许新设备注册') && has($h6, '在线超时'), "设置页有接入与在线判定");
ok(has($h6, '产品密钥'), "设置页有产品密钥项");
ok(!has($h6, 'TEST-PRODUCT-KEY') || has($h6, 'value="TEST-PRODUCT-KEY-123"'), "密钥回填在管理员表单里（正常）");
printf("\n");

/* ============================ T12 权限 ============================ */
echo "T12 权限：普通用户看不到设备档案\n";
$uid = wp_insert_user(array('user_login' => 'lp_sub_' . wp_rand(), 'user_pass' => wp_generate_password(),
                            'role' => 'subscriber'));
wp_set_current_user($uid);
ok(!current_user_can('manage_options'), '订阅者没有 manage_options');
add_filter('wp_die_handler', function () {
    return function ($msg) { throw new RuntimeException(is_scalar($msg) ? (string) $msg : 'wp_die'); };
});
ob_start();
$blocked = false;
try { lp_page_devices(); } catch (RuntimeException $e) { $blocked = true; }
$body = ob_get_clean();
ok($blocked, '订阅者访问设备页被 wp_die 拦下');
ok(!has($body, $dev1), '订阅者拿不到设备 ID');
remove_all_filters('wp_die_handler');
wp_delete_user($uid);
wp_set_current_user(1);
printf("\n");

/* ============================ T13 空数据 ============================ */
echo "T13 空数据时不编造在线数\n";
$wpdb->query("DELETE FROM {$t}devices");
$c0 = lp_stats_cards();
ok($c0['total'] === 0 && $c0['online'] === 0, "全 0");
wp_set_current_user(1);
ob_start(); lp_page_overview(); $h0 = ob_get_clean();
ok(has($h0, '尚未接入设备数据'), '空数据时明确提示"尚未接入设备数据"');
ob_start(); lp_page_stats(); $hs = ob_get_clean();
ok(has($hs, '暂无数据'), "图表区显示暂无数据");
printf("\n");

/* ============================ T14 现有功能未被破坏 ============================ */
echo "T14 现有商店/投稿/主题未受影响\n";
$idx = lp_json('/bbk/index.json');
ok(is_array($idx) && isset($idx['pkgs']) && count($idx['pkgs']) > 0, "读商店 index.json 正常（" . count($idx['pkgs'] ?? array()) . " 条）");
$fw = lp_firmware();
ok(count($fw) > 0, "固件列表仍可读（" . count($fw) . " 个）");
ok(has(do_shortcode('[lintos_resources]'), '应用商店'), "前台短代码仍正常输出");
ok(file_exists('/www/wwwroot/bbk.linit.cn/upload/lib.php'), "投稿系统文件仍在");
ok(has(file_get_contents('/www/wwwroot/bbk.linit.cn/upload/lib.php'), 'store_publish'), "投稿系统代码未被改");
/* 生产表未被触碰 */
$real = (int) $wpdb->get_var("SELECT COUNT(*) FROM information_schema.tables
        WHERE table_schema = DATABASE() AND table_name = 'wp_lp_devices'");
ok($real === 0, "生产库没有新建 wp_lp_devices（测试全程用临时表）");
printf("\n");

/* ---------- 回滚 ---------- */
$wpdb->query('ROLLBACK');
echo "================ {$PASS} PASS / {$FAIL} FAIL ================\n";
exit($FAIL ? 1 : 0);
