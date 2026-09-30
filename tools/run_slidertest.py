#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""编译并运行 tools/slidertest（QSlider 信号顺序实测）。

复用 tools/build.py 的 MSVC/Qt 环境拼装，避免重复维护工具链路径。
用法： python tools/run_slidertest.py
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build as B   # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
TEST_DIR = os.path.join(HERE, "slidertest")


def main():
    env = B.build_env()
    # 🚨 强制重新链接：试验台把产品 .obj 静态链进来，而 nmake 依赖里只有 main.obj ——
    #    产品重编后若试验台源码没动，nmake 会跳过链接 → 跑的还是上一次的产品代码，结论失真。
    _rel = os.path.join(TEST_DIR, "release")
    if os.path.isdir(_rel):
        for _f in os.listdir(_rel):
            if _f.endswith(".exe"):
                os.remove(os.path.join(_rel, _f))
    steps = [
        ([B.QMAKE, "-spec", "win32-msvc", "CONFIG+=release", "CONFIG-=debug"], "qmake"),
        ([B.NMAKE, "-f", "Makefile.Release"], "nmake"),
    ]
    for cmd, label in steps:
        p = subprocess.run(cmd, cwd=TEST_DIR, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = p.stdout.decode("utf-8", errors="replace")
        print("===== %s exit=%d =====" % (label, p.returncode))
        if p.returncode != 0 or "error" in out.lower():
            print(out[-4000:])
            return 1

    exe = os.path.join(TEST_DIR, "release", "slidertest.exe")
    if not os.path.exists(exe):
        print("exe missing:", exe)
        return 1
    p = subprocess.run([exe], cwd=TEST_DIR, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=60)
    print("===== RUN exit=%d =====" % p.returncode)
    print(p.stdout.decode("utf-8", errors="replace"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
