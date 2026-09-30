/*
 * @file editorwatch.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef EDITORWATCH_H
#define EDITORWATCH_H

// 内联重命名编辑框的「可见性 + 真焦点」看护（Dock 图标 / 收纳盒图标共用）
// 为什么需要它（2026-09-13 定论）：
// 日志证明 F2 与右键「重命名」每次都走到 startInlineRename，且编辑框创建成功、
// Qt 侧 isVisible()/hasFocus() 全为 true —— 但用户「看不到框、打不进字」。
// 原因只能落在两个 Qt 报不出来的事实上：
// ① 编辑框虽然 WS_VISIBLE，却被压在别的窗口之下（Z 序），Qt 无法感知；
// ② Qt 认为它有焦点，而 Windows 的真前台并不是它（前台锁/激活抖动），
// Qt 的 hasFocus() 在这种“假焦点”下依然返回 true。
// 本模块提供两件事：把这两件事变成**可读的字符串**（写进 diagtrace 日志），
// 以及一个幂等的“强制置顶”动作，供编辑期的看护定时器反复再断言。
// 与项目既有约定的关系：
// · 编辑框【不属于】band 窗口（不在 DesktopMirrorWindow::s_cloakTargets 里），
// 故 parkAllTargetsAtBottom / untopmostAllTargets 都不会碰它；
// · 项目禁止 band 窗口用 HWND_TOPMOST，但编辑框是**短暂的输入窗口**（存活数秒），
// 置顶是 Windows 行内重命名的正常语义（Explorer 的编辑框也在最上层），故此处允许。

#include <QString>
#include <QtGlobal>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// 与项目其它翻译单元保持一致：禁止 windows.h 定义 min/max 宏，否则会破坏 <algorithm>。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace EditorWatch {

struct Status {
    bool valid         = false;
    bool onTopAtCenter = false;   // WindowFromPoint(编辑框中心) 是否就是编辑框本身
    bool isForeground  = false;   // 编辑框是否真的是前台窗口（真焦点，不是 Qt 的自说自话）
    bool noActivate    = false;   // 是否带着 WS_EX_NOACTIVATE（带=永远无法成为前台）
    HWND aboveHwnd     = nullptr; // 盖住它的那个窗口（onTopAtCenter=false 时有意义）
    QString aboveDesc;            // 上面那个窗口的描述（类名/标题/pid）
    HWND owner         = nullptr; // GWLP_HWNDPARENT：被错误 owner 化会让它随 owner 一起被压到最底
};

// probe
inline Status probe(HWND eh) {
    Status st;
    if (!eh || !IsWindow(eh)) return st;
    st.valid = true;

    RECT rc = {};
    if (GetWindowRect(eh, &rc)) {
        POINT center = {(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
        HWND at = WindowFromPoint(center);
        // WindowFromPoint 命中的可能是编辑框的子窗口（QLineEdit 的子控件），沿 parent 上溯比对。
        HWND walk = at;
        while (walk && walk != eh) walk = GetParent(walk);
        st.onTopAtCenter = (walk == eh);
        st.aboveHwnd = at;
        if (at && !st.onTopAtCenter) {
            DWORD pid = 0;
            GetWindowThreadProcessId(at, &pid);
            char cls[64] = {0};
            GetClassNameA(at, cls, 63);
            st.aboveDesc = QStringLiteral("hwnd=%1 cls=%2 pid=%3")
                               .arg(reinterpret_cast<quintptr>(at))
                               .arg(QString::fromLatin1(cls))
                               .arg(pid);
        }
    }

    st.isForeground = (GetForegroundWindow() == eh);
    st.noActivate = (GetWindowLongPtrW(eh, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0;
    st.owner = reinterpret_cast<HWND>(GetWindowLongPtrW(eh, GWLP_HWNDPARENT));
    return st;
}

// 把编辑框重新抬到 Z 序最上层（幂等）。
// 用 HWND_TOPMOST 而非 HWND_TOP：本项目其余窗口的 Z 序会被多处周期性改写
// （parkAllTargetsAtBottom 1s、band 守卫、topmost 状态机），只有 topmost 层
// 能稳定不被这些改写波及，从而保证编辑框始终可见。
inline void forceOnTop(HWND eh) {
    if (!eh || !IsWindow(eh)) return;
    SetWindowPos(eh, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// 清掉 Qt 可能替我们指定的 owner（GWLP_HWNDPARENT）。
// 被 owner 化的窗口会跟随 owner 一起被 SetWindowPos(x, HWND_BOTTOM) 拖到最底
// （本项目 Dock 每秒都被压到 HWND_BOTTOM），此时编辑框会沉到桌面图标层之下 —— 完全不可见。
inline void clearOwner(HWND eh) {
    if (!eh || !IsWindow(eh)) return;
    if (GetWindowLongPtrW(eh, GWLP_HWNDPARENT) != 0)
        SetWindowLongPtrW(eh, GWLP_HWNDPARENT, 0);
}

} // namespace EditorWatch

#endif // Q_OS_WIN

#endif // EDITORWATCH_H
