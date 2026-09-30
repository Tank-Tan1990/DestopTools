# -*- coding: utf-8 -*-
"""取图耗时基线探针 —— 回答「图标是不是取图本身慢」。

用途：用户报"图标迟迟不出来 / 取图标几十秒"时，**先跑这个**再改代码。
      它按 categories.ini 里某个分类的真实条目逐项计时 DesktopScanner::loadIcon 的三条
      主要 Shell 路径，直接给出"整页冷取要多久"的硬数字。

判读：
  · 整页合计只有几百 ms  ⇒ 「取图慢」假设作废，问题一定在**次数/排队**（队列节拍、闸门、
                            缓存命中被限速）。去查 processIconQueue / setDockButtonIcon。
  · 单项几百 ms、整页几十秒 ⇒ 才是真的 Shell 取图慢（第三方 Shell 扩展/机械盘），
                            应考虑持久化图标缓存或跳过某些路径。

用法：
  python.exe tools/probe_icon_cost.py                 # 默认探测 Unity 分类
  python.exe tools/probe_icon_cost.py "分类名"
  python.exe tools/probe_icon_cost.py "分类名" C:/Users/xxx/AppData/Roaming/DestopTools/categories.ini
"""
import ctypes
import ctypes.wintypes as w
import os
import re
import sys
import time

DEFAULT_INI = os.path.join(os.environ.get("APPDATA", ""), "DestopTools", "categories.ini")
SHGFI_ICON = 0x100
SHGFI_PIDL = 0x8
SHGFI_SYSICONINDEX = 0x4000

shell32 = ctypes.WinDLL("shell32", use_last_error=True)
user32 = ctypes.WinDLL("user32")
ole32 = ctypes.WinDLL("ole32")


class SHFILEINFOW(ctypes.Structure):
    _fields_ = [("hIcon", w.HANDLE), ("iIcon", ctypes.c_int), ("dwAttributes", w.DWORD),
                ("szDisplayName", w.WCHAR * 260), ("szTypeName", w.WCHAR * 80)]


def unescape_qsettings(s):
    """QSettings 的 INI 会把非 ASCII 存成 \\xNNNN 字面文本（不是原始 UTF-8），先还原。"""
    s = re.sub(r"\\x([0-9a-fA-F]{4})", lambda m: chr(int(m.group(1), 16)), s)
    return s.replace("\\t", "\t")


def load_category_paths(ini_path, category):
    raw = open(ini_path, "rb").read().decode("utf-8-sig")
    line = [l for l in raw.splitlines() if l.startswith("mapping=")][0]
    body = unescape_qsettings(line[len("mapping="):])
    out = []
    for seg in body.split(","):
        seg = seg.strip()
        if "\t" not in seg:
            continue
        p, cat = seg.rsplit("\t", 1)
        if cat == category:
            out.append(p)
    return out


def t_shdefextract(path, size=48):
    """路径2：SHDefExtractIconW（对 .lnk 目标/普通文件按尺寸直取）。"""
    t = time.perf_counter()
    hl, hs = w.HICON(), w.HICON()
    shell32.SHDefExtractIconW(ctypes.c_wchar_p(path), 0, 0,
                              ctypes.byref(hl), ctypes.byref(hs), size | (16 << 16))
    dt = (time.perf_counter() - t) * 1000.0
    if hl:
        user32.DestroyIcon(hl)
    if hs:
        user32.DestroyIcon(hs)
    return dt


def t_parse_and_syslist(path):
    """路径3：SHParseDisplayName + SHGetFileInfoW(PIDL|SYSICONINDEX)。"""
    t = time.perf_counter()
    pidl = ctypes.c_void_p()
    if shell32.SHParseDisplayName(ctypes.c_wchar_p(path), None, ctypes.byref(pidl), 0, None) == 0 and pidl:
        fi = SHFILEINFOW()
        shell32.SHGetFileInfoW(ctypes.cast(pidl, ctypes.c_wchar_p), 0, ctypes.byref(fi),
                               ctypes.sizeof(fi), SHGFI_PIDL | SHGFI_SYSICONINDEX)
        ole32.CoTaskMemFree(pidl)
    return (time.perf_counter() - t) * 1000.0


def t_shgetfileinfo_icon(path):
    """路径5等价物 + 通用兜底：SHGetFileInfoW(SHGFI_ICON)，真提取图标。"""
    t = time.perf_counter()
    fi = SHFILEINFOW()
    shell32.SHGetFileInfoW(ctypes.c_wchar_p(path), 0, ctypes.byref(fi),
                           ctypes.sizeof(fi), SHGFI_ICON)
    dt = (time.perf_counter() - t) * 1000.0
    if fi.hIcon:
        user32.DestroyIcon(fi.hIcon)
    return dt


def main():
    category = sys.argv[1] if len(sys.argv) > 1 else "Unity"
    ini_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_INI
    if not os.path.isfile(ini_path):
        print("找不到分类库：%s" % ini_path)
        return 2

    paths = load_category_paths(ini_path, category)
    exist = [p for p in paths if os.path.exists(p)]
    print("分类「%s」：%d 项，其中存在 %d、已删 %d" % (category, len(paths), len(exist), len(paths) - len(exist)))
    if not exist:
        return 1

    rows = []
    t0 = time.perf_counter()
    for p in exist:
        a = t_shdefextract(p)
        b = t_parse_and_syslist(p)
        c = t_shgetfileinfo_icon(p)
        rows.append((a + b + c, p, a, b, c))
    wall = (time.perf_counter() - t0) * 1000.0
    rows.sort(reverse=True)

    print()
    print("%-52s %8s %8s %8s %8s" % ("name", "total", "SHDefEx", "Parse+GFI", "GFI_ICON"))
    for s, p, a, b, c in rows[:12]:
        print("%-52s %7.1f %7.1f %7.1f %7.1f" % (os.path.basename(p)[:50], s, a, b, c))
    total = sum(r[0] for r in rows)
    print()
    print(">>> 整页冷取 %d 项：求和 %.0f ms ｜ 墙钟 %.0f ms ｜ 平均 %.1f ms/项 <<<"
          % (len(rows), total, wall, total / len(rows)))
    print(">>> 判读：只有几百 ms ⇒ 问题在“次数/排队”而非取图本身；几十秒 ⇒ 才是真 Shell 取图慢 <<<")
    return 0


if __name__ == "__main__":
    sys.exit(main())
