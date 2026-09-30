# -*- coding: utf-8 -*-
"""
addr2func.py —— 用链接映射文件（.map）把崩溃日志里的地址反查成「函数名 + 偏移」。

为什么需要它
------------
本项目的 release 构建**没有 .pdb**，一旦崩溃，日志里的"异常地址"没有任何符号可查。
自 2026-09-24 起，DestopTools.pro 显式加了 `/MAP`，链接期生成
    DestopTools/release/DestopTools.map
CrashTrace 的 VEH 处理器会把异常地址与**完整调用栈**写进 DestopTools_crash.log，
其中每一帧都形如：
    #07 DestopTools.exe + 0x43241
这里的 `0x43241` 就是 **RVA**（相对模块基址的偏移）。本脚本把它映射回函数。

用法
----
    python tools/addr2func.py 0x43241 0x4399C 0x4544D
    python tools/addr2func.py --stack-from-log            # 直接解析 crash.log 里最后一组调用栈

地址既可按 RVA 给（0x43241），也可按 map 里的 Rva+Base 给（0x140043241），脚本自动识别。
"""

import os
import re
import sys
import bisect

MAP_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "DestopTools", "release", "DestopTools.map")
LOG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "DestopTools", "release", "DestopTools_crash.log")

# Rva+Base 段的一行：  0001:00001234  ?sym@@...  0000000140012345  f  obj.obj
_RX_LINE = re.compile(
    r'^\s+[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s+'      # 段:偏移
    r'(\S+)\s+'                                   # 符号名
    r'([0-9A-Fa-f]{16})'                          # Rva+Base（16 位十六进制）
    r'(?:\s+.*)?$'                                # 可选的 f/i 标记与 obj 名
)


def demangle(sym):
    """把 MSVC 修饰名压成可读形式：?func@Class@@YAXXZ -> Class::func"""
    if not sym.startswith('?'):
        return sym
    head = sym[1:].split('@@')[0]          # func@Class@Sub
    parts = [p for p in head.split('@') if p]
    if len(parts) >= 2:
        return "::".join(reversed(parts))
    return sym


def load_map(path):
    """返回 (排序后的 Rva+Base 列表, 与之一一对应的 (符号名, obj) 列表, 首选基址)"""
    addrs, syms, objs = [], [], []
    base = None
    in_publics = False
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if "Preferred load address is" in line:
                base = int(line.split()[-1], 16)
            if "Publics by Value" in line or "Static symbols" in line:
                in_publics = True
                continue
            if not in_publics:
                continue
            m = _RX_LINE.match(line)
            if not m:
                continue
            sym = m.group(1)
            addr = int(m.group(2), 16)
            if addr == 0:            # <absolute> 之类跳过
                continue
            obj = ""
            tail = line[m.end(2):].split()
            if tail:
                obj = tail[-1]
            addrs.append(addr)
            syms.append(sym)
            objs.append(obj)
    order = sorted(range(len(addrs)), key=lambda i: addrs[i])
    return ([addrs[i] for i in order],
            [(syms[i], objs[i]) for i in order],
            base or 0x140000000)


def lookup(addrs, syms, rva_addr):
    i = bisect.bisect_right(addrs, rva_addr) - 1
    if i < 0:
        return None
    sym, obj = syms[i]
    return demangle(sym), rva_addr - addrs[i], obj


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--stack-from-log":
        raw = open(LOG_PATH, "rb").read().decode("utf-16-le", "replace")
        # 取最后一组「调用栈:」块
        blocks = raw.split("调用栈:")
        if len(blocks) < 2:
            print("崩溃日志里没有调用栈块")
            return 1
        targets = []
        for line in blocks[-1].splitlines():
            m = re.search(r'#\d+\s+(\S+)\s+\+\s+0x([0-9A-Fa-f]+)', line)
            if m:
                targets.append((m.group(1), int(m.group(2), 16)))
        print("== 最后一组调用栈 ==")
    else:
        targets = [("DestopTools.exe", int(a, 16)) for a in sys.argv[1:]]
        print("== 指定地址 ==")

    if not targets:
        print("用法: python tools/addr2func.py 0x43241 [0x4399C ...]")
        print("     python tools/addr2func.py --stack-from-log")
        return 1

    addrs, syms, base = load_map(os.path.normpath(MAP_PATH))
    print("map: %d 个符号, 首选基址 0x%X" % (len(addrs), base))

    for mod, val in targets:
        if mod.lower().endswith("destoptools.exe"):
            # 调用栈里记的是 RVA（= 运行时地址 − 模块基址）；但也允许直接给 map 里的 Rva+Base。
            target = val if val >= base else base + val
            rva = target - base
            hit = lookup(addrs, syms, target)
            if hit:
                name, off, obj = hit
                print("  %-12s -> %s + 0x%X   [%s]" % (hex(rva), name, off, obj))
            else:
                print("  %-12s -> (map 中无匹配，可能是 CRT/编译期展开代码)" % hex(rva))
        else:
            print("  %s + 0x%X  (第三方模块，map 不含)" % (mod, val))
    return 0


if __name__ == "__main__":
    sys.exit(main())
