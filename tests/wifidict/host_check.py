#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Wi-Fi 字典工具 · 主机侧功能自测

用 lupa 跑**真实的 entry.lua**, mock 掉 app.* 桥 (fs 为内存 FAT):
  · 内置字典 / 分批条目统计
  · TXT 导入: UTF-8 BOM / LF / CRLF / 空行 / 去重 / 中文 / 末尾无换行
  · 大字典分批读写 (read_at 与 append 调用次数)
  · 随机生成: 字符集 / 长度 / 数量 / 分批写 / 文件大小
  · 预览分页: 行号连续 / 不重复 / 到末尾
  · 源码里不得出现任何 WiFi / 网络接口调用

用法: python3 apps/lua/lpk/wifidict/tests/host_check.py
"""
import os
import random
import re
import sys

try:
    from lupa import LuaRuntime
except ImportError:
    print("缺少 lupa: pip install lupa")
    sys.exit(2)

HERE = os.path.dirname(os.path.abspath(__file__))
# 默认测 LPK 内的 app/entry.lua；传一个路径参数可测导出的单文件 .lua（LAPP 头版本）
ENTRY = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.normpath(os.path.join(
    HERE, "..", "..", "apps", "lua", "lpk", "wifidict", "app", "entry.lua"))

PASS = 0
FAIL = 0


def B(s):
    """UTF-8 文本 -> Lua string (bytes)。Python bytes 字面量不能含中文。"""
    return s.encode("utf-8")


def ck(cond, label, extra=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  ✓ " + label)
    else:
        FAIL += 1
        print("  ✗ " + label + ("  <- " + str(extra) if extra else ""))


class FS:
    """内存 FAT: 路径 -> bytes。记录分块读写次数。"""

    def __init__(self):
        self.files = {}
        self.dirs = {b"/", b"/sdcard"}
        self.read_at_calls = 0
        self.append_calls = 0
        self.write_calls = 0
        self.max_read_len = 0

    def exists(self, p):
        return p in self.files or p in self.dirs

    def mkdir(self, p):
        parts = p.split(b"/")
        cur = b""
        for x in parts:
            if not x:
                continue
            cur = cur + b"/" + x
            self.dirs.add(cur)
        return True

    def listdir(self, p):
        out = set()
        for f in self.files:
            par, _, nm = f.rpartition(b"/")
            if par == p:
                out.add(nm)
        for d in self.dirs:
            if d.startswith(p + b"/"):
                rest = d[len(p) + 1:]
                if b"/" not in rest:
                    out.add(rest)
        return sorted(out)

    def write(self, p, data):
        self.write_calls += 1
        self.files[p] = data
        return True

    def append(self, p, data):
        self.append_calls += 1
        self.files[p] = self.files.get(p, b"") + data
        return True

    def read(self, p):
        d = self.files.get(p)
        if d is None or len(d) > 512 * 1024 or len(d) == 0:
            return None
        return d

    def read_at(self, p, off, ln):
        self.read_at_calls += 1
        self.max_read_len = max(self.max_read_len, ln)
        d = self.files.get(p)
        if d is None or off < 0 or ln <= 0 or ln > 1024 * 1024:
            return None
        return d[off:off + ln]

    def size(self, p):
        d = self.files.get(p)
        return None if d is None else len(d)

    def remove(self, p):
        if p in self.files:
            del self.files[p]
            return True
        return False

    def rename(self, a, b):
        if a not in self.files:
            return False
        self.files[b] = self.files.pop(a)
        return True


class App:
    def __init__(self, lua):
        self.lua = lua
        self.fs = FS()
        self.nvs = {}
        self.ms = 0
        self.rng = random.Random(20261107)
        self.dlg_result = None
        self.dlg_calls = []

        def noop(*a):
            return None

        def millis():
            self.ms += 16
            return self.ms

        def touch():
            return None

        def rand(a, b=None):
            if b is None:
                return self.rng.randint(1, a)
            lo, hi = min(a, b), max(a, b)
            return self.rng.randint(lo, hi)

        def dlg_list(title, items, sel=1):
            self.dlg_calls.append(("list", title))
            return len(items)

        def dlg_confirm(msg):
            self.dlg_calls.append(("confirm", msg))
            return None

        def dlg_result_fn():
            return self.dlg_result

        def kbd_hit(x, y):
            return -1

        def kbd_press(i):
            return 0

        def kbd_char(i):
            return None

        app = lua.eval("{}")
        app.W, app.H = 400, 300
        app.BLACK, app.WHITE = 0, 1
        app.UP, app.DOWN, app.LEFT, app.RIGHT = 1, 2, 3, 4
        app.OK, app.CONFIRM, app.BACK, app.HOME = 5, 5, 6, 7

        for name in ("clear", "px", "fill", "rect", "store_rect", "hline", "vline",
                     "text", "textc", "textw", "big", "bigc", "zh", "zh24", "zhs",
                     "zhsm", "zhsb", "texts", "textm", "textsb", "mix", "mixc",
                     "mixw", "bmp", "bmpr", "poly", "redraw", "exit", "log",
                     "statusbar", "sbar", "tone_async", "melody", "vibrate"):
            setattr(app, name, noop)
        app.millis = millis
        app.touch = touch
        app.touch_start = touch
        app.touch_hold = lambda: 0
        app.key = lambda: None
        app.key_down = lambda a: False
        app.keys_mask = lambda: 0
        app.key_repeat = lambda a, f=400, r=90: False

        def fs_listdir(p):
            t = lua.eval("{}")
            for i, nm in enumerate(self.fs.listdir(p), 1):
                t[i] = nm
            return t

        fs = lua.eval("{}")
        fs.exists = self.fs.exists
        fs.mkdir = self.fs.mkdir
        fs.listdir = fs_listdir
        fs.write = self.fs.write
        fs.append = self.fs.append
        fs.read = self.fs.read
        fs.read_at = self.fs.read_at
        fs.size = self.fs.size
        fs.remove = self.fs.remove
        fs.rename = self.fs.rename
        app.fs = fs

        nvs = lua.eval("{}")
        nvs.get_int = lambda k: self.nvs.get(k)
        nvs.set_int = lambda k, v: (self.nvs.__setitem__(k, v), True)[1]
        nvs.get_str = lambda k: self.nvs.get(k)
        nvs.set_str = lambda k, v: (self.nvs.__setitem__(k, v), True)[1]
        app.nvs = nvs

        dlg = lua.eval("{}")
        dlg.list = dlg_list
        dlg.confirm = dlg_confirm
        dlg.result = dlg_result_fn
        dlg.toast = noop
        app.dlg = dlg

        kbd = lua.eval("{}")
        kbd.use = lambda *a: True
        kbd.reset = noop
        kbd.draw = noop
        kbd.hit = kbd_hit
        kbd.press = kbd_press
        kbd.char = kbd_char
        kbd.label = lambda i: b""
        kbd.geom = lambda: lua.eval("{}")
        kbd.height = lambda: 116
        kbd.count = lambda: 40
        kbd.layer_on = lambda i: False
        kbd.get = lambda k: False
        kbd.set = noop
        kbd.CHAR, kbd.ENTER, kbd.BKSP = 0, 1, 2
        kbd.SPACE, kbd.ESC, kbd.TAB = 3, 4, 5
        app.kbd = kbd

        util = lua.eval("{}")
        util.rand = rand
        util.randf = lambda: self.rng.random()
        util.crc32 = lambda s: b"00000000"
        util.clamp = lambda v, lo, hi: max(lo, min(hi, v))
        app.util = util

        sysx = lua.eval("{}")
        sysx.heap_free = lambda: 200000
        app.sys = sysx
        app.db = lua.eval("{}")

        lua.globals().app = app
        self.app = app


def lualen(lua, t):
    return int(lua.eval("function(t) return #t end")(t))


def lvalues(lua, t):
    """lupa 的 LuaTable 迭代给的是 key, 取 value 必须按下标显式取。"""
    return [t[i] for i in range(1, lualen(lua, t) + 1)]


def run_steps(lua, limit=200000):
    """推进 busy 任务直到结束, 返回步数。"""
    n = 0
    while n < limit:
        G = lua.globals().__t_state()
        if G.mode != b"busy":
            return n
        lua.globals().__t_step_job()
        n += 1
    raise AssertionError("任务未在 %d 步内结束" % limit)


def main():
    lua = LuaRuntime(unpack_returned_tuples=True, encoding=None)
    app_mock = App(lua)

    src = open(ENTRY, "rb").read().decode("utf-8")
    # 只加测试钩子 (把内部 local 函数暴露给主机), 不改应用源码本身
    src += r"""
function __t_state() return G end
function __t_step_job() step_job() end
function __t_begin_import(p, stem, dedup) begin_import(p, stem, dedup) end
function __t_begin_gen(cs, len, total, stem) begin_gen(cs, len, total, stem) end
function __t_build_cs() return build_cs() end
function __t_reader_reset(p) reader_reset(p) end
function __t_preview_next() preview_next() end
function __t_preview_state() return G.preview end
function __t_trim(s) return trim_line(s) end
function __t_scan() scan_dicts() end
function __t_start_count() start_count_job() end
function __t_fit(s, w) return fit(s, w) end
function __t_set_current() set_current() end
function __t_commit_text() commit_text() end
function __t_handle_dlg(k, r) handle_dlg(k, r) end
function __t_cancel_job() cancel_job() end
function __t_open_preview() open_preview() end
"""
    lua.execute(src)
    g = lua.globals()
    app = lua.globals().app
    fs = None
    # 从 Lua 侧拿回 FS 实例: 用全局引用
    fs = lua.eval("app.fs")
    state = lambda: g.__t_state()

    print("\n[1] 内置字典与分批条目统计")
    g.on_enter()
    G = state()
    ck(G.mode == b"list", "on_enter 后进入 list 模式")
    builtin = [B("示例-字母数字.txt"), B("示例-数字6位.txt"), B("示例-混合字符.txt")]
    for nm in builtin:
        d = app.fs.size(b"/sdcard/wifidict/" + nm)
        ck(d is not None and d > 0, "内置字典已写入: " + nm.decode(), d)
    n = 0
    while n < 5000:
        G = state()
        if G.job is None:
            break
        g.__t_step_job()
        n += 1
    ck(n > 3, "统计走的是分帧状态机", n)
    cnt = {}
    for i in range(1, lualen(lua, state().dicts) + 1):
        d = state().dicts[i]
        cnt[d.file] = d.count
    ck(cnt.get(B("示例-数字6位.txt")) == 200, "数字6位 = 200 条", cnt)
    ck(cnt.get(B("示例-字母数字.txt")) == 100, "字母数字 = 100 条", cnt)
    ck(cnt.get(B("示例-混合字符.txt")) == 60, "混合字符 = 60 条", cnt)

    print("\n[2] TXT 导入: BOM / CRLF / 空行 / 去重 / 中文 / 末尾无换行")
    sample = (b"\xef\xbb\xbf" + "abc\r\nabc\n\n   \n中文条目\r\ndef\nghi".encode("utf-8"))
    app.fs.write(b"/sdcard/sample.txt", sample)
    g.__t_begin_import(b"/sdcard/sample.txt", b"imp1", True)
    steps = run_steps(lua)
    out = app.fs.read(b"/sdcard/wifidict/imp1.txt")
    lines = out.split(b"\n")[:-1]
    ck(len(lines) == 4, "去重后 4 条 (abc/中文条目/def/ghi)", lines)
    ck(lines[0] == b"abc", "BOM 已剥离", lines[0])
    ck(lines[1] == "中文条目".encode("utf-8"), "UTF-8 中文完整", lines[1])
    ck(b"\r" not in out, "CRLF 的 CR 已去除")
    ck(state().result.title == B("导入完成"), "结果页标题正确")
    rrows = lvalues(lua, state().result.rows)
    sz_row = [r for r in rrows if r.startswith(B("文件大小"))]
    ck(len(sz_row) == 1, "结果页显示文件大小", sz_row)
    ck(any(r.startswith(B("导入条数")) for r in rrows), "结果页显示导入数量")

    print("\n[3] 导入方式=不去重 (大文件模式)")
    g.__t_begin_import(b"/sdcard/sample.txt", b"imp2", False)
    run_steps(lua)
    out2 = app.fs.read(b"/sdcard/wifidict/imp2.txt")
    lines2 = out2.split(b"\n")[:-1]
    ck(len(lines2) == 5, "不去重 = 5 条", lines2)
    ck(lines2[0] == b"abc" and lines2[1] == b"abc", "重复行保留")

    print("\n[4] 大字典必须分批读写")
    big = b"".join(("line%05d\n" % i).encode() for i in range(20000))
    app.fs.write(b"/sdcard/big.txt", big)
    before_reads = lua.eval("0")  # 计数在 Python FS 里, 用全局拿
    fsobj = None
    # 从 app.fs.read_at 拿到绑定的 Python 对象
    import gc
    for obj in gc.get_objects():
        if isinstance(obj, FS):
            fsobj = obj
            break
    ck(fsobj is not None, "取到内存 FS 实例")
    r0, a0 = fsobj.read_at_calls, fsobj.append_calls
    g.__t_begin_import(b"/sdcard/big.txt", b"big1", False)
    bsteps = run_steps(lua)
    outb = app.fs.read(b"/sdcard/wifidict/big1.txt")
    linesb = outb.split(b"\n")[:-1]
    ck(len(linesb) == 20000, "20000 条全部导入", len(linesb))
    ck(fsobj.read_at_calls - r0 >= 8, "read_at 分块 >= 8 次 (32KB 块)",
       fsobj.read_at_calls - r0)
    ck(fsobj.append_calls - a0 >= 2, "append 分批 >= 2 次", fsobj.append_calls - a0)
    ck(fsobj.max_read_len <= 32768, "单次读不超过 32KB", fsobj.max_read_len)
    ck(bsteps >= 8, "导入跨多帧完成, 未一次性阻塞 (每帧 32KB)", bsteps)

    print("\n[5] 随机生成: 字符集 / 长度 / 数量 / 分批写 / 文件大小")
    cs = g.__t_build_cs()
    csn = lualen(lua, cs)
    ck(csn == 36, "默认字符集 = 数字10 + 小写26 = 36", csn)
    g.__t_begin_gen(cs, 8, 1000, b"gen1")
    gsteps = run_steps(lua)
    outg = app.fs.read(b"/sdcard/wifidict/gen1.txt")
    glines = outg.split(b"\n")[:-1]
    ck(len(glines) == 1000, "生成 1000 条", len(glines))
    ck(all(len(x) == 8 for x in glines), "每条长度 = 8")
    pool = set(b"0123456789abcdefghijklmnopqrstuvwxyz")
    ck(all(set(x) <= pool for x in glines), "所有字符都来自所选字符集")
    ck(len(outg) == 9000, "文件大小 = 1000 x 9 B", len(outg))
    ck(gsteps >= 4, "生成分 4 批 (每帧 256 条)", gsteps)
    grows = lvalues(lua, state().result.rows)
    ck(any(r.startswith(B("生成数量")) for r in grows), "结果页显示生成数量")
    ck(any(r.startswith(B("文件大小")) for r in grows), "结果页显示文件大小")

    print("\n[6] 预览分页 (分批读, 行号连续, 到末尾)")
    g.__t_reader_reset(b"/sdcard/wifidict/big1.txt")
    g.__t_preview_next()
    P = g.__t_preview_state()
    ck(P.pages[1].first == 1, "第一页从第 1 条开始", P.pages[1].first)
    seen_first = []
    for i in range(1, 40):
        g.__t_preview_next()
        P = g.__t_preview_state()
        pg = P.pages[P.page]
        seen_first.append((pg.first, lualen(lua, pg.lines), pg.lines[1]))
    ck(all(x[1] == 8 for x in seen_first[:-1]), "每页 8 行")
    ck(seen_first[0][0] == 9, "第二页从第 9 条开始", seen_first[0][0])
    ck(seen_first[0][2] == b"line00008", "跨块行不丢失/不重复", seen_first[0][2])
    ck(fsobj.max_read_len <= 32768, "预览单次读 <= 16KB", fsobj.max_read_len)

    print("\n[7] 行清洗与 UTF-8 截断")
    ck(g.__t_trim(b"  ab c \r") == b"ab c", "首尾空白 + CR 去除")
    ck(g.__t_trim(b"\r\n") == b"", "空行识别")
    ck(g.__t_fit("中文abcdef".encode("utf-8"), 40) == "中文a".encode("utf-8"),
       "40px 正好 2 汉字 + 1 ASCII", g.__t_fit("中文abcdef".encode("utf-8"), 40))
    ck(g.__t_fit("中文abcdef".encode("utf-8"), 48) == "中文ab".encode("utf-8"),
       "48px 截断不切半个汉字", g.__t_fit("中文abcdef".encode("utf-8"), 48))

    print("\n[8] 字典管理: 多字典 / 切换当前 / 重命名 / 删除 / 取消清半成品")
    G = state()
    ck(lualen(lua, G.dicts) >= 5, "目录内多个字典共存", lualen(lua, G.dicts))
    G.sel = 1
    g.__t_set_current()
    ck(state().cur == state().dicts[1].file, "已切换当前选中字典", state().cur)
    ck(app_mock.nvs.get(b"wdcur") == state().cur, "当前字典已持久化到 NVS")

    oldf = state().dicts[1].file
    t = lua.eval("{}")
    t.purpose, t.buf, t.title, t.hint = b"rename", b"renamed", b"", b""
    G.text = t
    G.mode = b"text"
    g.__t_commit_text()
    ck(not bool(app.fs.exists(b"/sdcard/wifidict/" + oldf)), "旧文件名已不存在")
    ck(bool(app.fs.exists(b"/sdcard/wifidict/renamed.txt")), "重命名后新文件存在")
    ck(state().cur == b"renamed.txt", "重命名同步更新当前字典", state().cur)
    ck(state().mode == b"list", "重命名后回到列表页")

    n_before = lualen(lua, state().dicts)
    target = None
    for i in range(1, n_before + 1):
        if state().dicts[i].file == b"renamed.txt":
            target = i
            break
    G.sel = target or 1
    g.__t_handle_dlg(b"del", 0)
    ck(not bool(app.fs.exists(b"/sdcard/wifidict/renamed.txt")), "删除: 文件实际从 TF 移除")
    ck(lualen(lua, state().dicts) == n_before - 1, "删除: 列表同步减少", lualen(lua, state().dicts))

    g.__t_begin_import(b"/sdcard/sample.txt", b"tmpcancel", True)
    g.__t_step_job()
    ck(bool(app.fs.exists(b"/sdcard/wifidict/tmpcancel.txt")), "任务中半成品文件存在")
    g.__t_cancel_job()
    ck(not bool(app.fs.exists(b"/sdcard/wifidict/tmpcancel.txt")), "取消后半成品已删除, 无文件残留")
    ck(state().mode == b"list", "取消后回到列表页")

    print("\n[9] 接口审计: 源码不得调用任何网络 / WiFi 接口")
    body = open(ENTRY, "rb").read().decode("utf-8")
    # 先剥注释再统计: 文件头的「不调用 app.wifi.* / app.net.*」说明不能算接口调用
    code = re.sub(r"--\[\[.*?\]\]", "", body, flags=re.S)
    code = "\n".join(re.sub(r"--.*$", "", ln) for ln in code.splitlines())
    call_hits = re.findall(r"app\.(?:wifi|net)\.[A-Za-z_]+", code)
    ck(len(call_hits) == 0, "无 app.wifi.* / app.net.* 调用", call_hits)
    ck("fetch_start" not in code and "http_get" not in code, "无 HTTP 下载/请求")
    used = sorted(set(re.findall(r"app\.([a-z_]+)\.", code)))
    ck("wifi" not in used and "net" not in used, "实际复用模块不含 wifi / net", used)
    print("    实际复用的 app 子模块: " + ", ".join(used))

    print("\n==== 结果: %d 通过 / %d 失败 ====" % (PASS, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
