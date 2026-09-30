#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Raw Input 通道自检（2026-09-24）

用途：验证「隐藏 WS_POPUP 窗口 + RIDEV_INPUTSINK」这套接收方式在**本机**是否真的能收到
      WM_INPUT —— 这是 DestopTools 用 Raw Input 取代 WH_MOUSE_LL 的前提。
      （Raw Input 是异步投递，不会像低级钩子那样阻塞系统输入链。）

做法与 DestopTools 里 rawmouse.h 完全同构：自注册窗口类 → 建隐藏窗口（不 Show）→
注册鼠标设备（RIDEV_INPUTSINK）→ 跑消息泵 PeekMessage/DispatchMessage 统计 WM_INPUT。

不移动/点击用户的鼠标：只被动观察；若 N 秒内一条都没收到，再补一次**零位移**事件
（MOUSEEVENTF_MOVE, dx=dy=0）以排除“用户恰好没动鼠标”的假阴性。

退出码：0 = 通道可用；1 = 不可用（DestopTools 里会自动降级回 WH_MOUSE_LL）。
"""
import ctypes
import sys
import time
from ctypes import wintypes

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

LRESULT = ctypes.c_ssize_t
WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wintypes.HWND, ctypes.c_uint,
                             wintypes.WPARAM, wintypes.LPARAM)


class WNDCLASSEXW(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_uint), ("style", ctypes.c_uint),
                ("lpfnWndProc", WNDPROC), ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int), ("hInstance", wintypes.HINSTANCE),
                ("hIcon", wintypes.HANDLE), ("hCursor", wintypes.HANDLE),
                ("hbrBackground", wintypes.HANDLE), ("lpszMenuName", wintypes.LPCWSTR),
                ("lpszClassName", wintypes.LPCWSTR), ("hIconSm", wintypes.HANDLE)]


class RAWINPUTDEVICE(ctypes.Structure):
    _fields_ = [("usUsagePage", wintypes.USHORT), ("usUsage", wintypes.USHORT),
                ("dwFlags", wintypes.DWORD), ("hwndTarget", wintypes.HWND)]


# —— ctypes 默认按 32 位 int 传参，64 位句柄/LPARAM 会溢出 —— 必须显式声明 ——
user32.DefWindowProcW.argtypes = [wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM]
user32.DefWindowProcW.restype = LRESULT
user32.RegisterClassExW.argtypes = [ctypes.POINTER(WNDCLASSEXW)]
user32.RegisterClassExW.restype = wintypes.WORD
user32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR,
                                   wintypes.DWORD, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_int, wintypes.HWND,
                                   wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID]
user32.CreateWindowExW.restype = wintypes.HWND
user32.RegisterRawInputDevices.argtypes = [ctypes.POINTER(RAWINPUTDEVICE), ctypes.c_uint, ctypes.c_uint]
user32.RegisterRawInputDevices.restype = wintypes.BOOL
user32.GetRawInputData.argtypes = [wintypes.HANDLE, ctypes.c_uint, wintypes.LPVOID,
                                   ctypes.POINTER(ctypes.c_uint), ctypes.c_uint]
user32.GetRawInputData.restype = ctypes.c_uint
user32.PeekMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND,
                                ctypes.c_uint, ctypes.c_uint, ctypes.c_uint]
user32.PeekMessageW.restype = wintypes.BOOL
user32.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]
user32.DispatchMessageW.restype = LRESULT
user32.DestroyWindow.argtypes = [wintypes.HWND]
user32.GetRawInputData.restype = ctypes.c_uint

WM_INPUT = 0x00FF
RID_INPUT = 0x10000003
RIDEV_INPUTSINK = 0x00000100
RIDEV_REMOVE = 0x00000001

stats = {"total": 0, "buttons": 0}


@WNDPROC
def sink_proc(hwnd, msg, wparam, lparam):
    if msg == WM_INPUT:
        stats["total"] += 1
        try:
            size = ctypes.c_uint(0)
            hdr = ctypes.sizeof(ctypes.c_uint) * 3   # sizeof(RAWINPUTHEADER) == 8 + 8 + 4 对齐后 16/24
            hdr = 24 if ctypes.sizeof(ctypes.c_void_p) == 8 else 16
            user32.GetRawInputData(wintypes.HANDLE(lparam), RID_INPUT, None,
                                   ctypes.byref(size), hdr)
            if 0 < size.value <= 4096:
                buf = ctypes.create_string_buffer(size.value)
                got = user32.GetRawInputData(wintypes.HANDLE(lparam), RID_INPUT, buf,
                                            ctypes.byref(size), hdr)
                if got == size.value and size.value >= hdr + 4:
                    # RAWMOUSE：usFlags(2) + usButtonFlags(2)
                    flags = int.from_bytes(buf.raw[hdr + 2:hdr + 4], "little")
                    if flags:
                        stats["buttons"] += 1
        except Exception as exc:            # noqa: BLE001 - 自检脚本，任何异常都不该中断消息泵
            print("  (raw read error: %s)" % exc)
        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)
    return user32.DefWindowProcW(hwnd, msg, wparam, lparam)


def pump(seconds):
    msg = wintypes.MSG()
    t0 = time.time()
    while time.time() - t0 < seconds:
        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        time.sleep(0.005)


def main():
    cls = "DestopToolsRawInputSelfTest"
    inst = kernel32.GetModuleHandleW(None)
    wc = WNDCLASSEXW()
    wc.cbSize = ctypes.sizeof(wc)
    wc.lpfnWndProc = sink_proc
    wc.hInstance = inst
    wc.lpszClassName = cls
    if not user32.RegisterClassExW(ctypes.byref(wc)):
        err = ctypes.GetLastError()
        if err != 1410:   # ERROR_CLASS_ALREADY_EXISTS
            print("RegisterClassExW FAILED err=%d" % err)
            return 1

    # 隐藏顶层窗口：不 ShowWindow（与 rawmouse.h 完全一致）
    WS_POPUP = 0x80000000
    WS_EX_TOOLWINDOW, WS_EX_NOACTIVATE = 0x00000080, 0x08000000
    hwnd = user32.CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls, "",
                                  WS_POPUP, 0, 0, 0, 0, None, None, inst, None)
    if not hwnd:
        print("CreateWindowExW FAILED err=%d" % ctypes.GetLastError())
        return 1
    print("sink hwnd = 0x%X (hidden, never shown)" % hwnd)

    rid = RAWINPUTDEVICE(0x01, 0x02, RIDEV_INPUTSINK, hwnd)
    if not user32.RegisterRawInputDevices(ctypes.byref(rid), 1, ctypes.sizeof(rid)):
        print("RegisterRawInputDevices FAILED err=%d" % ctypes.GetLastError())
        return 1
    print("RegisterRawInputDevices OK (RIDEV_INPUTSINK)")

    # 阶段 1：被动观察 6 秒（用户只要动一下鼠标就会有 WM_INPUT）
    pump(6.0)
    print("phase1 (passive 6s): WM_INPUT=%d buttons=%d" % (stats["total"], stats["buttons"]))

    if stats["total"] == 0:
        # 阶段 2：零位移注入（不移动光标）以排除“用户恰好没动鼠标”造成的假阴性
        print("no event seen -> inject zero-delta move (MOUSEEVENTF_MOVE,0,0)")
        user32.mouse_event(0x0001, 0, 0, 0, 0)
        pump(2.0)
        print("phase2 (after inject): WM_INPUT=%d" % stats["total"])

    rid_off = RAWINPUTDEVICE(0x01, 0x02, RIDEV_REMOVE, None)
    user32.RegisterRawInputDevices(ctypes.byref(rid_off), 1, ctypes.sizeof(rid_off))
    user32.DestroyWindow(hwnd)

    if stats["total"] > 0:
        print("RESULT: RAW INPUT CHANNEL OK")
        return 0
    print("RESULT: RAW INPUT CHANNEL DEAD (DestopTools 会自动降级回 WH_MOUSE_LL)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
