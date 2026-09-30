#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""DestopTools 打包驱动 —— 【固定方法 · 唯一入口 · 禁止再改】
（2026-09-21 锁定；依据项目既有打包产物 release\\deploy\\ 复刻固化）

唯一打包方法（用户 2026-09-21 指定：**固定打包到 release 目录本身，不要 deploy 子目录**）：
    windeployqt --release --no-translations --no-compiler-runtime \\
        --dir release release\\DestopTools.exe
    再确保 release\\ 里有 DestopTools.exe 与 region.json
产物：F:\\Desktop\\DestopTools\\DestopTools\\release\\   ← 就是它，不再有子目录
      （整体拷到任意机器即可独立运行，无需安装 Qt）

用法：
    python tools/build.py       # 1) 先编译出最新 exe
    python tools/pack.py        # 2) 再打包（唯一用法，无参数）

🚨 禁止事项：
  · 不得改用 --qmldir / PyInstaller / NSIS / Inno 等其它打包方式；
  · **不得再往 release\\ 下建 deploy 之类的子目录**（产物就是 release\\ 本身）；
  · 不得省掉 exe 与 region.json（否则不是可分发包）。

依据（历史物证）：release\\ 根目录自 2026-09-03 起就是「exe + Qt 运行时」同置的
可分发布局（Qt5*.dll、platforms、styles、imageformats、iconengines、bearer 均在根，
无 translations → 即 --no-translations）。本脚本把这一步固化成参数不变的一步。
"""
import os
import shutil
import subprocess
import sys
import time

ROOT = r"F:\Desktop\DestopTools\DestopTools"
QT_DIR = r"D:\Qt\Qt5.14.2\5.14.2\msvc2017_64"
WINDEPLOYQT = os.path.join(QT_DIR, "bin", "windeployqt.exe")

RELEASE = os.path.join(ROOT, "release")
EXE = os.path.join(RELEASE, "DestopTools.exe")
REGION = os.path.join(ROOT, "region.json")
OUTDIR = RELEASE  # 打包目录固定 = release\ 根（用户 2026-09-21 指定：不要 deploy 子目录）

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_PATH = os.path.join(SCRIPT_DIR, "pack_last.log")


def build_env():
    env = dict(os.environ)
    for k in ["HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy"]:
        env.pop(k, None)
    env["PATH"] = os.path.join(QT_DIR, "bin") + os.pathsep + env.get("PATH", "")
    return env


def main():
    chunks = []

    if not os.path.exists(EXE):
        print("ERROR: 找不到 %s —— 请先跑 python tools/build.py" % EXE)
        sys.exit(1)

    os.makedirs(OUTDIR, exist_ok=True)

    # ① 唯一的打包命令
    cmd = [WINDEPLOYQT,
           "--release",
           "--no-translations",
           "--no-compiler-runtime",
           "--dir", OUTDIR,
           EXE]
    print("[pack] %s" % " ".join(cmd))
    p = subprocess.run(cmd, cwd=RELEASE, env=build_env(),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = p.stdout.decode("utf-8", errors="replace")
    chunks.append("===== WINDEPLOYQT (exit %d) =====\n%s" % (p.returncode, out))
    if p.returncode != 0:
        chunks.append("WINDEPLOYQT FAILED\n")

    # ② 确保 exe 与数据文件在打包目录里（打包目录 = release\ 根时 exe 本就在此，跳过自拷贝）
    copied = []
    for src in (EXE, REGION):
        if not os.path.exists(src):
            continue
        dst = os.path.join(OUTDIR, os.path.basename(src))
        if os.path.abspath(src) == os.path.abspath(dst):
            copied.append("%s (已在打包目录)" % os.path.basename(src))
            continue
        shutil.copy2(src, dst)
        copied.append("%s (%d B)" % (os.path.basename(src), os.path.getsize(dst)))
    chunks.append("copied: %s\n" % (", ".join(copied) if copied else "none"))

    # ③ 清点产物
    files, total = 0, 0
    for root, _dirs, fs in os.walk(OUTDIR):
        for f in fs:
            files += 1
            total += os.path.getsize(os.path.join(root, f))
    info = "OUTDIR %s\nFILES %d  TOTAL %.1f MB\nexe %s\n" % (
        OUTDIR, files, total / 1024.0 / 1024.0,
        time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(EXE))))
    chunks.append(info)

    text = "\n".join(chunks)
    with open(LOG_PATH, "w", encoding="utf-8", errors="replace") as f:
        f.write(text)
    print(text[-4000:])
    sys.exit(0 if p.returncode == 0 else 1)


if __name__ == "__main__":
    main()
