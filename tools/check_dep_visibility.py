#!/usr/bin/env python3
"""check_dep_visibility.py — 依赖可见性守卫 (ESP-IDF 官方规则落地)

依据 ESP-IDF Build System 文档 "When Writing a Component":
    REQUIRES       = 被本组件**公共头文件** #include 的依赖
                     (会传递给下游组件, 下游能透过你的头文件看到它)
    PRIV_REQUIRES  = 只被本组件**源文件/私有头** #include 的依赖
                     (不污染下游 include 路径, 编译更快, 耦合更真实)

写错方向的后果:
    · 该私有的写成 REQUIRES  → 下游白拿一堆用不到的 include 路径, 依赖图虚胖,
      改下层会牵动一堆本不相干的上层重编; 真实耦合被掩盖。
    · 该公开的写成 PRIV      → 下游 include 你的头时找不到类型, 编译失败。

本脚本做传递闭包: 公共头 A 里 include B, B 里又 include C ⇒ B、C 都算"公共可达"。

用法:
    python3 tools/check_dep_visibility.py          # 打印建议
    python3 tools/check_dep_visibility.py --check  # 有 [私→公] 错误时退出码 1
"""
import os
import re
import sys
import glob
from collections import defaultdict, deque

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COMP_ROOT = os.path.join(ROOT, "components")

# ESP-IDF 的 common components: 无需声明即可 include, 不参与可见性分析
IDF_COMMON = {
    "cxx", "newlib", "esp_libc", "freertos", "esp_hw_support", "heap", "log",
    "soc", "hal", "esp_rom", "esp_common", "esp_system", "xtensa", "riscv",
    "driver", "esp_driver_gpio", "esp_driver_uart", "esp_driver_i2c",
    "esp_driver_spi", "esp_driver_gptimer", "esp_driver_ledc", "esp_driver_pcnt",
    "esp_driver_rmt", "esp_driver_sdmmc", "esp_driver_sdspi", "esp_driver_usb_serial_jtag",
    "esp_timer", "nvs_flash", "esp_event", "esp_partition", "spi_flash",
    "esp_psram", "pthread", "vfs", "lwip", "esp_netif", "mbedtls", "app_update",
    "efuse", "bootloader_support", "esp_app_format", "esp_bootloader_format",
}
# 内部组件名 (非 components/ 下, 不建索引)
INTERNAL = {"main"}


def find_components():
    """返回 {组件名: 组件目录}"""
    comps = {}
    for cm in glob.glob(os.path.join(COMP_ROOT, "**", "CMakeLists.txt"), recursive=True):
        cdir = os.path.dirname(cm)
        # 只认 components/<group>/<name> 或 components/<name>
        rel = os.path.relpath(cdir, COMP_ROOT)
        parts = rel.split(os.sep)
        if len(parts) > 2 or "build" in parts:
            continue
        name = parts[-1]
        comps[name] = cdir
    return comps


def parse_register(cdir):
    """解析 idf_component_register 的 REQUIRES / PRIV_REQUIRES / INCLUDE_DIRS"""
    p = os.path.join(cdir, "CMakeLists.txt")
    txt = open(p, encoding="utf-8", errors="replace").read()

    def grab(key):
        # 形如: REQUIRES a b c)  或 REQUIRES\n a b\n)  —— 取到下一个已知关键字或右括号
        m = re.search(r"\b" + key + r"\b(.*?)(?=\bREQUIRES\b|\bPRIV_REQUIRES\b|\bINCLUDE_DIRS\b|"
                      r"\bPRIV_INCLUDE_DIRS\b|\bSRCS\b|\bEMBED_FILES\b|\bLDFRAGMENTS\b|\))",
                      txt, re.S)
        if not m:
            return []
        body = m.group(1).replace("\\\n", " ")
        toks = []
        for t in body.split():
            t = t.strip('"')
            if not t or t.startswith("#"):
                break
            if "${" in t or "$<" in t or "(" in t:
                continue
            toks.append(t)
        return toks

    return {
        "requires": grab("REQUIRES"),
        "priv": grab("PRIV_REQUIRES"),
        "inc": grab("INCLUDE_DIRS") or ["."],
        "priv_inc": grab("PRIV_INCLUDE_DIRS"),
    }


def public_headers(cdir, inc_dirs):
    out = []
    for d in inc_dirs:
        d = d.strip()
        if d.startswith("/"):
            base = d
        else:
            base = os.path.normpath(os.path.join(cdir, d))
        if not os.path.isdir(base):
            continue
        for h in glob.glob(os.path.join(base, "**", "*.h"), recursive=True):
            out.append(h)
    return out


INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def main():
    comps = find_components()
    reg = {name: parse_register(cdir) for name, cdir in comps.items()}

    # 头文件 basename -> 组件名 (仅 public 目录)
    owner = {}
    pub_hdrs = {}
    for name, cdir in comps.items():
        hs = public_headers(cdir, reg[name]["inc"])
        pub_hdrs[name] = hs
        for h in hs:
            owner.setdefault(os.path.basename(h), name)

    # public 头 -> 它 include 的本地头 basename
    edges = defaultdict(set)
    for name in comps:
        for h in pub_hdrs[name]:
            try:
                txt = open(h, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            for inc in INCLUDE_RE.findall(txt):
                b = os.path.basename(inc)
                if b in owner and owner[b] != name:
                    edges[name].add(owner[b])

    # 传递闭包: 从自身出发, BFS 公共可达的组件
    def reachable(name):
        seen, q = set(), deque(edges[name])
        while q:
            c = q.popleft()
            if c in seen:
                continue
            seen.add(c)
            q.extend(edges[c] - seen)
        return seen

    downgrade, upgrade = [], []
    for name in sorted(comps):
        pub_reach = reachable(name)
        req = set(reg[name]["requires"])
        prv = set(reg[name]["priv"])
        for d in sorted(req):
            if d in IDF_COMMON or d in INTERNAL:
                continue
            if d in comps and d not in pub_reach:
                downgrade.append((name, d))
        for d in sorted(prv):
            if d in pub_reach:
                upgrade.append((name, d))

    print("=" * 68)
    print("依赖可见性检查  (REQUIRES = 公共头暴露 / PRIV_REQUIRES = 仅源文件用)")
    print("=" * 68)
    print("\n[私→公] 必须升级: PRIV_REQUIRES 声明了, 但公共头文件 include 了它 (%d 条)" % len(upgrade))
    for n, d in upgrade:
        print("   - %s 的公共头 include 了 %s ⇒ 应改为 REQUIRES" % (n, d))
    print("\n[公→私] 建议降级: REQUIRES 声明了, 但公共头没有暴露 (%d 条)" % len(downgrade))
    for n, d in downgrade:
        print("   - %s REQUIRES %s ⇒ 仅源文件用, 可降 PRIV_REQUIRES" % (n, d))

    if upgrade:
        print("\n❌ 存在 %d 条必须升级的可见性错误" % len(upgrade))
        return 1 if "--check" in sys.argv else 0
    print("\n✅ 没有必须升级的可见性错误; 降级建议 %d 条(可选优化)" % len(downgrade))
    return 0


if __name__ == "__main__":
    sys.exit(main())
