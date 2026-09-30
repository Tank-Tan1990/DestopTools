/*
 * @file desktopimmunityfilter.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "desktopimmunityfilter.h"

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "rawmouse.h"              // 交叉验证 Raw Input 通道是否真的通
#include "lowlevelhookmanager.h"   // 不通时降级回 WH_MOUSE_LL
#endif

#include <QWidget>
#include <QLineEdit>
#include <QDialog>

// 定位桌面图标层 SHELLDLL_DefView（跨 WorkerW 枚举，兼容 explorer 重启后重建）。
// 返回其 HWND，找不到返回 nullptr。
HWND findDesktopDefView() {
    HWND hProgman = FindWindowW(L"Progman", nullptr);
    if (!hProgman) return nullptr;
    // 先看 Progman 直接子级
    HWND hDefView = FindWindowExW(hProgman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (hDefView) return hDefView;
    // 再遍历所有 WorkerW 下的 SHELLDLL_DefView（多显示器/explorer 重启场景）
    HWND hWorker = nullptr;
    while ((hWorker = FindWindowExW(nullptr, hWorker, L"WorkerW", nullptr)) != nullptr) {
        HWND h = FindWindowExW(hWorker, nullptr, L"SHELLDLL_DefView", nullptr);
        if (h) return h;
    }
    return nullptr;
}

// 判断句柄是否仍是桌面图标层（SHELLDLL_DefView）。
// 只用 IsWindow 不够：HWND 会被系统复用，explorer 重启后旧句柄可能已属于别的窗口，
// 误把 owner 设过去后果不可控。故多一次很廉价的 GetClassNameW 校验。
static bool isDefViewWindow(HWND h) {
    if (!h || !IsWindow(h)) return false;
    wchar_t cls[64] = {0};
    return GetClassNameW(h, cls, 64) > 0 && lstrcmpW(cls, L"SHELLDLL_DefView") == 0;
}

// 定位桌面图标层，带进程内缓存。
// 缓存的意义：findDesktopDefView() 每次都要 FindWindowW + 若干次 FindWindowExW 枚举，
// 而调用它的 attachToDesktop() 挂在「每一条」本进程顶层窗口消息上（全屏 Dock 上鼠标
// 移动每秒可产生数百条消息），无条件枚举属于纯重复劳动。
// 失效自愈：句柄被销毁（explorer 重启）时缓存判定失败，此时才重新枚举一次。
static HWND cachedDesktopDefView() {
    static HWND s_defView = nullptr;
    if (isDefViewWindow(s_defView)) return s_defView;   // 命中缓存：零枚举
    s_defView = findDesktopDefView();
    return s_defView;
}

// 把桌面图标层 SHELLDLL_DefView 设为挂件窗口的 owner（GWLP_HWNDPARENT，非 SetParent）。
// owner 关系是 360 桌面助手采用的「桌面挂件归属桌面」双保险：owner 化后窗口随桌面显示/隐藏，
// 且不破坏 Qt 顶层窗口渲染（与 SetParent 变成 child 有本质区别）。
void attachToDesktop(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    const HWND cur = reinterpret_cast<HWND>(GetWindowLongPtrW(hwnd, GWLP_HWNDPARENT));
    // 常态快路径（绝大多数消息走这里）：owner 已指向桌面图标层 → 直接返回，不做任何枚举。
    // 旧实现无条件先跑 findDesktopDefView() 再判 owner，等于把"确认无需改动"也变成一次
    // Progman/WorkerW 枚举，是鼠标移动发涩的隐性来源之一。
    if (isDefViewWindow(cur)) return;
    // owner 为空或已失效（explorer 重启后 DefView 句柄变化）→ 定位一次（带缓存）再挂
    const HWND hDefView = cachedDesktopDefView();
    if (!hDefView || cur == hDefView) return;
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(hDefView));
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

namespace {

// 判断该 QWidget 是否是桌面挂件（Dock / 收纳盒 / 桌面助手）。
// 仅对这些窗口做 WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW（去任务栏、免疫“显示桌面”）。
bool isDesktopWidgetName(QWidget* w) {
    if (!w) return false;
    const QString objName = w->objectName();
    return objName == QStringLiteral("DesktopMirrorWindow") ||
           objName == QStringLiteral("IconGridWindow") ||
           objName == QStringLiteral("AssistantWindow");
}

// 判定「需要真实键盘焦点」的窗口：内联重命名编辑框（QLineEdit 顶层窗口）与各类输入对话框。
// 这些窗口【绝不能】被本过滤器加上 WS_EX_NOACTIVATE：
// 带该样式的窗口**永远无法成为前台窗口**，SetForegroundWindow / SwitchToThisWindow
// 一律被系统拒绝。后果是编辑框拿不到真正的键盘焦点 —— 打字进不去，回车提交的还是
// 未改动的旧名。用户观感就是「F2 / 右键弹框的重命名 失效」，而且：
// · 重启后**第一次可用**：首次编辑时焦点是在本过滤器加样式之前就抢到的；
// · 之后**永久失效**：新建的编辑框从 show() 起就带 NOACTIVATE，焦点再也抢不回来。
// 这正是「程序重启，首次 F2 重命名后 F2 与右键重命名一起失效」的真根因。
bool isInputWidget(QWidget* w) {
    if (!w) return false;
    if (qobject_cast<QLineEdit*>(w)) return true;   // 内联重命名编辑框（无父顶层窗口）
    if (qobject_cast<QDialog*>(w)) return true;     // 设置中心 / 输入框 / 消息框：都需可激活
    return false;
}

// A2：窗口分类。过滤器只需处理本进程的“顶层窗口”：
// 外部进程 / 本进程子控件 / 输入窗口 一律不干预（前两者本就无需处理，后者必须保留可激活语义）；
// 剩下的再按“桌面挂件 / 普通顶层窗”走两条既有通道。
// 该结果按 HWND 缓存，避免对每条消息重复做 GetWindowThreadProcessId + 样式读取 + Qt 侧查找。
enum Kind : qint8 {
    KindUnknown       = 0,   // 未缓存
    KindExternal      = 1,   // 其它进程的窗口
    KindChild         = 2,   // 本进程子控件（不在任务栏、不参与“显示桌面”）
    KindInput         = 3,   // 输入窗口（内联编辑框 / 各种 QDialog）
    KindDesktopWidget = 4,   // 桌面挂件：Dock / 收纳盒 / 桌面助手
    KindPlainTopLevel = 5    // 本进程普通顶层窗口
};

} // namespace

DesktopImmunityFilter::DesktopImmunityFilter(QObject* parent)
    : QAbstractNativeEventFilter()
{
    Q_UNUSED(parent)
}

DesktopImmunityFilter::~DesktopImmunityFilter() = default;

// 原生事件过滤
// 作者：谭征
bool DesktopImmunityFilter::nativeEventFilter(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    Q_UNUSED(eventType)
    MSG* msg = static_cast<MSG*>(message);
    HWND hwnd = msg->hwnd;
    if (!hwnd) return false;

    const quintptr key = reinterpret_cast<quintptr>(hwnd);

    // 窗口销毁时从集合移除，便于对话框等可重复创建的窗口再次处理。
    if (msg->message == WM_DESTROY) {
        m_noActivateApplied.remove(key);
        m_immunized.remove(key);
        m_kindCache.remove(key);
        m_kindWidget.remove(key);
        m_lastAttachTick.remove(key);
        return false;
    }

    // B7：高频鼠标消息**整体放行**，不进分类缓存判断。
    // 背景（鼠标移动卡顿优化）：全屏 Dock 是光标下方最常见的窗口，于是
    // WM_MOUSEMOVE / WM_NCHITTEST / WM_SETCURSOR / WM_MOUSEWHEEL 每秒有数百~上千条经本过滤器；
    // 而下面的 A2 分类缓存路径即便命中，也仍需一次 QWidget::find + 2~3 次 QHash 查找。
    // 这些消息的结论**恒为“不干预”**（免疫语义只与"窗口是不是桌面挂件/输入框"有关，
    // 与鼠标消息无关），所以直接放行、零查找。
    // 注意：这里 return false 不写缓存，只是把“首次建立该 HWND 的分类”推迟到它的下一条
    // 非鼠标消息 —— 窗口创建期必然伴随 WM_NCCREATE/WM_SHOW/WM_SIZE 等消息，语义不变。
    switch (msg->message) {
    case WM_INPUT:
        // Raw Input 的 WM_INPUT 也在本过滤器的视野里（Qt 的消息泵对**所有**消息先过过滤器
        // 再 DispatchMessage），而它每秒最多上千条（鼠标每次移动一条，即使我们在别处
        // 立刻丢弃）。这里必须最先放行 —— 否则每条都要做 GetWindowThreadProcessId +
        // GetWindowLongPtrW 两次系统调用，把 Raw Input 的收益当场吃光。
        // （隐藏接收窗不是 Qt 窗口，本来也判不出分类，只会走 !widgetId 的 return false 分支。）
        return false;
    case WM_MOUSEMOVE:
#ifdef Q_OS_WIN
        // 交叉验证 Raw Input 通道（2026-09-24，原理见 rawmouse.h 的 suspectedDead 注释）：
        // 能走到这里说明系统正在把一个**真实的鼠标移动消息**投递给本程序的 Qt 窗口，
        // 那么改用 Raw Input 接收鼠标按键的通道理应也收到了若干条 WM_INPUT（含纯移动）。
        // 若注册满 1.5 秒后仍是 0 条，说明这条通道在本机不通（被安全软件/沙箱拦截）；
        // 此时若不处理，表现就是 F2 / Delete / 框选 / 双击**静默全失效**且毫无报错，
        // 因此立刻降级回 WH_MOUSE_LL（功能与改造前一致，只是少了性能收益）。
        if (rawMouse().suspectedDead()) lowLevelHooks().fallbackToLegacyMouseHook();
#endif
        return false;
    case WM_NCMOUSEMOVE:
    case WM_MOUSELEAVE:    case WM_NCMOUSELEAVE:
    case WM_NCHITTEST:     case WM_SETCURSOR:
    case WM_MOUSEWHEEL:    case WM_MOUSEHWHEEL:
    case WM_LBUTTONDOWN:   case WM_LBUTTONUP:   case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:   case WM_RBUTTONUP:   case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:   case WM_MBUTTONUP:   case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:   case WM_XBUTTONUP:   case WM_XBUTTONDBLCLK:
        return false;
    default:
        break;
    }

    // A2：分类缓存（含“HWND 复用”防护 —— 见头文件说明）。
    // 命中条件 = 该 HWND 仍代表**同一个 QWidget 身份**；身份变了说明句柄被系统复用，
    // 必须丢弃陈旧分类重新判定，否则新窗口会继承旧窗口的免疫语义。
    // QWidget::find 只是一次 QHash 查找，无系统调用，热路径依然零系统调用。
    const quintptr widgetId = reinterpret_cast<quintptr>(QWidget::find(reinterpret_cast<WId>(hwnd)));
    qint8 kind = KindUnknown;
    {
        const auto it = m_kindCache.constFind(key);
        if (it != m_kindCache.constEnd()) {
            if (m_kindWidget.value(key, 0) == widgetId) {
                kind = it.value();
            } else {                    // 句柄被复用 / 身份变化 → 丢弃全部陈旧状态
                m_kindCache.remove(key);
                m_kindWidget.remove(key);
                m_noActivateApplied.remove(key);
                m_immunized.remove(key);
                m_lastAttachTick.remove(key);
            }
        }
    }

    if (kind == KindUnknown) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);      // 仅处理本进程创建的窗口，避免影响其它程序
        if (pid != GetCurrentProcessId()) {
            kind = KindExternal;
        } else if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) {
            kind = KindChild;                      // 子控件不在任务栏、不参与“显示桌面”，无需处理
        } else if (!widgetId) {
            // 认不出来（该 HWND 已在系统里建好，但 Qt 侧的 WId→QWidget 映射尚未登记）：
            // 此时**绝不能**把它当成“普通顶层窗口”并缓存 —— 若它其实是内联重命名编辑框，
            // 被加上 WS_EX_NOACTIVATE 后将永远拿不到键盘焦点（打不进字、Enter/Esc 收不到）。
            // 正确做法：本条消息不干预、且不写缓存，等后续消息重新判定（那时映射已就绪）。
            return false;
        } else {
            QWidget* w = reinterpret_cast<QWidget*>(widgetId);
            if (isInputWidget(w)) {
                kind = KindInput;
            } else if (isDesktopWidgetName(w)) {
                kind = KindDesktopWidget;
            } else {
                kind = KindPlainTopLevel;
            }
        }
        // KindExternal 不缓存（本进程过滤器几乎见不到外部窗口消息，缓存它只会引入
        // “外部句柄被复用成本进程窗口”的误判面）；其余分类连同 QWidget 身份一起缓存。
        if (kind != KindExternal) {
            m_kindCache.insert(key, kind);
            m_kindWidget.insert(key, widgetId);
        }
    }

    // 外部进程 / 本进程子控件 / 输入窗口：一律不干预。
    // 输入窗口豁免（2026-09-13 定论 —— “F2 / 右键重命名 首次后永久失效”的真根因）：
    // 内联重命名编辑框与设置中心/输入框/消息框都必须保留“可激活”语义，否则它们无法获得
    // 键盘焦点，用户在里面一个字都打不进去（详见 isInputWidget 注释）。这里直接 return，
    // 既不挂桌面、也不加 WS_EX_NOACTIVATE，同时跳过下面的最小化/关闭拦截（对话框本就该能正常关闭）。
    if (kind <= KindInput) return false;

    // 纵深防御：KindPlainTopLevel 也要再确认一次“不是输入窗口”。
    // 分类只在首条消息做一次，而“是否可激活”是**功能性**判据 —— 一旦判错，用户看到的是
    // “打字打不进去 / 重命名永远提交不上”，代价极高。故应用 WS_EX_NOACTIVATE 之前再验一次，
    // 命中则升级缓存为 KindInput。只对“本进程普通顶层窗口”（设置中心/输入对话框等，
    // 数量个位数、消息量低）付这份代价，全屏 Dock 走的是 KindDesktopWidget 分支，不受影响。
    if (kind == KindPlainTopLevel && widgetId) {
        if (isInputWidget(reinterpret_cast<QWidget*>(widgetId))) {
            m_kindCache.insert(key, KindInput);
            return false;
        }
    }

    if (kind == KindDesktopWidget) {
        // owner 双保险：惰性确保 owner 指向桌面 SHELLDLL_DefView。
        // A2：自愈改为节流复核 —— explorer 重启是低频事件，300ms 内确认过就不再重复；
        // 高频消息（鼠标移动等）因此零系统调用通过。
        const quint32 now = GetTickCount();
        const quint32 last = m_lastAttachTick.value(key, 0);
        if ((now - last >= 300) || !m_noActivateApplied.contains(key)) {
            m_lastAttachTick.insert(key, now);
            // owner 双保险：每次复核都对桌面挂件惰性确保 owner 指向桌面 SHELLDLL_DefView
            // （放在 m_noActivateApplied 判断之外，explorer 重启后 DefView 句柄变化可自愈）。
            attachToDesktop(hwnd);
            // A2 回归加固（2026-09-21）：免疫 EX 样式改为**每次复核都重新断言**（每 300ms 一次），
            // 而不是“只做一次并记档”。原因是“显示桌面”/Win+D/Win+M 只跳过**没有任务栏入口**
            // 的窗口：只要三窗口因任何原因丢掉 WS_EX_TOOLWINDOW（或被人为恢复 WS_EX_APPWINDOW），
            // 它们就会被外壳隐藏，且没有任何机制把它们改回来 —— 用户看到的就是
            // “Win+D/Win+M 之后三窗口不见了”。改为周期性复核后最坏 300ms 内自愈。
            // 常态下这一步只是 1 次 GetWindowLongPtrW（样式相符则不写），开销可忽略。
            {
                LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
                const LONG_PTR want = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
                if ((ex & want) != want || (ex & WS_EX_APPWINDOW)) {
                    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, (ex & ~WS_EX_APPWINDOW) | want);
                    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
                }
                m_noActivateApplied.insert(key);
            }
        } else if (msg->message != WM_SYSCOMMAND) {
            return false;   // 刚确认过 owner/样式 → 绝大多数消息在此零开销通过
        }
    }

    // 普通顶层窗口（设置中心等对话框）只做防御式处理：
    // 设置 WS_EX_NOACTIVATE 防止抢焦点，保留任务栏（便于被最小化后从任务栏找回）。
    // 不去掉 WS_EX_APPWINDOW、不挂桌面。
    if (kind == KindPlainTopLevel && !m_immunized.contains(key)) {
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (!(ex & WS_EX_NOACTIVATE)) {
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_FRAMECHANGED | SWP_NOACTIVATE);
        }
        m_immunized.insert(key);
    }

    // 防御其它来源（如 Win+M）通过系统命令触发的最小化/最大化/关闭，
    // 与“无任务栏”机制形成双重保险。
    if (msg->message == WM_SYSCOMMAND) {
        WPARAM cmd = msg->wParam & 0xFFF0;
        if (cmd == SC_MINIMIZE || cmd == SC_MAXIMIZE || cmd == SC_CLOSE) {
            if (result) *result = 0;
            return true;
        }
    }
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return false;
}
