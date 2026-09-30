#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""编译并运行 tools/tagprobe（链接产品 .obj，跑真实 IconGridWindow，模拟鼠标点击分类标签）。

⚠️ IconGridWindow::setItems() 会写用户 INI（IconGridWindow/currentCategory、分类顺序等），
   所以本脚本在运行前备份 %APPDATA%\\DestopTools\\DestopTools.ini，运行后原样恢复。

用法： python tools/run_tagprobe.py
"""
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build as B   # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
TEST_DIR = os.path.join(HERE, "tagprobe")
INI = os.path.join(os.environ.get("APPDATA", ""), "DestopTools", "DestopTools.ini")
BAK = INI + ".tagprobe.bak"


def main():
    env = B.build_env()
    # 🚨 强制重新链接：试验台把产品 .obj 静态链进来，而 nmake 依赖里只有 main.obj ——
    #    产品重编后若试验台源码没动，nmake 会跳过链接 → 跑的还是上一次的产品代码，结论失真。
    _rel = os.path.join(TEST_DIR, "release")
    if os.path.isdir(_rel):
        for _f in os.listdir(_rel):
            if _f.endswith(".exe"):
                os.remove(os.path.join(_rel, _f))

    # ① 备份用户 INI（试验台会写它）
    backed = False
    if os.path.exists(INI):
        shutil.copy2(INI, BAK)
        backed = True
        print("[tagprobe] 已备份用户 INI -> %s (%d B)" % (BAK, os.path.getsize(BAK)))

    rc = 1
    try:
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

        exe = os.path.join(TEST_DIR, "release", "tagprobe.exe")
        if not os.path.exists(exe):
            print("exe missing:", exe)
            return 1
        p = subprocess.run([exe], cwd=TEST_DIR, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print("===== RUN exit=%d =====" % p.returncode)
        print(p.stdout.decode("gbk", errors="replace"))
        rc = 0
    finally:
        # ② 恢复用户 INI
        if backed:
            shutil.copy2(BAK, INI)
            os.remove(BAK)
            print("[tagprobe] 已恢复用户 INI（%d B）" % os.path.getsize(INI))
    return rc


if __name__ == "__main__":
    sys.exit(main())
