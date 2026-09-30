# -*- coding: utf-8 -*-
"""复现 foregroundgate.h 的判据，直接在真机上测量它此刻的结论。
用途：判断 ForegroundGate::fullscreenActive() 是否在用户机器上误判为"全屏"。
"""
import ctypes
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
shell32 = ctypes.WinDLL("shell32", use_last_error=True)


class MONITORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD),
                ("rcMonitor", wintypes.RECT),
                ("rcWork", wintypes.RECT),
                ("dwFlags", wintypes.DWORD)]


def main():
    # ── 判据① ──
    st = ctypes.c_int(0)
    hr = shell32.SHQueryUserNotificationState(ctypes.byref(st))
    names = {0: "QUNS_NOT_PRESENT", 1: "QUNS_NOT_PRESENT", 2: "QUNS_BUSY",
             3: "QUNS_RUNNING_D3D_FULL_SCREEN", 4: "QUNS_PRESENTATION_MODE",
             5: "QUNS_ACCEPTS_NOTIFICATIONS", 6: "QUNS_QUIET_TIME",
             7: "QUNS_APP"}
    print("判据① SHQueryUserNotificationState: hr=0x%08X state=%d (%s)"
          % (hr & 0xFFFFFFFF, st.value, names.get(st.value, "?")))
    reason1 = st.value in (2, 3, 4)
    print("       => 判据① 认为全屏? %s" % reason1)

    # ── 判据② ──
    fg = user32.GetForegroundWindow()
    print("\n判据② GetForegroundWindow = 0x%X" % (fg or 0))
    if not fg:
        print("       => 判据② 认为全屏? False（无前台窗口）")
        return
    pid = wintypes.DWORD(0)
    user32.GetWindowThreadProcessId(fg, ctypes.byref(pid))
    cls = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(fg, cls, 255)
    r = wintypes.RECT()
    user32.GetWindowRect(fg, ctypes.byref(r))
    user32.MonitorFromWindow.restype = ctypes.c_void_p
    mon = user32.MonitorFromWindow(fg, 2)      # MONITOR_DEFAULTTONEAREST
    mi = MONITORINFO()
    mi.cbSize = ctypes.sizeof(MONITORINFO)
    ok = user32.GetMonitorInfoW(ctypes.c_void_p(mon), ctypes.byref(mi))
    print("       pid=%d (本进程=%d) class=%r" % (pid.value, ctypes.windll.kernel32.GetCurrentProcessId(), cls.value))
    print("       窗口矩形 = (%d,%d)-(%d,%d)" % (r.left, r.top, r.right, r.bottom))
    print("       rcMonitor= (%d,%d)-(%d,%d)" % (mi.rcMonitor.left, mi.rcMonitor.top,
                                                 mi.rcMonitor.right, mi.rcMonitor.bottom))
    print("       rcWork   = (%d,%d)-(%d,%d)" % (mi.rcWork.left, mi.rcWork.top,
                                                 mi.rcWork.right, mi.rcWork.bottom))
    if not ok:
        print("       => GetMonitorInfoW 失败")
        return
    tol = 2
    covers = (r.left <= mi.rcMonitor.left + tol and r.top <= mi.rcMonitor.top + tol
              and r.right >= mi.rcMonitor.right - tol and r.bottom >= mi.rcMonitor.bottom - tol)
    excluded = cls.value in ("Progman", "WorkerW", "SHELLDLL_DefView",
                             "Shell_TrayWnd", "Shell_SecondaryTrayWnd",
                             "Windows.UI.Core.CoreWindow")
    own = pid.value == ctypes.windll.kernel32.GetCurrentProcessId()
    print("       covers=%s  被排除(外壳类名)=%s  被排除(本进程)=%s" % (covers, excluded, own))
    reason2 = covers and not excluded and not own
    print("       => 判据② 认为全屏? %s" % reason2)

    print("\n==== 合成结论：fullscreenActive() = %s ====" % (reason1 or reason2))


if __name__ == "__main__":
    main()
