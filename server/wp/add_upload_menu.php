<?php
/**
 * 导航栏追加「Lua 上传」——幂等脚本，可反复执行。
 *
 * 用法（在生产站点根目录执行，需 WP 环境）：
 *   /www/server/php/82/bin/php server/wp/add_upload_menu.php          # 预览将要做什么
 *   /www/server/php/82/bin/php server/wp/add_upload_menu.php --apply  # 真正写入
 *
 * 行为：
 *   1. 找到主菜单（默认 term_id=12「菜单」，可用 --menu=N 指定）；
 *   2. 若已存在指向 /upload/ 的自定义链接项 → 只校正位置与标题，不重复添加；
 *   3. 否则新增一项（type=custom，url=上传地址），插到「LinTOS」之后；
 *   4. 打印最终菜单顺序，便于核对。
 *
 * 不改主题、不改其它菜单项、不新建菜单。
 */
if (PHP_SAPI !== 'cli') { exit("CLI only\n"); }

$apply  = in_array('--apply', $argv, true);
$menuId = 12;
foreach ($argv as $a) {
    if (strpos($a, '--menu=') === 0) { $menuId = (int) substr($a, 7); }
}

require '/www/wwwroot/bbk.linit.cn/wp-load.php';

$UPLOAD_URL   = 'https://bbk.linit.cn/upload/';
$UPLOAD_TITLE = 'Lua 上传';
$AFTER_TITLE  = 'LinTOS';

$menu = wp_get_nav_menu_object($menuId);
if (!$menu) { exit("✗ 找不到菜单 term_id=$menuId\n"); }
echo "菜单：#{$menu->term_id} {$menu->name}\n";

$items = wp_get_nav_menu_items($menuId);
if (!$items) { $items = array(); }

$existing = null;
$afterId  = 0;
foreach ($items as $it) {
    if (rtrim($it->url, '/') === rtrim($UPLOAD_URL, '/')) { $existing = $it; }
    if ($it->title === $AFTER_TITLE) { $afterId = (int) $it->ID; }
}

if ($existing) {
    echo "已存在「{$existing->title}」→ {$existing->url} (ID {$existing->ID})，不重复添加\n";
    $itemId = (int) $existing->ID;
} else {
    echo $apply ? "将新增「{$UPLOAD_TITLE}」→ {$UPLOAD_URL}\n"
                : "[预览] 将新增「{$UPLOAD_TITLE}」→ {$UPLOAD_URL}\n";
    if ($apply) {
        $itemId = wp_update_nav_menu_item($menuId, 0, array(
            'menu-item-title'     => $UPLOAD_TITLE,
            'menu-item-url'       => $UPLOAD_URL,
            'menu-item-type'      => 'custom',
            'menu-item-status'    => 'publish',
            'menu-item-position'  => 99,
            'menu-item-attr-title'=> '提交 Lua 应用到应用商店',
        ));
        if (is_wp_error($itemId)) { exit('✗ 添加失败: ' . $itemId->get_error_message() . "\n"); }
        echo "✓ 已新增，item ID = {$itemId}\n";
    }
}

/* 重新排序：把上传项紧跟在 LinTOS 之后 */
if ($apply && $itemId) {
    $order = array();
    foreach (wp_get_nav_menu_items($menuId) as $it) {
        if ((int) $it->ID === (int) $itemId) { continue; }
        $order[] = (int) $it->ID;
    }
    $pos = array_search($afterId, $order, true);
    if ($pos === false) { $pos = count($order) - 1; }
    array_splice($order, $pos + 1, 0, array((int) $itemId));

    $n = 1;
    foreach ($order as $id) {
        wp_update_post(array('ID' => $id, 'menu_order' => $n++));
    }
    echo "✓ 已重排位置\n";
}

echo "\n最终菜单顺序：\n";
foreach (wp_get_nav_menu_items($menuId) as $it) {
    printf("  %d. %-12s %s\n", (int) $it->menu_order, $it->title, $it->url);
}
if (!$apply) { echo "\n（预览模式，未写入。加 --apply 生效）\n"; }
