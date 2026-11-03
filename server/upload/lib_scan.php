<?php
/**
 * lib_scan.php — Lua 源码词法级风险扫描
 *
 * 为什么重写 scan_risks()：原来用 strpos 做整串匹配，三个明显缺陷：
 *   ① 注释里的 `-- app.fs.remove` 会被当成真实调用（误报）；
 *   ② 字符串 `"app.fs.remove"` 一样误报；
 *   ③ 反过来，`app.fs . remove`、`app["fs"]["write"]` 这种写法直接漏报。
 *
 * 这里的做法：先把源码切成 token（正确跳过注释与各种字符串字面量），
 * 再在 token 流上匹配 `标识符 (. 标识符)*` 序列，所以：
 *   - 注释/字符串里的名字不再误报（但要单独标记"字符串里出现危险 API 名"= 可能在拼名字）
 *   - `app . fs . write` 与 `app.fs.write` 等价命中
 *   - `app["fs"]["write"]` 命中"动态下标访问 app"
 *
 * ⚠ 这不是沙箱，也**不能**保证识别所有恶意代码。运行时隔离（固件 lua_sandbox）
 *   才是真正的边界；本扫描只用来决定"要不要人工重点看"，以及给管理员提供依据。
 */
declare(strict_types=1);

/** 把 Lua 源码切成 token: [ ['t'=>'name'|'str'|'op'|'num', 'v'=>.., 'line'=>n], ... ] */
function lua_tokenize(string $src): array
{
    $toks = [];
    $n = strlen($src);
    $i = 0;
    $line = 1;

    while ($i < $n) {
        $c = $src[$i];

        /* 换行 */
        if ($c === "\n") { $line++; $i++; continue; }
        if ($c === "\r" || $c === " " || $c === "\t" || $c === "\f" || $c === "\v") { $i++; continue; }

        /* 注释: -- 行注释 / --[=*[ 块注释 */
        if ($c === '-' && $i + 1 < $n && $src[$i + 1] === '-') {
            $j = $i + 2;
            $level = 0;
            if ($j < $n && $src[$j] === '[') {
                $k = $j + 1;
                while ($k < $n && $src[$k] === '=') { $level++; $k++; }
                if ($k < $n && $src[$k] === '[') {
                    /* 块注释 */
                    $close = ']' . str_repeat('=', $level) . ']';
                    $end = strpos($src, $close, $k + 1);
                    if ($end === false) { $line += substr_count($src, "\n", $i); $i = $n; continue; }
                    $line += substr_count(substr($src, $i, $end - $i), "\n");
                    $i = $end + strlen($close);
                    continue;
                }
            }
            /* 行注释: 到行尾 */
            $end = strpos($src, "\n", $i);
            $i = ($end === false) ? $n : $end;
            continue;
        }

        /* 长字符串 [=*[ ... ]=*] */
        if ($c === '[') {
            $k = $i + 1;
            $level = 0;
            while ($k < $n && $src[$k] === '=') { $level++; $k++; }
            if ($k < $n && $src[$k] === '[') {
                $close = ']' . str_repeat('=', $level) . ']';
                $end = strpos($src, $close, $k + 1);
                $body = ($end === false) ? substr($src, $k + 1) : substr($src, $k + 1, $end - $k - 1);
                $toks[] = ['t' => 'str', 'v' => $body, 'line' => $line];
                $line += substr_count($body, "\n");
                $i = ($end === false) ? $n : $end + strlen($close);
                continue;
            }
        }

        /* 短字符串 '..' / ".." */
        if ($c === '"' || $c === "'") {
            $q = $c;
            $j = $i + 1;
            $buf = '';
            $startline = $line;
            while ($j < $n) {
                $d = $src[$j];
                if ($d === '\\' && $j + 1 < $n) {
                    $e = $src[$j + 1];
                    /* 常见转义还原, 便于发现 "\97pp" 之类的拼接 */
                    if ($e === 'n') $buf .= "\n";
                    elseif ($e === 't') $buf .= "\t";
                    elseif ($e === 'r') $buf .= "\r";
                    elseif ($e === '\\') $buf .= '\\';
                    elseif ($e === '"') $buf .= '"';
                    elseif ($e === "'") $buf .= "'";
                    elseif ($e === "\n") { $buf .= "\n"; $line++; }
                    elseif (ctype_digit($e)) {
                        $num = '';
                        $k = $j + 1;
                        while ($k < $n && ctype_digit($src[$k]) && strlen($num) < 3) { $num .= $src[$k]; $k++; }
                        $val = (int)$num;
                        if ($val > 0 && $val < 256) $buf .= chr($val);
                        $j = $k;
                        continue;
                    } else $buf .= $e;
                    $j += 2;
                    continue;
                }
                if ($d === $q) { $j++; break; }
                if ($d === "\n") $line++;
                $buf .= $d;
                $j++;
            }
            $toks[] = ['t' => 'str', 'v' => $buf, 'line' => $startline];
            $i = $j;
            continue;
        }

        /* 数字 */
        if (ctype_digit($c) || ($c === '.' && $i + 1 < $n && ctype_digit($src[$i + 1]))) {
            $j = $i;
            while ($j < $n && (ctype_alnum($src[$j]) || $src[$j] === '.' || $src[$j] === '_')) $j++;
            $toks[] = ['t' => 'num', 'v' => substr($src, $i, $j - $i), 'line' => $line];
            $i = $j;
            continue;
        }

        /* 标识符 / 关键字 */
        if (ctype_alpha($c) || $c === '_') {
            $j = $i;
            while ($j < $n && (ctype_alnum($src[$j]) || $src[$j] === '_')) $j++;
            $toks[] = ['t' => 'name', 'v' => substr($src, $i, $j - $i), 'line' => $line];
            $i = $j;
            continue;
        }

        /* 其它符号(含 .. 拼接运算符) */
        if ($c === '.' && $i + 1 < $n && $src[$i + 1] === '.') {
            $toks[] = ['t' => 'op', 'v' => '..', 'line' => $line];
            $i += 2;
            continue;
        }
        $toks[] = ['t' => 'op', 'v' => $c, 'line' => $line];
        $i++;
    }
    return $toks;
}

/** 把 token 流还原成 "点分路径" 列表: app.fs.write / app["fs"] 记为 app[] */
function lua_paths(array $toks): array
{
    $out = [];
    $n = count($toks);
    for ($i = 0; $i < $n; $i++) {
        if ($toks[$i]['t'] !== 'name') continue;
        $parts = [$toks[$i]['v']];
        $dyn = false;
        $j = $i + 1;
        while ($j + 1 < $n && $toks[$j]['t'] === 'op' && $toks[$j]['v'] === '.'
               && $toks[$j + 1]['t'] === 'name') {
            $parts[] = $toks[$j + 1]['v'];
            $j += 2;
        }
        if ($j < $n && $toks[$j]['t'] === 'op' && $toks[$j]['v'] === '[') $dyn = true;
        $out[] = ['p' => implode('.', $parts), 'line' => $toks[$i]['line'], 'dyn' => $dyn];
    }
    return $out;
}

/**
 * 扫描结果: [ ['level'=>'high|med|low','code'=>..,'line'=>n,'why'=>..], ... ]
 * level 语义: high = 必须人工确认才可通过; med = 需管理员确认; low = 仅提示
 */
function scan_lua(string $src): array
{
    $toks  = lua_tokenize($src);
    $paths = lua_paths($toks);
    $hits  = [];        /* key = code, 值 = 首次行号 */

    $rules = [
        /* ---- 高风险: 直接触碰设备/他人数据 ---- */
        'app.hid.start'       => ['high', '可模拟 USB 键盘鼠标(BadUSB 风险)'],
        'app.hid.key'         => ['high', '可向电脑发送按键'],
        'app.hid.key_str'     => ['high', '可向电脑发送字符串'],
        'app.hid.shortcut'    => ['high', '可发送快捷键组合'],
        'app.hid.mouse_move'  => ['high', '可控制鼠标'],
        'app.hid.mouse_click' => ['high', '可控制鼠标点击'],
        'app.hid.cdc_read'    => ['med',  '可读取 USB 串口数据'],
        'io.popen'            => ['high', '尝试调用 shell 管道'],
        'os.execute'          => ['high', '尝试执行系统命令'],
        'osm.execute'         => ['high', '尝试执行系统命令'],
        'package.loadlib'     => ['high', '尝试加载动态库'],
        'app.fs.remove'       => ['high', '能删除设备上的文件(沙箱内已限自身数据目录)'],
        'os.remove'           => ['med',  '标准库删文件(沙箱内已限自身数据目录)'],
        'app.fs.rename'       => ['med',  '能改名/移动文件'],
        'os.rename'           => ['med',  '标准库改名'],
        'app.fs.write'        => ['med',  '能写入设备文件'],
        'app.fs.append'       => ['med',  '能追加写设备文件'],
        'io.open'             => ['med',  '标准库直接开关文件'],
        'app.wiki.open'       => ['med',  '可指定目录读离线维基库(文件读取面)'],
        'app.rec.start'       => ['med',  '会启用麦克风录音(隐私)'],
        'app.net.http_get'    => ['med',  '会联网获取数据'],
        'app.net.fetch_start' => ['med',  '会后台联网下载'],
        'app.net.port'        => ['med',  '能做端口探测'],
        'app.espnow.start'    => ['med',  '使用 ESP-NOW 无线组网'],
        'app.wifi.connect'    => ['low',  '会主动连 WiFi'],
    ];

    foreach ($paths as $it) {
        $code = $it['p'];
        if (isset($rules[$code]) && !isset($hits[$code])) {
            [$lvl, $why] = $rules[$code];
            $hits[$code] = ['level' => $lvl, 'code' => $code, 'line' => $it['line'], 'why' => $why];
        }
        /* 动态下标: app["fs"]["write"] —— 名字不在源码里, 只能标出来让人看 */
        if ($code === 'app' && $it['dyn'] && !isset($hits['app[]'])) {
            $hits['app[]'] = ['level' => 'med', 'code' => 'app[下标]', 'line' => $it['line'],
                              'why' => '用动态下标访问 app 表, 实际调用的 API 无法静态确定, 需人工阅读'];
        }
    }

    /* 死循环: 这是"关键字对关键字", 不在点分路径体系里, 必须单独看 token 序列。
     * （原来把它写进 $rules 里, 那条规则永远匹配不到 —— 因为规则匹配的是 app.xxx 形式。）*/
    for ($i = 0; $i + 1 < count($toks); $i++) {
        if ($toks[$i]['t'] === 'name' && $toks[$i]['v'] === 'while'
            && $toks[$i + 1]['t'] === 'name' && $toks[$i + 1]['v'] === 'true') {
            $hits['while_true'] = ['level' => 'low', 'code' => 'while true', 'line' => $toks[$i]['line'],
                                   'why' => '出现死循环写法（游戏主循环常见; 固件有 10 秒超时兜底, 但请确认不是每帧阻塞）'];
            break;
        }
    }

    /* 动态代码: load/loadstring 的实参不是字面量 */
    $n = count($toks);
    for ($i = 0; $i < $n; $i++) {
        if ($toks[$i]['t'] !== 'name') continue;
        $fn = $toks[$i]['v'];
        if ($fn !== 'load' && $fn !== 'loadstring' && $fn !== 'dofile' && $fn !== 'loadfile') continue;
        if ($i + 1 >= $n || $toks[$i + 1]['t'] !== 'op' || $toks[$i + 1]['v'] !== '(') continue;
        $arg = $toks[$i + 2] ?? null;
        $literal = $arg && ($arg['t'] === 'str' || $arg['t'] === 'name' && $arg['v'] === 'function');
        if (!$literal && !isset($hits['dyn_load'])) {
            $hits['dyn_load'] = ['level' => 'high', 'code' => $fn . '(...动态参数)',
                                 'line' => $toks[$i]['line'],
                                 'why' => '运行时拼出来的代码, 静态扫描无法分析, 必须人工审'];
        }
    }

    /* 字符串里出现危险 API 名: 可能在拼名字绕过扫描 */
    foreach ($toks as $t) {
        if ($t['t'] !== 'str') continue;
        if (preg_match('/(app\.[a-z]+\.[a-z_]+|os\.execute|io\.popen|loadstring)/i', $t['v'], $mm)
            && !isset($hits['str_api'])) {
            $hits['str_api'] = ['level' => 'med', 'code' => '字符串含 API 名',
                                'line' => $t['line'],
                                'why' => '字符串里出现 "' . $mm[1] . '"，可能用于动态拼接口名'];
        }
    }

    /* 硬编码 IP:端口 */
    if (preg_match('/\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}:\d{2,5}\b/', $src, $mm)) {
        $hits['hard_ip'] = ['level' => 'med', 'code' => '硬编码地址', 'line' => 0,
                            'why' => '直连 ' . $mm[0]];
    }

    return array_values($hits);
}

/** 旧版字符串匹配, **只保留作对照**, 新代码一律用 scan_lua() */
function scan_risks_legacy(string $src): array
{
    static $rules = [
        ['app.hid.start', 'high'], ['app.hid.shortcut', 'high'], ['app.hid.key', 'high'],
        ['app.fs.remove', 'high'], ['app.fs.write', 'med'], ['app.net.http_get', 'med'],
    ];
    $out = [];
    foreach ($rules as [$api, $lvl]) {
        if (strpos($src, $api) !== false) $out[] = ['level' => $lvl, 'code' => $api, 'line' => 0, 'why' => '字符串匹配'];
    }
    return $out;
}

/** 汇总结论: high / med / low / none */
function scan_verdict(array $hits): string
{
    $has = ['high' => 0, 'med' => 0, 'low' => 0];
    foreach ($hits as $h) $has[$h['level']] = ($has[$h['level']] ?? 0) + 1;
    if ($has['high']) return 'high';
    if ($has['med'])  return 'med';
    if ($has['low'])  return 'low';
    return 'none';
}

function scan_summary(array $hits): string
{
    $c = ['high' => 0, 'med' => 0, 'low' => 0];
    foreach ($hits as $h) $c[$h['level']]++;
    if ($c['high']) return "高 {$c['high']} · 中 {$c['med']} · 低 {$c['low']}";
    if ($c['med'])  return "中 {$c['med']} · 低 {$c['low']}";
    if ($c['low'])  return "低 {$c['low']}";
    return '无';
}
