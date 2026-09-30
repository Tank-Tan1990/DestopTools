# -*- coding: utf-8 -*-
"""产物断言：验证 2026-09-25「鼠标卡死 6 条修复方向」的关键字符串确实编进了 exe。
用法： python.exe tools/assert_fix6_strings.py
判据：QStringLiteral 以 UTF-16LE 落在 .rdata；ASCII 字面量（CrashTrace::mark）以 UTF-8/ASCII 落在。
"""
import os
import sys

EXE = r"F:\Desktop\DestopTools\DestopTools\release\DestopTools.exe"

# (说明, 字符串, 期望编码)
CHECKS = [
    # —— 本轮新增（修复方向③ 前置廉价变化门 / 方向④ 图标异步队列）——
    ("方向③ 廉价门短路埋点",            "refresh:cheap-skip",                        "ascii"),
    ("方向④ 图标队列空图告警",          "setDockButtonIcon: null icon name=%1 sp=%2 special=%3", "utf16"),
    # —— 取图链路哨兵（2026-09-24 回归修复：缓存优先快路径不得破坏这四条路径埋点）——
    ("取图 路径1 IExtractIcon",         "loadIcon:p1-IExtractIcon",                  "ascii"),
    ("取图 路径2 SHDefExtractIcon",     "loadIcon:p2-SHDefExtractIcon",              "ascii"),
    ("取图 路径3 系统镜像列表",         "loadIcon:p3-imageListIndex",                "ascii"),
    ("取图 完成",                       "loadIcon:done",                             "ascii"),
    # —— 既有回归哨兵（必须一条不丢，证明"没破坏现有功能"）——
    ("既有 Raw Input 路径",             "RawInput",                                  "utf16"),
    ("既有 版本号",                     "当前版本：0.1.0",                            "utf16"),
    ("既有 顶部留白设置文案",           "距桌面顶部始终留出 10 逻辑像素",              "utf16"),
    ("既有 刷新埋点",                   "refresh:enter",                             "ascii"),
    ("既有 刷新完成埋点",               "refresh:done",                              "ascii"),
]


def main():
    if not os.path.isfile(EXE):
        print("FAIL 找不到产物：%s" % EXE)
        return 2
    st = os.stat(EXE)
    import datetime
    print("EXE  %d B  %s" % (st.st_size, datetime.datetime.fromtimestamp(st.st_mtime)))
    data = open(EXE, "rb").read()
    failed = 0
    for desc, s, enc in CHECKS:
        if enc == "utf16":
            needle = s.encode("utf-16-le")
        else:
            needle = s.encode("utf-8")
        ok = data.find(needle) >= 0
        print("%-4s %-28s [%s] %s" % ("PASS" if ok else "FAIL", desc, enc, s))
        if not ok:
            failed += 1
    print("---- %d/%d passed ----" % (len(CHECKS) - failed, len(CHECKS)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
