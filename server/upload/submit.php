<?php
/** 接收 .lua 上传：校验 → 落盘 → 入库 → 邮件通知站主 */
require __DIR__ . '/lib.php';

$u = current_user();
if (!$u) { flash('请先登录', 'err'); header('Location: index.php'); exit; }
if ($_SERVER['REQUEST_METHOD'] !== 'POST' || !csrf_ok()) { flash('页面过期了，请重试', 'err'); header('Location: index.php'); exit; }

$lim = (array)cfg('limits');

/* ---------------- 1. 文件本身 ---------------- */
$f = $_FILES['lua'] ?? null;
if (!$f || ($f['error'] ?? UPLOAD_ERR_NO_FILE) !== UPLOAD_ERR_OK) {
    flash('没有收到文件，或者上传中断了', 'err'); header('Location: index.php'); exit;
}
if ((int)$f['size'] > (int)$lim['max_bytes']) {
    flash('文件超过 ' . (int)($lim['max_bytes'] / 1024) . ' KB', 'err'); header('Location: index.php'); exit;
}
if (strtolower(pathinfo((string)$f['name'], PATHINFO_EXTENSION)) !== 'lua') {
    flash('只接受 .lua 文件', 'err'); header('Location: index.php'); exit;
}
$src = (string)file_get_contents($f['tmp_name']);
if ($src === '') { flash('文件是空的', 'err'); header('Location: index.php'); exit; }

/* ---------------- 2. 解析头部（后端以文件为准，不信任前端） ---------------- */
$meta = lapp_parse($src);

/* 表单值可以覆盖头部（用户在页面上改过的），但一律重新校验 */
$name  = trim((string)($_POST['name']  ?? '')) ?: $meta['name'];
$descr = trim((string)($_POST['descr'] ?? '')) ?: $meta['desc'];
$catIn = trim((string)($_POST['cat']   ?? ''));
$verIn = trim((string)($_POST['ver']   ?? '')) ?: $meta['ver'];

/* 分类：下拉里选的合法值优先；否则用头里的，并按 store_build.py 同一张表归一化
 * （老应用写 工具/程序/系统/网络 的很多，不归一化会落进"软件"以外的死分类） */
$cat = in_array($catIn, (array)cfg('cats'), true)
     ? $catIn
     : norm_cat($catIn !== '' ? $catIn : (string)$meta['cat']);

/* 版本号：1.0 / v2 一律取整数段 */
$ver = norm_ver($verIn);

/* 用覆盖后的值再跑一遍校验 */
$merged = $meta;
$merged['name'] = $name; $merged['cat'] = $cat;
$merged['desc'] = $descr; $merged['ver'] = (string)$ver;
[$errs, $warns] = lapp_validate($merged, $src);

if ($errs) {
    flash('提交失败：' . $errs[0], 'err');
    header('Location: index.php'); exit;
}

/* ---------------- 3. 版本 / 重复提交 ---------------- */
$safeName = preg_replace('/\.lua$/i', '', safe_fname($name));   /* 商店里的 name = 文件名主干 */
$existing = store_find($safeName);
if ($existing) {
    $old = (int)($existing['ver'] ?? 0);
    if ($ver <= $old) {
        flash("商店里已经是 v{$old}，新版本号必须大于 {$old}（你填的是 v{$ver}）", 'err');
        header('Location: index.php'); exit;
    }
}
$st = db()->prepare("SELECT id FROM subs WHERE name = ? AND status = 'pending' LIMIT 1");
$st->execute([$safeName]);
if ($st->fetch()) {
    flash('这个应用已经有一份在等审核了，等站主处理完再提交', 'err');
    header('Location: index.php'); exit;
}
if (upload_quota_left((int)$u['id']) <= 0) {
    flash('今天提交次数用完了（每天 ' . (int)$lim['per_day_upload'] . ' 个）', 'err');
    header('Location: index.php'); exit;
}

/* ---------------- 4. 落盘 ---------------- */
$risks = scan_risks($src);
$sha   = hash('sha256', $src);
$crc   = sprintf('%08x', crc32($src));
$store = rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $sha . '.lua';
if (@file_put_contents($store, $src) === false) {
    flash('服务器写入失败，请稍后再试', 'err'); header('Location: index.php'); exit;
}
@chmod($store, 0600);

$fname = safe_fname($name);
db()->prepare('INSERT INTO subs
    (uid, name, label, cat, descr, ver, fname, size, crc, sha, risks, status, created, ip)
    VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)')
  ->execute([(int)$u['id'], $safeName, $name, $cat, $descr, $ver, $fname,
             strlen($src), $crc, $sha, json_encode($risks, JSON_UNESCAPED_UNICODE),
             'pending', time(), client_ip()]);
$id = (int)db()->lastInsertId();

/* ---------------- 5. 邮件通知站主 ---------------- */
$base  = rtrim((string)cfg('base_url'), '/');
$key   = (string)cfg('review_key');
$isUpd = $existing ? ('更新 v' . (int)$existing['ver'] . ' → v' . $ver) : ('新应用 v' . $ver);

$riskHtml = '';
if ($risks) {
    $riskHtml = '<p style="margin:18px 0 6px;font-weight:600">代码能力扫描</p><ul style="margin:0;padding-left:20px">';
    foreach ($risks as $r) {
        $color = $r['level'] === 'high' ? '#c0392b' : ($r['level'] === 'med' ? '#b25000' : '#6e6e73');
        $mark  = $r['level'] === 'high' ? '⛔ ' : ($r['level'] === 'med' ? '⚠ ' : '· ');
        $riskHtml .= '<li style="color:' . $color . ';font-size:14px;margin:4px 0">'
                   . h($mark . $r['api'] . ' — ' . $r['why']) . '</li>';
    }
    $riskHtml .= '</ul>';
} else {
    $riskHtml = '<p style="margin:18px 0 6px;color:#1a7f37;font-size:14px">代码能力扫描：没有命中危险 API</p>';
}
$warnHtml = $warns ? '<ul style="margin:6px 0;padding-left:20px;color:#b25000;font-size:14px">'
                   . implode('', array_map(fn($w) => '<li>' . h($w) . '</li>', $warns)) . '</ul>' : '';

$html = '<div style="font:15px/1.6 -apple-system,\'PingFang SC\',sans-serif;color:#1d1d1f;max-width:620px">'
 . '<h2 style="margin:0 0 4px">' . h($isUpd) . ' 待审核</h2>'
 . '<p style="color:#6e6e73;margin:0 0 16px">' . h($name) . ' · ' . h($cat) . '</p>'
 . '<table cellpadding="6" style="border-collapse:collapse;font-size:14px">'
 . '<tr><td style="color:#6e6e73">应用名</td><td><b>' . h($name) . '</b></td></tr>'
 . '<tr><td style="color:#6e6e73">分类</td><td>' . h($cat) . '</td></tr>'
 . '<tr><td style="color:#6e6e73">说明</td><td>' . h($descr) . '</td></tr>'
 . '<tr><td style="color:#6e6e73">版本</td><td>v' . $ver . ($existing ? '（商店当前 v' . (int)$existing['ver'] . '）' : '') . '</td></tr>'
 . '<tr><td style="color:#6e6e73">大小</td><td>' . number_format(strlen($src)) . ' 字节</td></tr>'
 . '<tr><td style="color:#6e6e73">crc32</td><td><code>' . h($crc) . '</code></td></tr>'
 . '<tr><td style="color:#6e6e73">提交者</td><td>' . h($u['email']) . '</td></tr>'
 . '</table>'
 . $warnHtml . $riskHtml
 . '<p style="margin:24px 0 10px">'
 . '<a href="' . h($base . '/review.php?k=' . $key . '&id=' . $id) . '"'
 . ' style="display:inline-block;background:#0071e3;color:#fff;text-decoration:none;'
 . 'padding:12px 26px;border-radius:10px;font-weight:600">打开审核页</a>'
 . '</p>'
 . '<p style="color:#98989d;font-size:12px">审核页里点「通过」才真正上架（版本写入 index.json，设备端刷新商店即出现「更新」）。'
 . '邮件里故意不放一键通过链接 —— 邮箱安全扫描器会预取链接，可能导致误上架。</p>'
 . '</div>';

[$mok, $mwhy] = smtp_send((string)cfg('admin_email'), '【待审】' . $name . ' v' . $ver, $html);

flash($mok ? '已提交，等站主审核' : ('已提交（但通知邮件没发出去：' . $mwhy . '），站主会在后台看到'));
header('Location: index.php');
