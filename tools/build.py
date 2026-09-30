#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""DestopTools 编译驱动 —— 【固定方法 · 唯一入口 · 禁止再改】
（配方 2026-08-19 确立，2026-09-21 锁定；与 DestopTools/build_msvc.bat 逐字等价）

唯一编译方法：
    qmake DestopTools.pro -spec win32-msvc "CONFIG+=release" "CONFIG-=debug"
    nmake release
产物：F:\\Desktop\\DestopTools\\DestopTools\\release\\DestopTools.exe

用法：
    python tools/build.py        # 唯一用法（无参数）

🚨 禁止事项 —— 不要再"优化/顺手改"本脚本：
  1. 不得改用 MinGW / Qt Creator / mingw32-make；
  2. 不得加"跳过 qmake 只跑 nmake"的快捷分支 —— 会漏编引用了【新增头文件】的 TU
     （nmake-only 的依赖表来自上次 qmake，新头不在表里 → 静默编不出新码，看着却"成功"）；
  3. LNK1104「无法打开 release\\DestopTools.exe」= 用户正在运行该 exe → 让用户关掉后重跑，
     不要用 DESTDIR_TARGET / 改名输出等手法绕过；
  4. 历史参数 --full 仍被接受，但行为与不带参数**完全相同**（每次都已包含 qmake），
     保留只是为了兼容旧笔记。

本脚本只做两件「沙箱环境适配」——这不是改编译方法，配方本身一个字没变：
  ① 拼绝对路径 PATH/INCLUDE/LIB：Bash 沙箱不透传环境，而 qmake 生成的 Makefile 用裸 cl/link；
  ② 就地补丁 Makefile：CC/CXX/LINKER 改绝对路径、INCPATH/LIBS 注入 VS+WinSDK 路径、
     去掉 /MANIFEST:embed 与 /MANIFESTDEPENDENCY（免 mt.exe → LNK1158）。

⚠️ .workbuddy/tmp 与 .workbuddy/tools 会被外部清理（已发生 5 次），故脚本放在项目根 tools/ 下。
"""
import os
import re
import subprocess
import sys
import time

ROOT = r"F:\Desktop\DestopTools\DestopTools"
QMAKE = r"D:\Qt\Qt5.14.2\5.14.2\msvc2017_64\bin\qmake.exe"
QT_DIR = r"D:\Qt\Qt5.14.2\5.14.2\msvc2017_64"
VS_DIR = r"F:\Program Files\Microsoft Visual Studio\2022\Community"
MSVC_VER = "14.44.35207"
SDK_DIR = r"D:\Windows Kits\10"
SDK_VER = "10.0.22621.0"

VCT = os.path.join(VS_DIR, "VC", "Tools", "MSVC", MSVC_VER)
CL = os.path.join(VCT, "bin", "Hostx64", "x64", "cl.exe")
LINK = os.path.join(VCT, "bin", "Hostx64", "x64", "link.exe")
NMAKE = os.path.join(VCT, "bin", "Hostx64", "x64", "nmake.exe")

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LOG_PATH = os.path.join(SCRIPT_DIR, "build_last.log")

EXE = os.path.join(ROOT, "release", "DestopTools.exe")


def system_include_dirs():
    return [p for p in [
        os.path.join(VCT, "include"),
        os.path.join(SDK_DIR, "Include", SDK_VER, "ucrt"),
        os.path.join(SDK_DIR, "Include", SDK_VER, "shared"),
        os.path.join(SDK_DIR, "Include", SDK_VER, "um"),
        os.path.join(SDK_DIR, "Include", SDK_VER, "winrt"),
        os.path.join(SDK_DIR, "Include", SDK_VER, "cppwinrt"),
    ] if os.path.isdir(p)]


def system_lib_dirs():
    return [p for p in [
        os.path.join(VCT, "lib", "x64"),
        os.path.join(SDK_DIR, "Lib", SDK_VER, "ucrt", "x64"),
        os.path.join(SDK_DIR, "Lib", SDK_VER, "um", "x64"),
    ] if os.path.isdir(p)]


def build_env():
    env = dict(os.environ)
    # 代理变量会干扰 qmake / nmake 的子进程行为，先清掉
    for k in ["HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy"]:
        env.pop(k, None)
    path = [
        os.path.join(QT_DIR, "bin"),
        os.path.join(VCT, "bin", "Hostx64", "x64"),
        os.path.join(SDK_DIR, "bin", SDK_VER, "x64"),
        r"C:\Windows\System32",
        r"C:\Windows",
        env.get("PATH", ""),
    ]
    env["PATH"] = os.pathsep.join([p for p in path if p])
    env["INCLUDE"] = ";".join(system_include_dirs())
    env["LIB"] = ";".join(system_lib_dirs())
    env["QTDIR"] = QT_DIR
    return env


def patch_makefile(path):
    """把 qmake 生成的 Makefile 补成「不依赖 vcvarsall」的自洽状态（幂等）。"""
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as f:
        s = f.read()
    if "DestopTools-build-patched" in s:
        return False
    inc = " ".join('-I"%s"' % p for p in system_include_dirs())
    lib = " ".join('/LIBPATH:"%s"' % p for p in system_lib_dirs())

    s = re.sub(r"(?m)^CC(\s*)=.*$", lambda m: 'CC%s= "%s"' % (m.group(1), CL), s)
    s = re.sub(r"(?m)^CXX(\s*)=.*$", lambda m: 'CXX%s= "%s"' % (m.group(1), CL), s)
    s = re.sub(r"(?m)^LINKER(\s*)=.*$", lambda m: 'LINKER%s= "%s"' % (m.group(1), LINK), s)
    s = re.sub(r"(?m)^INCPATH(\s*)=(.*)$",
               lambda m: "INCPATH%s=%s %s" % (m.group(1), m.group(2), inc), s)
    s = re.sub(r"(?m)^LIBS(\s*)=(.*)$",
               lambda m: "LIBS%s=%s %s" % (m.group(1), m.group(2), lib), s)
    # 去掉清单嵌入（不需要 mt.exe，避免 LNK1158）
    s = s.replace("/MANIFEST:embed ", "")
    s = re.sub(r'"[^"]*MANIFESTDEPENDENCY[^"]*"', "", s)
    s = "# DestopTools-build-patched (tools/build.py)\r\n" + s

    with open(path, "w", encoding="utf-8", errors="replace", newline="") as f:
        f.write(s)
    return True


def run(cmd, env, label):
    print("[build] %s: %s" % (label, " ".join(cmd)))
    p = subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", errors="replace")


def main():
    env = build_env()
    chunks = []

    # ① 保底补丁（万一 qmake 失败，旧 Makefile 至少是自洽的）
    fallback = 0
    for f in ["Makefile.Release", "Makefile"]:
        p = os.path.join(ROOT, f)
        if os.path.exists(p) and patch_makefile(p):
            fallback += 1

    # ② 固定第一步：qmake（win32-msvc / release）——每次必跑，杜绝"新增头文件不重编"
    rc_qmake, out = run([QMAKE, "-spec", "win32-msvc",
                         "CONFIG+=release", "CONFIG-=debug"], env, "qmake")
    chunks.append("===== QMAKE (exit %d) =====\n%s" % (rc_qmake, out))

    # ③ 补丁 qmake 新生成的 Makefile
    patched = 0
    for f in ["Makefile.Release", "Makefile"]:
        p = os.path.join(ROOT, f)
        if os.path.exists(p) and patch_makefile(p):
            patched += 1
    chunks.append("Makefile patched: %d (fallback %d)\n" % (patched, fallback))

    if rc_qmake != 0:
        chunks.append("QMAKE FAILED → 中止，不跑 nmake（避免用过期的依赖表）\n")
        text = "\n".join(chunks)
        with open(LOG_PATH, "w", encoding="utf-8", errors="replace") as f:
            f.write(text)
        print(text[-4000:])
        sys.exit(1)

    # ④ 固定第二步：nmake release
    rc_nmake, out = run([NMAKE, "-f", "Makefile.Release"], env, "nmake")
    chunks.append("===== NMAKE (exit %d) =====\n%s" % (rc_nmake, out))

    info = "NMAKE_EXIT=%d\n" % rc_nmake
    if os.path.exists(EXE):
        info += "EXE %d B  %s\n" % (os.path.getsize(EXE),
                                    time.strftime("%Y-%m-%d %H:%M:%S",
                                                  time.localtime(os.path.getmtime(EXE))))
    chunks.append(info)

    text = "\n".join(chunks)
    with open(LOG_PATH, "w", encoding="utf-8", errors="replace") as f:
        f.write(text)
    print(text[-6000:])
    sys.exit(0 if rc_nmake == 0 else 1)


if __name__ == "__main__":
    main()
