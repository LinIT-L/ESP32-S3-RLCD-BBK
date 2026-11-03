<?php
/**
 * /bbk/maps 的受控下载端点（路径式签名，兼容老的 map.lua）
 *
 * URL 形态： /bbk/dl.php/<过期时间戳>/<签名>/<文件名>
 *   · 设备端拼法是 mirror .. "/" .. path，所以 path 里必须保留最后一段是真实文件名
 *     （map.lua:1451 用 ([^/]+)$ 取落盘名），否则文件会被存成 "dl.php?f=..." 这种废名字。
 *   · 因此用 PATH_INFO 而不是 query string。
 *
 * 校验：过期时间 + HMAC-SHA256(过期|文件名) 前 32 位；文件名必须在 maps/ 内且真实存在。
 * 出流：X-Accel-Redirect 交给 nginx 的 internal location —— PHP 不读 20MB 文件，
 *       Range/断点续传、Content-Length、sendfile 全部由 nginx 负责。
 * 失败：一律纯文本短码（bad_request / expired / forbidden / not_found / too_many），
 *       不带任何服务器路径、不带文件名回显。
 */
declare(strict_types=1);

const DL_SECRET_FILE = '/www/wwwroot/.bbk_secret';
const DL_MAPS_DIR    = __DIR__ . '/maps/';
const DL_RATE_MAX    = 60;     // 每 IP 每 10 分钟
const DL_RATE_WINDOW = 600;

function dl_out(int $code, string $msg): void {
    http_response_code($code);
    header('Content-Type: text/plain; charset=utf-8');
    header('Cache-Control: no-store');
    echo $msg;
    exit;
}

/* ---- 频率限制（按 IP，写 /tmp；签名已能防枚举，这层只挡暴力重放）---- */
function dl_rate_ok(): bool {
    $ip = $_SERVER['REMOTE_ADDR'] ?? '0';
    $dir = sys_get_temp_dir() . '/bbk_dl_rl';
    if (!is_dir($dir)) { @mkdir($dir, 0700, true); }
    $fp = @fopen($dir . '/' . md5($ip), 'c+');
    if (!$fp) { return true; }            // 拿不到锁不挡用户
    flock($fp, LOCK_EX);
    $raw = stream_get_contents($fp);
    $now = time();
    [$start, $n] = [0, 0];
    if ($raw && strpos($raw, ':') !== false) {
        [$s, $c] = explode(':', $raw, 2);
        $start = (int) $s; $n = (int) $c;
    }
    if ($now - $start > DL_RATE_WINDOW) { $start = $now; $n = 0; }
    $n++;
    ftruncate($fp, 0); rewind($fp);
    fwrite($fp, $start . ':' . $n);
    flock($fp, LOCK_UN);
    fclose($fp);
    return $n <= DL_RATE_MAX;
}

/* ---- 取路径参数 ---- */
$uri = $_SERVER['REQUEST_URI'] ?? '';
$q   = strpos($uri, '?');
if ($q !== false) { $uri = substr($uri, 0, $q); }
$prefix = '/bbk/dl.php/';
$pos = strpos($uri, $prefix);
if ($pos === false) { dl_out(400, 'bad_request'); }
$rest = substr($uri, $pos + strlen($prefix));
$parts = array_values(array_filter(explode('/', $rest), static function ($s) { return $s !== ''; }));
if (count($parts) !== 3) { dl_out(400, 'bad_request'); }

[$exp_s, $sig, $name] = $parts;
if (!ctype_digit($exp_s) || !preg_match('/^[0-9a-f]{32}$/', $sig)) { dl_out(400, 'bad_request'); }
$exp = (int) $exp_s;
$name = rawurldecode($name);

if ($exp < time()) { dl_out(410, 'expired'); }

$secret = @file_get_contents(DL_SECRET_FILE);
if ($secret === false || strlen(trim($secret)) < 16) { dl_out(500, 'not_ready'); }
$secret = trim($secret);

$want = substr(hash_hmac('sha256', $exp_s . '|' . $name, $secret), 0, 32);
if (!hash_equals($want, $sig)) { dl_out(403, 'forbidden'); }

/* 文件名白名单：必须在 maps/ 内、无子目录、真实存在 */
if ($name === '' || $name[0] === '.' || strpos($name, '/') !== false || strpos($name, '\\') !== false) {
    dl_out(400, 'bad_request');
}
$full = realpath(DL_MAPS_DIR . $name);
$base = realpath(DL_MAPS_DIR);
if ($full === false || $base === false || strpos($full, $base . DIRECTORY_SEPARATOR) !== 0 || !is_file($full)) {
    dl_out(404, 'not_found');
}

if (!dl_rate_ok()) { dl_out(429, 'too_many'); }

/* 交给 nginx：internal location = /bbk/maps/ */
header('X-Accel-Redirect: /bbk/maps/' . rawurlencode($name));
header('Content-Type: application/octet-stream');
header('Cache-Control: private, max-age=0, no-store');
header('X-Content-Type-Options: nosniff');
echo '';
