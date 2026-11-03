<?php
/**
 * download.php — 管理员下载原始投稿包（人工测试用）
 *
 * 安全点:
 *   - 必须管理员登录（会话），不是靠 URL 令牌
 *   - 只按 submissions.id 从数据库取 sha，再由 sha 组路径；
 *     不接受任何来自请求的文件名/路径 ⇒ 不存在目录穿越面
 *   - 不作为可执行内容返回（Content-Disposition: attachment + text/plain）
 */
declare(strict_types=1);
require __DIR__ . '/lib.php';

admin_require();

$id = (int)($_GET['id'] ?? 0);
if ($id <= 0) { http_response_code(400); echo '缺少 id'; exit; }

$st = db()->prepare('SELECT id, name, fname, sha, size FROM subs WHERE id = ?');
$st->execute([$id]);
$s = $st->fetch();
if (!$s) { http_response_code(404); echo '没有这条投稿'; exit; }

$sha = (string)$s['sha'];
if (!preg_match('/^[0-9a-f]{64}$/', $sha)) { http_response_code(500); echo '记录里的 sha 不合法'; exit; }

$path = rtrim((string)cfg('data_dir'), '/') . '/pkgs/' . $sha . '.lua';
if (!is_file($path)) { http_response_code(404); echo '包文件不存在'; exit; }

/* 下载名只取数据库里已净化的 fname, 再去一遍路径分隔符 */
$dl = preg_replace('/[\/\\\\\x00-\x1f]/', '', (string)$s['fname']);
if ($dl === '' || $dl === null) $dl = 'app.lua';

audit((string)(admin_current()['user'] ?? '?'), 'sub.download', (string)$s['name'], null, null, null, 'ok', '下载 id=' . $id);

header('Content-Type: text/plain; charset=utf-8');
header('Content-Length: ' . filesize($path));
header("Content-Disposition: attachment; filename*=UTF-8''" . rawurlencode($dl));
header('X-Content-Type-Options: nosniff');
readfile($path);
