/*
 * @file desktopimmunityfilter.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef DESKTOPIMMUNITYFILTER_H
#define DESKTOPIMMUNITYFILTER_H

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QSet>
#include <QHash>

// 全局 Win32 消息过滤器：让 DestopTools 的桌面挂件窗口（Dock、收纳盒、桌面助手）
// 免疫 Windows 任务栏最右侧"显示桌面"按钮与 Win+D 的最小化/隐藏控制。
// 做法（经多轮验证的稳定方案，仿 360 桌面助手 / Rainmeter 等桌面挂件）：
// - 对三个桌面挂件窗口设置 WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW，并清除 Qt 默认带上的
// WS_EX_APPWINDOW。WS_EX_TOOLWINDOW 会去掉任务栏按钮；而 Windows“显示桌面”只向
// “拥有任务栏按钮的顶层窗口”下发最小化命令，因此无任务栏入口的窗口会被直接跳过——
// 从根本上不最小化、不隐藏、无“隐藏→恢复”闪烁。
// - 仅改 EX 样式，绝不做 SetParent 重定父：SetParent 到桌面 DefView 会让 Qt 顶层窗口
// 变成桌面子窗口，裁剪/坐标错乱导致 Dock 不可见、且破坏折叠按钮等子控件渲染。
// - owner 双保险（GWLP_HWNDPARENT = SHELLDLL_DefView）作为 Win+D/显示桌面按钮的免疫补充：
// 三窗口 owner 化后随桌面显示/隐藏，进一步强化“无任务栏”机制的免疫能力。
// - 普通对话框仅加 WS_EX_NOACTIVATE（保留任务栏，便于被最小化后从任务栏找回），
// 不去掉 WS_EX_APPWINDOW、不挂桌面。
class DesktopImmunityFilter : public QAbstractNativeEventFilter {
public:
    // 构造函数：初始化对象
    explicit DesktopImmunityFilter(QObject* parent = nullptr);
    ~DesktopImmunityFilter() override;
    // 原生事件过滤
    bool nativeEventFilter(const QByteArray& eventType, void* message, long* result) override;

private:
    // 已对桌面挂件设置 WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW 的窗口集合
    QSet<quintptr> m_noActivateApplied;
    // 已对普通对话框设置 WS_EX_NOACTIVATE 的窗口集合
    QSet<quintptr> m_immunized;

    // A2：按 HWND 缓存的窗口分类。消息派发器会对【每一条】取出的消息跑本过滤器
    // （全屏 Dock 上 WM_MOUSEMOVE/WM_NCHITTEST/WM_SETCURSOR 每秒数百条），而分类判定里有
    // 多次 Win32 系统调用（GetWindowThreadProcessId / GetWindowLongPtrW），属于纯重复劳动。
    // 首次遇到某 HWND 做一次完整判定，之后命中缓存即零系统调用。
    // A2 回归加固（2026-09-21）：HWND 会被系统【复用】（快速创建/销毁的窗口——内联重命名
    // 编辑框、临时收纳盒——极可能拿到刚释放的句柄值）。若只按 HWND 做键，新窗口会继承
    // 旧窗口的分类，最严重的后果是：一个复用了“普通顶层窗”句柄的**新编辑框**被判成
    // KindPlainTopLevel → 被加上 WS_EX_NOACTIVATE → 永远拿不到键盘焦点 → 打不进字
    // （正是 2026-09-13「F2 重命名首次后永久失效」那类缺陷的复现路径）。
    // 故缓存必须同时记录该句柄当时的 **QWidget 身份**（QWidget::find 的结果指针），
    // 命中时逐条比对身份；身份变了（句柄被复用）就丢弃陈旧分类重新判定。
    // 身份比对只是一次 QHash 查找，无系统调用，热路径收益不受影响。
    QHash<quintptr, qint8>    m_kindCache;   // HWND → Kind 枚举值（取值见 .cpp 中的 Kind）
    QHash<quintptr, quintptr> m_kindWidget;  // HWND → 当时 QWidget::find(hwnd) 的指针值（0=非 QWidget）
    // 桌面挂件 owner（GWLP_HWNDPARENT）自愈的节流时间戳：explorer 重启是低频事件，
    // 不必每条消息都确认 owner，节流后每 300ms 复核一次即可（高频消息在此窗口内零开销通过）。
    QHash<quintptr, quint32> m_lastAttachTick;
};

#endif // DESKTOPIMMUNITYFILTER_H
