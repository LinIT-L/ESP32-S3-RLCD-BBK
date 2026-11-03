<?php
/**
 * submit.php — 接收 .lua 投稿：服务端硬校验 → 落盘 → 入库 → 通知
 *
 * 原则: **什么都不信客户端**。
 *   - 文件名、路径、crc、sha 一律服务端重算；落盘名 = sha256(内容)
 *   - 内容先过 validate_upload()（大小 / UTF-8 / NUL / 二进制控制符）
 *   - LAPP 头以文件内容为准，表单只能改展示字段，且改完要重新校验
 *   - 重复/冲突检查：同 ID 待审、版本必须高于商店当前版本、每日配额
 *   - 被驳回的可以改后重投（resub_of 指向原记录，原记录保留）
 */
declare(strict_types=1);
require __DIR__ . '/lib.php';

function back(string $q = ''): void {
    header('Location: index.php' . ($q !== '' ? '?' . $q : ''));
    exit;
}

$u = current_user();
if (!$u) { flash('请先登录', 'err'); back(); }
if ($_SERVER['REQUEST_METHOD'] !== 'POST' || !csrf_ok()) { flash('页面过期了，请重试', 'err'); back(); }

$lim = (array)cfg('limits');
$resubOf = (int)($_POST['resub_of'] ?? 0);

/* ---------------- 1. 文件本体 ---------------- */
$f = $_FILES['lua'] ?? null;
if (!$f || ($f['error'] ?? UPLOAD_ERR_NO_FILE) !== UPLOAD_ERR_OK) {
    flash('没有收到文件，或者上传中断了', 'err'); back();
}
if (!is_uploaded_file((string)$f['tmp_name'])) {
    flash('上传来源不合法', 'err'); back();
}
if ((int)$f['size'] > (int)$lim['max_bytes']) {
    flash('文件超过 ' . (int)($lim['max_bytes'] / 1024) . ' KB', 'err'); back();
}
if (strtolower(pathinfo((string)$f['name'], PATHINFO_EXTENSION)) !== 'lua') {
    flash('只接受 .lua 文件', 'err'); back();
}

$src = (string)file_get_contents((string)$f['tmp_name']);
[$okUp, $upErrs] = validate_upload($src);
if (!$okUp) { flash('提交失败：' . $upErrs[0], 'err'); back(); }

/* ---------------- 2. 解析头部（以文件为准） ---------------- */
$meta = lapp_parse($src);

$name  = trim((string)($_POST['name']  ?? '')) ?: $meta['name'];
$descr = trim((string)($_POST['descr'] ?? '')) ?: $meta['desc'];
$catIn = trim((string)($_POST['cat']   ?? ''));
$verIn = trim((string)($_POST['ver']   ?? '')) ?: $meta['ver'];

$cat = in_array($catIn, (array)cfg('cats'), true)
     ? $catIn
     : norm_cat($catIn !== '' ? $catIn : (string)$meta['cat']);
$ver = norm_ver($verIn);

$merged = $meta;
$merged['name'] = $name; $merged['cat'] = $cat;
$merged['desc'] = $descr; $merged['ver'] = (string)$ver;
[$errs, $warns] = lapp_validate($merged, $src);
if ($errs) { flash('提交失败：' . $errs[0], 'err'); back(); }

$appId = store_app_id($name);
if ($appId === '') { flash('应用名不能为空', 'err'); back(); }
if (mb_strlen($appId, 'UTF-8') > 40) { flash('应用名太长（商店文件名最多 40 字）', 'err'); back(); }

/* ---------------- 3. 冲突检查 ---------------- */
$existing = store_find($appId);
if ($existing) {
    $old = (int)($existing['ver'] ?? 0);
    if ($ver <= $old) {
        flash("商店里「{$appId}」已经是 v{$old}，新版本号必须大于 {$old}（你填的是 v{$ver}）", 'err');
        back();
    }
}
/* 历史版本号也不能回退（回滚是管理员操作，不允许投稿绕过） */
$mx = db()->prepare('SELECT MAX(ver) m FROM versions WHERE app_id = ?');
$mx->execute([$appId]);
$maxHist = (int)($mx->fetch()['m'] ?? 0);
if ($maxHist > 0 && $ver <= $maxHist) {
    flash("这个应用历史上已经发布到 v{$maxHist}，新投稿的版本号必须大于 {$maxHist}", 'err');
    back();
}

$st = db()->prepare("SELECT id FROM subs WHERE name = ? AND status IN ('pending','reviewing','approved') LIMIT 1");
$st->execute([$appId]);
if ($st->fetch()) {
    flash('这个应用已经有一份在等审核了，等处理完再提交', 'err');
    back();
}
if (upload_quota_left((int)$u['id']) <= 0) {
    flash('今天提交次数用完了（每天 ' . (int)$lim['per_day_upload'] . ' 个）', 'err');
    back();
}

/* 改后重投: 只能重投自己那条且状态为驳回 */
if ($resubOf > 0) {
    $q = db()->prepare('SELECT * FROM subs WHERE id = ?');
    $q->execute([$resubOf]);
    $old = $q->fetch();
    if (!$old || (int)$old['uid'] !== (int)$u['id'] || $old['status'] !== 'rejected') {
        flash('要重投的那条记录不可用（不是你提交的，或者不是驳回状态）', 'err');
        back();
    }
}

/* ---------------- 4. 落盘（名字由内容决定，不信任客户端） ---------------- */
$sha   = hash('sha256', $src);
$crc   = sprintf('%08x', crc32($src));
$risks = scan_lua($src);
$verdict = scan_verdict($risks);
$store = rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $sha . '.lua';
if (!is_file($store)) {
    if (@file_put_contents($store, $src) === false) {
        flash('服务器写入失败，请稍后再试', 'err'); back();
    }
    @chmod($store, 0600);
}

/* build（可选）: 打包器写的 YYYYMMDDHHMM，用于发布时的单调性校验 */
$build = (string)($meta['build'] ?? '');

$fname = safe_fname($name);
db()->prepare('INSERT INTO subs
    (uid, name, label, cat, descr, ver, build, fname, size, crc, sha, risks, scan, verdict,
     status, created, ip, updated, resub_of)
    VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)')
  ->execute([(int)$u['id'], $appId, $name, $cat, $descr, $ver, $build, $fname,
             strlen($src), $crc, $sha,
             json_encode($risks, JSON_UNESCAPED_UNICODE),
             json_encode($risks, JSON_UNESCAPED_UNICODE),
             $verdict, 'pending', time(), client_ip(), time(), $resubOf ?: null]);
$id = (int)db()->lastInsertId();

audit((string)$u['email'], 'sub.create', $appId, $ver, null, 'pending',
      'ok', '投稿 #' . $id . ($resubOf ? " 重投自 #{$resubOf}" : '') . ' 扫描=' . $verdict);

/* ---------------- 5. 通知站主（发不出去不影响入库） ---------------- */
$base = rtrim((string)cfg('base_url'), '/');
$isUpd = $existing ? ('更新 v' . (int)$existing['ver'] . ' → v' . $ver) : ('新应用 v' . $ver);
$riskTxt = '';
foreach ($risks as $r) {
    $riskTxt .= '· [' . $r['level'] . '] ' . ($r['code'] ?? '') . ' — ' . ($r['why'] ?? '') . "\n";
}
$html = '<div style="font:15px/1.6 -apple-system,\'PingFang SC\',sans-serif">'
 . '<h2 style="margin:0 0 6px">' . h($isUpd) . ' 待审核</h2>'
 . '<p style="color:#6e6e73;margin:0 0 14px">' . h($appId) . ' · ' . h($cat) . '</p>'
 . '<table cellpadding="5" style="font-size:14px">'
 . '<tr><td style="color:#6e6e73">说明</td><td>' . h($descr) . '</td></tr>'
 . '<tr><td style="color:#6e6e73">大小</td><td>' . number_format(strlen($src)) . ' 字节</td></tr>'
 . '<tr><td style="color:#6e6e73">crc32</td><td><code>' . h($crc) . '</code></td></tr>'
 . '<tr><td style="color:#6e6e73">提交者</td><td>' . h((string)$u['email']) . '</td></tr>'
 . '<tr><td style="color:#6e6e73">扫描</td><td>' . h($verdict) . '</td></tr>'
 . '</table>'
 . ($riskTxt !== '' ? '<pre style="background:#f5f5f7;padding:12px;border-radius:8px;font-size:12px">' . h($riskTxt) . '</pre>' : '')
 . '<p style="margin:20px 0"><a href="' . h($base . '/review.php?id=' . $id) . '"'
 . ' style="background:#0071e3;color:#fff;padding:11px 24px;border-radius:10px;text-decoration:none;font-weight:600">'
 . '打开审核页</a></p>'
 . '<p style="color:#98989d;font-size:12px">审核页需要先登录（admin_login.php）。邮件里故意不放一键通过链接。</p></div>';

[$mok, $mwhy] = smtp_send((string)cfg('admin_email'), '【待审】' . $appId . ' v' . $ver, $html);

flash($mok ? '已提交，等站主审核' : ('已提交（通知邮件没发出去：' . $mwhy . '），站主会在后台看到'));
back();
