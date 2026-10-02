#!/usr/bin/env python3
"""new_app.py — LinTOS 新应用一键脚手架 (V1.2)

在 L4 apps 组件里创建一个可编译运行的应用页, 并自动接线 5 处:
  ① components/apps/pages/page_<id>.c            页面实现 (由模板生成)
  ② components/apps/templates/page_app_template.c  模板来源 (只读)
  ③ components/apps/CMakeLists.txt                SRCS 追加
  ④ components/apps/include/apps_pages.h          注册入口声明
  ⑤ components/apps/apps_registry.c               装配调用 + 内置应用表条目
  ⑥ components/system/os/include/os.h             os_page_t 枚举值

用法:
  python3 tools/new_app.py snakecase --label "\u8d2a\u5403\u86c7" --icon ICON_MINI_GAMES --cat "\u7a0b\u5e8f"
  python3 tools/new_app.py mytool --label "\u6211\u7684\u5de5\u5177" --fullscreen        # 全屏(隐藏状态栏)
  python3 tools/new_app.py mytool --label "\u6211\u7684\u5de5\u5177" --hidden           # 只进应用中心, 不上主菜单

参数:
  id          小写蛇形英文 id (如 snake / my_tool), 同时用作文件名/函数名/OS_PAGE_ 枚举
  --label     显示名 (中文/英文均可)
  --cat       应用中心子分类: \u7a0b\u5e8f / \u6e38\u620f / \u7cfb\u7edf / \u8fd0\u7ef4 / \u7f51\u7edc / \u5b66\u4e60 (默认 \u7a0b\u5e8f)
  --icon      ICON_* 枚举名 (默认 ICON_MINI_GAMES; 可选值见 icons_main_enum.h)
  --fullscreen  全屏应用 (隐藏系统状态栏)
  --hidden    默认不上主菜单 (只出现在应用中心)

生成后: idf.py build 即可看到 `page_<id>.c` 参与编译并出现在应用中心。
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APPS = os.path.join(ROOT, "components", "apps")
TPL = os.path.join(APPS, "templates", "page_app_template.c")
REG = os.path.join(APPS, "apps_registry.c")
PAGES_H = os.path.join(APPS, "include", "apps_pages.h")
APP_CMK = os.path.join(APPS, "CMakeLists.txt")
OS_H = os.path.join(ROOT, "components", "system", "os", "include", "os.h")


def fail(msg):
    print("✗ " + msg)
    sys.exit(1)


def read(p):
    return open(p, encoding="utf-8").read()


def write(p, s):
    open(p, "w", encoding="utf-8").write(s)


def remove_app(app_id, up_id):
    """与创建对称: 撤销 5 处接线 + 删除页面文件"""
    n = 0
    page_file = os.path.join(APPS, "pages", "page_%s.c" % app_id)
    if os.path.exists(page_file):
        os.remove(page_file)
        print("  ✓ 删除页面:  components/apps/pages/page_%s.c" % app_id)
        n += 1

    cmk = read(APP_CMK)
    cmk2 = re.sub(r'\n *"pages/page_%s\.c",?' % re.escape(app_id), "", cmk)
    if cmk2 != cmk:
        write(APP_CMK, cmk2); print("  ✓ 移除编译入口"); n += 1

    ph = read(PAGES_H)
    ph2 = re.sub(r'^void os_page_%s_register\(void\);.*\n' % re.escape(app_id), "", ph, flags=re.M)
    if ph2 != ph:
        write(PAGES_H, ph2); print("  ✓ 移除入口声明"); n += 1

    osh = read(OS_H)
    osh2 = re.sub(r'^ *OS_PAGE_%s,.*\n' % re.escape(up_id), "", osh, flags=re.M)
    if osh2 != osh:
        write(OS_H, osh2); print("  ✓ 移除页面枚举"); n += 1

    reg = read(REG)
    reg2 = re.sub(r'^ *os_page_%s_register\(\);.*\n' % re.escape(app_id), "", reg, flags=re.M)
    # 应用表条目: 该行含 .id="<id>"
    reg2 = re.sub(r'^ *\{ \.id="%s",.*\n' % re.escape(app_id), "", reg2, flags=re.M)
    if reg2 != reg:
        write(REG, reg2); print("  ✓ 移除装配调用 + 表条目"); n += 1

    if n == 0:
        fail("未找到 '%s' 的任何痕迹 (id 是否正确?)" % app_id)
    print("\n✅ 应用 '%s' 已移除 (%d 处接线已撤销)。记得 idf.py build 复核。" % (app_id, n))


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("id", help="小写蛇形 id")
    ap.add_argument("--label", help="显示名")
    ap.add_argument("--cat", default="\u7a0b\u5e8f", help="应用中心子分类")
    ap.add_argument("--icon", default="ICON_MINI_GAMES", help="ICON_* 枚举名")
    ap.add_argument("--fullscreen", action="store_true")
    ap.add_argument("--hidden", action="store_true")
    ap.add_argument("--remove", action="store_true",
                    help="反向操作: 删除该应用并撤销全部接线 (与创建对称)")
    a = ap.parse_args()

    app_id = a.id.lower()
    if not re.fullmatch(r"[a-z][a-z0-9_]{1,30}", app_id):
        fail("id 必须是小写字母开头的英文/数字/下划线 (2~31 字符)")
    up_id = app_id.upper()

    if a.remove:
        remove_app(app_id, up_id)
        return

    if not a.label:
        fail("创建应用必须给 --label (显示名)")

    page_file = os.path.join(APPS, "pages", "page_%s.c" % app_id)
    if os.path.exists(page_file):
        fail("已存在: %s" % page_file)

    # ---------- ① 页面实现 ----------
    tpl = read(TPL)
    body = (tpl.replace("@@ID@@", app_id)
               .replace("@@UPID@@", up_id)
               .replace("@@LABEL@@", a.label)
               .replace("@@TAG@@", app_id.upper()[:16]))
    write(page_file, body)
    print("  ✓ 页面实现:  components/apps/pages/page_%s.c" % app_id)

    # ---------- ② CMakeLists SRCS ----------
    cmk = read(APP_CMK)
    if '"pages/page_%s.c"' % app_id not in cmk:
        last = None
        for m in re.finditer(r'^ *"pages/page_\w+\.c"', cmk, re.M):
            last = m
        if not last:
            fail("CMakeLists 里找不到 pages 源文件锚点")
        cmk = cmk[:last.end()] + '\n        "pages/page_%s.c"' % app_id + cmk[last.end():]
        write(APP_CMK, cmk)
        print("  ✓ 编译入口:  components/apps/CMakeLists.txt")

    # ---------- ③ 注册入口声明 ----------
    ph = read(PAGES_H)
    decl = "void os_page_%s_register(void);" % app_id
    if decl not in ph:
        ph = ph.replace("#endif /* APPS_PAGES_H */",
                        "%-44s /* %s */\n\n#endif /* APPS_PAGES_H */" % (decl, a.label))
        write(PAGES_H, ph)
        print("  ✓ 入口声明:  components/apps/include/apps_pages.h")

    # ---------- ④ os_page_t 枚举 ----------
    osh = read(OS_H)
    if "OS_PAGE_%s," % up_id not in osh:
        osh = osh.replace("    OS_PAGE_COUNT",
                          "    OS_PAGE_%s,%s/* %s (new_app.py 生成) */\n    OS_PAGE_COUNT"
                          % (up_id, " " * max(1, 22 - len(up_id)), a.label), 1)
        write(OS_H, osh)
        print("  ✓ 页面枚举:  components/system/os/include/os.h (OS_PAGE_%s)" % up_id)

    # ---------- ⑤ 装配调用 + 应用表条目 ----------
    reg = read(REG)
    call = "    os_page_%s_register();" % app_id
    if call not in reg:
        m = list(re.finditer(r'^ *os_page_\w+_register\(\);.*$', reg, re.M))
        if not m:
            fail("apps_registry.c 里找不到装配调用锚点")
        last = m[-1]
        reg = reg[:last.end()] + "\n%-42s /* %s */" % (call, a.label) + reg[last.end():]
    entry = ('    { .id="%s", .label="%s", .icon_idx=%s, .page=OS_PAGE_%s, '
             '.hidden=%s, .cat="%s" }, /* %s (new_app.py) */\n'
             % (app_id, a.label, a.icon, up_id,
                "true" if a.hidden else "false", a.cat, a.label))
    if '"%s"' % app_id not in reg.split("#define BUILTIN_APP_COUNT")[0].split("s_builtin_apps[] = {")[-1]:
        anchor = "#define BUILTIN_APP_COUNT"
        i = reg.index(anchor)
        # 插到表尾 `};` 之前
        j = reg.rindex("};", 0, i)
        reg = reg[:j] + entry + reg[j:]
    write(REG, reg)
    print("  ✓ 装配+表条目: components/apps/apps_registry.c")

    # ---------- 提示 ----------
    print("\n✅ 应用 '%s' (%s) 已创建" % (a.label, app_id))
    print("   下一步:")
    print("     1) 编辑 components/apps/pages/page_%s.c 填实现" % app_id)
    print("     2) python3 tools/check_deps.py     # 确认没越层 (只许 include os_app_api.h)")
    print("     3) idf.py build")
    print("   备注: 图标 %s —— 若要新图标, 先按 docs/App制作规则.md §2.3 生成再改 icon_idx" % a.icon)


if __name__ == "__main__":
    main()
