#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""编译并运行 tools/scprobe（链接产品 .obj，跑真实 SettingCenterDialog）。

用途：实测 设置中心 → 外观设置 → 分区标签切换 的持久化是否真的失效。
⚠️ 该试验台会写用户 INI（对话框 saveSettings 落盘），故运行前备份、运行后原样恢复。

用法： python tools/run_scprobe.py
"""
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build as B   # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
TEST_DIR = os.path.join(HERE, "scprobe")
INI = os.path.join(os.environ.get("APPDATA", ""), "DestopTools", "DestopTools.ini")
BAK = INI + ".scprobe.bak"


def main():
    env = B.build_env()

    backed = False
    if os.path.exists(INI):
        shutil.copy2(INI, BAK)
        backed = True
        print("[scprobe] 已备份用户 INI -> %s (%d B)" % (BAK, os.path.getsize(BAK)))

    rc = 1
    try:
        # 🚨 必须先删掉试验台 exe！它把产品 .obj **静态链**进来，而 nmake 的依赖规则里只有
        #    main.obj —— 产品重编后若试验台源码没动，nmake 会直接跳过链接，跑的就还是
        #    「上一次的产品代码」，实测结论会整段失真（2026-09-21 实测踩到这个坑）。
        exe = os.path.join(TEST_DIR, "release", "scprobe.exe")
        if os.path.exists(exe):
            os.remove(exe)
            print("[scprobe] 已删除旧 exe，强制重新链接产品 .obj")

        steps = [
            ([B.QMAKE, "-spec", "win32-msvc", "CONFIG+=release", "CONFIG-=debug"], "qmake"),
            ([B.NMAKE, "-f", "Makefile.Release"], "nmake"),
        ]
        for cmd, label in steps:
            p = subprocess.run(cmd, cwd=TEST_DIR, env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            out = p.stdout.decode("gbk", errors="replace")
            print("===== %s exit=%d =====" % (label, p.returncode))
            if p.returncode != 0 or " error " in out.lower():
                print(out[-9000:])
                return 1

        if not os.path.exists(exe):
            print("exe missing:", exe)
            return 1
        p = subprocess.run([exe], cwd=TEST_DIR, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print("===== RUN exit=%d =====" % p.returncode)
        print(p.stdout.decode("gbk", errors="replace"))
        rc = 0
    finally:
        if backed:
            shutil.copy2(BAK, INI)
            os.remove(BAK)
            print("[scprobe] 已恢复用户 INI（%d B）" % os.path.getsize(INI))
    return rc


if __name__ == "__main__":
    sys.exit(main())
