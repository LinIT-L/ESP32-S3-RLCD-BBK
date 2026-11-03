<?php
/**
 * lp-device-core.php — 掌机项目 · 设备数据层 + 设备端 REST 接口
 *
 * 归属: linit-project 插件（被 linit-project.php require）
 *
 * 设计要点
 * --------
 * 1. **表前缀** 用 $wpdb->prefix . 'lp_'，不复用旧的裸表 `devices`
 *    （旧表 0 行、无写入方、注释乱码；保留不动，避免动到别人的东西）。
 * 2. **设备端接口走 WP REST**（/wp-json/lp/v1/register|heartbeat）：
 *    - 不再新增公开 .php 文件，DB 口令不外泄（旧 /bbk/api/report.php 正是把口令写在公开目录）
 *    - 与管理后台天然分离（不同 route + 不同鉴权）
 *    - 可在 CLI 用 rest_do_request() 直接测全链路，不必架 HTTP
 * 3. **身份认证**：注册用产品级 key（烧进固件，不是后台管理员密钥），心跳用每设备密钥。
 *    两者都做 HMAC + 时间窗 + nonce 去重 + 限频。
 * 4. **dev_id 由 MAC 派生且服务端强校验**：改 dev_id 必须同时改 MAC，
 *    而 MAC 唯一 ⇒ 无法只改 ID 冒充他人。
 * 5. 所有 SQL 走 $wpdb->prepare；所有输出走 esc_html。
 */
if (!defined('ABSPATH')) { exit; }

define('LP_DB_VERSION', '1');
define('LP_OPT_SETTINGS', 'lp_device_settings');
define('LP_PROTO_VERSION', 1);

/* ============================================================ 设置 */

function lp_default_settings() {
    return array(
        'allow_register'   => 1,        /* 是否允许新设备注册 */
        'hb_interval'      => 600,      /* 设备心跳间隔(秒), 下发给设备 */
        'online_timeout'   => 900,      /* 超过这么久没心跳 = 离线 */
        'offline_grace'    => 1800,     /* 超过这么久 = 长期离线(用于统计) */
        'product_key'      => '',       /* 注册用产品级 key(空 = 禁止注册) */
        'proto'            => LP_PROTO_VERSION,
        'retain_days'      => 365,      /* 设备记录保留天数(逻辑删除后物理清理) */
        'retain_events'    => 90,       /* 联网历史保留天数 */
        'latest_fw'        => '',
        'min_compat'       => '',
        'fw_notes'         => '',
        'abnormal_action'  => 'log',    /* 异常上报: log=只记录 */
    );
}

function lp_settings() {
    $s = get_option(LP_OPT_SETTINGS, array());
    if (!is_array($s)) { $s = array(); }
    return array_merge(lp_default_settings(), $s);
}

function lp_setting($k, $def = null) {
    $s = lp_settings();
    return array_key_exists($k, $s) ? $s[$k] : $def;
}

function lp_save_settings(array $in) {
    $d = lp_default_settings();
    $out = array();
    $out['allow_register']  = empty($in['allow_register']) ? 0 : 1;
    $out['hb_interval']     = max(60, min(86400, (int) ($in['hb_interval'] ?? $d['hb_interval'])));
    $out['online_timeout']  = max(120, min(86400, (int) ($in['online_timeout'] ?? $d['online_timeout'])));
    $out['offline_grace']   = max(300, min(2592000, (int) ($in['offline_grace'] ?? $d['offline_grace'])));
    $out['product_key']     = trim((string) ($in['product_key'] ?? ''));
    $out['proto']           = (int) ($in['proto'] ?? $d['proto']);
    $out['retain_days']     = max(1, min(3650, (int) ($in['retain_days'] ?? $d['retain_days'])));
    $out['retain_events']   = max(1, min(3650, (int) ($in['retain_events'] ?? $d['retain_events'])));
    $out['latest_fw']       = substr(trim((string) ($in['latest_fw'] ?? '')), 0, 32);
    $out['min_compat']      = substr(trim((string) ($in['min_compat'] ?? '')), 0, 32);
    $out['fw_notes']        = substr((string) ($in['fw_notes'] ?? ''), 0, 1000);
    $out['abnormal_action'] = in_array(($in['abnormal_action'] ?? 'log'), array('log', 'reject'), true)
                              ? $in['abnormal_action'] : 'log';
    update_option(LP_OPT_SETTINGS, $out, false);
    lp_audit('settings.save', '-', '心跳 ' . $out['hb_interval'] . 's / 在线阈值 ' . $out['online_timeout'] . 's');
    return $out;
}

/* ============================================================ 表名 */

/** 统一表名入口。测试用 LP_TBL_PREFIX 指向临时表，生产走 $wpdb->prefix。 */
function lp_tbl($n) {
    global $wpdb;
    $p = defined('LP_TBL_PREFIX') ? LP_TBL_PREFIX : $wpdb->prefix;
    return $p . 'lp_' . $n;
}

/* ============================================================ 建表 */

/** 返回建表 SQL 数组。$prefix 仅供测试注入（用 TEMPORARY 表跑真实 MySQL）。 */
function lp_tables_sql($prefix = null) {
    global $wpdb;
    $p = $prefix !== null ? $prefix : (defined('LP_TBL_PREFIX') ? LP_TBL_PREFIX : $wpdb->prefix);
    $c = $wpdb->get_charset_collate();
    $t = $p . 'lp_';

    return array(
        "CREATE TABLE IF NOT EXISTS {$t}devices (
            id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
            dev_id VARCHAR(40) NOT NULL,
            mac VARCHAR(17) NOT NULL,
            name VARCHAR(64) NULL,
            board VARCHAR(32) NULL,
            chip VARCHAR(32) NULL,
            fw VARCHAR(32) NULL,
            proto SMALLINT UNSIGNED NOT NULL DEFAULT 1,
            first_seen DATETIME NOT NULL,
            last_seen DATETIME NOT NULL,
            last_hb DATETIME NULL,
            last_ip VARCHAR(45) NULL,
            hits BIGINT UNSIGNED NOT NULL DEFAULT 0,
            source VARCHAR(16) NOT NULL DEFAULT 'api',
            secret VARCHAR(64) NULL,
            note VARCHAR(255) NULL,
            disabled TINYINT(1) NOT NULL DEFAULT 0,
            deleted_at DATETIME NULL,
            PRIMARY KEY (id),
            UNIQUE KEY uq_dev (dev_id),
            UNIQUE KEY uq_mac (mac),
            KEY idx_last_seen (last_seen),
            KEY idx_fw (fw),
            KEY idx_board (board),
            KEY idx_del (deleted_at)
        ) $c",

        "CREATE TABLE IF NOT EXISTS {$t}events (
            id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
            dev_id VARCHAR(40) NOT NULL,
            ts DATETIME NOT NULL,
            type VARCHAR(16) NOT NULL,
            fw VARCHAR(32) NULL,
            ip VARCHAR(45) NULL,
            detail VARCHAR(255) NULL,
            PRIMARY KEY (id),
            KEY idx_dev_ts (dev_id, ts),
            KEY idx_ts (ts),
            KEY idx_type (type)
        ) $c",

        "CREATE TABLE IF NOT EXISTS {$t}nonces (
            nonce VARCHAR(64) NOT NULL,
            dev_id VARCHAR(40) NOT NULL,
            ts DATETIME NOT NULL,
            PRIMARY KEY (nonce),
            KEY idx_ts (ts)
        ) $c",

        "CREATE TABLE IF NOT EXISTS {$t}fw (
            version VARCHAR(32) NOT NULL,
            released DATETIME NOT NULL,
            notes TEXT NULL,
            min_compat VARCHAR(32) NULL,
            PRIMARY KEY (version)
        ) $c",

        "CREATE TABLE IF NOT EXISTS {$t}audit (
            id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
            ts DATETIME NOT NULL,
            actor VARCHAR(64) NOT NULL,
            action VARCHAR(32) NOT NULL,
            target VARCHAR(64) NOT NULL,
            detail VARCHAR(255) NULL,
            PRIMARY KEY (id),
            KEY idx_ts (ts)
        ) $c",
    );
}

/** 建表（幂等）。返回执行失败数。 */
function lp_install($prefix = null) {
    global $wpdb;
    $fail = 0;
    foreach (lp_tables_sql($prefix) as $sql) {
        if ($wpdb->query($sql) === false) { $fail++; }
    }
    if ($prefix === null) { update_option('lp_db_version', LP_DB_VERSION, false); }
    return $fail;
}

/** 审计：后台关键操作留痕（不含任何密钥） */
function lp_audit($action, $target, $detail = '') {
    global $wpdb;
    $u = function_exists('wp_get_current_user') ? wp_get_current_user() : null;
    $actor = ($u && $u->exists()) ? $u->user_login : 'system';
    $wpdb->insert(lp_tbl('audit'), array(
        'ts' => current_time('mysql'), 'actor' => $actor,
        'action' => $action, 'target' => $target, 'detail' => substr($detail, 0, 255),
    ));
}

function lp_recent_audit($limit = 100) {
    global $wpdb;
    $t = lp_tbl('audit');
    return $wpdb->get_results($wpdb->prepare("SELECT * FROM $t ORDER BY id DESC LIMIT %d", $limit), ARRAY_A);
}

/* ============================================================ 设备 ID / 校验 */

/** dev_id 完全由 MAC 派生：改 ID 就必须改 MAC（MAC 唯一）⇒ 无法只改 ID 冒充 */
function lp_dev_id_from_mac($mac) {
    $hex = preg_replace('/[^0-9A-Fa-f]/', '', (string) $mac);
    if (strlen($hex) !== 12) { return ''; }
    return 'LNT' . strtoupper($hex);
}

function lp_mac_ok($mac) {
    return (bool) preg_match('/^([0-9A-F]{2}:){5}[0-9A-F]{2}$/', (string) $mac);
}

/** "本地时间的 epoch" —— WP 把 PHP 默认时区设成 UTC，而 current_time('mysql') 写的是站点本地时间；
 *  直接拿 time() 去比会差一个时区（实测：1 小时前的心跳被判成"在线"）。统一走这个函数。 */
function lp_now_ts() { return (int) current_time('timestamp'); }

/** 在线条件 SQL 片段（阈值来自设置，可配） */
function lp_online_sql() {
    $sec = (int) lp_setting('online_timeout', 900);
    return "last_hb IS NOT NULL AND last_hb >= (NOW() - INTERVAL $sec SECOND)";
}

/* ============================================================ 统计 */

/** 总览卡片：全部来自真实表，无数据时全部为 0（不编造） */
function lp_stats_cards() {
    global $wpdb;
    $t = lp_tbl('devices');
    $on = lp_online_sql();
    $grace = (int) lp_setting('offline_grace', 1800);

    $total   = (int) $wpdb->get_var("SELECT COUNT(*) FROM $t WHERE deleted_at IS NULL");
    $online  = (int) $wpdb->get_var("SELECT COUNT(*) FROM $t WHERE deleted_at IS NULL AND $on");
    $today   = (int) $wpdb->get_var("SELECT COUNT(*) FROM $t WHERE deleted_at IS NULL AND first_seen >= CURDATE()");
    $a7      = (int) $wpdb->get_var("SELECT COUNT(*) FROM $t WHERE deleted_at IS NULL AND last_hb >= (NOW() - INTERVAL 7 DAY)");
    $a30     = (int) $wpdb->get_var("SELECT COUNT(*) FROM $t WHERE deleted_at IS NULL AND last_hb >= (NOW() - INTERVAL 30 DAY)");
    $offline = max(0, $total - $online);
    $long    = (int) $wpdb->get_var("SELECT COUNT(*) FROM $t WHERE deleted_at IS NULL AND (last_hb IS NULL OR last_hb < (NOW() - INTERVAL $grace SECOND))");
    $fw_n    = (int) $wpdb->get_var("SELECT COUNT(DISTINCT fw) FROM $t WHERE deleted_at IS NULL AND fw IS NOT NULL AND fw <> ''");
    $last_any = $wpdb->get_var("SELECT MAX(last_hb) FROM $t WHERE deleted_at IS NULL");

    return array(
        'total' => $total, 'online' => $online, 'today' => $today,
        'active7' => $a7, 'active30' => $a30, 'offline' => $offline,
        'long_offline' => $long, 'fw_count' => $fw_n, 'last_report' => $last_any,
    );
}

/** 固件 / 板型分布（真实分组） */
function lp_dist($col = 'fw', $limit = 12) {
    global $wpdb;
    if (!in_array($col, array('fw', 'board', 'chip'), true)) { return array(); }
    $t = lp_tbl('devices');
    return $wpdb->get_results($wpdb->prepare(
        "SELECT $col AS k, COUNT(*) AS n FROM $t
         WHERE deleted_at IS NULL AND $col IS NOT NULL AND $col <> ''
         GROUP BY $col ORDER BY n DESC LIMIT %d", $limit), ARRAY_A);
}

/** 新增设备趋势：按 day/week/month 分组 */
function lp_new_series($gran = 'day', $points = 14) {
    global $wpdb;
    $t = lp_tbl('devices');
    $fmt = array('day' => '%Y-%m-%d', 'week' => '%x-W%v', 'month' => '%Y-%m')[ $gran ] ?? '%Y-%m-%d';
    $rows = $wpdb->get_results($wpdb->prepare(
        "SELECT DATE_FORMAT(first_seen, '$fmt') AS k, COUNT(*) AS n
         FROM $t WHERE deleted_at IS NULL
         GROUP BY k ORDER BY k DESC LIMIT %d", $points), ARRAY_A);
    return array_reverse($rows ?: array());
}

/** 在线数变化：用「每日最后心跳的设备数」近似（无历史快照表时的诚实近似，页面会写清口径） */
function lp_online_series($days = 14) {
    global $wpdb;
    $t = lp_tbl('devices');
    $rows = $wpdb->get_results($wpdb->prepare(
        "SELECT DATE(last_hb) AS k, COUNT(DISTINCT dev_id) AS n
         FROM $t WHERE deleted_at IS NULL AND last_hb IS NOT NULL
           AND last_hb >= (NOW() - INTERVAL %d DAY)
         GROUP BY k ORDER BY k ASC", $days), ARRAY_A);
    return $rows ?: array();
}

/** 最近活跃排行 */
function lp_top_active($limit = 10) {
    global $wpdb;
    $t = lp_tbl('devices');
    return $wpdb->get_results($wpdb->prepare(
        "SELECT dev_id, COALESCE(name, dev_id) AS nm, fw, hits, last_hb
         FROM $t WHERE deleted_at IS NULL ORDER BY hits DESC, last_hb DESC LIMIT %d", $limit), ARRAY_A);
}

/* ============================================================ 列表查询 */

/**
 * 设备列表：搜索 / 筛选 / 排序 / 分页。全部走 prepare。
 * $args: q, state(online|offline|all), fw, board, order(first|last), paged, per
 */
function lp_dev_list(array $args = array()) {
    global $wpdb;
    $t = lp_tbl('devices');

    $q      = trim((string) ($args['q'] ?? ''));
    $state  = (string) ($args['state'] ?? 'all');
    $fw     = trim((string) ($args['fw'] ?? ''));
    $board  = trim((string) ($args['board'] ?? ''));
    $order  = ((string) ($args['order'] ?? 'last') === 'first') ? 'first_seen DESC' : 'last_seen DESC';
    $per    = max(1, min(200, (int) ($args['per'] ?? 20)));
    $page   = max(1, (int) ($args['paged'] ?? 1));
    $off    = ($page - 1) * $per;

    $where = array('deleted_at IS NULL');
    $bind  = array();
    if ($q !== '') {
        $like = '%' . $wpdb->esc_like($q) . '%';
        $where[] = '(mac LIKE %s OR dev_id LIKE %s OR name LIKE %s)';
        array_push($bind, $like, $like, $like);
    }
    if ($state === 'online')  { $where[] = '(' . lp_online_sql() . ')'; }
    if ($state === 'offline') { $where[] = '(NOT (' . lp_online_sql() . '))'; }
    if ($fw !== '')    { $where[] = 'fw = %s';    $bind[] = $fw; }
    if ($board !== '') { $where[] = 'board = %s'; $bind[] = $board; }

    $w = implode(' AND ', $where);
    $count_sql = "SELECT COUNT(*) FROM $t WHERE $w";
    $list_sql  = "SELECT * FROM $t WHERE $w ORDER BY $order LIMIT %d OFFSET %d";

    $total = (int) ($bind
        ? $wpdb->get_var($wpdb->prepare($count_sql, $bind))
        : $wpdb->get_var($count_sql));

    $lb = $bind; $lb[] = $per; $lb[] = $off;
    $rows = $wpdb->get_results($wpdb->prepare($list_sql, $lb), ARRAY_A);

    return array('rows' => $rows ?: array(), 'total' => $total,
                 'per' => $per, 'page' => $page,
                 'pages' => max(1, (int) ceil($total / $per)));
}

function lp_dev_get($dev_id) {
    global $wpdb;
    $t = lp_tbl('devices');
    return $wpdb->get_row($wpdb->prepare("SELECT * FROM $t WHERE dev_id = %s", $dev_id), ARRAY_A);
}

function lp_dev_events($dev_id, $limit = 50) {
    global $wpdb;
    $t = lp_tbl('events');
    return $wpdb->get_results($wpdb->prepare(
        "SELECT * FROM $t WHERE dev_id = %s ORDER BY id DESC LIMIT %d", $dev_id, $limit), ARRAY_A);
}

/** 固件变更历史（从事件表里筛 fw 类型） */
function lp_dev_fw_history($dev_id) {
    global $wpdb;
    $t = lp_tbl('events');
    return $wpdb->get_results($wpdb->prepare(
        "SELECT ts, fw, detail FROM $t WHERE dev_id = %s AND type = 'fw' ORDER BY id DESC LIMIT 50", $dev_id), ARRAY_A);
}

function lp_event_add($dev_id, $type, $fw = null, $ip = null, $detail = '') {
    global $wpdb;
    $wpdb->insert(lp_tbl('events'), array(
        'dev_id' => $dev_id, 'ts' => current_time('mysql'), 'type' => $type,
        'fw' => $fw, 'ip' => $ip, 'detail' => substr((string) $detail, 0, 255),
    ));
}

/* ============================================================ 设备端接口 */

/** 统一 JSON 输出（极简：设备端只解析一个字段） */
function lp_json_out($code, array $data) {
    return new WP_REST_Response($data, $code);
}

/**
 * 限频：同一 key 在 $win 秒内最多 $max 次。
 * 用 transient 实现（无新表、无额外 IO）。
 */
function lp_rate_limit($key, $max, $win) {
    $k = 'lp_rl_' . md5($key);
    $n = (int) get_transient($k);
    if ($n >= $max) { return false; }
    set_transient($k, $n + 1, $win);
    return true;
}

/** 时间窗校验（防重放第一道）：±$win 秒 */
function lp_ts_ok($ts, $win = 300) {
    $ts = (int) $ts;
    if ($ts <= 0) { return false; }
    return abs(time() - $ts) <= $win;
}

/** nonce 去重（防重放第二道）。同一个 nonce 只用一次。 */
function lp_nonce_consume($dev_id, $nonce) {
    global $wpdb;
    $nonce = preg_replace('/[^A-Za-z0-9_-]/', '', (string) $nonce);
    if (strlen($nonce) < 8 || strlen($nonce) > 64) { return false; }
    $t = lp_tbl('nonces');
    $ok = $wpdb->query($wpdb->prepare(
        "INSERT IGNORE INTO $t (nonce, dev_id, ts) VALUES (%s, %s, NOW())", $nonce, $dev_id));
    return $ok === 1;                       /* 0 = 重复(已存在) */
}

/** 注册签名串：与固件侧必须逐字节一致 */
function lp_sig_string_register(array $a) {
    return "register\n" . (string) ($a['dev_id'] ?? '') . "\n" . (string) ($a['mac'] ?? '') . "\n"
         . (string) ($a['board'] ?? '') . "\n" . (string) ($a['chip'] ?? '') . "\n"
         . (string) ($a['fw'] ?? '') . "\n" . (int) ($a['proto'] ?? 0) . "\n"
         . (int) ($a['ts'] ?? 0) . "\n" . (string) ($a['nonce'] ?? '');
}
function lp_sig_string_hb(array $a) {
    return "heartbeat\n" . (string) ($a['dev_id'] ?? '') . "\n"
         . (int) ($a['ts'] ?? 0) . "\n" . (string) ($a['nonce'] ?? '');
}

/** 字段裁剪与范围校验（服务端不信任任何输入） */
function lp_sanitize_fields(array $in) {
    $out = array();
    $out['mac']   = strtoupper(trim((string) ($in['mac'] ?? '')));
    $out['dev_id']= strtoupper(trim((string) ($in['dev_id'] ?? '')));
    $out['board'] = substr(preg_replace('/[^A-Za-z0-9._-]/', '', (string) ($in['board'] ?? '')), 0, 32);
    $out['chip']  = substr(preg_replace('/[^A-Za-z0-9._-]/', '', (string) ($in['chip'] ?? '')), 0, 32);
    $out['fw']    = substr(preg_replace('/[^A-Za-z0-9._-]/', '', (string) ($in['fw'] ?? '')), 0, 32);
    $out['proto'] = (int) ($in['proto'] ?? 0);
    $out['ts']    = (int) ($in['ts'] ?? 0);
    $out['nonce'] = preg_replace('/[^A-Za-z0-9_-]/', '', (string) ($in['nonce'] ?? ''));
    $out['sig']   = strtolower(trim((string) ($in['sig'] ?? '')));
    return $out;
}

/* ---------------- 注册 ---------------- */
function lp_api_register(WP_REST_Request $req) {
    global $wpdb;
    $s = lp_settings();
    if (empty($s['allow_register'])) {
        return lp_json_out(403, array('ok' => false, 'err' => 'REGISTER_CLOSED'));
    }
    if ((string) $s['product_key'] === '') {
        return lp_json_out(403, array('ok' => false, 'err' => 'NO_KEY_CONFIGURED'));
    }
    $ip = isset($_SERVER['REMOTE_ADDR']) ? (string) $_SERVER['REMOTE_ADDR'] : '';
    if (!lp_rate_limit('reg|' . $ip, 30, 60)) {
        return lp_json_out(429, array('ok' => false, 'err' => 'RATE_LIMIT'));
    }

    /* ★ 验签用**原始值**：设备按它自己看到的字段算 HMAC，服务端先做大小写/截断就会对不上
     *   （实测：200 字符的 board 会让诚实设备报 BAD_SIG）。裁剪只用于入库。 */
    $raw = $req->get_params();
    $sig_raw = strtolower(trim((string) ($raw['sig'] ?? '')));
    $sig_src = array(
        'dev_id' => (string) ($raw['dev_id'] ?? ''), 'mac' => (string) ($raw['mac'] ?? ''),
        'board'  => (string) ($raw['board'] ?? ''),  'chip' => (string) ($raw['chip'] ?? ''),
        'fw'     => (string) ($raw['fw'] ?? ''),     'proto' => (int) ($raw['proto'] ?? 0),
        'ts'     => (int) ($raw['ts'] ?? 0),         'nonce' => (string) ($raw['nonce'] ?? ''),
    );
    $a = lp_sanitize_fields($raw);
    $a['sig'] = $sig_raw;

    if (!lp_mac_ok($a['mac'])) { return lp_json_out(400, array('ok' => false, 'err' => 'BAD_MAC')); }
    if ($a['dev_id'] !== lp_dev_id_from_mac($a['mac'])) {
        return lp_json_out(400, array('ok' => false, 'err' => 'BAD_DEV_ID'));
    }
    if ($a['proto'] < 1 || $a['proto'] > 99) { return lp_json_out(400, array('ok' => false, 'err' => 'BAD_PROTO')); }
    if (!lp_ts_ok($a['ts'])) { return lp_json_out(400, array('ok' => false, 'err' => 'BAD_TS')); }
    if (strlen($a['nonce']) < 8) { return lp_json_out(400, array('ok' => false, 'err' => 'BAD_NONCE')); }

    $want = hash_hmac('sha256', lp_sig_string_register($sig_src), (string) $s['product_key']);
    if (!hash_equals($want, $sig_raw)) { return lp_json_out(403, array('ok' => false, 'err' => 'BAD_SIG')); }
    if (!lp_nonce_consume($a['dev_id'], $a['nonce'])) {
        return lp_json_out(409, array('ok' => false, 'err' => 'REPLAY'));
    }

    $t = lp_tbl('devices');
    $row = $wpdb->get_row($wpdb->prepare("SELECT * FROM $t WHERE dev_id = %s", $a['dev_id']), ARRAY_A);
    $secret = bin2hex(random_bytes(24));          /* 每设备密钥，注册时下发（丢了就重新注册换新） */
    $now = current_time('mysql');

    if (!$row) {
        $wpdb->insert($t, array(
            'dev_id' => $a['dev_id'], 'mac' => $a['mac'], 'board' => $a['board'] ?: null,
            'chip' => $a['chip'] ?: null, 'fw' => $a['fw'] ?: null, 'proto' => $a['proto'],
            'first_seen' => $now, 'last_seen' => $now, 'last_hb' => $now,
            'last_ip' => $ip, 'hits' => 1, 'source' => 'api', 'secret' => $secret,
        ));
        lp_event_add($a['dev_id'], 'register', $a['fw'], $ip, 'first_seen');
        lp_audit('device.register', $a['dev_id'], 'ip=' . $ip);
    } else {
        /* 重复注册：不新建、不覆盖 first_seen；密钥轮换（设备重刷固件后重新注册） */
        $upd = array('last_seen' => $now, 'last_hb' => $now, 'last_ip' => $ip,
                     'proto' => $a['proto'], 'secret' => $secret);
        if ($a['board'] !== '') { $upd['board'] = $a['board']; }
        if ($a['chip'] !== '')  { $upd['chip']  = $a['chip']; }
        if ($a['fw'] !== '' && $a['fw'] !== $row['fw']) {
            $upd['fw'] = $a['fw'];
            lp_event_add($a['dev_id'], 'fw', $a['fw'], $ip, 'was=' . $row['fw']);
        }
        $wpdb->update($t, $upd, array('dev_id' => $a['dev_id']));
        lp_event_add($a['dev_id'], 'register', $a['fw'], $ip, 'repeat');
    }

    return lp_json_out(200, array(
        'ok' => true,
        'secret' => $secret,
        'hb' => (int) $s['hb_interval'],
        'proto' => (int) $s['proto'],
        'server_ts' => time(),
    ));
}

/* ---------------- 心跳 ---------------- */
function lp_api_heartbeat(WP_REST_Request $req) {
    global $wpdb;
    $s = lp_settings();
    $ip = isset($_SERVER['REMOTE_ADDR']) ? (string) $_SERVER['REMOTE_ADDR'] : '';
    if (!lp_rate_limit('hb|' . $ip, 240, 60)) {
        return lp_json_out(429, array('ok' => false, 'err' => 'RATE_LIMIT'));
    }

    $raw = $req->get_params();
    $sig_raw = strtolower(trim((string) ($raw['sig'] ?? '')));
    $sig_src = array('dev_id' => (string) ($raw['dev_id'] ?? ''),
                     'ts' => (int) ($raw['ts'] ?? 0), 'nonce' => (string) ($raw['nonce'] ?? ''));
    $a = lp_sanitize_fields($raw);
    $a['sig'] = $sig_raw;

    if ($a['dev_id'] === '') { return lp_json_out(400, array('ok' => false, 'err' => 'BAD_DEV_ID')); }
    if (!lp_ts_ok($a['ts'])) { return lp_json_out(400, array('ok' => false, 'err' => 'BAD_TS')); }
    if (strlen($a['nonce']) < 8) { return lp_json_out(400, array('ok' => false, 'err' => 'BAD_NONCE')); }

    $t = lp_tbl('devices');
    $row = $wpdb->get_row($wpdb->prepare(
        "SELECT * FROM $t WHERE dev_id = %s AND deleted_at IS NULL", $a['dev_id']), ARRAY_A);
    if (!$row) { return lp_json_out(404, array('ok' => false, 'err' => 'UNKNOWN_DEVICE')); }
    if ((int) $row['disabled'] === 1) { return lp_json_out(403, array('ok' => false, 'err' => 'DISABLED')); }

    $secret = (string) $row['secret'];
    if ($secret === '') { return lp_json_out(403, array('ok' => false, 'err' => 'RESET_REQUIRED')); }
    $want = hash_hmac('sha256', lp_sig_string_hb($sig_src), $secret);
    if (!hash_equals($want, $sig_raw)) { return lp_json_out(403, array('ok' => false, 'err' => 'BAD_SIG')); }
    if (!lp_nonce_consume($a['dev_id'], $a['nonce'])) {
        return lp_json_out(409, array('ok' => false, 'err' => 'REPLAY'));
    }

    $now = current_time('mysql');
    $upd = array('last_seen' => $now, 'last_hb' => $now, 'last_ip' => $ip,
                 'hits' => (int) $row['hits'] + 1);
    if ($a['fw'] !== '' && $a['fw'] !== (string) $row['fw']) {
        $upd['fw'] = $a['fw'];
        lp_event_add($a['dev_id'], 'fw', $a['fw'], $ip, 'was=' . $row['fw']);
    }
    /* 从离线回到在线：记一条 online 事件（详情页能区分"重新联网"） */
    $to = (int) $s['online_timeout'];
    if (empty($row['last_hb']) || strtotime($row['last_hb']) < time() - $to) {
        lp_event_add($a['dev_id'], 'online', $upd['fw'] ?? $row['fw'], $ip, 'back_online');
    }
    $wpdb->update($t, $upd, array('dev_id' => $a['dev_id']));

    return lp_json_out(200, array('ok' => true, 'hb' => (int) $s['hb_interval'], 'server_ts' => time()));
}

/* ---------------- 路由 ---------------- */
add_action('rest_api_init', function () {
    $pub = '__return_true';                       /* 设备端：靠 HMAC 鉴权，不靠 WP 用户 */
    register_rest_route('lp/v1', '/register', array(
        'methods' => 'POST', 'callback' => 'lp_api_register', 'permission_callback' => $pub,
    ));
    register_rest_route('lp/v1', '/heartbeat', array(
        'methods' => 'POST', 'callback' => 'lp_api_heartbeat', 'permission_callback' => $pub,
    ));
});

/* ---------------- 保留期清理（WP-Cron 每日 + 手动按钮） ---------------- */
function lp_cleanup() {
    global $wpdb;
    $s = lp_settings();
    $ev = lp_tbl('events');
    $nz = lp_tbl('nonces');
    $dv = lp_tbl('devices');

    $n1 = $wpdb->query($wpdb->prepare(
        "DELETE FROM $ev WHERE ts < (NOW() - INTERVAL %d DAY)", (int) $s['retain_events']));
    $n2 = $wpdb->query("DELETE FROM $nz WHERE ts < (NOW() - INTERVAL 1 DAY)");
    /* 逻辑删除超过保留期的设备：物理删除（历史事件一并清） */
    $old = $wpdb->get_col($wpdb->prepare(
        "SELECT dev_id FROM $dv WHERE deleted_at IS NOT NULL AND deleted_at < (NOW() - INTERVAL %d DAY)",
        (int) $s['retain_days']));
    $n3 = 0;
    foreach ((array) $old as $d) {
        $wpdb->delete($ev, array('dev_id' => $d));
        $wpdb->delete($dv, array('dev_id' => $d));
        $n3++;
    }
    lp_audit('data.cleanup', '-', "events={$n1} nonces={$n2} devices={$n3}");
    return array('events' => (int) $n1, 'nonces' => (int) $n2, 'devices' => $n3);
}
add_action('lp_daily_cleanup', 'lp_cleanup');
if (!wp_next_scheduled('lp_daily_cleanup')) {
    wp_schedule_event(time() + 3600, 'daily', 'lp_daily_cleanup');
}
