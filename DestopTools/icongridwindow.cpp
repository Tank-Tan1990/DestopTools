/*
 * @file icongridwindow.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "icongridwindow.h"
#include "desktopiconbutton.h"
#include "desktopscanner.h"
#include "fencebox.h"
#include "desktopmirrorwindow.h"   // clampBandZOrder：nativeEvent 提层守卫；raiseAboveBandWindows/ensureLayerAboveBandWindows
#include "editorwatch.h"           // clearOwner：独立顶层窗口不能被 owner 的压底拖着沉下去
#include "theme.h"
#include "thememanager.h"
#include "settingsmanager.h"
#include "categorystore.h"
#include "shellops.h"   // 删除/重命名的唯一落盘通道（与 Dock 图标共用）
#include "diagtrace.h"  // 临时：快捷键/重命名链路诊断日志
#include "lowlevelhookmanager.h"   // A1：进程级共享低级键鼠钩子（全进程 2 个钩子，Dock/收纳盒共用）
#include "windowsnap.h"            // 自动对齐（格子对齐）+ 窗口互相磁吸
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <QLineEdit>
#include <QLabel>
#include <QStyle>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QKeyEvent>
#include <QApplication>
#include <QScreen>
#include <QMenu>
#include <QCursor>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QCloseEvent>
#include <QDrag>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDragMoveEvent>
#include <QSet>
#include <QFileInfo>
#include <QDir>
#include <QTimer>
#include <QUrl>
#include <QVariant>   // settingChanged 广播的 payload（构造期订阅）
#include <climits>
#include <QDebug>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

// E1：钩子 / gate 诊断日志的开关短路。
// DiagTrace::log 的实参在【调用点】就已构造（QStringLiteral(...).arg(...)），即使 log() 内部
// 因开关关闭而立即返回，字符串构造的代价也已经付出；而这些判据点全部位于低级钩子回调
// 路径上（回调超时会被 Windows 静默摘钩），所以统一用本宏包一层：开关关闭时连参数都不构造。
// 打开方式见 diagtrace.h（仅 INI 的 [Diagnostics] traceEnabled 一项开关）。
#define GRID_GATE_TRACE(expr) do { if (DiagTrace::enabled()) DiagTrace::log(expr); } while (0)

// 前向声明：全屏收纳跨屏铺满用的多屏可用区并集（定义见 setFencesMode 附近）
static QRect allScreensAvailableGeometry();

// 图标条目的稳定键：归一化分隔符 + 小写。与选中接力 / 顺序持久化 / FenceBox 内的复用判定同口径 ——
// 三处必须同源，否则会出现"明明还是同一个图标，却被当成删了旧的又新建了一个"。
static QString gridItemKey(const QString& path) {
    return QDir::fromNativeSeparators(path).toLower();
}

// 「点空白取消选中」的独立全局过滤器
// 为什么必须是独立对象、而不能让 IconGridWindow 自己挂 qApp（2026-09-24 崩溃定位）：
// 问题3（多选后点别处取消不掉）需要"本窗口之外的鼠标按下"这个信号，最自然的做法是
// qApp->installEventFilter(this)。但 IconGridWindow::eventFilter 同时承担了大量**窗口内**
// 逻辑 —— 尤其 MouseMove 分支里那句 `w->mapTo(this, me->pos())`（边缘光标）。一旦挂到
// qApp 上，这些逻辑就会对**本进程所有窗口**（Dock / 别的收纳盒 / 设置中心）的部件执行；
// 对象在析构/重建（deleteLater）时，`mapTo` 解引用悬垂的 `this`/`watched` → 读取野指针
// 0x28 → 启动阶段事件循环刚起就崩（VEH 实锤：异常地址 Qt5Widgets.dll + 0x41420 即
// QWidget::mapTo，调用栈 IconGridWindow::eventFilter+0x60F）。
// 故把这一全局职责剥离成下面这个极简过滤器：只响应 MouseButtonPress、只做一次落点判断、
// 绝不碰 watched 的深层成员，天然无悬垂风险。真实判断逻辑在 handleGlobalPressCancel。
class PressOutsideCancelFilter : public QObject {
    IconGridWindow* m_owner = nullptr;
public:
    explicit PressOutsideCancelFilter(IconGridWindow* owner) : m_owner(owner) {}
protected:
    bool eventFilter(QObject* /*watched*/, QEvent* event) override {
        // 只在"左键/右键按下"这一种事件上做点事，其余一律放行（返回 false，绝不吞事件）。
        if (event->type() != QEvent::MouseButtonPress) return false;
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton && me->button() != Qt::RightButton) return false;
        if (m_owner) m_owner->handleGlobalPressCancel(me->globalPos());
        return false;
    }
};

// 网格/收纳盒的系统级快捷键（F2 重命名 / Delete·D 删除）
// 为什么必须走 WH_KEYBOARD_LL 而不是 Qt 的 keyPressEvent：
// 本窗口在 showEvent 里被设为 WS_EX_NOACTIVATE（Win+D / “显示桌面”免疫所必需），
// 于是它**永远不会成为活动窗口**，整个窗口树都拿不到键盘焦点 ——
// keyPressEvent / QShortcut / 菜单加速键在它身上从不触发。这与 Dock 图标完全同源，
// 因此采取同一策略：判定只看“物理按键 + 屏幕坐标”，与窗口激活状态、焦点归属、
// Qt 事件路由统统无关。
// 作用域（防误删的关键）：两个条件必须**同时**成立才认这一键属于网格 ——
// ① 最近一次鼠标按下落在某个网格窗口上（WH_MOUSE_LL 维护）；
// ② 光标当前也在那个网格窗口上。
// 只看①：点完收纳盒再切到资源管理器按 Delete，会被本钩子抢走并删掉桌面文件；
// 只看②：在别处点过、只是把鼠标扫过收纳盒再按 Delete，同样会误删。
// 另外，Ctrl/Shift/Alt/Win 组合一律放行（属于系统或别的程序）。
#ifdef Q_OS_WIN
// A1：不再各自安装低级钩子，改为注册到进程级 LowLevelHookManager。
// 消费者回调签名与管理器约定一致：键盘返回 true = 已消费（吞键）；鼠标无返回值（不吞事件）。
bool gridKeyConsumer(int vk, void* ctx);                        // 前置声明
void gridMouseConsumer(WPARAM msg, LPARAM lParam, void* ctx);   // 前置声明

static bool  s_gridHooksActive = false;                 // 是否已注册到共享钩子管理器（句柄由管理器持有）
static QVector<QPointer<IconGridWindow>> s_gridWindows; // 已登记（已显示）的网格窗口
static QPointer<IconGridWindow> s_gridHot;              // 最近一次鼠标按下落在哪个网格窗口上
// 共享管理器的注册 token：用 s_gridWindows 的地址（文件级静态变量，地址唯一且稳定）。

// 屏幕点（物理像素）落在哪个已登记的网格窗口上。
// 用 Win32 命中测试而不是 QApplication::widgetAt：钩子回调里【绝不能做重活】，
// 低级钩子回调超时会被 Windows 静默摘除（句柄仍非空，于是后续 install 误以为“已装”而跳过）。
// 作者：谭征
static IconGridWindow* gridWindowAtScreenPoint(POINT pt)
{
    const HWND h = WindowFromPoint(pt);
    if (!h) return nullptr;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return nullptr;
    const HWND root = GetAncestor(h, GA_ROOT);
    if (!root) return nullptr;
    for (const auto& p : s_gridWindows) {
        if (p && reinterpret_cast<HWND>(p->winId()) == root) return p.data();
    }
    return nullptr;
}

// 自愈式安装：每次窗口显示、以及每一次网格内交互都先卸后装。
// 原因：低级钩子回调超时会被系统静默摘除而句柄仍非空，若不重装就会“某次之后 F2/Delete 全部失效”。
// C：重活期间临时摘钩的嵌套计数。>0 时 ensureGridHooks() 一律不装钩 ——
// 低级钩子是【系统同步回调到本 GUI 线程】的，线程被重活钉住时全系统鼠标都会等我们返回。
static int s_gridSuspendDepth = 0;

static void ensureGridHooks()
{
    if (s_gridSuspendDepth > 0) return;   // 暂停中：不装钩（重活结束由 resumeGridHooks 统一恢复）
    if (s_gridWindows.isEmpty()) return;
    // A1：注册到进程级共享管理器。对同一 token 重复 subscribe 等价于「刷新回调 + 先卸后装」，
    // 自愈语义（规避低级钩子被系统静默摘除）与原先完全一致，但全进程只维持 1 套钩子。
    lowLevelHooks().subscribe(&s_gridWindows, &gridKeyConsumer, &gridMouseConsumer, nullptr);
    // A1 回归修复（2026-09-21）：闸门取“仍在注册名单里”，**绝不能**取 active()。
    // 共享钩子是全进程一起摘的（m_suspendDepth 为全进程嵌套计数），而两侧 resume 分别调用：
    // mainwindow 的 suspend(Dock)→suspend(Grid)→resume(Grid)→resume(Dock) 序下，
    // 本函数在 resume(Grid) 里被调用时计数才从 2 减到 1（Dock 还没 resume）→ reinstall()
    // 被跳过 → active() 为 false → 旧码把 s_gridHooksActive 清成 false
    // → hotkeyGateOk() 第 680 行直接拒绝 → 收纳盒的 F2 / Delete / 重命名【全部静默失效】
    // （启动 2.5s 后那次 refreshDesktop 兜底重扫就会触发，用户看到的就是“收纳盒快捷键坏了”）。
    s_gridHooksActive = lowLevelHooks().registered(&s_gridWindows);
    DiagTrace::log(QStringLiteral("[grid] ensureGridHooks key=%1 mouse=%2 active=%3 windows=%4 hot=%5 consumers=%6")
                       .arg((qulonglong)lowLevelHooks().keyHook(), 0, 16)
                       .arg((qulonglong)lowLevelHooks().mouseHook(), 0, 16)
                       .arg(DiagTrace::boolStr(s_gridHooksActive))
                       .arg(s_gridWindows.size())
                       .arg((qulonglong)s_gridHot.data(), 0, 16)
                       .arg(lowLevelHooks().consumerCount()));
}

static void releaseGridHooks()
{
    s_gridHooksActive = false;
    // A1：从共享管理器注销；最后一个消费者注销时管理器自动 UnhookWindowsHookEx。
    lowLevelHooks().unsubscribe(&s_gridWindows);
}

static void registerGridWindow(IconGridWindow* w)
{
    if (!w) return;
    for (const auto& p : s_gridWindows) if (p == w) { ensureGridHooks(); return; }
    s_gridWindows.append(QPointer<IconGridWindow>(w));
    s_gridHot = w;              // 刚显示：先当作热窗口，用户点它之后的第一个 F2 即可生效
    DiagTrace::log(QStringLiteral("[grid] registerGridWindow windows=%1 hot=%2")
                       .arg(s_gridWindows.size()).arg((qulonglong)s_gridHot.data(), 0, 16));
    ensureGridHooks();
}

static void unregisterGridWindow(IconGridWindow* w)
{
    for (int i = s_gridWindows.size() - 1; i >= 0; --i) {
        if (s_gridWindows[i] == w || s_gridWindows[i].isNull()) s_gridWindows.removeAt(i);
    }
    if (s_gridHot == w) s_gridHot = nullptr;
    if (s_gridWindows.isEmpty()) releaseGridHooks();
    else ensureGridHooks();
}

// 每次网格内交互都调用：把“热窗口”指向本窗口并重装钩子（自愈）。
// 必须在这里也设 s_gridHot：钩子可能刚被系统摘除，那一次点击没有被钩子看到，
// 若只依赖钩子记录，紧随其后的 F2/Delete 会因“最近按下不在网格上”而失效。
static void armGridHotkey(IconGridWindow* w)
{
    if (!w) return;
    s_gridHot = w;
    DiagTrace::log(QStringLiteral("[grid] armGridHotkey hot=%1").arg((qulonglong)s_gridHot.data(), 0, 16));
    // ⑤补漏：任何一次交互都【立即】登记并装钩（registerGridWindow 幂等：已登记则先卸后装）。
    // 启动期把首次登记延后到了启动重活之后（见 showEvent），这里保证「用户一旦操作，
    // 快捷键立刻可用」——延后不会带来任何功能缺失。
    registerGridWindow(w);
}

// 鼠标消费者：只维护“最近一次按下落在哪个网格窗口上”。不吞事件、不做重活。
void gridMouseConsumer(WPARAM wParam, LPARAM lParam, void* /*ctx*/)
{
    // A1：nCode 已由 LowLevelHookManager 过滤；鼠标消费者从不吞事件。
    {
        switch (wParam) {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN: {
            const MSLLHOOKSTRUCT* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            if (ms) {
                IconGridWindow* w = gridWindowAtScreenPoint(ms->pt);
                // 只有“真正落在本程序网格窗口上”才更新热窗口；未命中时还要再看一眼：
                // 若该点属于本进程的其它窗口（最典型的是无父顶层的重命名编辑框），
                // 用户仍在操作我们的 UI，保持原热窗口不变 —— 否则点一下编辑框就会把热窗口清空，
                // 提交后必须先回网格再点一次 F2 才重新可用（“F2 时灵时不灵”的一种来源）。
                if (w) {
                    s_gridHot = w;
                } else {
                    const HWND h = WindowFromPoint(ms->pt);
                    DWORD pid = 0;
                    if (h) GetWindowThreadProcessId(h, &pid);
                    // 点到的若是「桌面外壳面」（Progman/WorkerW/SHELLDLL_DefView），那不是“别的程序”，
                    // 而是 Dock 全屏取代桌面后的“桌面面”（Dock 的 nativeHitTestIsDock 也这么判）。
                    // 若在这里把它当外部进程清空热窗口，用户在 Dock 上点一下空白（空白处穿透到
                    // Progman），就会让收纳盒的 F2/Delete 连带失效（本 bug 的直接来源）。
                    // 故仅当点到的确实是外部程序窗口时才清空热窗口，桌面外壳面保持原热窗口不变。
                    if (pid != GetCurrentProcessId()) {
                        wchar_t cls[64] = {0};
                        const bool shellSurface = h && GetClassNameW(h, cls, 63)
                            && (lstrcmpiW(cls, L"Progman") == 0
                                || lstrcmpiW(cls, L"WorkerW") == 0
                                || lstrcmpiW(cls, L"SHELLDLL_DefView") == 0);
                        if (!shellSurface) s_gridHot = nullptr;
                    }
                }
                // 点空白 = 结束重命名 + 清空选中（兜底通道）。钩子回调里【绝不做重活】：
                // 只投递 0ms 请求，几何判定与清空都在事件循环里做。
                if (w && (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN)) {
                    const long nx = ms->pt.x, ny = ms->pt.y;   // 低级钩子给的是物理像素
                    QPointer<IconGridWindow> p(w);
                    QTimer::singleShot(0, w, [p, nx, ny]() {
                        if (p) p->blankPressFromHook(QPoint(static_cast<int>(nx), static_cast<int>(ny)));
                    });
                }
            }
            break;
        }
        default:
            break;
        }
    }
}

// 键盘消费者：只做“识别 + 投递 0ms 请求”，立即返回。返回 true = 已消费（吞掉该键）。
bool gridKeyConsumer(int vk, void* /*ctx*/)
{
    // A1：nCode 与“仅 KEYDOWN/SYSKEYDOWN”过滤已由 LowLevelHookManager 统一完成。
    if (vk != VK_F2 && vk != VK_DELETE && vk != 'D') return false;
    GRID_GATE_TRACE(QStringLiteral("[grid] hook keydown vk=%1 active=%2 windows=%3")
                        .arg(vk).arg(DiagTrace::boolStr(s_gridHooksActive))
                        .arg(s_gridWindows.size()));
    // 带修饰键的组合属于系统或别的程序（Ctrl+C / Alt+F4 / Win+D…），一律放行。
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000)
        || (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        || (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) {
        return false;
    }
    IconGridWindow* target = nullptr;
    for (const auto& p : s_gridWindows) {
        if (p && p->hotkeyGateOk()) { target = p.data(); break; }
    }
    if (target) {
        QPointer<IconGridWindow> ptr(target);
        if (vk == VK_F2) {
            QTimer::singleShot(0, target, [ptr]() { if (ptr) ptr->triggerRenameShortcut(); });
        } else {
            QTimer::singleShot(0, target, [ptr]() { if (ptr) ptr->triggerDeleteShortcut(); });
        }
        return true;   // 吞掉：该键已由收纳盒消费，不再落到前台程序
    }
    return false;
}
#else
static void ensureGridHooks() {}
static void releaseGridHooks() {}
static void registerGridWindow(IconGridWindow*) {}
static void unregisterGridWindow(IconGridWindow*) {}
static void armGridHotkey(IconGridWindow*) {}
#endif // Q_OS_WIN


IconGridWindow::IconGridWindow(QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint) {
    setObjectName(QStringLiteral("IconGridWindow"));
    setWindowTitle(QStringLiteral("桌面整理"));
    setAttribute(Qt::WA_TranslucentBackground, true);
    resize(960, 720);
    // 最小高度：标题栏 38 + 分类栏 34 + 一行图标区（边距 10+10 + 按钮 84）= 176
    setMinimumSize(480, 176);

    // 窗口外框：经 Theme::boxChrome 走「盒子显示边框」开关（Appearance/boxBorder，默认显示）。
    // 关闭时不画那圈 1px 线框，背景/圆角不变。
    setStyleSheet(Theme::boxChrome(Theme::windowStyle()));

    setupUi();

    // 恢复“查看”视图档位（大/中等/小图标 + 列表）。默认“中等图标”。
    // 主窗口与各收纳盒窗口共用同一份持久化设置，因此这里对所有窗口一律读取。
    {
        SettingsManager sm;
        const QVariant saved = sm.loadValue(QStringLiteral("IconGridWindow/viewMode"),
                                            int(ViewMedium));
        int mode = saved.toInt();
        if (mode < ViewLarge || mode > ViewList) mode = ViewMedium;   // 脏数据兜底
        m_viewMode = mode;
        if (mode != ViewList) m_lastGridMode = mode;
        m_gridColumns = (mode == ViewList) ? 1 : 8;
        updateViewToggleButton();
    }

    // 恢复锁定按钮状态（仅主窗口；收纳盒窗口不共享此状态）
    if (!m_isBoxWindow) {
        SettingsManager sm;
        m_locked = sm.loadValue(QStringLiteral("IconGridWindow/locked"), false).toBool();
        updateLockButton();
    }

    // 分类标签切换方式（设置中心「分区标签切换」）。主窗口与各收纳盒窗口共用同一份设置，
    // 故对所有窗口一律读取；标签按钮尚未创建，先只记模式，buildTabs() 末尾再下发到按钮。
    loadTagSwitchMode();
    // 光在构造/showEvent 读取不够：设置中心改完「分区标签切换」时本窗口**正在显示**，
    // showEvent 不会再发一次 → 界面停在旧模式，用户看到「选了没反应」。
    // 订阅全局设置变更广播，改完就用（只认本项 key，其它设置一律不理）。
    connect(ThemeManager::instance(), &ThemeManager::settingChanged, this,
            [this](const QString& key, const QVariant&) {
                if (key == QLatin1String("Appearance/tagSwitchMode")) reloadTagSwitchMode();
                else if (key == QLatin1String("Appearance/menuLabelShow")) reloadMenuRevealMode();
                // 「在快捷方式图标上显示箭头」（2026-09-22）：箭头是取图那一刻合成进位图的，
                // 开关变了必须清缓存 + 让当前窗口里的图标重新取图，否则旧位图一直挂到下次重建。
                // （boxBorder / boxRound 两项不在这里处理：它们写进样式串，由 ThemeManager
                // reloadAppearanceFlags() 追加的 themeChanged 经登记表重刷，覆盖面更全。）
                else if (key == QLatin1String("Appearance/shortcutArrow")) reloadShortcutArrowMode();
            });

    // 头部菜单按钮显示方式（设置中心「分区菜单标签显示」）。与标签切换方式同款：对所有窗口一律读取，
    // 构造期先按持久化值定初始显隐（悬停显示 → 先隐藏，等鼠标移入再出现）。
    loadMenuRevealMode();
    applyMenuRevealVisibility();

    // 记录初始展开尺寸，作为展开时的回退尺寸
    m_expandedWidth = width();
    m_expandedHeight = height();

    // 折叠/展开状态不在构造期应用，统一由 MainWindow::loadLayout 在恢复几何后处理，
    // 避免用默认 960x720 覆盖用户已修改的展开尺寸。

    // 窗口化默认使用单分类网格视图（顶部分类标签 + 下方图标网格），m_boxView=false。
    // 进入全屏收纳时自动把 m_boxView 置 true 并切换为平铺盒子；退出全屏后恢复为网格视图。
    // 注意：滚动区内容的切换（setWidget(m_fencesContainer)）不在构造期进行，
    // 而是改由 buildFences() 在首次填充数据、窗口几何就绪后执行，
    // 避免构造阶段窗口树未稳定时重定父引发的偶发访问越界崩溃。

    // —— 应用级「点空白取消选中」过滤器（独立对象，见 PressOutsideCancelFilter）——
    // 为什么不能用本窗口 this 直接挂 qApp（2026-09-24 崩溃定位）：本窗口 eventFilter 里
    // 塞了大量窗口内逻辑（MouseMove 的 mapTo / 标签拖拽 / 图标拖放），一旦挂到 qApp 上，
    // 这些逻辑就会对**本进程所有窗口**（Dock / 别的收纳盒 / 设置中心）的部件执行；对象在
    // 析构 / 重建（deleteLater）时，`mapTo` 解引用悬垂的 this / watched → 读取野指针 →
    // 启动阶段事件循环刚起就崩（VEH 实锤：Qt5Widgets.dll+0x41420 = QWidget::mapTo）。
    // 故改用独立极简过滤器：只响应 MouseButtonPress、只做一次落点判断，天然无悬垂风险。
    m_pressCancelFilter = new PressOutsideCancelFilter(this);
    qApp->installEventFilter(m_pressCancelFilter);
}

// C：重活期间摘/装网格这一套低级钩子。刻意放在 #endif 之后定义，
// 使其在两种平台下都存在（Win 走真实实现，#else 走空实现）。
// 作者：谭征
void IconGridWindow::suspendGridHooks() {
#ifdef Q_OS_WIN
    if (s_gridSuspendDepth++ > 0) return;   // 已在暂停中：只加计数
    lowLevelHooks().suspend();              // A1：全进程摘钩（管理器内部同样是嵌套计数）
    s_gridHooksActive = false;
    DiagTrace::log(QStringLiteral("[grid] suspendGridHooks 摘钩（重活期间不再拦截键鼠）"));
#endif
}

// 只有「确实处于暂停中」才恢复；未暂停时是空操作，
// 避免把 unregisterGridWindow（已随窗口销毁释放）之后的状态又给装回去。
// 作者：谭征
void IconGridWindow::resumeGridHooks() {
#ifdef Q_OS_WIN
    if (s_gridSuspendDepth == 0) return;     // 未暂停 → 什么都不做
    if (--s_gridSuspendDepth > 0) return;    // 仍有外层暂停
    lowLevelHooks().resume();                // A1：管理器计数归零 → 恢复共享钩子
#endif
    ensureGridHooks();
}

IconGridWindow::~IconGridWindow() {
    // ① 摘掉应用级「点空白取消选中」过滤器（独立对象）：本对象即将析构，qApp 不能再回调它。
    // qApp 是 QCoreApplication::instance() 的宏 —— 若 QApplication 已先于本窗口析构（异常退出
    // 路径），它就是空指针；加这层判断，绝不在析构路径上解引用一个可能已失效的实例指针。
    if (qApp && m_pressCancelFilter) {
        qApp->removeEventFilter(m_pressCancelFilter);
        delete m_pressCancelFilter;   // 该过滤器无父对象，需显式释放
        m_pressCancelFilter = nullptr;
    }
    // ② 落盘兜底：把防抖期间尚未写出的「当前选中分类」立刻写掉。
    // 漏了这一步，用户在 400ms 窗口内关闭窗口，这次切换就永远没落盘。
    flushCurrentCategory();
    // ③ 注销快捷键登记并（在最后一个窗口消失时）释放钩子：窗口已不存在却仍持钩，
    // 会让 F2/Delete 继续被一个不可见的窗口消费 —— 用户会看到“某个程序里按 Delete 没反应”。
    unregisterGridWindow(this);
}

// 图标选中集合（Windows 桌面语义）

// 作者：谭征
void IconGridWindow::clearSelection() {
    if (m_selection.isEmpty() && !m_anchor) return;
    for (DesktopIconButton* b : m_selection) {
        if (b) b->setSelected(false);
    }
    m_selection.clear();
    m_anchor = nullptr;
}

// 独立全局过滤器（PressOutsideCancelFilter）的落点：跨窗口「点空白取消选中」。
// 与 blankPressFromHook 的分工：blankPressFromHook 只处理「本窗口矩形内」的空白点击；
// 本函数处理「本进程任意窗口」的鼠标按下（点 Dock / 别的收纳盒 / 设置中心 / 桌面空白）。
// 规则：本窗口有选中项时取落点 —— 命中图标（要自己处理 Ctrl 加选 / Shift 范围选，这里先清
// 会让加选失效）或改名框（改名途中不能抹掉）或菜单，一律放行；其余任何落点都清空选中。
// 作者：谭征
void IconGridWindow::handleGlobalPressCancel(const QPoint& globalPos) {
    if (m_selection.isEmpty() && !m_anchor) return;
    if (isEditingAnyIcon() || m_menuOpen) return;
    QWidget* hit = QApplication::widgetAt(globalPos);
    bool onIcon = false;
    // 向上追溯：命中的可能是图标内部的子对象或中间容器，只看最内层不够
    for (QWidget* w = hit; w; w = w->parentWidget()) {
        if (qobject_cast<DesktopIconButton*>(w)) { onIcon = true; break; }
    }
    if (!onIcon && !qobject_cast<QLineEdit*>(hit) && !qobject_cast<QMenu*>(hit)) {
        DiagTrace::log(QStringLiteral("[grid] clearSelection by press-outside n=%1")
                           .arg(m_selection.size()));
        clearSelection();
    }
}

// —— 选中集合管理（支持 Ctrl/Shift 多选 + 画框多选），贴近 Windows 桌面 ——
// 作者：谭征
void IconGridWindow::selectExclusive(DesktopIconButton* b) {
    if (!b) return;
    for (DesktopIconButton* s : m_selection) {
        if (s && s != b) s->setSelected(false);
    }
    m_selection.clear();
    m_selection.append(b);
    b->setSelected(true);
    m_anchor = b;
}

// 切换选中
// 作者：谭征
void IconGridWindow::toggleSelection(DesktopIconButton* b) {
    if (!b) return;
    if (m_selection.contains(b)) {
        b->setSelected(false);
        m_selection.removeAll(b);
        if (m_anchor == b) m_anchor = m_selection.isEmpty() ? nullptr : m_selection.last();
    } else {
        m_selection.append(b);
        b->setSelected(true);
        m_anchor = b;
    }
}

// 范围选择
// 作者：谭征
void IconGridWindow::rangeSelect(DesktopIconButton* b) {
    if (!b || !m_gridLayout) return;
    // 按网格布局的实际位置取“用户看到的顺序”——行列顺序就是视觉顺序。
    QVector<DesktopIconButton*> ordered;
    ordered.reserve(m_gridLayout->count());
    for (int i = 0; i < m_gridLayout->count(); ++i) {
        QLayoutItem* li = m_gridLayout->itemAt(i);
        if (!li) continue;
        if (auto* ib = qobject_cast<DesktopIconButton*>(li->widget())) ordered.append(ib);
    }
    int from = m_anchor ? ordered.indexOf(m_anchor) : -1;
    int to = ordered.indexOf(b);
    if (to < 0) return;
    if (from < 0) from = to;
    if (from > to) qSwap(from, to);
    clearSelection();
    for (int i = from; i <= to; ++i) {
        ordered[i]->setSelected(true);
        m_selection.append(ordered[i]);
    }
    m_anchor = b;
}

// 当前选中目标
// 作者：谭征
DesktopIconButton* IconGridWindow::currentSelectionTarget() const {
    // F2 的目标 = 锚点（最后一次单击选中的项）；多选时同样只改这一项（Windows 亦然）。
    if (m_anchor && m_selection.contains(m_anchor) && m_anchor->canRename()) return m_anchor;
    for (int i = m_selection.size() - 1; i >= 0; --i) {
        if (m_selection[i] && m_selection[i]->canRename()) return m_selection[i];
    }
    return nullptr;
}

// —— 图标删除 / 重命名：落盘 + 本地持久化数据同步 ——
// 作者：谭征
void IconGridWindow::onIconSelectionRequested(DesktopIconButton* b, bool additive, bool range) {
    if (!b) return;
    armGridHotkey(this);      // 自愈式重装快捷键钩子，并把“热窗口”指向本窗口
    commitActiveRename();     // 点别的图标 = 结束正在进行的重命名（Windows 语义）
    if (range)        rangeSelect(b);
    else if (additive) toggleSelection(b);
    else               selectExclusive(b);
    DiagTrace::log(QStringLiteral("[grid] onIconSelectionRequested sel=%1 name=%2")
                       .arg(m_selection.size())
                       .arg(b->item().displayName));
}

// 响应图标选中for菜单
// 作者：谭征
void IconGridWindow::onIconSelectionForMenu(DesktopIconButton* b) {
    if (!b) return;
    armGridHotkey(this);
    commitActiveRename();
    // Windows 语义：右键未选中项会先把它选中（再弹菜单）；右键已选中项保持整组选中。
    if (!m_selection.contains(b)) selectExclusive(b);
}

// 响应图标删除请求信号
// 作者：谭征
void IconGridWindow::onIconDeleteRequested(DesktopIconButton* b) {
    if (!b) return;
    armGridHotkey(this);
    // 右键菜单“删除”：该项在选中集合里就删整组（Windows 桌面同样如此），否则只删它。
    QList<DesktopIconButton*> targets;
    if (m_selection.contains(b)) targets = m_selection;
    else targets.append(b);
    deleteIcons(targets);
}

// 删除 / 重命名：落盘 + 本地持久化数据同步

// 作者：谭征
void IconGridWindow::removePathsFromMemory(const QStringList& paths) {
    QSet<QString> norm;
    for (const QString& p : paths) {
        if (!p.isEmpty()) norm.insert(QDir::fromNativeSeparators(p).toLower());
    }
    if (norm.isEmpty()) return;
    for (auto it = m_allItems.begin(); it != m_allItems.end(); ++it) {
        QVector<DesktopItem>& vec = it.value();
        for (int i = vec.size() - 1; i >= 0; --i) {
            const QString key = QDir::fromNativeSeparators(vec[i].sourcePath).toLower();
            if (!key.isEmpty() && norm.contains(key)) vec.removeAt(i);
        }
    }
}

// 删除图标
// 作者：谭征
void IconGridWindow::deleteIcons(const QList<DesktopIconButton*>& targets) {
    if (targets.isEmpty()) return;
    // 拷贝一份：调用方可能直接把 m_selection 传进来，而本函数中途会 clearSelection()。
    const QList<DesktopIconButton*> list = targets;
    commitActiveRename();        // 先结束命名，再按最新 sourcePath 收集（改名会换路径）

    QStringList paths;           // 绝对路径（正斜杠）
    QList<DesktopIconButton*> toHide;
    QSet<QString> seen;
    for (DesktopIconButton* b : list) {
        if (!b || !b->canDelete()) continue;
        const QString abs = QFileInfo(b->item().sourcePath).absoluteFilePath();
        const QString key = QDir::fromNativeSeparators(abs).toLower();
        if (key.isEmpty() || seen.contains(key)) continue;
        seen.insert(key);
        paths << QDir::fromNativeSeparators(abs);
        toHide.append(b);
    }
    if (paths.isEmpty()) return;

    // 落盘走 ShellOps —— 与 Dock 图标**同一条通道**（进回收站、可还原）。
    // 失败或用户在系统确认框里取消 → 一个都不删，且下面任何本地状态都不许改。
    if (!ShellOps::deleteToRecycleBin(paths, (void*)winId())) return;

    // —— 本地数据 + 持久化数据的同步（不做就会出现“删了但重启后又回来 / 点不动”） ——
    // ① 分类库（path → 分类）：不删记录就会留下指向已消失文件的孤儿，重启后它还在盒子里。
    for (const QString& p : paths) CategoryStore::clearCategory(p);
    // ② 本窗口内存分类
    removePathsFromMemory(paths);
    // ③ 图标顺序持久化（键为 sourcePath）：删完按当前集合重存一次，即自然剔除被删项。
    if (!m_isBoxWindow) {
        const QStringList cats = m_allItems.keys();
        for (const QString& cat : cats) saveItemOrder(cat);
    }
    clearSelection();
    // 立刻隐藏被删按钮：若等重建（需等事件循环）才消失，用户会看到“删了还在”的闪烁。
    for (DesktopIconButton* b : toHide) {
        if (b) b->hide();
    }
    // ④ 延后重建：本函数可能由右键菜单命令返回后同步调用，栈上仍有正在使用的按钮引用。
    QTimer::singleShot(0, this, [this]() { rebuildGrid(); });
    // ⑤ 通知 MainWindow：清理 m_allItems / 各收纳盒数据、重算 Dock 隐藏集合、刷新所有窗口。
    emit itemsDeleted(paths);
}

// migrate条目顺序路径
// 作者：谭征
void IconGridWindow::migrateItemOrderPath(const QString& category,
                                          const QString& oldPath, const QString& newPath) {
    if (category.isEmpty() || oldPath.isEmpty() || newPath.isEmpty()) return;
    SettingsManager sm;
    QStringList order = sm.loadValue(itemOrderKey(category)).toStringList();
    if (order.isEmpty()) return;
    const QString oldNorm = QDir::fromNativeSeparators(oldPath);
    bool changed = false;
    for (QString& s : order) {
        if (QDir::fromNativeSeparators(s).compare(oldNorm, Qt::CaseInsensitive) == 0) {
            s = newPath;
            changed = true;
        }
    }
    if (changed) sm.saveValue(itemOrderKey(category), order);
}

// 响应图标renamed
// 作者：谭征
void IconGridWindow::onIconRenamed(const QString& oldPath, const QString& newPath) {
    if (oldPath.isEmpty() || newPath.isEmpty()) return;
    const QString oldNorm = QDir::fromNativeSeparators(oldPath);
    if (oldNorm.compare(QDir::fromNativeSeparators(newPath), Qt::CaseInsensitive) == 0) return;

    // ① 本窗口内存：路径、显示名与图标全部换新（保留分类归属与顺序位置）。
    // 显示名必须以**磁盘上的真实文件名**为准 —— displayName 只是绘制用副本，
    // 一旦它与真实路径不同步，编辑框会预填旧名、右键菜单也会按旧名去解析 pidl。
    QString cat;
    for (auto it = m_allItems.begin(); it != m_allItems.end(); ++it) {
        for (DesktopItem& item : it.value()) {
            if (QDir::fromNativeSeparators(item.sourcePath)
                    .compare(oldNorm, Qt::CaseInsensitive) != 0) continue;
            item.sourcePath = newPath;
            item.shellPath = newPath;
            item.targetPath = newPath;   // 下面按 isShortcut 重新解析
            item.displayName = QFileInfo(newPath).fileName();
            // 改后缀会改变类型与图标（.txt → .jpg 等），缓存必须失效后重取，
            // 否则网格重建时新按钮会拿到旧图标（loadIconNow 只在 icon 为空时才取图）。
            item.icon = QIcon();
            item.systemImageIndex = -1;
            item.isShortcut = QFileInfo(newPath).suffix().compare(QLatin1String("lnk"),
                                                                 Qt::CaseInsensitive) == 0
                              && DesktopScanner::isRealShortcut(newPath);
            // targetPath 必须是解析后的真实目标（取图只认目标文件，理由见 DesktopScanner::effectiveTarget）
            item.targetPath = DesktopScanner::effectiveTarget(item.sourcePath, item.isShortcut);
            cat = it.key();
        }
    }
    // ② 分类库记录迁移：旧记录必须删掉，否则留下指向旧路径的孤儿
    // （盒子里的项点不动、重启后该项按“查不到记录”被重新规则归类）。
    const QString stored = CategoryStore::categoryOf(oldPath);
    if (!stored.isEmpty()) {
        CategoryStore::setCategory(newPath, stored);
        CategoryStore::clearCategory(oldPath);
    }
    // ③ 图标顺序持久化以 sourcePath 为键：不迁移就会丢掉该项在网格里的位置。
    if (!m_isBoxWindow && !cat.isEmpty()) {
        migrateItemOrderPath(cat, oldPath, newPath);
        saveItemOrder(cat);
    }
    // ④ 通知 MainWindow 同步 m_allItems / 各收纳盒数据 / Dock 隐藏集合，并刷新所有窗口。
    emit itemRenamed(oldPath, newPath);
}

// 响应上下文菜单可见变化信号
// 作者：谭征
void IconGridWindow::onContextMenuVisibleChanged(bool visible) {
    m_menuOpen = visible;
    // 菜单收起 = 解冻：统一补做被推迟的网格工作（**优先原地重排**，不适用才全量重建）。
    if (!visible) flushPendingGridWork();
}

// 内联编辑开始/结束。编辑期冻结重建的理由与菜单期同源：
// 重建会 deleteLater 掉“正在被编辑的那个按钮”，而按钮析构时会顺手销毁它的编辑框
// （编辑框是无父顶层窗口，必须由按钮显式清理）—— 用户看到的就是输入框一闪即逝。
// 编辑结束后再补做这次被推迟的重建，保证改名后的数据照样刷新到位。
// 作者：谭征
void IconGridWindow::onIconEditingChanged(bool editing) {
    // 开始编辑时影子指针已在连接点按“具体按钮”设置好了。
    // 不用 sender()：全屏盒子视图的信号要经 FenceBox 转发，sender() 会变成 FenceBox 而非按钮。
    if (editing) return;
    // 结束编辑（含按钮析构时的补发）：确认它已不在编辑态后再清影子。
    if (m_editingBtn && !m_editingBtn->isEditing()) m_editingBtn = nullptr;
    if (m_menuOpen) return;               // 菜单期优先由 onContextMenuVisibleChanged 收尾
    flushPendingGridWork();               // 解冻后补做：优先原地重排，不适用才全量重建
}

// 图标拖拽进行中标志（由 DesktopIconButton 在 QDrag::exec() 前后调用）。
// 为什么必须冻结重建：FenceBox（及其中的图标按钮）就是拖拽源，重建会把它 hide +
// setParent(nullptr) + deleteLater —— 源控件一消失，Windows 的 DoDragDrop 当场结束，
// QDrag::exec() 提前返回，此时 QCursor::pos() 只是“中止那一刻”的光标位置（通常正好在
// 盒子边缘）。使用者看到的现象就是“鼠标刚离开收纳盒，图标就落在盒边”，而不是落在松手处。
// 拖拽结束后补做被推迟的那次重建（与菜单期 / 编辑期同一套 m_pendingRebuild 机制）。
// 作者：谭征
void IconGridWindow::setIconDragInProgress(bool on) {
    if (m_iconDragInProgress == on) return;
    m_iconDragInProgress = on;
    if (on) return;
    if (m_menuOpen || isEditingAnyIcon()) return;   // 仍被其它冻结源挡着，交由它们收尾
    // 「盒内交换两个图标的位置」走的正是这条：drop 事件是在 QDrag::exec() 内部同步派发的，
    // 那一刻 m_iconDragInProgress 仍为 true ⇒ 重排请求被记账推迟到这里才补做。
    // 改走 flushPendingGridWork() 后，补做只做**原地重排**（一个控件都不重建）；
    // 若仍照旧无条件 rebuildGrid()，就会把全部盒子的全部按钮全量重建一遍。
    flushPendingGridWork();
}

// 快捷键（F2 / Delete·D）落点

// 本窗口内的全部图标按钮：窗口化网格视图挂在 m_gridContainer 下，
// 全屏/常驻盒子视图挂在各个 FenceBox 下。
// 以前只扫 m_gridContainer —— 于是全屏盒子视图下的图标对窗口完全“隐形”：
// 编辑态识别不到（重建不被冻结、F2 上下文判定失灵）、点空白也认不出图标。
// 作者：谭征
QVector<DesktopIconButton*> IconGridWindow::allIconButtons() const {
    QVector<DesktopIconButton*> out;
    auto collect = [&out](QWidget* root) {
        if (!root) return;
        const QList<DesktopIconButton*> found = root->findChildren<DesktopIconButton*>();
        for (DesktopIconButton* b : found) if (b) out.append(b);
    };
    collect(m_gridContainer);
    for (FenceBox* box : m_fenceBoxes) collect(box);
    return out;
}

// 重建后仍“活着且已被布局”的按钮集合（用于选中接力）。与 allIconButtons() 的区别：
// allIconButtons 用 findChildren 扫描容器，会把上一代刚 takeAt/deleteLater 的旧按钮也捞出来
// （它们仍挂在父控件下，要等事件循环才销毁），选中一旦挂到旧按钮就是野指针。
// 这里网格视图只扫 m_gridLayout（旧按钮已被 takeAt 移出布局），盒子视图只扫 m_fenceBoxes
// （旧盒子在 buildFences 里已 setParent(nullptr) 并从 m_fenceBoxes 移除）。
// 作者：谭征
QVector<DesktopIconButton*> IconGridWindow::freshGridButtons() const {
    QVector<DesktopIconButton*> out;
    if (m_fencesMode || m_boxView) {
        for (FenceBox* box : m_fenceBoxes) {
            const QList<DesktopIconButton*> found = box->findChildren<DesktopIconButton*>();
            for (DesktopIconButton* b : found) if (b) out.append(b);
        }
    } else if (m_gridLayout) {
        for (int i = 0; i < m_gridLayout->count(); ++i) {
            QLayoutItem* li = m_gridLayout->itemAt(i);
            if (!li) continue;
            if (auto* ib = qobject_cast<DesktopIconButton*>(li->widget())) out.append(ib);
        }
    }
    return out;
}

// 若不处理，用户松手后直接按 F2/Delete 会因 “empty selection / last click not on dock” 失效。
// 作者：谭征
void IconGridWindow::selectIconByShellPathAndArm(const QString& shellPath) {
    if (shellPath.isEmpty()) return;
    const QString key = QDir::fromNativeSeparators(shellPath).toLower();
    DesktopIconButton* target = nullptr;
    const QVector<DesktopIconButton*> btns = freshGridButtons();
    for (DesktopIconButton* b : btns) {
        if (!b) continue;
        if (QDir::fromNativeSeparators(b->item().shellPath).toLower() == key) { target = b; break; }
    }
    DiagTrace::log(QStringLiteral("[grid] selectIconByShellPathAndArm found=%1 name='%2'")
                       .arg(DiagTrace::boolStr(target != nullptr))
                       .arg(target ? target->item().displayName : shellPath));
    if (target) selectExclusive(target);
    // 松手落在本窗口面上 = 一次“与本窗口的交互”：把热窗口指向本窗口并重装钩子，
    // 让随后的 F2/Delete 立即命中（拖动期间按下发生在 Dock，s_gridHot 未指向本窗口）。
    armGridHotkey(this);
}

// 判断editingany图标
// 作者：谭征
bool IconGridWindow::isEditingAnyIcon() const {
    // ① 影子指针（O(1)，不会因“待删除的旧按钮”或“挂在 FenceBox 下的图标”而漏判）
    if (m_editingBtn && m_editingBtn->isEditing()) return true;
    // ② 兜底扫描：只在 F2/Delete/D 这几个键上调用，开销可忽略；宁可慢一点也不能漏。
    const QVector<DesktopIconButton*> btns = allIconButtons();
    for (DesktopIconButton* b : btns) {
        if (b && b->isEditing()) return true;
    }
    return false;
}

// 直接遍历 m_buttons 判定 isEditing()，不依赖 m_editingBtn 这一路追踪，避免极端时序下漏提交。
// 作者：谭征
void IconGridWindow::commitActiveRename() {
    const QVector<DesktopIconButton*> btns = allIconButtons();
    for (DesktopIconButton* b : btns) {
        if (b && b->isEditing()) b->commitInlineRename();
    }
}

// 钩子上下文判定：全部条件成立才认为“这一键是发给本收纳盒的”。
// E1：判据顺序按“代价从低到高、常见拒绝原因靠前”重排（各判据无副作用，
// AND 语义完全等价）。原顺序把 isEditingAnyIcon()（内含 findChildren 全量扫描 + QVector 分配）
// 排在最前，于是“没有选中项”这种最常见的拒绝路径反而要先扫一遍整棵控件树。
// 作者：谭征
bool IconGridWindow::hotkeyGateOk() const {
#ifdef Q_OS_WIN
    if (!s_gridHooksActive) { GRID_GATE_TRACE(QStringLiteral("[grid] gate FAIL: hooks inactive")); return false; }
    if (!isVisible()) { GRID_GATE_TRACE(QStringLiteral("[grid] gate FAIL: !isVisible")); return false; }
    // —— 两条 O(1) 且最常见的拒绝原因前置，尽早短路 ——
    if (m_selection.isEmpty()) { GRID_GATE_TRACE(QStringLiteral("[grid] gate FAIL: empty selection")); return false; }
    if (s_gridHot != this) { GRID_GATE_TRACE(QStringLiteral("[grid] gate FAIL: not hot window")); return false; }
    if (m_menuOpen) { GRID_GATE_TRACE(QStringLiteral("[grid] gate FAIL: m_menuOpen (stuck?)")); return false; }
    // —— 重型判定（含控件树全量扫描）放到最后：只有“看起来真能响应”时才付这份代价 ——
    if (isEditingAnyIcon()) { GRID_GATE_TRACE(QStringLiteral("[grid] gate FAIL: editing")); return false; }
    // 光标当前也必须在本窗口上（“焦点跟随鼠标”语义）。两条必须同时成立，见文件头说明。
    // 这里用【纯几何矩形判定】(GetWindowRect + PtInRect)，绝不能再走 gridWindowAtScreenPoint 的
    // WindowFromPoint 命中测试。根因：内联重命名编辑框是无父顶层窗口，commit 后 hide+deleteLater
    // 是异步的；用户“点 A → F2 编辑 → 紧接着点 B → 再按 F2”时，第二次 F2 的 WindowFromPoint
    // 会间歇性命中尚未彻底销毁的编辑框（或其中间态层），GetAncestor(GA_ROOT) 拿到编辑框 HWND，
    // 不匹配任何 grid 窗口 → 误判“光标不在窗口面” → F2/Delete 静默失效（实测约 13% 失败率，
    // 且每次失败都伴随一次 commit）。几何矩形判定与 z 序/编辑框残留/HWND 重建无关：光标落在
    // 本窗口矩形内即认为“仍在收纳盒面上”，移出矩形才放行，与 Windows 桌面语义一致。
    POINT cur = {};
    if (!GetCursorPos(&cur)) { GRID_GATE_TRACE(QStringLiteral("[grid] gate FAIL: GetCursorPos")); return false; }
    RECT rc = {};
    const HWND self = reinterpret_cast<HWND>(winId());
    const bool hit = (self != nullptr) && GetWindowRect(self, &rc) && (PtInRect(&rc, cur) != FALSE);
    if (!hit) GRID_GATE_TRACE(QStringLiteral("[grid] gate FAIL: cursor not over window"));
    return hit;
#else
    return false;
#endif
}

// 进程内的 WH_KEYBOARD_LL 统一接管（见 icongridwindow.cpp）；下面三个入口即钩子的落点。
// 作者：谭征
void IconGridWindow::triggerRenameShortcut() {
    DiagTrace::log(QStringLiteral("[grid] triggerRenameShortcut IN sel=%1 editing=%2 fences=%3 menu=%4 vis=%5")
                       .arg(m_selection.size()).arg(DiagTrace::boolStr(isEditingAnyIcon()))
                       .arg(DiagTrace::boolStr(m_fencesMode)).arg(DiagTrace::boolStr(m_menuOpen))
                       .arg(DiagTrace::boolStr(isVisible())));
    // 这里**不再重跑 hotkeyGateOk()**。钩子在回调里已按当时的上下文判过一次；
    // 二次判定会引入新的否定条件（s_gridHot 影子、GetCursorPos 命中），只要这 0ms 之间
    // 有任何一次抖动，F2 就会被静默丢弃 —— 用户观感即“F2 时灵时不灵 / 某次之后彻底失效”。
    // 只保留不会误伤的必要校验。
    if (!isVisible() || m_menuOpen) return;
    if (isEditingAnyIcon()) {
        // 已在编辑：F2 视作“回到编辑框”（幂等，不再新开一个）
        if (m_editingBtn) m_editingBtn->startInlineRename();
        return;
    }
    DesktopIconButton* target = currentSelectionTarget();
    DiagTrace::log(QStringLiteral("[grid] triggerRenameShortcut target=%1")
                       .arg(target ? target->item().displayName : QStringLiteral("<null>")));
    if (!target) return;
    target->startInlineRename();
}

// trigger删除快捷方式
// 作者：谭征
void IconGridWindow::triggerDeleteShortcut() {
    if (!isVisible() || m_menuOpen) return;
    if (isEditingAnyIcon()) return;
    if (m_selection.isEmpty()) return;
    deleteIcons(m_selection);
}

// 屏幕坐标（Qt 逻辑坐标）命中的网格图标；没有则返回 nullptr。
// 注意本控件就是 72×84 的完整单元格（图标+文字居中铺满），所以“落在按钮矩形内”
// 与“落在图标可视区上”在这里等价，不需要 Dock 侧那套可视矩形收窄。
// 作者：谭征
DesktopIconButton* IconGridWindow::iconAtGlobal(const QPoint& globalPos) const {
    // 用“按钮自己的全局矩形”判定：不依赖它挂在哪一层容器下，
    // 网格视图（m_gridContainer）与全屏盒子视图（FenceBox）都成立。
    const QVector<DesktopIconButton*> btns = allIconButtons();
    for (DesktopIconButton* b : btns) {
        if (!b || !b->isVisible()) continue;
        const QRect r(b->mapToGlobal(QPoint(0, 0)), b->size());
        if (r.contains(globalPos)) return b;
    }
    return nullptr;
}

// 系统级鼠标钩子的“点空白”入口。
// 为什么不能只靠 Qt 的 mousePressEvent：空白处的按下要穿过 QScrollArea/容器才可能冒泡到
// 本窗口，链路并不可靠（与 Dock 上“点空白清不掉选中”同源）。这里用纯几何判定，
// 与 Qt 事件是否到达无关，且幂等 —— 两条通道谁到谁生效、都到也不重复出事。
// 只做两件事：结束正在进行的重命名 + 清空选中（Windows 桌面语义：点空白即取消选中）。
// 作者：谭征
void IconGridWindow::blankPressFromHook(const QPoint& physScreenPt) {
    if (!isVisible() || m_menuOpen) return;
    qreal dpr = devicePixelRatioF();
    if (dpr <= 0) dpr = 1.0;
    // 物理 → Qt 逻辑：除以设备像素比。DPI 虚拟化（dpr=1，钩子给虚拟化坐标）与
    // PerMonitorV2（钩子给物理坐标、dpr=缩放比）两种情况下都成立。
    const QPoint logical(qRound(physScreenPt.x() / dpr), qRound(physScreenPt.y() / dpr));
    const QRect winRect(mapToGlobal(QPoint(0, 0)), size());
    if (!winRect.contains(logical)) return;
    if (iconAtGlobal(logical)) return;   // 点在图标上：交给该图标自身的选中/菜单逻辑
    commitActiveRename();
    clearSelection();
}

// 初始化ui
// 作者：谭征
void IconGridWindow::setupUi() {
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // 红框内分类标签按钮禁用右键弹框
    setContextMenuPolicy(Qt::NoContextMenu);

    // 与桌面助手（SidePanelWidget）保持一致的玻璃卡片容器
    auto* card = new QFrame(this);
    card->setFrameShape(QFrame::NoFrame);
    card->setStyleSheet(Theme::boxChrome(Theme::panelStyle()));
    auto* cardShadow = new QGraphicsDropShadowEffect(card);
    cardShadow->setBlurRadius(24);
    cardShadow->setColor(Theme::shadow());
    cardShadow->setOffset(0, 4);
    card->setGraphicsEffect(cardShadow);
    m_card = card;
    m_cardShadow = cardShadow;

    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    rootLayout->addWidget(card);

    // 第一行：顶部标题/操作栏（左侧当前模块徽章 + 右侧视图/操作按钮）
    m_headerBar = new QWidget(card);
    m_headerBar->setFixedHeight(Theme::titleBarHeight());
    // 自动对齐（2026-09-24）：标题栏悬停提示说明吸附规则 + Alt 逃生口（行为无 UI 按钮，靠此可发现）。
    m_headerBar->setToolTip(WindowSnap::hintText());
    m_headerBar->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 rgba(17,26,46,1.0),stop:1 rgba(17,26,46,1.0));"
        "border-top-left-radius: 16px; border-top-right-radius: 16px;"
    )));

    // 主题色/透明度联动：重绘窗口/面板/标题栏样式并同步窗口不透明度，
    // 实现「改主题色 → 全部界面颜色联动；改透明度 → 全部界面透明度联动」。
    auto applyTheme = [this, card]() {
        // 全屏收纳模式下根窗口与卡片均需透明，让壁纸透出来；否则沿用玻璃样式。
        // 非全屏分支经 Theme::boxChrome：受「盒子显示边框」开关控制（关闭时窗口与卡片都不画线框）。
        this->setStyleSheet(m_fencesMode
                            ? QStringLiteral("background: transparent; border: none;")
                            : Theme::boxChrome(Theme::windowStyle()));
        card->setStyleSheet(m_fencesMode
                            ? QStringLiteral("background: transparent; border: none;")
                            : Theme::boxChrome(Theme::panelStyle()));
        if (m_headerBar) m_headerBar->setStyleSheet(Theme::applyTokens(QStringLiteral(
            "background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 %1,stop:1 %1);"
            "border-top-left-radius: 16px; border-top-right-radius: 16px;")
            .arg(Theme::windowBgString(1.0))
        ));
        // 分类栏 + 图标区两块**常驻**线框：随主题色/透明度/「盒子使用圆角」一起重刷。
        // （它们不再依赖 card 样式传播，所以 card 的 boxChrome 分支变不动它们 —— 这正是需求。）
        if (m_categoryBar) m_categoryBar->setStyleSheet(Theme::categoryStripStyle());
        if (m_gridScroll) {
            m_gridScroll->viewport()->setObjectName(QStringLiteral("iconGridViewport"));
            m_gridScroll->viewport()->setStyleSheet(Theme::iconAreaStyle());
        }
        // 标题徽章 + 顶部工具按钮 + 分类标签按钮：均为构造时按当时主题色设定，需重绘
        if (m_titleBadge) m_titleBadge->setStyleSheet(Theme::applyTokens(QStringLiteral(
            "QWidget { background: %1; border-radius: 6px; border: 1px solid rgba(34,211,238,%2); }")
            .arg(Theme::windowBgString(1.0))
            .arg(Theme::alphaF(0.12), 0, 'f', 2)
        ));
        for (QToolButton* b : {m_lockBtn, m_viewToggleBtn, m_moreBtn, m_closeBtn, m_addBtn, m_menuBtn}) {
            if (b) b->setStyleSheet(Theme::toolButtonStyle());
        }
        // addBtn 带有下拉菜单，主题刷新后会重置样式表，必须再次隐藏 Qt 默认菜单三角，
        // 否则十字架图标会与下拉三角形叠加。
        if (m_addBtn) {
            QString addBtnStyle = m_addBtn->styleSheet();
            if (!addBtnStyle.contains(QStringLiteral("menu-indicator"))) {
                addBtnStyle.append(QStringLiteral(" QToolButton::menu-indicator { image: none; width: 0px; }"));
                m_addBtn->setStyleSheet(addBtnStyle);
            }
        }
        if (m_tabLayout) {
            for (int i = 0; i < m_tabLayout->count(); ++i) {
                QWidget* w = m_tabLayout->itemAt(i)->widget();
                if (w) w->setStyleSheet(Theme::tabButtonStyle());
            }
        }
        // 插入指示线跟随主题色
        if (m_insertMarker) {
            m_insertMarker->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 1px;")
                .arg(ThemeManager::instance()->accentColor().name(QColor::HexRgb)));
        }
    };
    // E：登记到 ThemeManager 刷新表 —— 主题色与背景透明度**两类变化都走这里**：
    // 背景 alpha 由 bgAlphaF 写进上面的样式串（windowStyle/panelStyle/windowBgString），
    // 因此不再需要单独的「整窗不透明度」通道（它会把头部圆角与文字一起淡化，与本轮需求冲突）。
    ThemeManager::instance()->registerThemeTarget(this, applyTheme);
    auto* headerLayout = new QHBoxLayout(m_headerBar);
    headerLayout->setContentsMargins(10, 4, 10, 4);
    headerLayout->setSpacing(8);

    // 左侧当前模块标识：小图标 + 文字，会随选中分类同步更新
    auto* titleBadge = new QWidget(m_headerBar);
    m_titleBadge = titleBadge;
    titleBadge->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QWidget { background: rgba(17,26,46,1.0); border-radius: 6px; border: 1px solid rgba(34,211,238,0.12); }"
    )));
    auto* badgeLayout = new QHBoxLayout(titleBadge);
    badgeLayout->setContentsMargins(6, 2, 8, 2);
    badgeLayout->setSpacing(4);

    auto* shortcutIcon = new QLabel(titleBadge);
    shortcutIcon->setFixedSize(16, 16);
    shortcutIcon->setScaledContents(true);
    shortcutIcon->setPixmap(Theme::icon("home").pixmap(16, 16));
    shortcutIcon->setStyleSheet(QStringLiteral("background: transparent; border: none;"));

    m_badgeLabel = new QLabel(QStringLiteral("快捷方式"), titleBadge);
    m_badgeLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; font-weight: bold; background: transparent; border: none;"));

    badgeLayout->addWidget(shortcutIcon);
    badgeLayout->addWidget(m_badgeLabel);

    // 右侧视图/操作按钮
    m_lockBtn = makeViewButton(QStringLiteral("锁"), QStringLiteral("点击锁定窗口"));
    m_lockBtn->setIcon(Theme::icon("unlock"));
    m_lockBtn->setIconSize(QSize(16, 16));
    m_lockBtn->setText(QString());
    m_viewToggleBtn = makeViewButton(QStringLiteral("视"), QStringLiteral("切换为列表视图"));
    m_viewToggleBtn->setIcon(Theme::icon("view_grid"));
    m_viewToggleBtn->setIconSize(QSize(16, 16));
    m_viewToggleBtn->setText(QString());
    auto* addBtn = makeViewButton(QStringLiteral("加"), QStringLiteral("添加"));
    m_addBtn = addBtn;
    addBtn->setIcon(Theme::icon("add"));
    addBtn->setIconSize(QSize(16, 16));
    addBtn->setText(QString());
    addBtn->setMenu(createAddMenu());
    addBtn->setPopupMode(QToolButton::InstantPopup);
    // 左侧红框内十字架按钮：隐藏 Qt 默认菜单下拉三角，避免与十字架图标重叠
    {
        QString addBtnStyle = addBtn->styleSheet();
        addBtnStyle.append(QStringLiteral(" QToolButton::menu-indicator { image: none; width: 0px; }"));
        addBtn->setStyleSheet(addBtnStyle);
    }
    auto* menuBtn = makeViewButton(QStringLiteral("菜"), QStringLiteral("菜单"));
    m_menuBtn = menuBtn;
    menuBtn->setIcon(Theme::icon("menu"));
    menuBtn->setIconSize(QSize(16, 16));
    menuBtn->setText(QString());
    m_moreBtn = makeViewButton(QStringLiteral("更"), QStringLiteral("展开/收起"));
    m_moreBtn->setIcon(QIcon(Theme::collapsePixmap(m_gridCollapsed)));
    m_moreBtn->setIconSize(QSize(14, 14));
    m_moreBtn->setText(QString());
    m_moreBtn->setFixedSize(Theme::headerButtonSize());
    m_closeBtn = makeViewButton(QStringLiteral("关"), QStringLiteral("关闭窗口"));
    m_closeBtn->setIcon(Theme::icon("close"));
    m_closeBtn->setIconSize(QSize(16, 16));
    m_closeBtn->setText(QString());
    m_closeBtn->setVisible(false);
    connect(m_lockBtn, &QToolButton::clicked, this, &IconGridWindow::toggleLock);
    connect(m_viewToggleBtn, &QToolButton::clicked, this, &IconGridWindow::toggleViewMode);
    connect(menuBtn, &QToolButton::clicked, this, &IconGridWindow::menuRequested);
    connect(m_moreBtn, &QToolButton::clicked, this, &IconGridWindow::toggleGrid);
    // 「收起后，鼠标移动到标题自动展开」（Appearance/autoExpandOnHover，2026-09-22）：
    // 折叠按钮需要被本窗口过滤 Enter 事件（见 eventFilter）。用 Enter 而非 HoverEnter ——
    // 前者是 Qt 无条件派发的真实进入事件，不依赖 WA_Hover（HoverEnter 需要该属性才发，
    // 若某天按钮样式表里没有 :hover 规则就会静默失效）。
    m_moreBtn->installEventFilter(this);
    connect(m_closeBtn, &QToolButton::clicked, this, &IconGridWindow::closeRequested);

    headerLayout->addWidget(titleBadge);
    headerLayout->addStretch();
    headerLayout->addWidget(m_lockBtn);
    headerLayout->addWidget(m_viewToggleBtn);
    headerLayout->addWidget(addBtn);
    headerLayout->addWidget(menuBtn);
    headerLayout->addWidget(m_moreBtn);
    headerLayout->addWidget(m_closeBtn);

    // 第二行：分类标签栏（仿截图红框内容，放到第二行显示）
    m_categoryBar = new QWidget(card);
    m_categoryBar->setObjectName(QStringLiteral("iconGridCategoryBar"));
    m_categoryBar->setContextMenuPolicy(Qt::NoContextMenu);
    m_categoryBar->setFixedHeight(34);
    // 常驻线框（2026-09-22）：**不要**改回裸 `background: 透明;`。
    // 裸规则会让本控件退回"靠 card 样式传播"的旧机制 —— 「盒子显示边框」一关，
    // 这条分类栏的外框就跟着消失（用户实测）。见 Theme::categoryStripStyle() 注释。
    m_categoryBar->setStyleSheet(Theme::categoryStripStyle());
    auto* categoryLayout = new QHBoxLayout(m_categoryBar);
    categoryLayout->setContentsMargins(10, 4, 10, 4);
    categoryLayout->setSpacing(3);

    m_tabBar = new QWidget(m_categoryBar);
    m_tabBar->setStyleSheet(QStringLiteral("background: transparent;"));
    m_tabBar->setAcceptDrops(true);
    m_tabLayout = new QHBoxLayout(m_tabBar);
    m_tabLayout->setContentsMargins(0, 0, 0, 0);
    m_tabLayout->setSpacing(3);
    m_tabLayout->addStretch();

    categoryLayout->addWidget(m_tabBar, 1);

    // 拖拽插入位置指示线：覆盖在 tabBar 之上，不纳入布局，鼠标事件穿透到下方 tabBar
    m_insertMarker = new QFrame(m_tabBar);
    m_insertMarker->setFrameShape(QFrame::NoFrame);
    m_insertMarker->setFixedWidth(2);
    m_insertMarker->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_insertMarker->raise();
    m_insertMarker->hide();

    // 图标网格
    m_gridScroll = new QScrollArea(card);
    m_gridScroll->setWidgetResizable(true);
    m_gridScroll->setFrameShape(QFrame::NoFrame);
    m_gridScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_gridScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // 去除滚动区自身及 viewport 的边框/轮廓，避免在图标区外残留圆角矩形线框；
    // 不覆盖 QScrollBar 样式，保留全局滑块（handle）外观。
    m_gridScroll->setStyleSheet(QStringLiteral(
        "QScrollArea { background: transparent; border: none; outline: none; }"
        "QScrollArea::viewport { background: transparent; border: none; outline: none; }"
    ));

    // 图标区（viewport）自身的**常驻**线框：与分类栏同源问题（见 Theme::iconAreaStyle()）。
    // 必须给 viewport 直接设样式 —— 上面 `QScrollArea::viewport` 那条不是 Qt 认得的
    // 子控件选择器（viewport 只是个普通 QWidget），它一直在"看起来写了但不生效"的状态：
    // 过去 viewport 的框实际来自 card 样式传播，所以「盒子显示边框」一关就没了。
    m_gridScroll->viewport()->setObjectName(QStringLiteral("iconGridViewport"));
    m_gridScroll->viewport()->setStyleSheet(Theme::iconAreaStyle());

    m_gridContainer = new QWidget(m_gridScroll);
    m_gridContainer->setStyleSheet(QStringLiteral("background: transparent; border: none; outline: none;"));
    m_gridContainer->setAcceptDrops(true);
    m_gridLayout = new QGridLayout(m_gridContainer);
    m_gridLayout->setSpacing(18);   // 间隔 = 图标宽度(72) / 4
    m_gridLayout->setContentsMargins(10, 10, 10, 10);
    m_gridScroll->setWidget(m_gridContainer);

    // 分类页缓存的隐藏宿主：暂存「已加载过的其它分类」的图标按钮（保活但不显示）。
    // 刻意挂在 this 而不是 m_gridContainer 下：既不被网格布局接管，也不会被
    // allIconButtons() 的 findChildren(m_gridContainer) 扫到（缓存按钮不该参与 F2/Delete 判定）。
    m_pageCacheHost = new QWidget(this);
    m_pageCacheHost->setObjectName(QStringLiteral("gridPageCacheHost"));
    m_pageCacheHost->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    m_pageCacheHost->hide();

    // 全屏收纳（Fences）模式：承载所有分类盒子的容器（默认不挂到 scroll，进入模式时才切换）。
    // 这里刻意不使用布局管理器——盒子由用户自由拖动/缩放，几何按分类持久化（真·Fences 行为），
    // 仅在没有保存几何时由 relayoutFences() 做一次流式自动排布。
    m_fencesContainer = new QWidget(this);
    m_fencesContainer->setStyleSheet(QStringLiteral("background: transparent;"));
    m_fencesContainer->setObjectName(QStringLiteral("fencesContainer"));
    m_fencesContainer->hide();   // 未进入全屏模式前不显示

    // 全屏模式下，顶栏被隐藏，提供一个浮动在右上角的菜单按钮作为唯一入口
    m_fencesMenuBtn = new QToolButton(this);
    m_fencesMenuBtn->setIcon(Theme::icon("menu"));
    m_fencesMenuBtn->setIconSize(QSize(16, 16));
    m_fencesMenuBtn->setFixedSize(30, 30);
    m_fencesMenuBtn->setStyleSheet(Theme::toolButtonStyle());
    m_fencesMenuBtn->setVisible(false);
    m_fencesMenuBtn->raise();
    connect(m_fencesMenuBtn, &QToolButton::clicked, this, &IconGridWindow::menuRequested);

    // 全屏模式下顶栏（含桌面助手的搜索框）都不可见，这里提供一个浮动搜索框，
    // 进入全屏时自动聚焦，直接敲键盘即可跨分类筛选图标；Esc 清空。
    m_fencesSearchEdit = new QLineEdit(this);
    m_fencesSearchEdit->setPlaceholderText(QStringLiteral("搜索桌面图标"));
    m_fencesSearchEdit->setClearButtonEnabled(true);
    Theme::applyLineEditStyle(m_fencesSearchEdit);
    m_fencesSearchEdit->setVisible(false);
    m_fencesSearchEdit->installEventFilter(this);   // 接管 Esc 清空
    connect(m_fencesSearchEdit, &QLineEdit::textChanged, this, &IconGridWindow::setSearchText);

    // 网格内拖拽时的“将插入到此处”竖直指示线：覆盖在 grid 之上，鼠标事件穿透到下方图标
    m_gridInsertMarker = new QFrame(m_gridContainer);
    m_gridInsertMarker->setFrameShape(QFrame::NoFrame);
    m_gridInsertMarker->setFixedWidth(3);
    m_gridInsertMarker->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_gridInsertMarker->raise();
    m_gridInsertMarker->setStyleSheet(QStringLiteral("background-color: #2f7bff; border-radius: 1px;"));
    m_gridInsertMarker->hide();

    cardLayout->addWidget(m_headerBar);
    cardLayout->addWidget(m_categoryBar);
    cardLayout->addWidget(m_gridScroll, 1);

    // 让窗口自身及所有子控件都开启鼠标跟踪，并通过事件过滤器感知悬停位置（子控件覆盖整窗，
    // 不开启跟踪则无按键时收不到 mouseMove，无法在边缘悬停时切换光标）
    setMouseTracking(true);
    enableChildMouseTracking(this);

    // 多显示器热插拔：全屏收纳模式下，屏幕增减时重新铺满虚拟桌面并重排盒子。
    auto relayoutOnScreensChanged = [this]() {
        if (m_fencesMode) {
            setGeometry(allScreensAvailableGeometry());
            positionFencesOverlay();
            relayoutFences();
        }
    };
    connect(qApp, &QGuiApplication::screenAdded, this, relayoutOnScreensChanged);
    connect(qApp, &QGuiApplication::screenRemoved, this, relayoutOnScreensChanged);

    // 初始按当前主题色/透明度应用一次：必须放在 setupUi 末尾，
    // 确保所有控件（m_tabLayout/分类标签按钮等）已创建，避免对未初始化指针做样式重绘导致启动崩溃。
    applyTheme();
}

// make视图按钮
// 作者：谭征
QToolButton* IconGridWindow::makeViewButton(const QString& text, const QString& tip) {
    auto* btn = new QToolButton(this);
    btn->setText(text);
    btn->setFixedSize(24, 24);
    btn->setStyleSheet(Theme::toolButtonStyle());
    btn->setToolTip(tip);
    return btn;
}

// 创建添加菜单
// 作者：谭征
QMenu* IconGridWindow::createAddMenu() {
    auto* menu = new QMenu(this);
    Theme::applyMenuStyle(menu);
    menu->setFixedWidth(160);
    QAction* newCategory = menu->addAction(Theme::icon("new_category"), QStringLiteral("新建分类"));
    QAction* newBox = menu->addAction(Theme::icon("new_box"), QStringLiteral("新建收纳盒"));
    connect(newCategory, &QAction::triggered, this, &IconGridWindow::requestNewCategory);
    connect(newBox, &QAction::triggered, this, &IconGridWindow::requestNewBox);
    return menu;
}

// 两份分类数据是否"在界面上会呈现成同一个样子"：分类键集合、每类条目的**顺序**、以及每条的关键属性。
// 绝不能比 QUuid id：DesktopItem 构造时 createUuid()，每次扫描桌面都是全新 UUID，按 id 比永远"不相等"，
// 早退形同不存在。也绝不要比 QIcon（同一文件的图标可能因异步加载时机不同而暂态不等）。
static bool sameGridItemSet(const QMap<QString, QVector<DesktopItem>>& a,
                            const QMap<QString, QVector<DesktopItem>>& b) {
    if (a.size() != b.size()) return false;
    auto ia = a.cbegin();
    auto ib = b.cbegin();
    for (; ia != a.cend(); ++ia, ++ib) {
        if (ia.key() != ib.key()) return false;          // QMap 迭代按 key 升序，两侧可逐项对齐
        const QVector<DesktopItem>& va = ia.value();
        const QVector<DesktopItem>& vb = ib.value();
        if (va.size() != vb.size()) return false;
        for (int i = 0; i < va.size(); ++i) {
            if (va[i].sourcePath != vb[i].sourcePath) return false;
            if (va[i].displayName != vb[i].displayName) return false;
            if (va[i].shellPath != vb[i].shellPath) return false;
            if (va[i].isShortcut != vb[i].isShortcut) return false;
            if (va[i].isSpecial != vb[i].isSpecial) return false;
            if (va[i].systemImageIndex != vb[i].systemImageIndex) return false;
        }
    }
    return true;
}

// 条目内容是否等同。必须逐字段比 —— DesktopItem **没有 operator==**，
// 直接写 `QVector<DesktopItem> == QVector<DesktopItem>` 会编译失败（它要求元素可比较）。
// 刻意不比 id（每次重建都会换）与 icon（QIcon 无可比性，且图标变化由别处负责失效）。
static bool sameItemContent(const DesktopItem& a, const DesktopItem& b) {
    return a.sourcePath       == b.sourcePath
        && a.displayName      == b.displayName
        && a.shellPath        == b.shellPath
        && a.targetPath       == b.targetPath
        && a.category         == b.category
        && a.launchCommand    == b.launchCommand
        && a.isShortcut       == b.isShortcut
        && a.isExecutable     == b.isExecutable
        && a.isSpecial        == b.isSpecial
        && a.systemImageIndex == b.systemImageIndex;
}
// 逐项（顺序敏感）比较两个条目列表
static bool sameItemList(const QVector<DesktopItem>& a, const QVector<DesktopItem>& b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i)
        if (!sameItemContent(a[i], b[i])) return false;
    return true;
}

// 条目**多重集合**是否相同（**忽略顺序**）。用途见 setItems() 里的"收纳盒顺序保护"：
// 判断"只是顺序不同"与"真的增删了条目"这两种完全不同性质的数据变化。
static bool sameItemMultiset(const QVector<DesktopItem>& a, const QVector<DesktopItem>& b) {
    if (a.size() != b.size()) return false;
    QSet<QString> sa, sb;
    sa.reserve(a.size());
    sb.reserve(b.size());
    for (const DesktopItem& d : a) sa.insert(gridItemKey(d.sourcePath));
    for (const DesktopItem& d : b) sb.insert(gridItemKey(d.sourcePath));
    return sa == sb;
}

// 条目内容签名：该页显示内容的"身份"＝sourcePath 序列（**顺序敏感** —— 用户拖拽排序后必须重建，
// 否则装回的页还是旧顺序）。用于逐页判断缓存页是否仍然有效（替代原先的"全局数据代次"）。
static QStringList pageSignatureOf(const QVector<DesktopItem>& items) {
    QStringList s;
    s.reserve(items.size());
    for (const DesktopItem& it : items) s << it.sourcePath;
    return s;
}

// 设置条目
// 作者：谭征
void IconGridWindow::setItems(const QMap<QString, QVector<DesktopItem>>& items) {
    // 先判断"内容是不是真的变了"，没变就直接返回（2026-09-22）：
    // MainWindow 有近 20 处 setItems() 调用（各类同步/刷新路径），其中不少是内容未变的冗余同步；
    // 而本函数尾部是无条件 rebuildGrid() + buildTabs() —— 前者 invalidateCategoryPages() 把分类页
    // 缓存全销毁，后者销毁并重建整条标签栏。用户看到的就是"没动过什么却又刷新了一遍 / 高亮跳一下"。
    // （与"分类页缓存"是同一件事的两面：缓存再好，也架不住外部每几秒把它推倒一次。）
    QMap<QString, QVector<DesktopItem>> normalized = items;
    if (m_forceShortcutCategory && !normalized.contains(QStringLiteral("快捷方式"))) {
        normalized.insert(QStringLiteral("快捷方式"), {});
    }
    // 先在**副本**上应用持久化顺序，再与已排好序的 m_allItems 比较：否则拿"扫描顺序"比"持久化顺序"，
    // 只要用户拖过图标顺序就永远不相等，早退形同不存在。applyItemOrder 只读 INI + 重排，幂等无副作用。
    if (!m_isBoxWindow) {
        for (auto it = normalized.begin(); it != normalized.end(); ++it) {
            applyItemOrder(it.value(), it.key());
        }
    }
    // —— 收纳盒窗口的「顺序保护」（2026-09-24）——
    // 收纳盒的图标顺序由**窗口内的权威数据**承载（用户拖拽交换位置就发生在窗口里），
    // 而 MainWindow 的 box.items 只在部分路径同步、可能滞后。若照滞后数据把顺序改回去，
    // 用户刚交换好的位置就会被"莫名其妙地还原"。
    // 规则：某分类的条目**集合**与窗口内完全一致（只是顺序不同）⇒ 一律沿用窗口内顺序；
    // 集合真的变了（增 / 删）则照收 —— 那是真实的数据变化。
    if (m_isBoxWindow) {
        for (auto it = normalized.begin(); it != normalized.end(); ++it) {
            const auto cur = m_allItems.constFind(it.key());
            if (cur == m_allItems.constEnd()) continue;
            if (sameItemList(cur.value(), it.value())) continue;            // 完全一致，无需处理
            if (sameItemMultiset(cur.value(), it.value()))
                it.value() = cur.value();                                   // 仅顺序不同 ⇒ 保窗口内顺序
        }
    }

    if (sameGridItemSet(normalized, m_allItems)) return;

    // —— 增量出口（2026-09-24）：只刷新**真正变了**的那些分类对应的盒子 ——
    // 必须在 m_allItems 赋值**之前**调用：它要拿旧值做差异比对。
    // 命中则末尾不再走 rebuildGrid() —— 后者在盒视图下会 buildFences()，把**全部盒子**的
    // **全部按钮**推倒重建，于是"从 Dock 拖一个图标进收纳盒"会变成"整个收纳界面重新加载一遍"。
    const bool itemsDeltaOk = applyItemsDeltaInPlace(normalized);

    m_allItems = normalized;

    // 分类重命名映射与用户自定义分类名的还原，已移至 MainWindow::refreshDesktop() 在
    // “应用已解散”之前执行（applyPersistedCategories），以保证“重命名后再解散”等组合在
    // 刷新/重启后依然正确；此处不再重复应用，同时避免收纳盒被主窗口的重命名映射污染。
    SettingsManager sm;

    if (m_currentCategory.isEmpty() || !m_allItems.contains(m_currentCategory)
        || isHiddenCategory(m_currentCategory)) {
        m_currentCategory = firstVisibleCategory();
    }
    loadCategoryOrder();
    syncCategoryOrder();
    // 刷新/启动后把当前实际分类集合同步回持久化，避免旧版残留分类名或新规则分类
    // 与 categoryNames 不同步，导致下次启动“已解散”的分类又被补回来。
    saveCategoryOrder();
    // 恢复上次选中的分类（仅主窗口，避免与收纳盒窗口互相覆盖）
    if (!m_isBoxWindow) {
        const QString savedCur = sm.loadValue(QStringLiteral("IconGridWindow/currentCategory"), QString()).toString();
        if (!savedCur.isEmpty() && m_allItems.contains(savedCur) && !isHiddenCategory(savedCur)) {
            m_currentCategory = savedCur;
        } else if (isHiddenCategory(m_currentCategory)) {
            m_currentCategory = firstVisibleCategory();
        }
    }
    // 无论是否从持久化恢复，都把当前选中分类立即落盘，保证 IconGridWindow/currentCategory 键始终存在。
    // 这样即使本次会话用户未手动切换分类，重启后也能恢复到同一个分类（而非不可预期的 firstKey）。
    saveCurrentCategory();
    buildTabs();
    updateBadgeLabel();
    if (itemsDeltaOk) {
        // 增量路径已把受影响的盒子原地刷好，无需全量重建。
        // buildTabs() 刚重建过分类标签按钮，旧的标签拖拽源指针必须立即失效
        // （与 rebuildGrid 同款收口；否则它会指向已销毁的按钮，被复用地址后误判成"仍在拖标签"）。
        m_dragSourceTab = nullptr;
        m_tabDragging = false;
    } else {
        rebuildGrid();
    }
}

// 重命名某分类：把所有映射中等于 oldName 的分类改为 newName
// 作者：谭征
void IconGridWindow::renameCategory(const QString& oldName, const QString& newName) {
    if (oldName.isEmpty() || newName.isEmpty() || oldName == newName) return;
    if (!m_allItems.contains(oldName)) return;
    if (m_allItems.contains(newName)) return;

    // 原地重命名分类：只改键名，不新增或保留旧分类，避免重命名“快捷方式”时被 setItems 的自动补全逻辑再插回空分类
    QVector<DesktopItem> items = m_allItems.take(oldName);
    m_allItems.insert(newName, items);

    // 同步迁移该分类的图标顺序持久化键
    if (!m_isBoxWindow) {
        SettingsManager sm;
        const QStringList order = sm.loadValue(itemOrderKey(oldName)).toStringList();
        if (!order.isEmpty()) {
            sm.saveValue(itemOrderKey(newName), order);
            removeItemOrder(oldName);
        }
        // 同步迁移全屏收纳盒子的自定义几何，改名后盒子留在原位、尺寸不变
        const QRect geom = loadFenceGeometry(oldName);
        if (geom.isValid()) {
            saveFenceGeometry(newName, geom);
            removeFenceGeometry(oldName);
        }
    }

    // 记录重命名映射（原始扫描/新建名 -> 当前名），以便刷新桌面后仍能还原。
    // 若 oldName 本身已是某次重命名的结果（某条映射的值==oldName），则源头是那条映射的键；
    // 否则源头就是 oldName 本身（规则扫描名或新建分类名）。
    QString origin = oldName;
    for (auto it = m_categoryRenames.begin(); it != m_categoryRenames.end(); ++it) {
        if (it.value() == oldName) { origin = it.key(); break; }
    }
    // 清理所有与本次改名相关的陈旧条目，避免连续改名（如 新分类→A→新分类）留下
    // “A>新分类”“新分类>A”之类的重复/恒等式映射，导致 IconGridWindow/categoryRenames
    // 累积脏数据、后续再改名时源头解析错乱。
    // - 删除键为 oldName 的条目（oldName 已不再是有效分类）
    // - 删除键为 newName 的条目（newName 若曾是某分类的“原始名”，现已指向新值，下方统一写入）
    // - 删除值为 oldName 的条目（已被本次改名消费）
    auto rit = m_categoryRenames.begin();
    while (rit != m_categoryRenames.end()) {
        if (rit.key() == oldName || rit.key() == newName || rit.value() == oldName) {
            rit = m_categoryRenames.erase(rit);
        } else {
            ++rit;
        }
    }
    // 改回原始名（newName == origin）时，该分类已回到其天然扫描名/新建名，无需再保留
    // 改名映射（否则会留下“新分类>新分类”恒等式，重启后虽能还原但属冗余且易脏）；直接删除即可。
    if (newName != origin) {
        m_categoryRenames[origin] = newName;
    }

    // 在保存的顺序列表中原地替换名称，保持原位置不变（不因字母排序而移动）
    int idx = m_categoryOrder.indexOf(oldName);
    if (idx >= 0) {
        m_categoryOrder[idx] = newName;
    } else {
        m_categoryOrder.append(newName);
    }

    if (  m_currentCategory == oldName) {
        m_currentCategory = newName;
    }
    buildTabs();
    updateBadgeLabel();
    rebuildGrid();
    emit categoryChanged(m_currentCategory);
    saveCategoryOrder();   // 持久化名称与重命名映射
    // A 方案：若该分类名下有文件记录，同步重命名 CategoryStore 中的分类名
    CategoryStore::renameCategory(oldName, newName);
}

// 设置搜索文本
// 作者：谭征
void IconGridWindow::setSearchText(const QString& text) {
    const QString t = text.trimmed();
    // 文本没变就直接返回，别小看这一句：切换分类会走
    // `emit categoryChanged → MainWindow::onCategoryChanged → 助手面板 clearSearch() →
    // QLineEdit::textChanged("") → onSearchChanged("") → 本函数`，
    // 而切换流程自己早已清空搜索并换过页；这里若照样 rebuildGrid()，其开头的
    // invalidateCategoryPages() 会**当场把刚 park 好的分类页缓存全部销毁** →
    // 用户每次切换都在重建，页缓存形同不存在（2026-09-22「多切几次还是能看到刷新」）。
    if (t == m_searchText) return;
    m_searchText = t;
    rebuildGrid();
}

// 设置当前分类
// 作者：谭征
void IconGridWindow::setCurrentCategory(const QString& cat) {
    // 允许传入空字符串：表示当前没有选中任何分类（例如解散最后一个分类后）
    if (cat.isEmpty() || !m_allItems.contains(cat) || isHiddenCategory(cat)) {
        m_currentCategory = QString();
    } else {
        m_currentCategory = cat;
    }
    buildTabs();
    updateBadgeLabel();
    rebuildGrid();
    saveCurrentCategory();   // 持久化当前选中分类
    if (!m_currentCategory.isEmpty()) {
        emit categoryChanged(m_currentCategory);
    }
}

// 当前分类
// 作者：谭征
QString IconGridWindow::currentCategory() const {
    return m_currentCategory;
}

// 不显示为分类的类型：系统（走 Dock）、未分类（按用户要求去掉该分类显示）
// 作者：谭征
bool IconGridWindow::isHiddenCategory(const QString& cat) {
    // “系统”不显示为分类盒子/标签，而是保留在原生桌面直接呈现；
    // “其它”作为兜底分类正常显示，不再有隐藏的“未分类”分类。
    return cat == QStringLiteral("系统");
}

// 返回第一个非隐藏分类；没有可见分类时返回空字符串
// 作者：谭征
QString IconGridWindow::firstVisibleCategory() const {
    for (const QString& c : m_categoryOrder) {
        if (m_allItems.contains(c) && !isHiddenCategory(c)) return c;
    }
    for (auto it = m_allItems.cbegin(); it != m_allItems.cend(); ++it) {
        if (!isHiddenCategory(it.key())) return it.key();
    }
    return QString();
}

// sorted分类
// 作者：谭征
QStringList IconGridWindow::sortedCategories() const {
    // 按显式保存的分类顺序返回，重命名后顺序保持不变；仅过滤掉已不存在的分类
    QStringList cats;
    for (const QString& c : m_categoryOrder) {
        if (m_allItems.contains(c)) cats.append(c);
    }
    return cats;
}

// 同步分类顺序
// 作者：谭征
void IconGridWindow::syncCategoryOrder() {
    // 保留已有顺序中仍然存在的分类；新增的分类追加到末尾。
    // 首次同步（尚无顺序）时，按桌面整理规则页面的默认顺序排列分类，
    // 使标签顺序与设置中心红框选项顺序一致（目录/文档/压缩/图片/快捷方式/网址/视频/音频/其它）。
    QStringList order;
    for (const QString& c : m_categoryOrder) {
        if (m_allItems.contains(c)) order.append(c);
    }

    if (order.isEmpty()) {
        const QVector<OrganizeRule> rules = DesktopScanner::defaultRules();
        for (const OrganizeRule& rule : rules) {
            if (m_allItems.contains(rule.targetCategory) && !order.contains(rule.targetCategory)) {
                order.append(rule.targetCategory);
            }
        }
    }

    // 再把剩余未排序的分类（自定义分类或未来新增分类）追加到末尾。
    for (auto it = m_allItems.cbegin(); it != m_allItems.cend(); ++it) {
        if (!order.contains(it.key())) order.append(it.key());
    }
    m_categoryOrder = order;
}

// 设置分类顺序
// 作者：谭征
void IconGridWindow::setCategoryOrder(const QStringList& order) {
    // 重建收纳盒时，按本地持久化保存的顺序覆盖当前顺序：只保留仍存在（有图标）的分类，
    // 缺失的新增分类交由 syncCategoryOrder 追加到末尾，已不存在的分类自然丢弃。
    QStringList filtered;
    for (const QString& c : order) {
        if (!c.isEmpty() && m_allItems.contains(c)) filtered.append(c);
    }
    if (filtered.isEmpty()) return;   // 没有有效顺序则不覆盖（保留 syncCategoryOrder 的结果）
    m_categoryOrder = filtered;
    syncCategoryOrder();
    buildTabs();
    updateBadgeLabel();
    rebuildGrid();
}

// 无框窗口下，右键弹出的分类上下文菜单首次点击容易被“吞掉”：
// - 右键本身不会激活顶层窗口（仅左键会），菜单弹出时窗口处于非激活态；
// - 模态对话框（如重命名输入框 GlassInputDialog）关闭后，Windows 前台锁会拦截
// SetForegroundWindow，导致 activateWindow() 静默失败。
// 通过“短暂置顶再取消置顶”可强制本进程取得前台权限，确保菜单首次点击正确命中菜单项。
static void activateWindowWithForeground(QWidget* w) {
    Q_UNUSED(w);
#ifdef Q_OS_WIN
    // 不再提升 owner 窗口（WS_EX_NOACTIVATE 桌面层窗口）——那样会横跨 app band 触发闪烁
    // （这正是"先置顶再压入"的字面代码）。改为直接授权本进程前台，配合菜单自身
    // WindowStaysOnTopHint，使菜单首次点击不被吞且 owner 不闪（复刻 360）。
    AllowSetForegroundWindow(GetCurrentProcessId());
#endif
}

// 构建标签
// 作者：谭征
void IconGridWindow::buildTabs() {
    // 旧标签按钮即将被 deleteLater，紧接着就会重建一批新的；m_dragSourceTab 若仍指着旧按钮，
    // 就成了跨重建存活的裸指针（见 rebuildGrid 中的同款清理）。
    m_dragSourceTab = nullptr;
    m_tabDragging = false;
    while (m_tabLayout->count() > 1) {
        QLayoutItem* item = m_tabLayout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    const QStringList cats = sortedCategories();
    for (const QString& cat : cats) {
        // “系统”“未分类”不在分类标签栏显示（系统走壁纸 Dock，未分类按用户要求去掉）
        if (isHiddenCategory(cat)) continue;
        auto* btn = new QToolButton(m_tabBar);
        btn->setText(cat);
        btn->setCheckable(true);
        btn->setAutoExclusive(true);
        btn->setFixedHeight(26);
        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        btn->setProperty("category", cat.toUtf8().constData());
        // 必须给分类标签一个**专属标记**：网格图标（DesktopIconButton）同样带 "category"
        // 动态属性（拖拽时用它标记来源分类），而图标也在本窗口的事件过滤范围内。
        // 若只靠 qobject_cast<QToolButton*> + category 非空来识别标签，图标会被误判成标签，
        // 于是"按住图标拖动"会启动分类标签拖拽（把图标当标签拖、重排分类顺序），
        // 并且 m_dragSourceTab 会持有一个网格图标的裸指针 —— 网格一重建该按钮即被析构，
        // 指针复用后新的图标按钮会被同一次手势误判为标签拖拽源。
        btn->setProperty("isCategoryTab", true);
        btn->setStyleSheet(Theme::tabButtonStyle());
        btn->setMouseTracking(true);
        // 安装事件过滤器以接管鼠标按下/移动，实现拖拽排序
        btn->installEventFilter(this);
        if (cat == m_currentCategory) btn->setChecked(true);
        connect(btn, &QToolButton::clicked, this, &IconGridWindow::onCategoryTabClicked);
        connect(btn, &QToolButton::customContextMenuRequested, this, [this, cat](const QPoint&) {
            // 右键选项卡：可直接重命名或解散该分类，无需先切换选中；所有分类均可解散
            QMenu menu(this);
            menu.setWindowFlag(Qt::WindowStaysOnTopHint, true);   // 菜单自身置顶，不提升 owner（防闪烁）
            Theme::applyMenuStyle(&menu);   // 必须在 popup(exec) 之前：内部会重建平台窗口以拿到 alpha 合成位
            QAction* rename = menu.addAction(Theme::icon("rename"), QStringLiteral("重命名"));
            menu.addSeparator();
            QAction* del = menu.addAction(Theme::icon("delete"), QStringLiteral("解散"));
            // 授权本进程前台 + 菜单自身置顶即可，菜单首次点击不被吞（见 activateWindowWithForeground）。
            activateWindowWithForeground(this);
            QAction* chosen = menu.exec(QCursor::pos());
            if (chosen == rename) {
                emit requestRenameCategory(cat);
            } else if (chosen == del) {
                emit requestDeleteCategory(cat);
            }
        });
        m_tabLayout->insertWidget(m_tabLayout->count() - 1, btn);
    }

    // 新建的标签按钮要立刻带上当前的切换方式（悬停切换需要 WA_Hover 才能收到 HoverEnter）
    applyTagSwitchModeToTabs();
}

// 响应分类标签clicked
// 作者：谭征
void IconGridWindow::onCategoryTabClicked() {
    activateCategoryTab(qobject_cast<QToolButton*>(sender()));
}

// 分类标签切换方式：设置中心 → 外观设置 → 分区标签切换
// 键 `Appearance/tagSwitchMode` 曾经是"空设置"（只有设置中心在读写在自娱自乐，产品里没有
// 任何消费者）：所以「悬停切换」勾了毫无反应；「点击切换」虽然"看着生效"，但那只是因为
// 历史实现本来就是左键点击 —— 并非该设置起了作用。此处补齐消费端。
// 作者：谭征
void IconGridWindow::loadTagSwitchMode() {
    SettingsManager sm;
    const int mode = sm.loadValue(QStringLiteral("Appearance/tagSwitchMode"), 0).toInt();
    m_tagSwitchMode = (mode == 1) ? 1 : 0;   // 脏数据一律兜底为「点击切换」
}

// 设置中心改完「分区标签切换」后**立即生效**（不必关闭窗口、更不必重启）。
// 由 ThemeManager::settingChanged 广播驱动（订阅见构造函数）；showEvent 也复用它。
// 只有模式真的变了才重新下发到标签按钮 —— setAttribute 会触发重绘，无谓地下发纯属浪费。
// 作者：谭征
void IconGridWindow::reloadTagSwitchMode() {
    const int before = m_tagSwitchMode;
    loadTagSwitchMode();
    if (m_tagSwitchMode != before) applyTagSwitchModeToTabs();
}

// 应用标签switch模式到标签
// 作者：谭征
void IconGridWindow::applyTagSwitchModeToTabs() {
    if (!m_tabLayout) return;
    const bool hoverMode = (m_tagSwitchMode == 1);
    for (int i = 0; i < m_tabLayout->count(); ++i) {
        QLayoutItem* item = m_tabLayout->itemAt(i);
        if (!item) continue;
        auto* btn = qobject_cast<QToolButton*>(item->widget());
        if (!btn || !btn->property("isCategoryTab").toBool()) continue;
        // 悬停切换必须能收到 QEvent::HoverEnter：样式表通常已隐式开启 WA_Hover，这里显式声明，
        // 避免日后样式表调整后静默失效。点击切换（默认）下关掉它 —— 从源头杜绝任何悬停副作用。
        btn->setAttribute(Qt::WA_Hover, hoverMode);
    }
    if (!hoverMode) {
        if (m_hoverSwitchTimer) m_hoverSwitchTimer->stop();
        m_hoverPendingTab = nullptr;
    }
}

// 标签「选中态」同步（2026-09-22）：让高亮框跟着当前分类走，两种切换方式结果一致。
// 为什么必须显式做：标签是 `setCheckable(true)` + `setAutoExclusive(true)`，
// 「点击切换」下的选中态**不是我们设的，而是点击的副作用** —— Qt 在 mouseRelease → click() 里
// 自动 nextCheckState()，再互斥地取消同父兄弟。悬停切换**根本没有点击这一步**，
// 于是：分类内容切了、updateBadgeLabel 也更新了，唯独标签高亮还留在旧标签上，
// 看上去就像"悬停只换了内容，没换选中"。
// 顺序：先给目标置位（autoExclusive 会顺带取消兄弟），再兜底扫一遍未清干净的兄弟 ——
// 不依赖 Qt 隐式互斥的实现细节，跑完保证**恰好一个**标签被选中，且就是当前分类那个。
// 作者：谭征
void IconGridWindow::syncCategoryTabChecked(QToolButton* target) {
    if (!target || !m_tabLayout) return;
    if (!target->property("isCategoryTab").toBool()) return;
    if (!target->isChecked()) target->setChecked(true);
    for (int i = 0; i < m_tabLayout->count(); ++i) {
        QLayoutItem* item = m_tabLayout->itemAt(i);
        if (!item) continue;
        auto* b = qobject_cast<QToolButton*>(item->widget());
        if (!b || b == target) continue;
        if (!b->property("isCategoryTab").toBool() || !b->isChecked()) continue;
        // 专属组里"已选中"的那个按钮可能被 Qt 拒绝直接取消（保护组内恒有一个选中），
        // 临时摘掉互斥标记再清，随后原样恢复 —— 只在这一帧内有效，不改变任何外部语义。
        const bool ae = b->autoExclusive();
        if (ae) b->setAutoExclusive(false);
        b->setChecked(false);
        if (ae) b->setAutoExclusive(true);
    }
}

// 头部菜单按钮：始终显示 ↔ 悬停显示（设置中心 → 外观设置 → 分区菜单标签显示）
// 与 `Appearance/tagSwitchMode` 同款的**空设置**：全项目只有设置中心在读写在自娱自乐，
// 勾「悬停显示」毫无反应（勾「始终显示」看着正常，只是因为历史实现本来就是常驻）。
// 用户口径（2026-09-22）：它控制**收纳盒窗口头部那排按钮**（锁/视图/添加/菜单/展开）——
// 始终显示 = 常驻；悬停显示 = 鼠标移入窗口才出现，移出即隐藏。
// 作者：谭征
void IconGridWindow::loadMenuRevealMode() {
    SettingsManager sm;
    const int mode = sm.loadValue(QStringLiteral("Appearance/menuLabelShow"), 0).toInt();
    m_menuRevealMode = (mode == 1) ? 1 : 0;   // 脏数据一律兜底为「始终显示」
}

// reload菜单显示模式
// 作者：谭征
void IconGridWindow::reloadMenuRevealMode() {
    const int before = m_menuRevealMode;
    loadMenuRevealMode();
    if (m_menuRevealMode != before) applyMenuRevealVisibility();
}

// 「在快捷方式图标上显示箭头」（设置中心 → 外观设置 → 其他，2026-09-22）
// 又一个此前的**空设置**（全项目只有设置中心在读写）。口径：勾选则 dock / 收纳盒内所有
// 快捷方式显示箭头，取消则全部不显示。
// 箭头是在 DesktopScanner::loadIcon 取图那一刻由 composeShortcutOverlay 合成进位图的
// （经 Theme::shortcutArrowOverlayScale 传入的系数；关闭时传 0 → 原图返回）。因此开关一变
// **两步都必须做**，少一步就是"勾了没反应"：
// ① DesktopScanner::clearIconCache() —— 进程内图标缓存里的旧条目还带着相反态；
// ② 清空每个按钮的 m_item.icon 并重新入队 —— loadIconNow() 只在 `icon.isNull()` 时才取图，
// 不清空的话它会认为"已经有图了"，直接用旧位图重设一次，视觉上纹丝不动。
// 取图是异步分批的（每拍 2 个，见 processIconQueue），不会卡住 UI。
// 作者：谭征
void IconGridWindow::reloadShortcutArrowMode() {
    DesktopScanner::clearIconCache();
    const QVector<DesktopIconButton*> btns = freshGridButtons();
    for (DesktopIconButton* b : btns) {
        if (!b) continue;
        b->item().icon = QIcon();
        DesktopIconButton::queueIconLoad(b);
    }
}

// **唯一**显隐出口：禁止在别处直接 setVisible 这 5 个按钮，否则两套逻辑会互相打架
// （典型：退出全屏时无条件 show()，会把悬停显示模式下本该隐藏的按钮亮出来）。
// 作者：谭征
void IconGridWindow::applyMenuRevealVisibility() {
    if (!m_headerBar) return;
    // 全屏接管（分区平铺）模式下整条 headerBar 都是隐藏的，这里不该把按钮单独显示出来；
    // 退出全屏由 setFencesMode(false) 末尾再调一次本函数收尾。
    if (m_fencesMode) return;
    const bool show = (m_menuRevealMode == 0) || m_pointerInside;
    for (QToolButton* b : {m_lockBtn, m_viewToggleBtn, m_addBtn, m_menuBtn, m_moreBtn}) {
        if (b) b->setVisible(show);
    }
}

// 切换动作的唯一实现：左键点击与悬停切换共用，避免两处逻辑漂移。
// 作者：谭征
void IconGridWindow::activateCategoryTab(QToolButton* btn) {
    if (!btn) return;
    const QString cat = QString::fromUtf8(btn->property("category").toByteArray());
    if (cat.isEmpty()) return;
    // 选中态先同步：点击路径下是幂等的（Qt 已置位），悬停路径下则靠这一句把高亮带过来。
    syncCategoryTabChecked(btn);
    // 重复激活当前分类（含悬停划过当前标签）：直接返回 —— 不换页、不刷新内容。
    if (cat == m_currentCategory) return;

    const QString from = m_currentCategory;
    const bool wasSearching = !m_searchText.isEmpty();   // 必须在 clear 之前取
    m_currentCategory = cat;
    updateBadgeLabel();
    m_searchText.clear();
    saveCurrentCategory();   // 持久化当前选中分类
    emit categoryChanged(cat);

    // 分类页缓存只在「窗口化网格视图 + 非冻结态」下启用：
    // · 全屏/常驻盒子视图按分类铺盒子（buildFences），与网格页无关；
    // · 右键菜单 / 内联重命名 / 图标拖拽进行中必须避开换页 —— 与 rebuildGrid 同源理由：
    // 原生模态循环里的 deleteLater 会当场析构正在交互的按钮 → 退回原重建路径（记账，冻结解除后补做）。
    const bool frozen = (m_menuOpen || isEditingAnyIcon() || m_iconDragInProgress);
    if (!m_fencesMode && !m_boxView && !frozen) {
        // 已加载过的分类 ⇒ 装回缓存页（不刷新）；首次显示的分类 ⇒ 构建一次。
        switchCategoryPage(from, cat, !wasSearching);
    } else {
        rebuildGrid();
    }
}

// 悬停切换防抖：鼠标划着扫过标签栏时只保留"最后一个落点"，停稳 140ms 才真正切换；
// 否则会连着 rebuildGrid 多次（每个标签一次），肉眼看到网格反复重建。
// 作者：谭征
void IconGridWindow::startHoverSwitch(QToolButton* btn) {
    if (!btn) return;
    if (!m_hoverSwitchTimer) {
        m_hoverSwitchTimer = new QTimer(this);
        m_hoverSwitchTimer->setSingleShot(true);
        m_hoverSwitchTimer->setInterval(140);
        connect(m_hoverSwitchTimer, &QTimer::timeout, this, [this]() {
            // QPointer：拿到时若标签已随 rebuildGrid/buildTabs 销毁则自动为空，不会野指针
            QToolButton* t = m_hoverPendingTab.data();
            m_hoverPendingTab = nullptr;
            if (t) activateCategoryTab(t);
        });
    }
    m_hoverPendingTab = btn;
    m_hoverSwitchTimer->start();
}

// 更新徽章标签
// 作者：谭征
void IconGridWindow::updateBadgeLabel() {
    if (m_badgeLabel) {
        // 当前分类为空时（如解散最后一个分类后），显示窗口标题而不是默认“快捷方式”
        m_badgeLabel->setText(m_currentCategory.isEmpty() ? windowTitle() : m_currentCategory);
    }
}

// 视图模式 → 网格单元格尺寸 + 图标绘制像素。三档图标尺寸对齐“查看 → 大/中等/小图标”；
// 列表模式用**小图标尺寸** + 单行高度（图标在左、文字在右，行宽由 listRowWidth() 按视口宽度算，
// 这里的 cellW 只是算不出视口宽度时的兜底值）。
// 作者：谭征
void IconGridWindow::iconViewMetrics(int mode, int& cellW, int& cellH, int& iconPx) {
    switch (mode) {
    case ViewLarge:  cellW = 144; cellH = 168; iconPx = 96; break;   // 大图标 = 当前中等(48/72×84) 的 2 倍
    case ViewSmall:  cellW = 60; cellH =  72; iconPx = 32; break;
    case ViewList:   cellW = 260; cellH =  40; iconPx = 32; break;   // 列表：小图标尺寸 + 单行（行宽见 listRowWidth）
    case ViewMedium:
    default:         cellW = 72; cellH =  84; iconPx = 48; break;
    }
}

// 切换“查看”档位：更新列数（列表=单列，其余=网格自适应）并按新尺寸重建全部图标按钮。
// 作者：谭征
void IconGridWindow::setViewMode(int mode) {
    if (mode < ViewLarge || mode > ViewList) mode = ViewMedium;
    const bool changed = (mode != m_viewMode);
    m_viewMode = mode;
    if (mode != ViewList) m_lastGridMode = mode;   // 记下网格图标档，供列表⇄网格切换恢复
    m_gridColumns = (mode == ViewList) ? 1 : 8;
    updateViewToggleButton();
    // 尺寸/列数变化必须重建按钮（按钮是固定尺寸的，光 update() 不会改变几何与图标分辨率）。
    // 档位一变，**所有**缓存页的按钮尺寸与图标像素都随之过期 → 显式全清（不然要等各页被访问才逐个淘汰）。
    if (changed) { invalidateCategoryPages(); rebuildGrid(); }
}

// 切换视图模式
// 作者：谭征
void IconGridWindow::toggleViewMode() {
    // 顶部“视”按钮：在列表视图与“上一次的网格图标档”之间切换（右键“查看”里四档仍可直接选）。
    setViewMode(m_viewMode == ViewList ? m_lastGridMode : ViewList);
}

// 更新视图切换按钮
// 作者：谭征
void IconGridWindow::updateViewToggleButton() {
    if (!m_viewToggleBtn) return;
    if (m_gridColumns == 1) {
        m_viewToggleBtn->setText(QStringLiteral(""));
        m_viewToggleBtn->setToolTip(QStringLiteral("切换为网格视图"));
    } else {
        m_viewToggleBtn->setText(QStringLiteral("▦"));
        m_viewToggleBtn->setToolTip(QStringLiteral("切换为列表视图"));
    }
}

// 切换锁定
// 作者：谭征
void IconGridWindow::toggleLock() {
    m_locked = !m_locked;
    updateLockButton();

    // 持久化锁定状态（仅主窗口）
    if (!m_isBoxWindow) {
        SettingsManager sm;
        sm.saveValue(QStringLiteral("IconGridWindow/locked"), m_locked);
    }
}

// 为 false 时仅更新内部状态与界面，供启动恢复阶段使用，避免覆盖已持久化的展开尺寸。
// 作者：谭征
void IconGridWindow::setCollapsed(bool collapsed, bool save) {
    // 全屏收纳模式下不支持折叠/展开
    if (m_fencesMode) return;
    if (!m_gridScroll || !m_categoryBar || collapsed == m_gridCollapsed) return;

    m_gridCollapsed = collapsed;
    if (m_gridCollapsed) {
        // 记录当前展开尺寸，供下次展开恢复
        m_expandedWidth = width();
        m_expandedHeight = height();
        // 折叠时连分类标签栏一起收起，只保留第一行标题栏
        m_categoryBar->hide();
        m_gridScroll->hide();
        if (m_moreBtn) m_moreBtn->setIcon(QIcon(Theme::collapsePixmap(m_gridCollapsed)));
        setMinimumHeight(Theme::collapsedHeight());
        resize(width(), Theme::collapsedHeight());
    } else {
        m_categoryBar->show();
        m_gridScroll->show();
        if (m_moreBtn) m_moreBtn->setIcon(QIcon(Theme::collapsePixmap(m_gridCollapsed)));
        setMinimumHeight(176);
        resize(m_expandedWidth, qMax(m_expandedHeight, 176));
    }

    if (save) {
        // 持久化主网格窗口的折叠状态（收纳盒窗口不共享此状态）
        if (!m_isBoxWindow) {
            SettingsManager sm;
            sm.saveValue(QStringLiteral("IconGridWindow/gridCollapsed"), m_gridCollapsed);
            sm.saveValue(QStringLiteral("IconGridWindow/expandedSize"), expandedSize());
        }
        // 折叠/展开会改变窗口大小，通知主窗口保存当前几何
        emit windowGeometryChanged();
    }
}

// 切换网格
// 作者：谭征
void IconGridWindow::toggleGrid() {
    setCollapsed(!m_gridCollapsed, true);
}

// 更新锁定按钮
// 作者：谭征
void IconGridWindow::updateLockButton() {
    if (!m_lockBtn) return;
    if (m_locked) {
        m_lockBtn->setIcon(Theme::icon("tool_lock"));
        m_lockBtn->setToolTip(QStringLiteral("点击解锁窗口"));
    } else {
        m_lockBtn->setIcon(Theme::icon("unlock"));
        m_lockBtn->setToolTip(QStringLiteral("点击锁定窗口"));
    }
}

// 分类标签拖拽排序
// 作者：谭征
void IconGridWindow::startTabDrag(QToolButton* btn) {
    if (!btn) return;
    const QString category = QString::fromUtf8(btn->property("category").toByteArray());
    if (category.isEmpty()) return;

    auto* mime = new QMimeData;
    mime->setText(category);

    auto* drag = new QDrag(btn);
    drag->setMimeData(mime);

    // 使用按钮当前外观作为拖拽预览
    const QPixmap pixmap = btn->grab();
    if (!pixmap.isNull()) {
        drag->setPixmap(pixmap);
        drag->setHotSpot(QPoint(pixmap.width() / 2, pixmap.height() / 2));
    }

    drag->exec(Qt::MoveAction);

    // 拖拽循环结束后清理状态
    hideInsertMarker();
    m_dragSourceTab = nullptr;
    m_tabDragging = false;
}

// handle标签投放
// 作者：谭征
void IconGridWindow::handleTabDrop(const QString& category, const QPoint& dropPos) {
    if (!m_tabLayout || category.isEmpty() || !m_allItems.contains(category)) return;

    // 收集当前 tabBar 中所有分类按钮（按布局索引顺序），但排除正在拖拽的源按钮，
    // 这样插入索引是相对于“源已移除”后的列表计算的，与下方 removeAll 后再 insert 保持一致，
    // 否则当源在插入点之前时索引会整体右移一位，导致落点与指示线不符。
    QVector<QToolButton*> buttons;
    for (int i = 0; i < m_tabLayout->count(); ++i) {
        QLayoutItem* item = m_tabLayout->itemAt(i);
        if (!item) continue;
        auto* w = item->widget();
        if (!w) continue;
        if (auto* btn = qobject_cast<QToolButton*>(w)) {
            const QString cat = QString::fromUtf8(btn->property("category").toByteArray());
            if (!cat.isEmpty() && cat != category) buttons.append(btn);
        }
    }

    // 根据 drop 位置判断应插入到哪个按钮之前
    int insertIndex = buttons.size();
    for (int i = 0; i < buttons.size(); ++i) {
        const QRect rect = buttons[i]->geometry();
        if (dropPos.x() < rect.center().x()) {
            insertIndex = i;
            break;
        }
    }

    // 修正：insertIndex 是“可见分类按钮”中的相对位置，但 m_categoryOrder 还包含
    // 隐藏分类（系统/未分类）。直接把可见索引插到 m_categoryOrder 会导致落点被前
    // 面的隐藏分类顶回左边（例如：第一个拖到“最后一个的后面”时会插到倒数第二）。
    // 需要把可见索引映射回 m_categoryOrder 的真实插入位置。
    m_categoryOrder.removeAll(category);
    int orderInsertIndex = 0;
    int visibleSeen = 0;
    for (; orderInsertIndex < m_categoryOrder.size(); ++orderInsertIndex) {
        if (visibleSeen >= insertIndex) break;
        if (!isHiddenCategory(m_categoryOrder[orderInsertIndex])) ++visibleSeen;
    }
    m_categoryOrder.insert(orderInsertIndex, category);

    // 持久化并刷新界面
    saveCategoryOrder();
    // 收纳盒窗口的 saveCategoryOrder 为 no-op（不写主窗口共享键），分类顺序改由
    // MainWindow::saveLayout → saveUserBoxes 经 p.categoryOrder 落地；此处发 categoryChanged
    // 触发 requestSaveLayout，使拖拽交换位置即时入本地持久化（与默认收纳盒即时 sync 对齐）。
    if (m_isBoxWindow) emit categoryChanged(QString());
    buildTabs();
    updateBadgeLabel();
    rebuildGrid();
}

// 更新插入指示线
// 作者：谭征
void IconGridWindow::updateInsertMarker(const QPoint& pos) {
    if (!m_insertMarker || !m_tabLayout || !m_tabBar) return;

    // 收集 tabBar 中的分类按钮，但排除正在拖拽的源按钮，使指示线的插入索引
    // 与 handleTabDrop 中“源已移除”后的计算保持一致（详见 handleTabDrop 注释）。
    const QString dragCat = m_dragSourceTab
        ? QString::fromUtf8(m_dragSourceTab->property("category").toByteArray())
        : QString();
    QVector<QToolButton*> buttons;
    for (int i = 0; i < m_tabLayout->count(); ++i) {
        QLayoutItem* item = m_tabLayout->itemAt(i);
        if (!item || !item->widget()) continue;
        if (auto* btn = qobject_cast<QToolButton*>(item->widget())) {
            const QString cat = QString::fromUtf8(btn->property("category").toByteArray());
            if (!cat.isEmpty() && cat != dragCat) buttons.append(btn);
        }
    }

    // 与 handleTabDrop 一致的插入位置判定（落点 x 小于某按钮中心则插到它之前）
    int insertIndex = buttons.size();
    for (int i = 0; i < buttons.size(); ++i) {
        if (pos.x() < buttons[i]->geometry().center().x()) {
            insertIndex = i;
            break;
        }
    }

    // 计算插入边界的 x 坐标（相对 m_tabBar）
    int markerX = 0;
    if (buttons.isEmpty()) {
        markerX = 0;
    } else if (insertIndex <= 0) {
        markerX = buttons.first()->geometry().left();
    } else if (insertIndex >= buttons.size()) {
        markerX = buttons.last()->geometry().right();
    } else {
        markerX = buttons[insertIndex]->geometry().left();
    }

    const int h = m_tabBar->height();
    m_insertMarker->setGeometry(markerX - 1, 0, 2, h);
    m_insertMarker->raise();
    m_insertMarker->show();
}

// 隐藏插入指示线
// 作者：谭征
void IconGridWindow::hideInsertMarker() {
    if (m_insertMarker) m_insertMarker->hide();
}

// 更新网格插入指示线
// 作者：谭征
void IconGridWindow::updateGridInsertMarker(const QPoint& pos) {
    if (!m_gridInsertMarker || !m_gridLayout) { hideGridInsertMarker(); return; }
    const int count = m_gridLayout->count();
    if (count == 0) { hideGridInsertMarker(); return; }

    const int cols = qMax(1, m_actualGridCols);
    const QRect c0 = m_gridLayout->cellRect(0, 0);
    if (!c0.isValid()) { hideGridInsertMarker(); return; }
    const int pitchX = c0.width() + m_gridLayout->spacing();
    const int pitchY = c0.height() + m_gridLayout->spacing();
    if (pitchX <= 0 || pitchY <= 0) { hideGridInsertMarker(); return; }

    // 最后一行可能不满：实际有效行/列以已放置图标为准，避免 cellRect 取到不存在的格子而隐藏指示线
    const int lastRow = (count - 1) / cols;
    int row = (pos.y() - c0.top()) / pitchY;
    if (row < 0) row = 0;
    if (row > lastRow) row = lastRow;

    // 该行实际存在的最后一列：满行为 cols-1，最后一行(可能不足)为 (count-1)%cols
    const int maxColInRow = (row < lastRow) ? (cols - 1) : ((count - 1) % cols);

    int col = (pos.x() - c0.left()) / pitchX;
    if (col < 0) col = 0;
    if (col > maxColInRow) col = maxColInRow;

    const QRect cell = m_gridLayout->cellRect(row, col);
    if (!cell.isValid()) { hideGridInsertMarker(); return; }
    const bool after = pos.x() > cell.center().x();

    QRect line;
    if (after && col < maxColInRow) {
        // 落在某格右半：指示线画在“下一格”左侧（行内后移一格）
        const QRect next = m_gridLayout->cellRect(row, col + 1);
        if (!next.isValid()) { hideGridInsertMarker(); return; }
        line = QRect(next.left() - 1, cell.top(), 3, cell.height());
    } else if (after && col == maxColInRow) {
        // 落在某行最后一格右半：指示线画在“本行末尾”（而非下一行开头）
        line = QRect(cell.right() + 1, cell.top(), 3, cell.height());
    } else {
        // 落在某格左半：指示线画在“本格”左侧
        line = QRect(cell.left() - 1, cell.top(), 3, cell.height());
    }

    m_gridInsertMarker->setGeometry(line);
    m_gridInsertMarker->raise();
    m_gridInsertMarker->show();
}

// 隐藏网格插入指示线
// 作者：谭征
void IconGridWindow::hideGridInsertMarker() {
    if (m_gridInsertMarker) m_gridInsertMarker->hide();
}

// 加载分类顺序
// 作者：谭征
void IconGridWindow::loadCategoryOrder() {
    // 收纳盒窗口为会话内存在、不跨重启持久化，也不应读取主窗口的分类顺序/名称，
    // 否则会污染主窗口的排序与自定义分类集合。此处直接跳过读取。
    if (m_isBoxWindow) return;
    SettingsManager sm;
    // 分类名称集合优先取 categoryNames，兼容旧版仅有 categoryOrder 的情况（两者内容等价）
    const QString saved = sm.loadValue(QStringLiteral("IconGridWindow/categoryNames"),
                                       sm.loadValue(QStringLiteral("IconGridWindow/categoryOrder"), QString()).toString()).toString();
    if (!saved.isEmpty()) {
        QStringList order;
        const QStringList parts = saved.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString& c : parts) {
            if (!c.isEmpty()) order.append(c);
        }
        if (!order.isEmpty()) {
            m_categoryOrder = order;
        }
    }
    // 加载重命名映射：oldName>newName
    const QString renameStr = sm.loadValue(QStringLiteral("IconGridWindow/categoryRenames"), QString()).toString();
    if (!renameStr.isEmpty()) {
        m_categoryRenames.clear();
        for (const QString& pair : renameStr.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            const int sep = pair.indexOf(QLatin1Char('>'));
            if (sep <= 0) continue;
            const QString oldName = pair.left(sep);
            const QString newName = pair.mid(sep + 1);
            if (!oldName.isEmpty() && !newName.isEmpty()) m_categoryRenames.insert(oldName, newName);
        }
    }
}

// 保存分类顺序
// 作者：谭征
void IconGridWindow::saveCategoryOrder() {
    // 收纳盒窗口为会话内存在、不跨重启持久化；其排序/名称不应写入主窗口共享的
    // IconGridWindow/categoryOrder 等键，否则会覆盖主窗口的分类顺序与自定义分类。
    if (m_isBoxWindow) return;
    SettingsManager sm;
    sm.saveValue(QStringLiteral("IconGridWindow/categoryOrder"), m_categoryOrder.join(QLatin1String(",")));
    sm.saveValue(QStringLiteral("IconGridWindow/categoryNames"), m_categoryOrder.join(QLatin1String(",")));
    // 重命名映射序列化：oldName>newName;...
    QStringList pairs;
    for (auto it = m_categoryRenames.begin(); it != m_categoryRenames.end(); ++it) {
        pairs.append(it.key() + QLatin1Char('>') + it.value());
    }
    sm.saveValue(QStringLiteral("IconGridWindow/categoryRenames"), pairs.join(QLatin1Char(';')));
    sm.sync();   // 立即刷写到 INI，避免进程异常退出时丢失
}

// 移除分类从persistence
// 作者：谭征
void IconGridWindow::removeCategoryFromPersistence(const QString& name) {
    if (name.isEmpty()) return;
    // 从分类顺序中移除
    m_categoryOrder.removeAll(name);
    // 从重命名映射中移除（无论是原始扫描名还是当前自定义名）
    auto it = m_categoryRenames.begin();
    while (it != m_categoryRenames.end()) {
        if (it.key() == name || it.value() == name) {
            it = m_categoryRenames.erase(it);
        } else {
            ++it;
        }
    }
    // 删除该分类的图标顺序持久化键，避免已解散分类留下脏数据
    removeItemOrder(name);
    // 同时清掉该分类的全屏收纳盒子几何，避免同名分类重建后沿用旧位置
    removeFenceGeometry(name);
    saveCategoryOrder();
}

// 把「当前选中分类」写入本地持久化数据（仅主窗口；收纳盒窗口不持有该状态）。
// 为什么改成防抖（2026-09-24）：本函数原先每次分类切换都 `saveValue + sync()` —— sync() 是
// **同步刷 INI**，机械盘 + 安全软件抢盘下单次数十~数百 ms，且全程在 GUI 线程。它挂在
// activateCategoryTab()/setCurrentCategory() 上，也就是**每一次**分类切换，而"来回切换分类"
// 正是用户报告变涩的操作之一。它要存的只是"下次打开默认选中哪个分类"，晚几百毫秒写、
// 甚至偶尔丢一次都完全无感。故：值未变不写；连续切换合并为"停止切换 400ms 后写一次"。
// 首次写入仍走同步：那是启动路径（用户还没开始来回切），且必须保证
// `IconGridWindow/currentCategory` 键始终存在（见 setItems 末尾的说明）。
// 窗口析构由 flushCurrentCategory() 兜底，保证排队中的那次写入不会丢。
// 作者：谭征
void IconGridWindow::saveCurrentCategory() {
    // 仅主窗口持久化当前选中分类，避免与收纳盒窗口互相覆盖。
    if (m_isBoxWindow) return;
    if (m_categorySavedOnce && m_currentCategory == m_savedCategory) return;   // 值未变 → 零写入
    m_savedCategory = m_currentCategory;

    if (!m_categorySavedOnce) {
        m_categorySavedOnce = true;
        SettingsManager sm;
        sm.saveValue(QStringLiteral("IconGridWindow/currentCategory"), m_savedCategory);
        sm.sync();   // 首次：立即刷写到 INI，保证键始终存在
        return;
    }

    if (!m_saveCategoryTimer) {
        m_saveCategoryTimer = new QTimer(this);
        m_saveCategoryTimer->setSingleShot(true);
        m_saveCategoryTimer->setInterval(400);
        connect(m_saveCategoryTimer, &QTimer::timeout, this, [this]() {
            if (m_isBoxWindow) return;
            SettingsManager sm;
            sm.saveValue(QStringLiteral("IconGridWindow/currentCategory"), m_savedCategory);
            sm.sync();
        });
    }
    m_saveCategoryTimer->start();   // 连续切换：只把 400ms 计时重新推后，最终只写一次
}

// 立即落盘（取消防抖排队）。两个调用点：窗口析构、以及将来任何"必须立刻持久化"的场合。
// 未排过队（m_categorySavedOnce 为 false）时直接返回：连首次同步写都不需要，更不该凭空写一个空值。
// 作者：谭征
void IconGridWindow::flushCurrentCategory() {
    if (m_isBoxWindow) return;
    if (m_saveCategoryTimer && m_saveCategoryTimer->isActive()) m_saveCategoryTimer->stop();
    if (!m_categorySavedOnce) return;
    SettingsManager sm;
    sm.saveValue(QStringLiteral("IconGridWindow/currentCategory"), m_savedCategory);
    sm.sync();
}

// 否则会作用在网格窗口尚未填充的空 m_allItems 上，导致重命名/自定义分类在重启后失效。
// 作者：谭征
void IconGridWindow::applyPersistedCategories(QMap<QString, QVector<DesktopItem>>& items, const QSet<QString>& disbanded) {
    // 旧版 classify() 自动产生的默认分类名。程序已改用规则扫描，这些名字不应再被
    // 当作“用户自定义分类”补回；否则即使用户解散了它们，每次刷新/重启后仍会以空分类
    // 重新出现，造成“解散不了”的假象。若用户曾经对这些旧分类做过重命名（会记录在
    // categoryRenames 的值中），则保留。
    static const QSet<QString> legacyDefaultCategories = {
        QStringLiteral("已安装软件"),
        QStringLiteral("办公文档"),
        QStringLiteral("图片影音"),
        QStringLiteral("开发工具"),
        QStringLiteral("其他"),
        QStringLiteral("其它"),
        QStringLiteral("文件夹")
    };

    SettingsManager sm;
    // 解析重命名映射 oldName>newName（同时收集所有“改名后的目标名”，用于旧分类过滤）
    const QString renameStr = sm.loadValue(QStringLiteral("IconGridWindow/categoryRenames"), QString()).toString();
    QMap<QString, QString> renameMap;
    QSet<QString> renamedTargets;
    if (!renameStr.isEmpty()) {
        for (const QString& pair : renameStr.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            const int sep = pair.indexOf(QLatin1Char('>'));
            if (sep <= 0) continue;
            const QString oldName = pair.left(sep);
            const QString newName = pair.mid(sep + 1);
            if (newName.isEmpty()) continue;
            renameMap.insert(oldName, newName);
            renamedTargets.insert(newName);
        }
    }

    // 1) 应用重命名映射：扫描得到的原始分类名 -> 用户自定义名。
    // 注意：必须作用在扫描结果 items 上（而非网格窗口尚未填充的 m_allItems），
    // 否则重命名在刷新/重启后会被规则扫描名覆盖（“名称复原”）。
    for (auto it = renameMap.begin(); it != renameMap.end(); ++it) {
        if (items.contains(it.key()) && !items.contains(it.value())) {
            QVector<DesktopItem> its = items.take(it.key());
            items.insert(it.value(), its);
        }
    }

    // 2) 补回扫描中不存在的用户自定义分类名称（保持为空分类），使其持续可见。
    // 过滤掉：旧版自动分类名（未被重命名的）、以及已解散的分类（避免被解散的
    // 自定义分类在刷新/重启后重新出现）。同样作用在 items 上，修复“新建分类重启丢失”。
    const QString namesStr = sm.loadValue(QStringLiteral("IconGridWindow/categoryNames"),
                                           sm.loadValue(QStringLiteral("IconGridWindow/categoryOrder"), QString()).toString()).toString();
    if (!namesStr.isEmpty()) {
        for (const QString& name : namesStr.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            if (name.isEmpty() || items.contains(name)) continue;
            if (disbanded.contains(name)) continue;
            if (legacyDefaultCategories.contains(name) && !renamedTargets.contains(name)) continue;
            items.insert(name, {});
        }
    }
}

// 当前条目
// 作者：谭征
QVector<DesktopItem> IconGridWindow::currentItems() const {
    if (m_searchText.isEmpty()) {
        return m_allItems.value(m_currentCategory);
    }
    QVector<DesktopItem> result;
    for (auto it = m_allItems.cbegin(); it != m_allItems.cend(); ++it) {
        for (const auto& item : it.value()) {
            if (item.displayName.contains(m_searchText, Qt::CaseInsensitive) ||
                item.sourcePath.contains(m_searchText, Qt::CaseInsensitive)) {
                result.append(item);
            }
        }
    }
    return result;
}

// 重建网格
// 作者：谭征
void IconGridWindow::rebuildGrid() {
    // 右键菜单弹出期禁止重建：菜单是**原生模态循环**，期间的 deleteLater 会被它自己的
    // 事件循环立刻执行 —— 正在弹菜单的那个按钮会在 exec 尚未返回时就被销毁，之后访问即崩溃
    // （与 Dock 侧“菜单里选重命名静默失效”是同一机制的两种表现）。改为记账、菜单收起后补做。
    // 改名编辑期同理禁止重建：编辑框挂在“正在被编辑的那个按钮”上，按钮被 deleteLater 时
    // 会连编辑框一起销毁 —— 用户看到输入框一闪即逝，与“点了重命名没反应”无法区分。
    // 三种冻结都记账，解除后补做，数据不会丢。
    // ③ 图标拖拽进行中：FenceBox 及其中按钮就是拖拽源，重建会把源 hide / 重父化 / deleteLater，
    // Windows 随即中止这次拖放（DoDragDrop 随源窗口消失而结束），落点会退化成
    // “光标离开盒子的那一瞬”—— 必须先冻结，等拖拽真正结束（左键抬起）再补做。
    if (m_menuOpen || isEditingAnyIcon() || m_iconDragInProgress) { m_pendingRebuild = true; return; }

    // 真正开始重建 ⇒ 当前页要重来（由 applyGridItems 负责）。但**不再**把所有分类的缓存页一起作废：
    // 缓存是否有效改由"逐页内容签名"在装回时判定 —— 本页内容没变就复用。这里只收走"分类已不存在"的
    // 孤儿页（分类被删除/改名后的残留），避免它们白占内存。
    pruneCategoryPages();
    DiagTrace::log(QStringLiteral("[grid] rebuildGrid cat=%1 cachedPages=%2 cats=%3")
                       .arg(m_currentCategory).arg(m_categoryPages.size()).arg(m_allItems.size()));

    // 选中接力：重建会销毁全部按钮并清空 m_selection（裸指针）。先把当前选中/锚点归一化成
    // shellPath 键，重建后按键找回。否则任何一次重建（文件增删改名 / 盒↔Dock 拖拽 / 回收站还原
    // 触发）都会让 F2/Delete 丢失目标 —— 跨窗口拖拽后“收纳盒 F2 失效”的直接根因。
    // 与 Dock 侧 layoutButtons() 的选中接力同构：key 用 QDir 归一化 + 小写，与磁盘真名对齐。
    QStringList selKeys;
    QString anchorKey;
    for (DesktopIconButton* b : m_selection)
        if (b) selKeys << QDir::fromNativeSeparators(b->item().shellPath).toLower();
    if (m_anchor) anchorKey = QDir::fromNativeSeparators(m_anchor->item().shellPath).toLower();

    clearSelection();   // 重建后控件全部换新，旧的选中指针必须立即失效（否则是野指针）
    // 同理清掉“标签拖拽源”：它指向某个 QToolButton，重建后那个按钮已被 deleteLater；
    // 留下裸指针，等新按钮复用同一地址后会被误判成“仍是拖拽源”，
    // 把一次普通的图标点击/拖动变成分类标签拖拽。
    m_dragSourceTab = nullptr;
    m_tabDragging = false;
    // 收纳盒视图（全屏接管或窗口化常驻）均构建盒子；其余情况显示单分类网格
    if (m_fencesMode || m_boxView) {
        // 确保滚动区内容切到盒子容器，并隐藏单分类网格
        if (m_gridScroll && m_gridScroll->widget() != m_fencesContainer) {
            m_gridScroll->setWidget(m_fencesContainer);
        }
        if (m_gridContainer) m_gridContainer->hide();
        if (m_fencesContainer) m_fencesContainer->show();
        buildFences();
    } else {
        // 窗口化网格视图：滚动区内容切回网格容器，显示分类标签下方的图标网格
        if (m_gridScroll && m_gridScroll->widget() != m_gridContainer) {
            m_gridScroll->setWidget(m_gridContainer);
        }
        if (m_fencesContainer) m_fencesContainer->hide();
        if (m_gridContainer) m_gridContainer->show();
        applyGridItems(currentItems());
    }

    // 重建后按 shellPath 找回选中与锚点（保持原选中顺序；已消失/被收走的项自然匹配不到→丢弃）。
    const QVector<DesktopIconButton*> fresh = freshGridButtons();
    for (const QString& key : selKeys) {
        for (DesktopIconButton* b : fresh) {
            if (!b) continue;
            if (QDir::fromNativeSeparators(b->item().shellPath).toLower() != key) continue;
            if (m_selection.contains(b)) continue;   // 同一键只接一次
            b->setSelected(true);
            m_selection.append(b);
            break;
        }
    }
    if (!anchorKey.isEmpty()) {
        for (DesktopIconButton* b : fresh) {
            if (!b) continue;
            if (QDir::fromNativeSeparators(b->item().shellPath).toLower() == anchorKey) {
                m_anchor = b;
                break;
            }
        }
    }
}

// 直接销毁当前网格里的全部按钮（不缓存）。两处用到：重建前清空、以及“当前显示的页不属于
// 任何分类”（搜索结果页）时切换分类 —— 那种页缓存下来没有意义。
// 作者：谭征
void IconGridWindow::discardCurrentGridPage() {
    while (m_gridLayout->count()) {
        QLayoutItem* item = m_gridLayout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
}

// 配置网格布局参数（spacing / 对齐 / 尺寸约束 / 拉伸清理）并算出本次度量。
// “首次构建”与“装回缓存页”两条路径共用：缓存页装回时列数可能已经变了（窗口被缩放），
// 必须按当前列数重排，而不是照搬构建时的行列位置。
IconGridWindow::GridMetrics IconGridWindow::prepareGridLayout(const QVector<DesktopItem>& items) {
    GridMetrics m;
    // 单元格与图标像素由当前“查看”档位决定（大/中等/小图标 + 列表）。
    int iconW = 72, iconH = 84, iconPx = 48;
    iconViewMetrics(m_viewMode, iconW, iconH, iconPx);
    // 列表档：行宽铺满滚动区可视宽度（单列），图标用小图标尺寸、按钮改为“左图标 + 右文字”。
    m.listMode = (m_viewMode == ViewList);
    if (m.listMode) {
        iconW = listRowWidth();
        m_listRowWidth = iconW;   // 记住本次行宽，resizeEvent 据此判断是否需要重排
    }
    // 间隔取图标格子宽度的 1/4（与 FenceBox 的 kCellGap 保持一致）；列表档行距收紧成 2px
    m.spacing = m.listMode ? 2 : qMax(6, iconW / 4);
    m.iconW = iconW;
    m.iconH = iconH;
    m.iconPx = iconPx;
    m_gridLayout->setSpacing(m.spacing);
    m_gridLayout->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    // 让网格容器尺寸严格跟随实际图标内容，避免空白 stretch 空间触发滚动条常驻
    m_gridLayout->setSizeConstraint(QLayout::SetFixedSize);
    m.cols = computeGridColumns();
    m_actualGridCols = m.cols;
    m.rows = items.isEmpty() ? 0 : (items.size() + m.cols - 1) / m.cols;

    // 清理旧的拉伸策略，避免列数/图标数量变化后残留 stretch 导致布局从中间开始
    for (int c = 0; c < m.cols + 1; ++c) {
        m_gridLayout->setColumnStretch(c, 0);
        m_gridLayout->setColumnMinimumWidth(c, 0);
    }
    for (int r = 0; r < m.rows + 2; ++r) {
        m_gridLayout->setRowStretch(r, 0);
        m_gridLayout->setRowMinimumHeight(r, 0);
    }
    return m;
}

// 应用网格条目
// 作者：谭征
void IconGridWindow::applyGridItems(const QVector<DesktopItem>& items) {
    discardCurrentGridPage();
    const GridMetrics m = prepareGridLayout(items);

    for (int i = 0; i < items.size(); ++i) {
        auto* btn = new DesktopIconButton(items[i], m_gridContainer);
        btn->setShortcutArrowScale(2.0);   // 收纳盒：快捷方式箭头放大到当前 2 倍
        // 按当前“查看”档位设置单元格尺寸 + 图标像素（同时按该像素重取图标，避免放大发虚）。
        // 列表档先开“左图标 + 右文字”绘制形态，再给行宽/行高。
        btn->setListMode(m.listMode);
        btn->setViewMetrics(m.iconW, m.iconH, m.iconPx);
        btn->setProperty("category", m_currentCategory.toUtf8().constData());
        connect(btn, &DesktopIconButton::fileMovedOut, this, &IconGridWindow::onItemFileMovedOut);
        // 图标被拖到所有收纳盒窗口之外（＝放回 Dock / 桌面）：交给跨窗口落点解析器判定并执行。
        connect(btn, &DesktopIconButton::dragDroppedOutside,
                this, &IconGridWindow::onIconDragDroppedOutside);
        // —— 与 Dock 图标同一套操作语义：左键选中、右键菜单、F2 重命名、Delete/D 删除 ——
        connect(btn, &DesktopIconButton::selectionRequested,
                this, &IconGridWindow::onIconSelectionRequested);
        connect(btn, &DesktopIconButton::selectionRequestedForMenu,
                this, &IconGridWindow::onIconSelectionForMenu);
        connect(btn, &DesktopIconButton::deleteRequested,
                this, &IconGridWindow::onIconDeleteRequested);
        connect(btn, &DesktopIconButton::renameCommitted,
                this, &IconGridWindow::onIconRenamed);
        // 菜单弹出期冻结重排（原生模态循环内 deleteLater 会当场析构正在弹菜单的按钮）。
        connect(btn, &DesktopIconButton::contextMenuVisibleChanged,
                this, &IconGridWindow::onContextMenuVisibleChanged);
        // 改名编辑期同样冻结重排（重建会连带销毁挂在按钮上的编辑框）。
        // 影子指针在这里按“具体按钮”绑定（onIconEditingChanged 不能依赖 sender()，
        // 因为全屏盒子视图的信号要经 FenceBox 转发，sender() 会变成 FenceBox）。
        connect(btn, &DesktopIconButton::editingChanged, this, [this, btn](bool on) {
            if (on) m_editingBtn = btn;
            onIconEditingChanged(on);
        });
        const int row = i / m.cols;
        const int col = i % m.cols;
        m_gridLayout->addWidget(btn, row, col, Qt::AlignTop | Qt::AlignLeft);
    }

    // 新创建的图标按钮是动态子控件，需纳入鼠标跟踪与事件过滤范围
    enableChildMouseTracking(m_gridContainer);
}

// 把当前网格里的按钮**收进缓存宿主保活**（不销毁、不丢信号连接），并按分类登记进缓存表。
// 只负责“摘下 + 登记”，随后由调用方决定装上哪个分类的页。
// 作者：谭征
void IconGridWindow::parkCurrentGridPage(const QString& category) {
    QVector<DesktopIconButton*> btns;
    // 倒序遍历 + prepend：得到的顺序与布局里的网格顺序一致（表里顺序 = 装回时的行列顺序）
    for (int i = m_gridLayout->count() - 1; i >= 0; --i) {
        QLayoutItem* li = m_gridLayout->takeAt(i);
        if (!li) continue;
        auto* b = qobject_cast<DesktopIconButton*>(li->widget());
        delete li;                          // takeAt 交出所有权；删掉 item 不会删按钮
        if (!b) continue;
        // setParent 到隐藏宿主：按钮立刻隐藏并脱离网格；它连到本窗口的信号不受换父影响。
        // host 为空时绝不能 setParent(nullptr)（按钮会变成顶层窗口飘在桌面上）→ 退化为直接隐藏。
        if (m_pageCacheHost) b->setParent(m_pageCacheHost);
        else                 b->hide();
        btns.prepend(b);
    }
    if (category.isEmpty() || !m_pageCacheHost) {
        for (DesktopIconButton* b : btns) if (b) b->deleteLater();
        return;
    }
    // 同分类的旧页（正常不存在，防重入）先丢弃，别让两批按钮挂同一个分类名
    const CachedPage stale = m_categoryPages.value(category);
    for (DesktopIconButton* b : stale.buttons) if (b) b->deleteLater();

    CachedPage page;
    // 签名取自**按钮自己的 item()**，不能取 currentItems()：本函数被调用时 m_currentCategory 已经
    // 是新分类了（activateCategoryTab 先改分类再换页），currentItems() 会返回新分类的条目。
    page.signature.reserve(btns.size());
    for (DesktopIconButton* b : btns) page.signature << (b ? b->item().sourcePath : QString());
    page.viewMode = m_viewMode;
    page.rowWidth = (m_viewMode == ViewList) ? m_listRowWidth : 0;
    // 记下"切走这一刻"的滚动位置：装回时还原。否则 park 后容器变空 → 滚动范围塌成 0 → 滚动位置被夹到
    // 顶部，切回来虽然内容是缓存好的，用户却看到"回到开头"，同样会被当成"重新加载了一遍"。
    page.scrollY = (m_gridScroll && m_gridScroll->verticalScrollBar())
                       ? m_gridScroll->verticalScrollBar()->value() : 0;
    page.buttons = btns;
    m_categoryPages.insert(category, page);
    m_pageOrder.removeAll(category);
    m_pageOrder.append(category);           // 队尾 = 最近使用
    trimPageCache();
}

// 装回某分类的缓存页（命中即复用，不重建按钮）。返回 false = 未命中，调用方走首次构建。
// 作者：谭征
bool IconGridWindow::adoptCachedGridPage(const QString& category, const GridMetrics& m, int* scrollYOut) {
    auto it = m_categoryPages.find(category);
    if (it == m_categoryPages.end()) return false;
    const CachedPage page = it.value();
    // 命中判据：视图档位相同 + **内容签名相同**（该分类当前条目与页内按钮的 sourcePath 序列逐项相等）。
    // 任一不符 ⇒ 内容或图标分辨率已过期，丢弃这一页改走首次构建 —— 宁可多建一次，也不显示过期内容。
    // 用"逐页签名"而非"全局数据代次"：那样任何一处数据变更都会把**所有**分类的页一起作废，导致
    // "新增/删除一个分类或往别的分类里加个文件，之后每个分类切回去都要重刷一遍"（用户 2026-09-22）。
    // 列表档行宽**不作判据**：滚动条出现/消失、窗口微调都会让它抖动，据此丢弃缓存等于"切回来仍重建"；
    // 改为装回后按当前度量重设按钮尺寸即可（setViewMetrics 在图标像素未变时不重取位图）。
    const QVector<DesktopItem> items = currentItems();
    const QStringList wantSig = pageSignatureOf(items);
    const bool viewChanged = (page.viewMode != m_viewMode);
    const bool sameOrder = (page.signature == wantSig);

    // HIT-RESORT：顺序变了、但**集合没变** ⇒ 原地复用，不重建、不重取图
    // 为什么必须有这条：页签名是**顺序敏感**的，于是任何一次纯粹的"顺序变化"（用户拖拽排序、
    // applyItemOrder 的结果差异、Dock 侧同步顺序、盒内交换位置…）都会把整页判成 stale，走进
    // "deleteLater 全部按钮 → 重新 new 每个按钮 → 每个按钮重新入队取图（30ms / 每拍 1 个）"。
    // 一个上百项的"图标很多"的分类，这就是**几秒的连续界面占用** —— 用户感受正是"来回切换分类后
    // 鼠标移动发涩"，而且来回切会让它反复发生。顺序变化根本不需要重建按钮：只要按新顺序
    // 把同一批按钮放回布局即可 —— 零销毁、零取图。
    // 安全性：下面全部是**落地之前的纯计算**；任何一步对不上就清空 resortOrder，
    // 退回原来的全量重建路径 —— 那时按钮**尚未被动过**，不会出现"改了一半"的中间态。
    QVector<DesktopIconButton*> resortOrder;   // 非空 = 走原地复用；元素 = 已按新顺序排好的现有按钮
    if (!viewChanged && !sameOrder && page.signature.size() == wantSig.size()) {
        QStringList a = page.signature;
        QStringList b = wantSig;
        a.sort();   // 排序后逐项比对 = 多重集相等（重复项也计）
        b.sort();
        if (a == b) {
            QHash<QString, DesktopIconButton*> byKey;   // 键与选中接力 / 顺序持久化同口径
            byKey.reserve(page.buttons.size());
            for (DesktopIconButton* btn : page.buttons) {
                if (!btn) continue;
                const QString k = gridItemKey(btn->item().sourcePath);
                if (!k.isEmpty()) byKey.insert(k, btn);
            }
            QSet<DesktopIconButton*> used;
            resortOrder.reserve(items.size());
            for (const DesktopItem& di : items) {
                DesktopIconButton* btn = byKey.value(gridItemKey(di.sourcePath), nullptr);
                if (!btn || used.contains(btn)) { resortOrder.clear(); break; }   // 对不上 → 退回重建
                used.insert(btn);
                resortOrder.append(btn);
            }
            if (resortOrder.size() != items.size()) resortOrder.clear();
        }
    }

    const bool stale = viewChanged || (!sameOrder && resortOrder.isEmpty());
    if (stale) {
        for (DesktopIconButton* b : page.buttons) if (b) b->deleteLater();
        m_categoryPages.erase(it);
        m_pageOrder.removeAll(category);
        DiagTrace::log(QStringLiteral("[grid] adoptPage MISS cat=%1 viewMode=%2/%3 sig=%4/%5")
                           .arg(category).arg(page.viewMode).arg(m_viewMode)
                           .arg(page.signature.size()).arg(wantSig.size()));
        return false;
    }
    m_categoryPages.erase(it);
    m_pageOrder.removeAll(category);

    // 装回顺序：原地复用取 resortOrder（新顺序）；顺序本就一致则沿用页内原顺序。
    const QVector<DesktopIconButton*> btns = resortOrder.isEmpty() ? page.buttons : resortOrder;
    DiagTrace::log(resortOrder.isEmpty()
                       ? QStringLiteral("[grid] adoptPage HIT cat=%1 n=%2 scrollY=%3")
                             .arg(category).arg(btns.size()).arg(page.scrollY)
                       : QStringLiteral("[grid] adoptPage HIT-RESORT cat=%1 n=%2 scrollY=%3")
                             .arg(category).arg(btns.size()).arg(page.scrollY));
    for (int i = 0; i < btns.size(); ++i) {
        DesktopIconButton* b = btns[i];
        if (!b) continue;
        // 按**当前**度量重设（列表档行宽可能已随视口宽度变化；图标档位未变则此调用几乎无开销）。
        b->setListMode(m.listMode);
        b->setViewMetrics(m.iconW, m.iconH, m.iconPx);
        // addWidget 会把按钮重新父化回 m_gridContainer；换父后部件处于隐藏态，必须显式 show()。
        m_gridLayout->addWidget(b, i / m.cols, i % m.cols, Qt::AlignTop | Qt::AlignLeft);
        b->show();
    }
    enableChildMouseTracking(m_gridContainer);
    if (scrollYOut) *scrollYOut = page.scrollY;   // 交由 switchCategoryPage 在滚动条稳定后还原
    return true;
}

// 显式全清：只用于"所有缓存页都确实过期"的场合（目前仅视图档位切换 —— 图标像素与按钮尺寸全变）。
// 不要再把它挂到 rebuildGrid() 上：那会让"任一分类的数据变化 → 全部分类缓存作废"，
// 用户感受就是"改一次分类，之后每个分类切回去都重刷一遍"。常规失效交由逐页签名判定（见 adoptCachedGridPage）。
// 作者：谭征
void IconGridWindow::invalidateCategoryPages() {
    if (m_categoryPages.isEmpty() && m_pageOrder.isEmpty()) return;
    const int n = m_categoryPages.size();
    QHash<QString, CachedPage> old;
    old.swap(m_categoryPages);
    m_pageOrder.clear();
    for (auto it = old.begin(); it != old.end(); ++it)
        for (DesktopIconButton* b : it.value().buttons) if (b) b->deleteLater();
    DiagTrace::log(QStringLiteral("[grid] invalidatePages all=%1 reason=explicit").arg(n));
}

// 只收走"分类已不存在"的孤儿页（分类被删除 / 改名后残留）：重建网格时调用一次。
// 该分类的条目若变少/消失，其页由"签名不符"自然淘汰（见 adoptCachedGridPage），不必在这里动。
// 作者：谭征
void IconGridWindow::pruneCategoryPages() {
    if (m_categoryPages.isEmpty()) return;
    QStringList doomed;
    for (auto it = m_categoryPages.cbegin(); it != m_categoryPages.cend(); ++it) {
        if (!m_allItems.contains(it.key())) doomed << it.key();
    }
    for (const QString& cat : doomed) {
        auto it = m_categoryPages.find(cat);
        if (it == m_categoryPages.end()) continue;
        for (DesktopIconButton* b : it.value().buttons) if (b) b->deleteLater();
        m_categoryPages.erase(it);
        m_pageOrder.removeAll(cat);
        DiagTrace::log(QStringLiteral("[grid] prunePage cat=%1 (分类已不存在)").arg(cat));
    }
}

// 缓存页数量上限：每页就是该分类的全部按钮（含图标位图），分类多、图标多时内存会累积。
// 超出即丢弃最久未用的那些页（队首）；下次切到该分类重建一次，行为仍然正确。
// 作者：谭征
void IconGridWindow::trimPageCache() {
    // 上限必须**盖住全部分类**，不能写死小数（2026-09-22 踩坑）：
    // 原先写死 6，而用户实际有 7 个分类（快捷方式/目录/文档/压缩/图片/其它/网址）→ 循环切换时，
    // 每切到第 7 个就把队首那页（往往正是"下一次要切回的那页"）淘汰掉 → 下次切回只能重建，
    // 现象就是"多切换几次又看到加载"。这里改为按分类总数给足（等价于"每类一页都不淘汰"）：
    // 一页就是一个分类的全部按钮，全缓存的总量 = 全部桌面条目，与"桌面本身那么多图标"同阶，可接受。
    // 下限 8 兜住分类很少的情况；上限 24 兜住"分类极多"的极端（那时优先保内存，允许 LRU 淘汰）。
    const int kMaxCachedPages = qBound(8, m_allItems.size(), 24);
    while (m_pageOrder.size() > kMaxCachedPages) {
        const QString victim = m_pageOrder.takeFirst();
        auto it = m_categoryPages.find(victim);
        if (it == m_categoryPages.end()) continue;
        for (DesktopIconButton* b : it.value().buttons) if (b) b->deleteLater();
        m_categoryPages.erase(it);
        DiagTrace::log(QStringLiteral("[grid] trimPage evict=%1 limit=%2 cats=%3")
                           .arg(victim).arg(kMaxCachedPages).arg(m_allItems.size()));
    }
}

// 分类切换的唯一实现：已加载过的分类**复用**上一次建好的按钮页，不再每次重新创建。
// （用户 2026-09-22 口径：只在第一次切换时加载，加载过之后再切换不刷新。）
// 作者：谭征
void IconGridWindow::switchCategoryPage(const QString& from, const QString& to, bool parkOld) {
    // 与 rebuildGrid 同款收口：换页会让控件集合变化，旧的选中指针/拖拽源必须立即失效。
    clearSelection();
    m_dragSourceTab = nullptr;
    m_tabDragging = false;

    // —— 换页期间合批重绘（2026-09-24）——
    // park（n 次 setParent → 隐藏）与 adopt（n 次 addWidget + show）全程逐控件触发中间态重绘 /
    // 布局失效，几百个图标时是成片的无效绘制。整段关掉更新，结束后一次性恢复并 update()。
    // 恢复必须无条件执行：漏恢复 = 整块网格永远不刷新（比卡顿严重得多）。本函数体内
    // **没有中途 return**，故直接首尾成对书写，不做 RAII。
    const bool updatesWas = m_gridContainer && m_gridContainer->updatesEnabled();
    if (m_gridContainer) m_gridContainer->setUpdatesEnabled(false);

    if (parkOld) parkCurrentGridPage(from);
    else         discardCurrentGridPage();   // 当前页不属于任何分类（搜索结果）→ 直接销毁

    DiagTrace::log(QStringLiteral("[grid] switchPage %1 -> %2 parkOld=%3 cached=%4")
                       .arg(from, to).arg(parkOld ? 1 : 0).arg(m_categoryPages.size()));

    const QVector<DesktopItem> items = currentItems();
    const GridMetrics m = prepareGridLayout(items);   // 先把布局参数配好（列数可能已变）
    int restoreY = 0;
    if (!adoptCachedGridPage(to, m, &restoreY)) {
        // 首次显示该分类：构建一次；下一次切走时会被 park 进缓存，之后切回复用。
        applyGridItems(items);
    }

    if (m_gridContainer) {
        m_gridContainer->setUpdatesEnabled(updatesWas);
        if (updatesWas) m_gridContainer->update();
    }

    // 收尾（延后一帧，等滚动条状态重新稳定）：
    // ① 纯几何重排校正列数 —— prepareGridLayout 是在 park **之后**（容器已空、无竖直滚动条、视口偏宽）
    // 算出的列数，装回内容后滚动条回归、视口变窄，首帧可能排得过宽 → 这里按真实视口宽度重排；
    // ② 还原缓存记录里的滚动位置，否则"回到顶部"同样会被当成"重新加载了一遍"。
    // 两者都必须走 relayout（不重建、不动分类页缓存）；顺序必须是先重排再设值（重排会改变内容总高）。
    QTimer::singleShot(0, this, [this, restoreY]() {
        relayoutGridForGeometry();
        if (restoreY > 0 && m_gridScroll && m_gridScroll->verticalScrollBar())
            m_gridScroll->verticalScrollBar()->setValue(restoreY);
    });
}

// 几何变化的唯一出口：只把当前页的**现有按钮**按新列数重新摆位。
// 与 rebuildGrid() 的分工：数据/结构变了 → rebuildGrid（会作废分类页缓存）；只是窗口尺寸、
// 滚动条出现/消失让可用宽度变了 → 这里（按钮、图标位图、信号连接、其它分类的缓存页全部原地不动）。
// 作者：谭征
void IconGridWindow::relayoutGridForGeometry() {
    if (!m_gridLayout) return;
    // 冻结态（右键菜单 / 内联改名 / 图标拖拽）不能动布局：与 rebuildGrid 同源理由 —— 原生模态循环里的
    // 换父/摘除会当场毁掉正在交互的那个按钮。记账，解冻后由既有补做逻辑统一处理。
    if (m_menuOpen || isEditingAnyIcon() || m_iconDragInProgress) { m_pendingRebuild = true; return; }
    // 全屏/常驻盒子视图由 relayoutFences() 负责；滚动区当前挂的不是网格容器同样不插手。
    if (m_fencesMode || m_boxView) return;
    if (m_gridScroll && m_gridScroll->widget() != m_gridContainer) return;
    if (!m_gridScroll || !m_gridScroll->viewport() || m_gridScroll->viewport()->width() <= 0) return;

    QVector<DesktopIconButton*> btns;
    for (int i = 0; i < m_gridLayout->count(); ++i) {
        if (QLayoutItem* it = m_gridLayout->itemAt(i))
            if (auto* b = qobject_cast<DesktopIconButton*>(it->widget())) btns.append(b);
    }
    const QVector<DesktopItem> items = currentItems();
    // 条目数 ≠ 现有按钮数 ⇒ 数据已变但尚未重建（或首次显示还没建过）→ 交给 rebuildGrid 走全量路径。
    // 数量相同则只可能是几何变化，重排即可。
    if (btns.size() != items.size()) { rebuildGrid(); return; }
    if (btns.isEmpty()) return;

    // 列数快照必须取在 prepareGridLayout **之前**：它内部会把 m_actualGridCols 更新成新值，
    // 之后再比就永远"相等"，早退判据形同不存在。
    const int prevCols = m_actualGridCols;
    const GridMetrics gm = prepareGridLayout(items);   // 刷新 m_actualGridCols / m_listRowWidth 并配好布局参数
    // 列数未变 ⇒ 单元格尺寸与每个按钮的行列位置都没变，下面这套"全摘再全放"是纯白跑。
    // 拖动窗口边缘是逐像素 resize，每个像素都会走到这里，收益很直接。
    // 列表档（gm.listMode）不能早退：行宽是逐像素变化的，必须按新行宽重设按钮尺寸并重排。
    if (!gm.listMode && gm.cols == prevCols) return;
    for (DesktopIconButton* b : btns) {
        if (!b) continue;
        b->setListMode(gm.listMode);
        b->setViewMetrics(gm.iconW, gm.iconH, gm.iconPx);   // 图标像素未变时廉价（不重取位图）
    }
    // 摘下再放回：列数变小时旧行列位置会撑出多余空行/空列，必须先清空布局。
    // 只 delete item，不删按钮（按钮引用已收在 btns 里，等下按新列数放回）。
    while (m_gridLayout->count()) {
        QLayoutItem* it = m_gridLayout->takeAt(0);
        if (!it) continue;
        if (it->widget() && !qobject_cast<DesktopIconButton*>(it->widget())) it->widget()->deleteLater();
        delete it;
    }
    for (int i = 0; i < btns.size(); ++i)
        m_gridLayout->addWidget(btns[i], i / gm.cols, i % gm.cols, Qt::AlignTop | Qt::AlignLeft);
}

// 「顺序变了、集合没变」的廉价出口 —— 见头文件声明处的完整说明。
// 只做"能不能走廉价路径"的判定与分发；任何一步对不上都返回 false，由调用方退回全量重建。
// 作者：谭征
bool IconGridWindow::tryReorderInPlace(const QString& category) {
    if (category.isEmpty() || !m_allItems.contains(category)) return false;
    // 搜索页显示的是跨分类子集，其先后与"分类内顺序"不是一回事 ⇒ 不适用
    if (!m_searchText.isEmpty()) return false;
    // 冻结态（菜单 / 内联改名 / 图标拖拽）不能动控件 —— 与 rebuildGrid 同源理由：
    // 原生模态循环里换父/摘除会当场毁掉正在交互的那个按钮。记账，解冻后由 flushPendingGridWork() 补做。
    if (m_menuOpen || isEditingAnyIcon() || m_iconDragInProgress) {
        m_pendingReorderCategory = category;
        return true;   // 已受理 ⇒ 调用方不要再 rebuildGrid()
    }

    const QVector<DesktopItem> items = m_allItems.value(category);
    if (m_fencesMode || m_boxView) {
        // 盒子视图：变动的只有 category 这一个盒子，其余盒子原地不动（这正是最省的一条路）
        for (FenceBox* box : m_fenceBoxes) {
            if (!box || box->category() != category) continue;
            if (!box->reorderItemsInPlace(items)) return false;
            DiagTrace::log(QStringLiteral("[grid] reorderInPlace box=%1 n=%2").arg(category).arg(items.size()));
            return true;
        }
        return false;   // 该分类没有盒子（隐藏分类 / 被搜索过滤）⇒ 交回全量路径
    }

    // 窗口化网格视图：当前页必须是这个分类，否则改的不是屏幕上这一页
    if (m_currentCategory != category) return false;
    if (!reorderGridButtonsInPlace(items)) return false;
    DiagTrace::log(QStringLiteral("[grid] reorderInPlace grid=%1 n=%2").arg(category).arg(items.size()));
    return true;
}

// 「只有少数分类的条目发生了增删」时的廉价出口（盒视图专用）—— 见头文件声明处的完整说明。两阶段：
// ① 预检（只读）：分类集合必须一致，且每个变化分类都是"纯顺序变化"或"小幅增删"；
// ② 执行：只对变化分类对应的那个盒子做原地更新（其余盒子一个控件都不动）。
// 任何一步不符合预期 → 返回 false，调用方走原来的全量重建 —— 本函数只做优化、不承担正确性。
// 作者：谭征
bool IconGridWindow::applyItemsDeltaInPlace(const QMap<QString, QVector<DesktopItem>>& next) {
    // 只有盒视图才有"按分类局部刷新"的语义；窗口化单分类网格交给原路径（它本就只重建当前那一页）
    if (!(m_fencesMode || m_boxView)) return false;
    if (!m_searchText.isEmpty()) return false;   // 搜索态下盒内显示的是过滤子集，与整类数据对不上
    // 冻结态（菜单 / 编辑 / 拖拽）不能动控件：交回原路径，由 m_pendingRebuild 记账后推迟补做
    if (m_menuOpen || isEditingAnyIcon() || m_iconDragInProgress) return false;
    if (next.size() != m_allItems.size()) return false;   // 分类有增删 ⇒ 盒子数量变化，交回全量路径

    // ① 预检：挑出真正变化的分类，并确认每个都能原地处理
    QStringList dirty;
    for (auto it = next.cbegin(); it != next.cend(); ++it) {
        const auto oldIt = m_allItems.constFind(it.key());
        if (oldIt == m_allItems.constEnd()) return false;
        if (sameItemList(oldIt.value(), it.value())) continue;   // 该分类一字未动
        if (qAbs(it.value().size() - oldIt.value().size()) > 8) return false;   // 大幅增删 ⇒ 全量更干净
        // 新旧条目的路径交集太小 ⇒ 实质是"换了一批"，增量复用率极低，交回全量重建
        QSet<QString> oldKeys;
        oldKeys.reserve(oldIt.value().size());
        for (const DesktopItem& d : oldIt.value()) oldKeys.insert(gridItemKey(d.sourcePath));
        int inter = 0;
        for (const DesktopItem& d : it.value())
            if (oldKeys.contains(gridItemKey(d.sourcePath))) ++inter;
        if (inter * 2 < qMin(it.value().size(), oldIt.value().size())) return false;
        dirty << it.key();
    }
    if (dirty.isEmpty()) return true;    // 数据被判定为"没变"：无需任何刷新
    if (dirty.size() > 4) return false;  // 一次动到太多分类 ⇒ 交回全量路径（避免"部分成功"的中间态）

    // ② 执行：逐分类只更新对应的那一个盒子（FenceBox 内部复用已有按钮，只为差异项建/删）
    for (const QString& cat : dirty) {
        const QVector<DesktopItem> items = next.value(cat);
        for (FenceBox* box : m_fenceBoxes) {
            if (!box || box->category() != cat) continue;
            if (!box->applyItemsDelta(items)) return false;   // 理论上不会；真返回 false 由调用方全量兜底
            enableChildMouseTracking(box);                    // 新增的按钮要纳入鼠标跟踪 / 事件过滤
            break;
        }
        // 找不到对应盒子（该分类被隐藏 / 未建盒）⇒ 屏幕上没有它，无需刷新
    }
    DiagTrace::log(QStringLiteral("[grid] itemsDelta in-place dirty=%1 n=%2 cats=%3")
                       .arg(dirty.join(QLatin1Char(',')))
                       .arg(dirty.size())
                       .arg(m_allItems.size()));
    return true;
}

// 窗口化网格视图的原地重排：按新顺序把**现有按钮**放回布局。
// 手法与 relayoutGridForGeometry 相同（只 takeAt/delete item，按钮引用收在本地向量里再放回），
// 区别只有一个：这里会**按新数据顺序**重排，而几何重排保持原顺序。
// 作者：谭征
bool IconGridWindow::reorderGridButtonsInPlace(const QVector<DesktopItem>& items) {
    if (!m_gridLayout) return false;
    if (m_gridScroll && m_gridScroll->widget() != m_gridContainer) return false;   // 滚动区挂的不是网格

    QHash<QString, DesktopIconButton*> byKey;
    QSet<DesktopIconButton*> allBtns;
    for (int i = 0; i < m_gridLayout->count(); ++i) {
        QLayoutItem* li = m_gridLayout->itemAt(i);
        auto* b = li ? qobject_cast<DesktopIconButton*>(li->widget()) : nullptr;
        if (!b) continue;
        allBtns.insert(b);
        const DesktopItem& bi = b->item();
        if (!bi.sourcePath.isEmpty())
            byKey.insert(QDir::fromNativeSeparators(bi.sourcePath).toLower(), b);
        if (!bi.shellPath.isEmpty())
            byKey.insert(QDir::fromNativeSeparators(bi.shellPath).toLower(), b);
    }
    if (allBtns.size() != items.size()) return false;   // 有增删 / 布局里混了别的控件 ⇒ 交回 rebuildGrid()

    QVector<DesktopIconButton*> ordered;
    ordered.reserve(items.size());
    QSet<DesktopIconButton*> used;
    for (const DesktopItem& it : items) {
        DesktopIconButton* b = nullptr;
        if (!it.sourcePath.isEmpty())
            b = byKey.value(QDir::fromNativeSeparators(it.sourcePath).toLower(), nullptr);
        if (!b && !it.shellPath.isEmpty())
            b = byKey.value(QDir::fromNativeSeparators(it.shellPath).toLower(), nullptr);
        if (!b || used.contains(b)) return false;       // 新条目 / 重复项 ⇒ 交回 rebuildGrid()
        used.insert(b);
        ordered.append(b);
    }
    if (used.size() != allBtns.size()) return false;

    const GridMetrics gm = prepareGridLayout(items);    // 刷新列数与布局参数（图标像素未变 ⇒ 不重取位图）
    while (m_gridLayout->count()) {
        QLayoutItem* it = m_gridLayout->takeAt(0);
        if (!it) continue;
        if (it->widget() && !qobject_cast<DesktopIconButton*>(it->widget()))
            it->widget()->deleteLater();
        delete it;
    }
    for (int i = 0; i < ordered.size(); ++i)
        m_gridLayout->addWidget(ordered[i], i / gm.cols, i % gm.cols, Qt::AlignTop | Qt::AlignLeft);
    return true;
}

// 冻结解除后统一补做：**优先原地重排**（只动位置、一个控件都不重建），不适用才退回全量重建。
// 三个冻结源（菜单 / 编辑 / 拖拽）共用它 —— 关键收益出现在"拖拽交换位置"：
// drop 事件是在 QDrag::exec() 内部同步派发的，那一刻 m_iconDragInProgress 仍为 true，
// 于是重排请求会被记账推迟；若补做时直接 rebuildGrid()，就会把**全部盒子**的按钮全量重建，
// 表现成"松手后整个收纳盒重新加载了一遍"。改走这里后，补做只交换位置。
// 作者：谭征
void IconGridWindow::flushPendingGridWork() {
    if (m_pendingReorderCategory.isEmpty() && !m_pendingRebuild) return;
    const QString cat = m_pendingReorderCategory;
    const bool wantRebuild = m_pendingRebuild;
    m_pendingReorderCategory.clear();
    m_pendingRebuild = false;
    QTimer::singleShot(0, this, [this, cat, wantRebuild]() {
        // 再次冻结（例如补做期间又弹了菜单）时 tryReorderInPlace 会重新记账，不会死循环
        if (!cat.isEmpty() && tryReorderInPlace(cat)) return;
        if (wantRebuild || !cat.isEmpty()) rebuildGrid();
    });
}

// 其余按滚动区可用宽度自适应，图标固定 72×84，间隔为图标宽度的 1/4。
// 作者：谭征
int IconGridWindow::computeGridColumns() const {
    if (m_gridColumns == 1) return 1;   // 列表视图强制单列
    // 单元格宽度随“查看”档位变化（大/中/小图标），列数据此自适应可用宽度。
    int iconW = 72, iconH = 84, iconPx = 48;
    iconViewMetrics(m_viewMode, iconW, iconH, iconPx);
    Q_UNUSED(iconH)
    Q_UNUSED(iconPx)
    const int spacing = qMax(6, iconW / 4);   // 间隔 = 图标宽度 / 4
    const QMargins m = m_gridLayout->contentsMargins();
    const int avail = m_gridScroll->viewport()->width() - m.left() - m.right();
    if (avail <= iconW) return 1;
    // 在可用宽度内尽可能多地排下 iconW+spacing 的格子（含末尾一个间隔用于对齐）
    return qMax(1, (avail + spacing) / (iconW + spacing));
}

// 列表档的行宽 = 滚动区可视宽度 - 布局边距 - 预留滚动条宽度 - 4px 余量（下限 160px）。
// 预留滚动条宽度见头文件说明：不预留则“内容超高 → 竖向滚动条出现 → 视口变窄 → 行超出可视区”
// 会多出一条横向滚动条。这里用样式里滚动条的标准宽度（QSS 把滚动条做得更细，预留多几像素无害）。
// 作者：谭征
int IconGridWindow::listRowWidth() const {
    int avail = width();
    if (m_gridScroll && m_gridScroll->viewport()) avail = m_gridScroll->viewport()->width();
    const QMargins m = m_gridLayout ? m_gridLayout->contentsMargins() : QMargins();
    int sb = 0;
    if (m_gridScroll && m_gridScroll->style())
        sb = m_gridScroll->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    return qMax(160, avail - m.left() - m.right() - sb - 4);
}

// 所有显示器的可用工作区（各自排除任务栏）的并集，用于全屏收纳跨屏铺满。
static QRect allScreensAvailableGeometry() {
    QRect r;
    const auto& screens = QApplication::screens();
    if (screens.isEmpty()) {
        QScreen* p = QApplication::primaryScreen();
        return p ? p->availableGeometry() : QRect(0, 0, 800, 600);
    }
    for (QScreen* sc : screens) r = r.isEmpty() ? sc->availableGeometry() : r.united(sc->availableGeometry());
    return r;
}

// 文件不搬动，跨盒拖拽只改分类记录（A 方案）。
// 作者：谭征
void IconGridWindow::setFencesMode(bool on) {
    if (on == m_fencesMode) return;
    m_fencesMode = on;
    if (on) {
        m_boxView = true;   // 进入全屏即开启收纳盒视图；退出全屏后盒子仍常驻（不切回网格）
        // 记录进入全屏前的浮动几何，供退出时还原，并用于持久化（避免落盘全屏几何）
        m_preFencesGeom = geometry();

        // 隐藏顶栏/分类栏/折叠与视图按钮等窗口级控件；卡片与根窗口置透明，露出壁纸
        if (m_headerBar) m_headerBar->hide();
        if (m_categoryBar) m_categoryBar->hide();
        if (m_moreBtn) m_moreBtn->hide();
        if (m_viewToggleBtn) m_viewToggleBtn->hide();
        if (m_addBtn) m_addBtn->hide();
        if (m_cardShadow) m_cardShadow->setEnabled(false);   // 去掉整窗阴影，避免全屏矩形阴影
        if (m_card) m_card->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        this->setStyleSheet(QStringLiteral("background: transparent; border: none;"));

        // 切换滚动区内容为平铺盒子容器
        m_gridScroll->setWidget(m_fencesContainer);
        m_fencesContainer->show();

        // 铺满所有显示器的工作区（各自不含任务栏），让图标直接落在壁纸上，贴近 360 整屏接管观感
        QRect screen = allScreensAvailableGeometry();
        setGeometry(screen);

        // 进入全屏时清空上次残留的搜索词，避免旧筛选把其它分类盒子整组隐藏
        m_fencesSearchEdit->clear();
        m_fencesSearchEdit->show();
        // 浮动菜单按钮 + 搜索框定位到右上角（搜索框在菜单按钮左侧），由 positionFencesOverlay 统一摆放
        positionFencesOverlay();
        m_fencesMenuBtn->show();
        // 进入全屏后自动聚焦搜索框，直接敲键盘即可跨分类筛选图标；
        // 延迟到事件循环，规避无框窗口激活前台权竞态导致 setFocus 静默失败。
        QTimer::singleShot(0, this, [this]() {
            if (m_fencesSearchEdit) m_fencesSearchEdit->setFocus();
        });
    } else {
        // 还原窗口级控件与玻璃外观
        if (m_cardShadow) m_cardShadow->setEnabled(true);
        if (m_card) m_card->setStyleSheet(Theme::boxChrome(Theme::panelStyle()));
        this->setStyleSheet(Theme::boxChrome(Theme::windowStyle()));
        if (m_headerBar) m_headerBar->show();
        if (m_categoryBar) m_categoryBar->show();
        if (m_moreBtn) m_moreBtn->show();
        if (m_viewToggleBtn) m_viewToggleBtn->show();
        if (m_addBtn) m_addBtn->show();
        // 上面的 show() 是无条件的（脱离全屏就该恢复头部按钮），但若当前是「悬停显示」模式且
        // 鼠标不在窗内，必须由唯一出口再收一次 —— 否则退出全屏会把本该隐藏的按钮亮出来。
        applyMenuRevealVisibility();
        m_fencesMenuBtn->hide();

        // 退出全屏：隐藏“系统”壁纸 Dock（它只在全屏接管模式下作为桌面元素存在）
        if (m_systemDock) m_systemDock->hide();

        // 退出全屏：恢复为窗口化单分类网格视图（顶部分类标签 + 下方图标网格），
        // 隐藏全屏系统 Dock（已在上方处理），隐藏盒子容器。
        m_boxView = false;
        if (m_fencesContainer) m_fencesContainer->hide();
        if (m_gridScroll && m_gridScroll->widget() != m_gridContainer) {
            m_gridScroll->setWidget(m_gridContainer);
        }
        if (m_gridContainer) m_gridContainer->show();

        // 还原进入全屏前的浮动几何（无效时退回到展开尺寸）
        if (m_preFencesGeom.isValid()) {
            setGeometry(m_preFencesGeom);
        } else {
            setGeometry(QApplication::primaryScreen()->availableGeometry().width() - 1000,
                        QApplication::primaryScreen()->availableGeometry().height() - 760,
                        960, 720);
        }
    }

    rebuildGrid();
    // 进入/退出全屏后窗口几何已变化：若当前是收纳盒视图，延后一帧再排布盒子，
    // 使其位置贴合新的窗口尺寸；网格视图无需排布盒子。
    if (m_fencesMode || m_boxView) {
        QTimer::singleShot(0, this, &IconGridWindow::relayoutFences);
    }
    emit fencesModeChanged(m_fencesMode);
}

// 位置全屏收纳浮层
// 作者：谭征
void IconGridWindow::positionFencesOverlay() {
    // 全屏模式下浮动在右上角的“搜索框 + 菜单按钮”：菜单按钮贴最右上角，
    // 搜索框紧随其左；两者均 raise 到最上层，压在壁纸盒子之上。仅全屏模式调用。
    if (!m_fencesMode) return;
    const int margin = 8;
    const int btnSize = 30;
    const int gap = 8;
    const int searchW = 240;
    const int searchH = 30;
    if (m_fencesMenuBtn) {
        m_fencesMenuBtn->setGeometry(width() - margin - btnSize, margin, btnSize, btnSize);
        m_fencesMenuBtn->raise();
    }
    if (m_fencesSearchEdit) {
        m_fencesSearchEdit->setGeometry(width() - margin - btnSize - gap - searchW, margin, searchW, searchH);
        m_fencesSearchEdit->raise();
    }
}

// 避免在构造期提前 setWidget() 重定父引发窗口树未稳定导致的启动崩溃
// 作者：谭征
void IconGridWindow::ensureFencesScrollWidget() {
    if (!m_gridScroll || !m_fencesContainer) return;
    if (m_gridScroll->widget() != m_fencesContainer) {
        if (m_gridContainer) m_gridContainer->hide();
        m_gridScroll->setWidget(m_fencesContainer);
    }
    m_fencesContainer->show();
}

// 全屏收纳模式下，把所有分类渲染为平铺在壁纸上的半透明盒子
// 作者：谭征
void IconGridWindow::buildFences() {
    // 切换到收纳盒视图：把滚动区内容从单分类网格切到盒子容器（幂等，构造期不执行）
    ensureFencesScrollWidget();
    // 清理旧盒子（延迟删除，避免拖放过程中直接析构 this）
    for (FenceBox* b : m_fenceBoxes) {
        b->hide();
        b->setParent(nullptr);
        b->deleteLater();
    }
    m_fenceBoxes.clear();

    const QStringList cats = sortedCategories();
    for (const QString& cat : cats) {
        // “系统”与“未分类”不创建收纳盒子：“系统”图标改为以壁纸 Dock 形式直接显示在桌面上，
        // “未分类”按用户要求去掉该分类显示。
        if (isHiddenCategory(cat)) continue;
        QVector<DesktopItem> items = m_allItems.value(cat);
        // 搜索过滤：跨所有分类按名称/路径筛选
        if (!m_searchText.isEmpty()) {
            QVector<DesktopItem> filtered;
            for (const auto& it : items) {
                if (it.displayName.contains(m_searchText, Qt::CaseInsensitive) ||
                    it.sourcePath.contains(m_searchText, Qt::CaseInsensitive))
                    filtered.append(it);
            }
            items = filtered;
            // 搜索无命中：不创建/显示该分类盒子，避免空盒堆叠
            // （如搜“微信”时其它分类整组消失，只剩一堆空框）
            if (items.isEmpty()) continue;
        }
        auto* box = new FenceBox(cat, m_fencesContainer);
        box->setItems(items);
        connect(box, &FenceBox::itemDropped, this, &IconGridWindow::onFenceItemDropped);
        connect(box, &FenceBox::filesDropped, this, &IconGridWindow::onFenceFilesDropped);
        connect(box, &FenceBox::fileMovedOut, this, &IconGridWindow::onItemFileMovedOut);
        // 盒内图标被拖到所有收纳盒窗口之外（＝放回 Dock / 桌面）：同上抛给窗口统一判定。
        connect(box, &FenceBox::iconDragDroppedOutside,
                this, &IconGridWindow::onIconDragDroppedOutside);
        // 用户拖动/缩放盒子结束 -> 持久化该分类的盒子几何
        connect(box, &FenceBox::geometryEdited, this,
                [this](const QString& c, const QRect& g) { saveFenceGeometry(c, g); });
        // 双击标题就地重命名：延后到事件循环，避免在发信号的 FenceBox 内部把自己析构
        connect(box, &FenceBox::renameCommitted, this,
                [this](const QString& oldName, const QString& newName) {
            QTimer::singleShot(0, this, [this, oldName, newName]() {
                onFenceRenameCommitted(oldName, newName);
            });
        });
        // —— 盒内图标的操作语义统一上抛给窗口 ——
        // FenceBox 只是布局容器；选中集合、快捷键上下文、编辑态、改名同步都必须由窗口统一维护。
        // 缺了这组接线，全屏盒子视图里的图标对窗口完全“隐形”，后果是一整串：
        // · 点击不进 m_selection → F2/Delete 因“没有选中项”永远不触发；
        // · 编辑态识别不到 → 重建不被冻结、编辑框可能被连带销毁；
        // · 改名不通知 MainWindow → m_allItems / box.items 保留旧路径，
        // 下次 setItems 把旧名灌回来，且再次改名会因“磁盘真名 == 输入名”被判成无操作。
        connect(box, &FenceBox::iconSelectionRequested,
                this, &IconGridWindow::onIconSelectionRequested);
        connect(box, &FenceBox::iconSelectionForMenu,
                this, &IconGridWindow::onIconSelectionForMenu);
        connect(box, &FenceBox::iconDeleteRequested,
                this, &IconGridWindow::onIconDeleteRequested);
        connect(box, &FenceBox::iconContextMenuVisibleChanged,
                this, &IconGridWindow::onContextMenuVisibleChanged);
        connect(box, &FenceBox::iconEditingChanged, this,
                [this](DesktopIconButton* b, bool on) {
            if (on) m_editingBtn = b;
            onIconEditingChanged(on);
        });
        connect(box, &FenceBox::iconRenameCommitted,
                this, &IconGridWindow::onIconRenamed);
        m_fenceBoxes.append(box);
    }
    relayoutFences();
    buildSystemDock();   // “系统”分类不进盒子，以壁纸 Dock 形式直接显示在桌面上
}

// 盒子几何（自由拖动/缩放结果）按分类持久化；收纳盒窗口不落盘
// 作者：谭征
QString IconGridWindow::fenceGeomKey(const QString& category) {
    return QStringLiteral("IconGridWindow/fenceGeom/") + category;
}

// 加载收纳盒几何
// 作者：谭征
QRect IconGridWindow::loadFenceGeometry(const QString& category) const {
    if (m_isBoxWindow || category.isEmpty()) return QRect();
    SettingsManager sm;
    const QString s = sm.loadValue(fenceGeomKey(category)).toString();
    const QStringList p = s.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (p.size() != 4) return QRect();
    const QRect r(p[0].toInt(), p[1].toInt(), p[2].toInt(), p[3].toInt());
    return r.isValid() ? r : QRect();
}

// 保存收纳盒几何
// 作者：谭征
void IconGridWindow::saveFenceGeometry(const QString& category, const QRect& geom) {
    if (m_isBoxWindow || category.isEmpty() || !geom.isValid()) return;
    SettingsManager sm;
    sm.saveValue(fenceGeomKey(category),
                 QStringLiteral("%1,%2,%3,%4").arg(geom.x()).arg(geom.y())
                     .arg(geom.width()).arg(geom.height()));
    sm.sync();
}

// 移除收纳盒几何
// 作者：谭征
void IconGridWindow::removeFenceGeometry(const QString& category) {
    if (m_isBoxWindow || category.isEmpty()) return;
    SettingsManager sm;
    sm.saveValue(fenceGeomKey(category), QString());
    sm.sync();
}

// 清除所有盒子的自定义位置/尺寸，回到自动流式排布（右键菜单“重排收纳盒”）
// 作者：谭征
void IconGridWindow::resetFenceLayout() {
    for (FenceBox* b : m_fenceBoxes) removeFenceGeometry(b->category());
    relayoutFences();
}

// 重排全屏收纳
// 作者：谭征
void IconGridWindow::relayoutFences() {
    if (!m_fencesContainer) return;
    // 容器尚未布局完成（如构造期 / show() 之前）时几何为 0，此时排布会得到重叠/越界位置；
    // 延后到事件循环稳定、窗口尺寸确定后再排布，规避窗口化收纳盒视图的启动崩溃。
    if (m_fencesContainer->width() <= 0 || m_fencesContainer->height() <= 0) {
        // A3：此前这里是**无限**自重排 —— 容器几何持续为 0 时，每一轮事件循环
        // 都会再投一个 0ms 定时器，形成活锁：GUI 线程永远"有事可做"、CPU 空转拉满，同时把
        // 常驻的 WH_KEYBOARD_LL 同步回调一并拖住（表现为"程序卡 + 鼠标发涩"）。
        // 加一个重试上限：正常情况 1~2 次内几何就绪；超过上限说明该容器本就不该排布
        // （未 show / 被隐藏 / 尺寸恒为 0），放弃更合理 —— 后续任何真实几何变化
        // （resize、showEvent、进出全屏、切换盒子视图）都会重新调用本函数。
        if (++m_fencesRelayoutRetry <= 8) {
            QTimer::singleShot(0, this, &IconGridWindow::relayoutFences);
        }
        return;
    }
    m_fencesRelayoutRetry = 0;
    const int boundsW = qMax(1, m_fencesContainer->width());
    const int boundsH = qMax(1, m_fencesContainer->height());
    const int margin = 24;   // 距壁纸边缘
    const int gap = 24;      // 盒子间距

    int penX = margin, penY = margin, rowH = 0;
    for (FenceBox* box : m_fenceBoxes) {
        const QRect saved = loadFenceGeometry(box->category());
        if (saved.isValid() && saved.width() >= 80 && saved.height() >= 80) {
            // 用户自定义位置/尺寸：夹取到当前可视区域内，避免换分辨率/拔屏后盒子跑到屏幕外
            QSize s(qMin(saved.width(), boundsW), qMin(saved.height(), boundsH));
            const QPoint p(qBound(0, saved.x(), qMax(0, boundsW - s.width())),
                           qBound(0, saved.y(), qMax(0, boundsH - s.height())));
            box->setGeometry(QRect(p, s));
        } else {
            // 无自定义几何：按图标数量的自然尺寸做一次流式自动排布，并落盘固定下来，
            // 使后续增删图标不会导致所有盒子位置整体跳动。
            QSize s = box->naturalSize();
            s.setWidth(qMin(s.width(), qMax(120, boundsW - margin * 2)));
            s.setHeight(qMin(s.height(), qMax(120, boundsH - margin * 2)));
            if (penX > margin && penX + s.width() + margin > boundsW) {
                penX = margin;
                penY += rowH + gap;
                rowH = 0;
            }
            box->setGeometry(penX, penY, s.width(), s.height());
            penX += s.width() + gap;
            rowH = qMax(rowH, s.height());
            saveFenceGeometry(box->category(), box->geometry());
        }
        box->show();
    }
    positionSystemDock();
}

// 双击盒子标题就地重命名提交后的处理（含几何键迁移）
// 作者：谭征
void IconGridWindow::onFenceRenameCommitted(const QString& oldName, const QString& newName) {
    const QString target = newName.trimmed();
    if (target.isEmpty() || target == oldName) return;
    if (!m_allItems.contains(oldName)) return;
    // 系统保留名与已存在的分类名一律拒绝（就地编辑不弹框，静默还原为原名）
    if (isHiddenCategory(target) || m_allItems.contains(target)) {
        rebuildGrid();
        return;
    }
    // renameCategory 内部会迁移 items / 图标顺序 / 盒子几何 / 重命名映射，并 rebuildGrid + 持久化
    renameCategory(oldName, target);
}

// “系统”分类（我的电脑/回收站/网络）不创建盒子，而是以无标题 Dock 直接浮在壁纸上
// 作者：谭征
void IconGridWindow::buildSystemDock() {
    // “系统”分类（我的电脑/回收站/网络）不被收纳进盒子，而是以无标题 Dock 直接浮在壁纸上，
    // 双击用 explorer.exe ::{CLSID} 打开；图标不可拖拽、不提供原生右键菜单（见 DesktopIconButton）。
    const QVector<DesktopItem> items = m_allItems.value(QStringLiteral("系统"));
    if (items.isEmpty()) {
        if (m_systemDock) m_systemDock->hide();
        return;
    }
    if (!m_systemDock) {
        m_systemDock = new QWidget(this);
        m_systemDock->setObjectName(QStringLiteral("systemDock"));
        m_systemDock->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* hl = new QHBoxLayout(m_systemDock);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(8);
    } else {
        // 复用已有 Dock：清空旧图标后重建
        QLayout* hl = m_systemDock->layout();
        while (QLayoutItem* it = hl->takeAt(0)) {
            if (it->widget()) it->widget()->deleteLater();
            delete it;
        }
    }
    QLayout* hl = m_systemDock->layout();
    for (const auto& it : items) {
        auto* btn = new DesktopIconButton(it, m_systemDock);
        btn->setShortcutArrowScale(2.0);   // 收纳盒系统区：快捷方式箭头同样放大到 2 倍
        btn->setFixedSize(72, 84);
        hl->addWidget(btn);
    }
    m_systemDock->adjustSize();
    m_systemDock->show();
    positionSystemDock();
}

// 位置系统Dock
// 作者：谭征
void IconGridWindow::positionSystemDock() {
    // 固定在全屏窗口（壁纸）右下角，距边 24px；非全屏模式或不可见时不定位。
    if (!m_systemDock || !m_fencesMode || !m_systemDock->isVisible()) return;
    const int margin = 24;
    m_systemDock->move(width() - m_systemDock->width() - margin,
                       height() - m_systemDock->height() - margin);
    m_systemDock->raise();
}

// 响应收纳盒条目投放
// 作者：谭征
void IconGridWindow::onFenceItemDropped(const QUuid& id, const QString& sourceCategory,
                                        const QString& targetCategory, int targetIndex) {
    if (targetCategory.isEmpty()) return;
    if (sourceCategory == targetCategory) {
        // 盒内重排序
        reorderItemInCategory(targetCategory, id, targetIndex);
    } else if (m_isBoxWindow) {
        // 收纳盒窗口：纯内存跨分类移动
        moveItemToCategory(id, sourceCategory, targetCategory, targetIndex);
    } else {
        // A 方案：跨分类只改分类记录，文件不搬动（targetIndex 用于排序，此处追加到目标分类）
        handleStorageDrop(id, sourceCategory, targetCategory);
    }
}

// 响应收纳盒files投放
// 作者：谭征
void IconGridWindow::onFenceFilesDropped(const QStringList& paths, const QString& targetCategory) {
    if (targetCategory.isEmpty() || paths.isEmpty()) return;
    if (m_isBoxWindow) {
        // 收纳盒窗口：仅内存追加，不写 CategoryStore
        for (const QString& p : paths) {
            DesktopItem it;
            it.sourcePath = p;
            it.displayName = QFileInfo(p).fileName();
            it.category = targetCategory;
            m_allItems[targetCategory].append(it);
        }
        rebuildGrid();
    } else {
        for (const QString& p : paths) {
            CategoryStore::setCategory(p, targetCategory);
        }
        QTimer::singleShot(0, this, &IconGridWindow::requestRefresh);
    }
}

// 缩放事件
// 作者：谭征
void IconGridWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // 外部拖动提示框覆盖全窗口，尺寸必须跟着窗口走（惰性创建，未显示过则为空）。
    if (m_dropHint) m_dropHint->setGeometry(rect());
    if (m_fencesMode) {
        positionFencesOverlay();
        relayoutFences();
    } else if (!m_boxView) {
        // 网格视图（带分类标签）：宽度变化导致列数变化时，按新宽度重排，
        // 延迟到事件循环稳定后执行，确保 viewport 宽度已更新。
        // 列表档列数恒为 1，靠“列数变化”永远判不出来，改判行宽是否变化。
        // 必须走 relayoutGridForGeometry（只重排、不重建、不毁分类页缓存）：原先调 rebuildGrid，
        // 于是「换个分类 → 滚动条出现/消失 → 视口宽度变 → 列数变 → rebuildGrid → 缓存全废」，
        // 来回切几次必然撞上，表现为“每次切回来又刷新一遍”。
        if (m_viewMode == ViewList) {
            if (listRowWidth() != m_listRowWidth) {
                QTimer::singleShot(0, this, [this]() { relayoutGridForGeometry(); });
            }
        } else if (computeGridColumns() != m_actualGridCols) {
            QTimer::singleShot(0, this, [this]() { relayoutGridForGeometry(); });
        }
    }
}

// 显示事件
// 作者：谭征
void IconGridWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);

    // 分类标签切换方式：显示窗口时重读一次，兜住「设置中心写入期间本窗口尚未存在」的情形
    // （例如先改设置、再新建一个收纳盒）。已开着的窗口由 settingChanged 广播就地热更新，
    // 不依赖这里；两处共用 reloadTagSwitchMode()，只认「模式真的变了」才下发。
    reloadTagSwitchMode();

    // 头部菜单按钮显示方式：重读 + 按"此刻光标是否已在窗内"定显隐。不能只看 m_pointerInside 成员：
    // 窗口隐藏期间 Enter/Leave 不会再派发（最小化恢复、先改设置再新建收纳盒等），
    // 成员会停留在上次离开时的 false，直接复用它会让按钮该亮的时候不亮。
    reloadMenuRevealMode();
    m_pointerInside = rect().contains(mapFromGlobal(QCursor::pos()));
    applyMenuRevealVisibility();
#ifdef Q_OS_WIN
    // 设置 WS_EX_NOACTIVATE：本窗口永不被系统激活，因此 Windows“显示桌面”
    // （任务栏最右侧按钮 / Win+D）会直接跳过它，既不最小化也不隐藏，避免可见闪烁。
    if (HWND h = reinterpret_cast<HWND>(winId())) {
        LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
        if (!(ex & WS_EX_NOACTIVATE)) {
            SetWindowLongW(h, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
            SetWindowPos(h, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
        }
    }
#endif
    // 窗口化收纳盒视图（非全屏）首次显示后容器几何才就绪：补一次排布修正盒子位置，
    // relayoutFences() 内部会在几何未就绪时自动延后到事件循环稳定，故此处调用安全。
    if (m_boxView && !m_fencesMode && !m_fenceBoxes.isEmpty()) {
        relayoutFences();
    } else if (!m_fencesMode && !m_boxView) {
        // 网格视图首次显示后容器几何就绪，按实际宽度重排列数（覆盖首屏/最小化恢复等场景）
        // 同样走几何重排而非 rebuildGrid：窗口每次显示（含收纳盒弹回）都重建会毁掉分类页缓存。
        QTimer::singleShot(0, this, [this]() { relayoutGridForGeometry(); });
    }
    // 登记进快捷键路由并确保 WH_KEYBOARD_LL / WH_MOUSE_LL 在位：
    // 本窗口是 WS_EX_NOACTIVATE，收不到任何 Qt 键盘事件，F2/Delete 只能靠系统级钩子。
    // ⑤补漏（2026-09-21）：网格窗的 show() 发生在 MainWindow 构造内部
    // （mainwindow.cpp: m_gridWindow->show()），其后启动重活还要继续跑一两秒
    // （建助手窗/收纳盒、Dock 首刷的逐图标跨进程读取、全屏分层窗首次合成…）。
    // 此刻装钩 = 「钩子在位 + GUI 线程被钉住」叠加 → 打开程序瞬间全系统鼠标僵住。
    // 上一轮只把 Dock 的钩子延后到了首刷之后、漏了这一套，所以「打开瞬间卡」并未根治。
    // 故：启动重活窗口期（首个网格窗显示后 1.8s 内）内显示的窗口一律延后登记；
    // 之后（用户新建收纳盒等）按原样立即登记，不拖慢它们的快捷键可用性。
    // 延后期间功能不缺失 —— 任何一次交互都会经 armGridHotkey → registerGridWindow
    // 立即自愈装钩；而 F2/Delete 本就要求先有选中项，选中又必先经过一次交互。
    static bool s_startupBusy = true;   // 启动重活窗口期标志（函数内静态：进程内唯一，GUI 线程独占）
    if (s_startupBusy) {
        static bool s_clearTimerArmed = false;
        if (!s_clearTimerArmed) {
            s_clearTimerArmed = true;
            QTimer::singleShot(1800, qApp, []() { s_startupBusy = false; });
        }
        QPointer<IconGridWindow> guard(this);
        QTimer::singleShot(1950, this, [guard]() {
            if (guard && guard->isVisible()) registerGridWindow(guard.data());
        });
        DiagTrace::log(QStringLiteral("[grid] 启动重活期：延后 1950ms 再登记快捷键钩子"));
    } else {
        registerGridWindow(this);
    }

    // 独立收纳盒窗口：无边框顶层窗三件套（与壁纸窗同源）
    // 收纳盒是独立顶层窗口，必须提到全部 band 窗口（主整理窗口/桌面助手）之上、其它程序之下，
    // 否则会被全屏桌面层（主整理窗口）埋住而“看不见”——表现为“新建收纳盒失败”。
    // 主整理窗口本身即是桌面最底层 band，绝不可提层，故仅对 m_isBoxWindow 生效。
    if (m_isBoxWindow) {
        const HWND h = reinterpret_cast<HWND>(winId());
        if (h && IsWindow(h)) {
            EditorWatch::clearOwner(h);                    // ① 清 owner，避免随其压底而沉下去
            DesktopMirrorWindow::raiseAboveBandWindows(h);  // ② 插到全部 band 窗口之上
        }
        // show() 之后 Qt 仍会沿 parent 链补一次内部 z 序调整，单次断言会被它覆盖，故错峰多轮再钉。
        for (int delayMs : {0, 60, 200, 400, 800}) {
            QTimer::singleShot(delayMs, this, [this]() {
                if (!isVisible()) return;
                const HWND hw = reinterpret_cast<HWND>(winId());
                if (!hw || !IsWindow(hw)) return;
                EditorWatch::clearOwner(hw);
                DesktopMirrorWindow::raiseAboveBandWindows(hw);
            });
        }
        // 层级看护：仅被可见 band 窗压住才救，避免无谓打扰。
        // B5：500ms → 1000ms（被压住是低频事件，1s 延迟无体感差异）。
        if (!m_layerKeeper) {
            m_layerKeeper = new QTimer(this);
            m_layerKeeper->setInterval(1000);
            connect(m_layerKeeper, &QTimer::timeout, this, [this]() {
                if (!isVisible()) return;
                const HWND hw = reinterpret_cast<HWND>(winId());
                if (!hw || !IsWindow(hw)) return;
                DesktopMirrorWindow::ensureLayerAboveBandWindows(hw);
            });
            m_layerKeeper->start();
        }
    }
}

// 常态拦截提层：WM_WINDOWPOSCHANGING（同步发送、不走消息队列，全局过滤器看不到）
// 阶段调用 DesktopMirrorWindow::clampBandZOrder，在 z 序变更应用之前冻结「顶层提层」，
// 杜绝「开其它程序后点击 → 收纳盒被顶到程序之上再被压回」的一闪而过。
// show desktop 激活期守卫放行，topmost 状态机不受影响。
// 作者：谭征
bool IconGridWindow::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    Q_UNUSED(result)
    MSG* msg = static_cast<MSG*>(message);
    if (msg && msg->message == WM_WINDOWPOSCHANGING) {
        DesktopMirrorWindow::clampBandZOrder(msg->hwnd,
                                             reinterpret_cast<WINDOWPOS*>(msg->lParam));
        return false;   // 改写已就位，走默认处理链
    }
#else
    Q_UNUSED(eventType) Q_UNUSED(message) Q_UNUSED(result)
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

// 图标网格内拖拽排序与跨分类移动
// 作者：谭征
bool IconGridWindow::isGridChild(QObject* obj) const {
    QWidget* w = qobject_cast<QWidget*>(obj);
    if (!w || !m_gridContainer) return false;
    for (QWidget* p = w->parentWidget(); p; p = p->parentWidget()) {
        if (p == m_gridContainer) return true;
    }
    return false;
}

// 目标索引从投放位置
// 作者：谭征
int IconGridWindow::targetIndexFromDropPos(const QPoint& gridContainerPos) const {
    const int count = m_gridLayout ? m_gridLayout->count() : 0;
    if (count == 0) return 0;
    const int cols = qMax(1, m_actualGridCols);

    const QRect c0 = m_gridLayout->cellRect(0, 0);
    if (!c0.isValid()) return count;
    const int pitchX = c0.width() + m_gridLayout->spacing();
    const int pitchY = c0.height() + m_gridLayout->spacing();
    if (pitchX <= 0 || pitchY <= 0) return count;

    const int lastRow = (count - 1) / cols;

    int row = (gridContainerPos.y() - c0.top()) / pitchY;
    if (row < 0) row = 0;
    if (row > lastRow) row = lastRow;

    const int maxColInRow = (row < lastRow) ? (cols - 1) : ((count - 1) % cols);

    int col = (gridContainerPos.x() - c0.left()) / pitchX;
    if (col < 0) col = 0;
    if (col > maxColInRow) col = maxColInRow;

    const QRect cell = m_gridLayout->cellRect(row, col);
    if (!cell.isValid()) return count;
    const bool after = gridContainerPos.x() > cell.center().x();

    int target = row * cols + col;
    if (after) ++target;
    if (target > count) target = count;
    return target;
}

// 移动条目到分类
// 作者：谭征
void IconGridWindow::moveItemToCategory(const QUuid& id, const QString& sourceCategory,
                                        const QString& targetCategory, int targetIndex) {
    if (sourceCategory == targetCategory || !m_allItems.contains(sourceCategory)) return;
    if (targetCategory.isEmpty()) return;

    auto& src = m_allItems[sourceCategory];
    int sourceIndex = -1;
    for (int i = 0; i < src.size(); ++i) {
        if (src[i].id == id) { sourceIndex = i; break; }
    }
    if (sourceIndex < 0) return;

    DesktopItem item = src.takeAt(sourceIndex);
    item.category = targetCategory;
    auto& dst = m_allItems[targetCategory];
    if (targetIndex < 0) targetIndex = dst.size();
    targetIndex = qBound(0, targetIndex, dst.size());
    dst.insert(targetIndex, item);

    rebuildGrid();
    if (!m_isBoxWindow) {
        saveItemOrder(sourceCategory);
        saveItemOrder(targetCategory);
    }
}

// reorder条目in分类
// 作者：谭征
void IconGridWindow::reorderItemInCategory(const QString& category, const QUuid& id, int targetIndex) {
    if (!m_allItems.contains(category)) return;

    auto& vec = m_allItems[category];
    int sourceIndex = -1;
    for (int i = 0; i < vec.size(); ++i) {
        if (vec[i].id == id) { sourceIndex = i; break; }
    }
    if (sourceIndex < 0) return;

    DesktopItem item = vec.takeAt(sourceIndex);
    // 移除位于更前位置的元素后，后续索引整体前移一位，落点需同步减一，
    // 否则向下/向右拖动时会多偏移一格（且蓝色指示线与最终落点不一致）。
    if (sourceIndex < targetIndex) --targetIndex;
    targetIndex = qBound(0, targetIndex, vec.size());
    vec.insert(targetIndex, item);

    // 顺序变了、**集合没变** ⇒ 先走廉价出口：只把现有按钮按新顺序放回布局
    // （不新建 / 不销毁控件、不重取图标、不毁分类页缓存）。
    // 不适用时（搜索态 / 该分类没有盒子 / 网格页不是该分类）返回 false，退回全量重建 = 原行为。
    if (!tryReorderInPlace(category)) rebuildGrid();
    if (!m_isBoxWindow) {
        saveItemOrder(category);
    } else {
        // 收纳盒窗口没有独立的顺序键：它的图标顺序由**窗口权威数据**（items()）承载，
        // 落盘走 MainWindow::saveLayout → saveUserBoxes（那里读的正是 box.window->items()）。
        // 发一次 categoryChanged 只为**即时触发**那次防抖保存（与 handleTabDrop 完全同款手法），
        // 否则这次交换要等到下一次几何变化/退出才落盘。该信号在收纳盒窗口上只连到
        // requestSaveLayout（不回调 setItems），所以不会把刚排好的顺序覆盖掉。
        emit categoryChanged(QString());
    }
}

// 条目顺序按键
// 作者：谭征
QString IconGridWindow::itemOrderKey(const QString& category) {
    return QStringLiteral("IconGridWindow/itemOrder/") + category;
}

// 图标被拖出/拖入后，延后请求 MainWindow 重新扫描刷新
// 作者：谭征
void IconGridWindow::onItemFileMovedOut() {
    // 延后到事件循环空闲再刷新：拖拽刚结束，立即重建控件可能干扰仍在收尾的事件处理。
    QTimer::singleShot(0, this, &IconGridWindow::requestRefresh);
}

// 跨窗口拖拽：收纳盒 ⇄ Dock

// Dock → 本窗口 的落点解析：这块屏幕点属于本窗口的哪个分类？
// 判定顺序刻意如此：
// ① 先按窗口矩形粗筛 —— 绝大多数拖动点（在别的窗口/桌面上）在这一步就被排除，代价极低；
// ② 全屏盒子视图下精确定位到“命中的那一个盒子”，拖到哪个盒子就进哪个分类，
// 这是用户最直观的预期（拖到“工作”盒上就归入“工作”）；
// ③ 落在盒子之间的空白、或窗口化的单分类网格视图上 —— 归入本窗口的当前分类：
// 从用户视角看，“拖到这个盒窗口上”＝“放进这个盒子”，不需要再精确瞄准某个格子。
// 不可作为目标的分类（“系统”/“未分类”，见 isHiddenCategory）一律返回空，由调用方放弃。
// 作者：谭征
QString IconGridWindow::dropCategoryAtGlobal(const QPoint& globalPos) const {
    if (!isVisible()) return QString();
    if (!frameGeometry().contains(globalPos)) return QString();

    for (FenceBox* box : m_fenceBoxes) {
        if (!box || !box->isVisible()) continue;
        const QRect boxRect(box->mapToGlobal(QPoint(0, 0)), box->size());
        if (!boxRect.contains(globalPos)) continue;
        const QString cat = box->category();
        if (!cat.isEmpty() && !isHiddenCategory(cat)) return cat;
    }

    const QString cur = m_currentCategory;
    if (!cur.isEmpty() && !isHiddenCategory(cur)) return cur;
    return QString();
}

// 「盒 → Dock」判定用：松手点是否落在**真实可放置目标**上（差别见头文件说明）。
// 这里刻意不做“当前分类”回退、也不把窗口的透明区域算作目标 ——
// 全屏接管模式下本窗口铺满整屏且背景全透明，若用帧矩形判定，任何落点都会被判成
// “还在盒子里”，盒 → Dock 就永远不生效。
// 作者：谭征
bool IconGridWindow::hitRealDropTarget(const QPoint& globalPos) const {
    if (!isVisible()) return false;
    if (!frameGeometry().contains(globalPos)) return false;

    // ① 命中某个真实盒子 → 算目标。注意这里**不看分类是否可收纳**：落在盒子上就不是“拖出去”。
    for (FenceBox* box : m_fenceBoxes) {
        if (!box || !box->isVisible()) continue;
        const QRect boxRect(box->mapToGlobal(QPoint(0, 0)), box->size());
        if (boxRect.contains(globalPos)) return true;
    }

    // ② 盒子视图（全屏 / 独立盒子窗口）：盒子之外的区域是壁纸 —— 用户就是要把图标丢到桌面上。
    if (m_fencesMode || m_boxView) return false;

    // ③ 窗口化单分类网格视图：整窗即一个分类容器，落在窗口内即算目标。
    return true;
}

// 本窗口 → Dock：图标被拖拽后没有任何拖放接收方接住（QDrag 返回 IgnoreAction）且源文件仍在原处。
// 注意这里**不做**“是否真的离开了本窗口”的判断 —— 那是 MainWindow 的职责：
// 它同时掌握主整理窗口与所有独立收纳盒窗口的几何，只有它才能回答“这个松手点是不是在
// 所有收纳盒窗口之外”。本窗口只负责把事件精确转发上去。
// 作者：谭征
void IconGridWindow::onIconDragDroppedOutside(const QString& sourcePath, const QString& displayName,
                                              const QPoint& globalPos) {
    if (sourcePath.isEmpty() || !m_dockDropResolver) return;
    m_dockDropResolver(sourcePath, displayName, globalPos);
}

// 外部（Dock）拖动悬停在本窗口上时的提示边框。
// 用“覆盖全窗口的透明描边层 + raise()”而不是在本窗口 paintEvent 里画：
// 本窗口内部是一张玻璃卡片（m_card）加若干子控件，窗口自身的绘制会被子控件盖住，
// 只有独立且被 raise 的子控件才能保证这圈提示始终可见。
// 作者：谭征
void IconGridWindow::setExternalDropHighlight(bool on) {
    if (m_externalDropHighlight == on) return;
    m_externalDropHighlight = on;
    if (on) {
        if (!m_dropHint) {
            m_dropHint = new QFrame(this);
            m_dropHint->setObjectName(QStringLiteral("externalDropHint"));
            m_dropHint->setAttribute(Qt::WA_TransparentForMouseEvents, true);   // 纯提示层，绝不截获鼠标
            m_dropHint->setStyleSheet(QStringLiteral(
                "#externalDropHint { background: rgba(56, 189, 248, 32);"
                " border: 3px solid rgba(56, 189, 248, 235); border-radius: 12px; }"));
        }
        m_dropHint->setGeometry(rect());
        m_dropHint->show();
        m_dropHint->raise();
    } else if (m_dropHint) {
        m_dropHint->hide();
    }
}

// 外部拖拽（Dock 图标 / 资源管理器文件）的插入位置指示
// 两条承载路径，与 dropCategoryAtGlobal 的命中口径保持一致：
// ① 全屏盒子视图 → 转发给命中的 FenceBox（它用与盒内拖拽完全相同的那条竖线）；
// ② 窗口化网格视图 → 用本窗口的网格指示线（updateGridInsertMarker）。
// 落点必须落在**图标网格的可视区**（滚动区 viewport）内才算“插入到此处”：
// 标题栏、分类标签栏、收纳盒之间的壁纸都不算 —— 那几种情况按“追加末尾”处理（outIndex = -1），
// 否则用户拖到标题栏上也会看到一条落在第一格的竖线，语义错乱。
// 作者：谭征
bool IconGridWindow::updateExternalDropIndicatorAt(const QPoint& globalPos, int* outIndex) {
    if (outIndex) *outIndex = -1;
    if (!isVisible()) return false;

    // ① 全屏盒子视图：命中的 FenceBox 自己画盒内指示线
    for (FenceBox* box : m_fenceBoxes) {
        if (!box || !box->isVisible()) continue;
        int idx = -1;
        if (box->showExternalDropMarker(globalPos, &idx)) {
            hideGridInsertMarker();          // 两条指示线互斥，避免同屏出现两条
            if (outIndex) *outIndex = idx;
            setCursor(Qt::DragMoveCursor);   // 光标反馈：“这里可以放”
            return true;
        }
    }
    // ② 网格视图：以滚动区 viewport 为准（内容宽度可大于视口，用容器矩形会误判到视口外）
    if (m_gridScroll && m_gridContainer && m_gridScroll->isVisible()
        && m_gridContainer->isVisible()) {
        QWidget* vp = m_gridScroll->viewport();
        const QRect vpRect(vp->mapToGlobal(QPoint(0, 0)), vp->size());
        if (vpRect.contains(globalPos)) {
            const QPoint local = m_gridContainer->mapFromGlobal(globalPos);
            updateGridInsertMarker(local);
            if (outIndex) *outIndex = targetIndexFromDropPos(local);
            setCursor(Qt::DragMoveCursor);
            return true;
        }
    }
    hideGridInsertMarker();
    // 指示线只在图标网格上出现，但“可放置”光标要在**整片可接管区域**上都给：
    // 拖到标题栏 / 分类标签栏松手同样会被本窗口接管（收进当前分类，落点按“追加末尾”），
    // 若只在网格上给光标，同一窗口内光标会忽明忽暗，反而让人以为“这里放不进去”。
    if (!dropCategoryAtGlobal(globalPos).isEmpty()) {
        setCursor(Qt::DragMoveCursor);
    } else if (!m_dragging && !m_resizing && !m_locked && !m_tabDragging) {
        unsetCursor();
    }
    return false;
}

// 本窗口在 globalPos 处**真正遮挡住下层（Dock）的不透明表面**矩形（屏幕坐标）。
// 与 hitRealDropTarget 的区别：那个回答“能不能放进去”（要分类可收纳、要看 FenceBox 是不是落点），
// 这个只回答“看不看得见”—— 遮挡是纯几何事实，与能否投放无关（拖到不收东西的盒子上，图标同样被盖住）。
// 作者：谭征
QRect IconGridWindow::externalDropSurfaceRectGlobal(const QPoint& globalPos) const {
    if (!isVisible()) return QRect();
    // ① 命中某个 FenceBox 卡片 → 卡片本身不透明（盒内有底色），是遮挡物
    for (FenceBox* box : m_fenceBoxes) {
        if (!box || !box->isVisible()) continue;
        const QRect r(box->mapToGlobal(QPoint(0, 0)), box->size());
        if (r.contains(globalPos)) return r;
    }
    // ② 全屏接管：窗口铺满整屏且背景完全透明，盒子之外的区域透出 Dock/壁纸 → 不遮挡
    if (m_fencesMode) return QRect();
    // ③ 窗口化视图：整扇窗口就是一块不透明卡片面板，落在窗口内即被遮挡
    const QRect winRect(mapToGlobal(QPoint(0, 0)), size());
    if (winRect.contains(globalPos)) return winRect;
    return QRect();
}

// 只查询插入下标（松手时用），-1 = 落点不在本盒的图标网格上；不改变指示线显示
// 作者：谭征
int IconGridWindow::externalDropIndexAt(const QPoint& globalPos) const {
    if (!isVisible()) return -1;
    for (FenceBox* box : m_fenceBoxes) {
        if (!box || !box->isVisible()) continue;
        const int idx = box->externalDropIndexAt(globalPos);
        if (idx >= 0) return idx;
    }
    if (m_gridScroll && m_gridContainer && m_gridScroll->isVisible()
        && m_gridContainer->isVisible()) {
        QWidget* vp = m_gridScroll->viewport();
        const QRect vpRect(vp->mapToGlobal(QPoint(0, 0)), vp->size());
        if (vpRect.contains(globalPos))
            return targetIndexFromDropPos(m_gridContainer->mapFromGlobal(globalPos));
    }
    return -1;
}

// 收起插入位置指示线（悬停离开 / 拖拽结束）
// 作者：谭征
void IconGridWindow::clearExternalDropIndicator() {
    hideGridInsertMarker();
    for (FenceBox* box : m_fenceBoxes) if (box) box->hideExternalDropMarker();
    if (!m_dragging && !m_resizing && !m_locked && !m_tabDragging) unsetCursor();
}

// 用户“插到光标位置”的落点就白做了。
// 作者：谭征
void IconGridWindow::setCategoryOrderedItems(const QString& category,
                                             const QVector<DesktopItem>& items) {
    if (category.isEmpty()) return;
    m_allItems[category] = items;
    // 顺序键与显示顺序同源：先落盘，后续 setItems() 的 applyItemOrder() 才会按这个顺序排，
    // 新插入的条目才不会被甩到末尾（仅主整理窗口有顺序键；收纳盒窗口的顺序就是传入顺序）。
    if (!m_isBoxWindow) saveItemOrder(category);
}

// 在指定分类中按 id 找到图标的当前源路径（用于改分类记录）
// 作者：谭征
QString IconGridWindow::sourcePathOf(const QUuid& id, const QString& category) const {
    if (!m_allItems.contains(category)) return QString();
    for (const auto& it : m_allItems[category]) {
        if (it.id == id) return it.sourcePath;
    }
    return QString();
}

// 仅主窗口（非收纳盒）使用；收纳盒仍走纯内存 moveItemToCategory。
// 作者：谭征
void IconGridWindow::handleStorageDrop(const QUuid& id, const QString& sourceCategory,
                                       const QString& targetCategory) {
    Q_UNUSED(sourceCategory)
    const QString sourcePath = sourcePathOf(id, sourceCategory);
    if (sourcePath.isEmpty()) return;
    // A 方案：拖到某分类只是把该文件的分类记录写入 CategoryStore，文件始终留在原桌面位置。
    // “其它”也是正常分类，拖入即归入“其它”，不再有隐藏的“未分类”取消归类概念。
    CategoryStore::setCategory(sourcePath, targetCategory);
    // 改完记录后延后刷新视图（文件未移动，仅重排显示）
    QTimer::singleShot(0, this, &IconGridWindow::requestRefresh);
}

// 保存条目顺序
// 作者：谭征
void IconGridWindow::saveItemOrder(const QString& category) {
    if (m_isBoxWindow || category.isEmpty() || !m_allItems.contains(category)) return;

    QStringList order;
    order.reserve(m_allItems[category].size());
    for (const auto& item : m_allItems[category]) {
        order.append(item.sourcePath);   // 以 sourcePath 作稳定 key：同一文件每次扫描路径不变
    }
    SettingsManager sm;
    sm.saveValue(itemOrderKey(category), order);
}

// 应用条目顺序
// 作者：谭征
void IconGridWindow::applyItemOrder(QVector<DesktopItem>& items, const QString& category) {
    if (m_isBoxWindow || category.isEmpty()) return;

    SettingsManager sm;
    const QStringList order = sm.loadValue(itemOrderKey(category)).toStringList();
    if (order.isEmpty()) return;

    // 以 sourcePath 为稳定 key（同一文件每次扫描路径不变，而 QUuid id 每次重建、fileName 可能撞名）。
    // 兼容旧版本以文件名作 key 的残留数据：匹配不到 sourcePath 时回退按 fileName 查找。
    QMap<QString, DesktopItem> byPath;
    QMap<QString, DesktopItem> byName;
    for (const auto& item : items) {
        byPath.insert(item.sourcePath, item);
        byName.insert(QFileInfo(item.sourcePath).fileName(), item);
    }

    QVector<DesktopItem> reordered;
    reordered.reserve(items.size());
    QSet<QString> placed;   // 用 sourcePath 去重，避免同名文件被重复放置
    for (const QString& key : order) {
        auto it = byPath.find(key);
        if (it == byPath.end()) it = byName.find(key);
        if (it == byName.end() || placed.contains(it->sourcePath)) continue;
        reordered.append(it.value());
        placed.insert(it->sourcePath);
    }
    for (const auto& item : items) {
        if (!placed.contains(item.sourcePath)) reordered.append(item);
    }
    items = reordered;
}

// 移除条目顺序
// 作者：谭征
void IconGridWindow::removeItemOrder(const QString& category) {
    if (m_isBoxWindow || category.isEmpty()) return;
    SettingsManager sm;
    sm.saveValue(itemOrderKey(category), QStringList());
}

Qt::Edges IconGridWindow::detectResizeEdges(const QPoint& pos) const {
    const int margin = 5;
    Qt::Edges edges;
    if (pos.x() < margin) edges |= Qt::LeftEdge;
    if (pos.x() > width() - margin) edges |= Qt::RightEdge;
    if (pos.y() > height() - margin) edges |= Qt::BottomEdge;
    return edges;
}

Qt::CursorShape IconGridWindow::resizeCursorShape(Qt::Edges edges) const {
    // 左/右 + 底部组合（左下角）显示对角线光标（左上↔右下）；仅左右显示水平光标；仅底部显示垂直光标
    if ((edges & (Qt::LeftEdge | Qt::RightEdge)) && (edges & Qt::BottomEdge))
        return Qt::SizeFDiagCursor;
    if (edges & (Qt::LeftEdge | Qt::RightEdge))
        return Qt::SizeHorCursor;
    if (edges & Qt::BottomEdge)
        return Qt::SizeVerCursor;
    return Qt::ArrowCursor;
}

// 悬停光标：在子控件上也能感知鼠标移动并切换边缘光标
// 作者：谭征
void IconGridWindow::updateHoverCursor(const QPoint& pos) {
    // 拖动 / 缩放 / 锁定时不干预光标（交由对应逻辑处理）
    if (m_locked || m_dragging || m_resizing) return;
    // C2：加变化检测。本函数挂在【每个子控件】（含每个图标与标签）的 MouseMove 上，
    // 是逐像素频率；而 QWidget::setCursor() 在同值时不会短路，每次都会走一遍平台层设置光标。
    // 边缘光标只在窗口最外圈 5px 内变化，绝大多数 move 的目标形状与上次相同。
    const int want = static_cast<int>(resizeCursorShape(detectResizeEdges(pos)));
    if (want == m_lastHoverCursorShape) return;
    m_lastHoverCursorShape = want;
    setCursor(static_cast<Qt::CursorShape>(want));
}

// 启用child鼠标tracking
// 作者：谭征
void IconGridWindow::enableChildMouseTracking(QWidget* w) {
    if (!w) return;
    w->setMouseTracking(true);
    w->installEventFilter(this);
    const QObjectList children = w->children();
    for (QObject* child : children) {
        if (auto* cw = qobject_cast<QWidget*>(child))
            enableChildMouseTracking(cw);
    }
}

// 开始缩放
// 作者：谭征
void IconGridWindow::startResize(const QPoint& globalPos, Qt::Edges edges) {
    m_resizing = true;
    m_resizeEdges = edges;
    m_resizeStartGeom = frameGeometry();
    m_resizeStartPos = globalPos;
}

// do缩放
// 作者：谭征
void IconGridWindow::doResize(const QPoint& globalPos) {
    if (!m_resizing || m_resizeEdges == Qt::Edges()) return;

    const int dx = globalPos.x() - m_resizeStartPos.x();
    const int dy = globalPos.y() - m_resizeStartPos.y();

    int newX = m_resizeStartGeom.x();
    int newY = m_resizeStartGeom.y();
    int newW = m_resizeStartGeom.width();
    int newH = m_resizeStartGeom.height();

    const QSize minSize = minimumSizeHint().isValid() ? minimumSizeHint() : QSize(480, 40);

    if (m_resizeEdges & Qt::LeftEdge) {
        newX = m_resizeStartGeom.x() + dx;
        newW = m_resizeStartGeom.width() - dx;
        if (newW < minSize.width()) {
            newW = minSize.width();
            newX = m_resizeStartGeom.right() - newW + 1;
        }
    }
    if (m_resizeEdges & Qt::RightEdge) {
        newW = m_resizeStartGeom.width() + dx;
        if (newW < minSize.width()) newW = minSize.width();
    }
    if (m_resizeEdges & Qt::BottomEdge) {
        newH = m_resizeStartGeom.height() + dy;
        if (newH < minSize.height()) newH = minSize.height();
    }

    setGeometry(newX, newY, newW, newH);

    // 非折叠状态下记录当前尺寸作为展开尺寸，便于下次展开恢复
    if (!m_gridCollapsed) {
        m_expandedWidth = newW;
        m_expandedHeight = newH;
    }
}

// 标题栏本身不承载任何编辑交互，不会与输入/选择冲突。
// 作者：谭征
void IconGridWindow::mousePressEvent(QMouseEvent* event) {
    // 全屏收纳模式下窗口固定铺满工作区，不再支持拖动/缩放
    if (m_fencesMode) { QWidget::mousePressEvent(event); return; }
    // 落在图标可视区上的按下由 DesktopIconButton 自己消费并上报（onIconSelectionRequested）；
    // 能走到这里的就是空白 / 边距：左键与右键都先结束正在进行的重命名并清空选中，
    // 与 Windows 桌面“点空白＝取消选中”一致。
    if (event->button() == Qt::LeftButton || event->button() == Qt::RightButton) {
        commitActiveRename();
        clearSelection();
        armGridHotkey(this);    // 自愈式重装快捷键钩子（点空白也应让随后按 F2/Delete 可用）
    }
    if (event->button() == Qt::LeftButton && !m_locked) {
        // 非锁定状态下：在窗口边缘按下开始缩放，否则开始拖拽移动窗口
        Qt::Edges edges = detectResizeEdges(event->pos());
        if (edges != Qt::Edges()) {
            startResize(event->globalPos(), edges);
            setCursor(resizeCursorShape(edges));   // 按下即显示缩放光标
        } else {
            m_dragging = true;
            m_dragPos = event->globalPos() - frameGeometry().topLeft();
            setCursor(Qt::SizeAllCursor);          // 按下即显示移动（四向箭头）光标
        }
        event->accept();
        return;
    }
    // 锁定状态下既不移动也不缩放，交由默认处理
    QWidget::mousePressEvent(event);
}

// 鼠标移动事件
// 作者：谭征
void IconGridWindow::mouseMoveEvent(QMouseEvent* event) {
    // 全屏收纳模式下不处理窗口边缘缩放/拖动光标
    if (m_fencesMode) { QWidget::mouseMoveEvent(event); return; }
    if (m_locked) {
        // 锁定状态下：既不可移动也不可缩放
        unsetCursor();
        QWidget::mouseMoveEvent(event);
        return;
    }
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        setCursor(Qt::SizeAllCursor);              // 拖动移动中：四向箭头
        QPoint target = event->globalPos() - m_dragPos;
        // —— 自动对齐（2026-09-24）——
        // 拖动收纳盒/主整理窗口时：先试「与其它挂件窗口/屏幕边缘磁吸对齐」，都没命中则
        // 左上角吸附到桌面图标网格（格子对齐）。Alt 按住 = 临时关闭（想精确摆位时用）。
        // 每帧都从当前鼠标位置重算自由坐标（而非在已吸附坐标上累加），所以不会"吸住后拖不动"。
        if (WindowSnap::enabledNow()) {
            target = WindowSnap::resolve(target, size(),
                                         WindowSnap::collectSnapTargets(this),
                                         WindowSnap::gridOrigin(),
                                         nullptr, nullptr,
                                         // 距桌面顶部留出一根折叠收纳盒的高度（不贴顶边）
                                         WindowSnap::topLimitGlobal(QRect(target, size())));
        }
        move(target);
        event->accept();
    } else if (m_resizing && (event->buttons() & Qt::LeftButton)) {
        setCursor(resizeCursorShape(m_resizeEdges)); // 拖动缩放中：对应方向缩放光标
        doResize(event->globalPos());
        event->accept();
    } else {
        // 非锁定状态下鼠标悬停边缘时显示对应光标
        updateHoverCursor(event->pos());
        event->accept();
    }
    QWidget::mouseMoveEvent(event);
}

// 应用级“点别处即提交”守卫（装在 qApp 上）：见 cpp 中的实现说明。
// 作者：谭征
bool IconGridWindow::eventFilter(QObject* watched, QEvent* event) {
    // 类型闸门（防御性保留）
    // 本过滤器现在只挂在**本窗口子件**上（m_moreBtn / m_fencesSearchEdit / 每个图标与标签），
    // watched 理论上都是 QWidget。但保留这道闸门作为防御：万一未来某个非 QWidget 的
    // QObject 被装了本过滤器，下面多处对 watched 直接做 `static_cast<QWidget*>`（典型如
    // MouseMove 分支的 `w->mapTo(this, ...)`）就是未定义行为。
    // 「点空白取消选中」已剥离到独立的 PressOutsideCancelFilter（挂 qApp）—— 本窗口
    // **不再**直接挂 qApp，因此这里不会收到 QWindow 这类非 QWidget 的全局事件，
    // 也彻底消除了「MouseMove 的 mapTo 对全进程窗口执行 → 解引用悬垂指针 → 启动即崩」。
    if (!qobject_cast<QWidget*>(watched)) return QWidget::eventFilter(watched, event);

    // 全屏浮动搜索框：Esc 清空搜索词并失焦（放在 m_fencesMode 早返回之前，确保全屏下也能命中）
    if (watched == m_fencesSearchEdit && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            m_fencesSearchEdit->clear();
            m_fencesSearchEdit->clearFocus();
            return true;
        }
    }
    // 全屏收纳模式下，盒子的拖放由其自身处理，窗口级事件过滤仅放行默认行为
    if (m_fencesMode) return QWidget::eventFilter(watched, event);

    // 「收起后，鼠标移动到标题自动展开」（Appearance/autoExpandOnHover，2026-09-22）
    // 口径：**收起状态下**，鼠标移到标题栏的折叠/展开按钮上即自动展开（等于替用户点一次该按钮）；
    // 取消勾选则必须自己点击。
    // 只认「收起态 + 鼠标移入折叠按钮」，不做"整条标题栏 hover"：
    // 收起后整窗就只剩这条标题栏，若整条都判成触发展开区，鼠标随手划过屏幕碰到盒子就会弹开。
    // 关闭时第一句就短路（先比一个 bool 成员），零额外开销。
    if (m_gridCollapsed && watched == m_moreBtn && event->type() == QEvent::Enter) {
        if (ThemeManager::instance()->autoExpandOnHoverEnabled()) {
            toggleGrid();
            return true;
        }
    }
    // 分类标签「悬停切换」（设置中心 → 分区标签切换 = 悬停切换）：
    // 默认的「点击切换」下这段整体短路（先比一个 int 成员，零额外开销），
    // 保证鼠标划过标签栏绝不会切换分类 —— 切换只由左键点击触发。
    if (m_tagSwitchMode == 1 && event->type() == QEvent::HoverEnter) {
        if (auto* btn = qobject_cast<QToolButton*>(watched)) {
            if (btn->property("isCategoryTab").toBool()) {
                const QString cat = QString::fromUtf8(btn->property("category").toByteArray());
                if (!cat.isEmpty() && cat != m_currentCategory) startHoverSwitch(btn);
            }
        }
    }
    // 分类标签按钮拖拽排序：仅左键按下才记录起点；右键留给上下文菜单（重命名/解散），
    // 否则右键按下后轻微移动会被误判为拖拽，导致右键菜单无法弹出（自定义分类尤为明显）。
    if (event->type() == QEvent::MouseButtonPress) {
        if (auto* btn = qobject_cast<QToolButton*>(watched)) {
            // 只有**分类标签**才参与标签拖拽排序。网格图标虽然也是 QToolButton 且同样带
            // category 属性，但必须排除，否则图标的左键拖动会被误当成标签拖拽
            // （见 buildTabs 中 isCategoryTab 的说明）。
            const bool isTab = btn->property("isCategoryTab").toBool();
            const QString category = QString::fromUtf8(btn->property("category").toByteArray());
            if (isTab && !category.isEmpty()) {
                auto* me = static_cast<QMouseEvent*>(event);
                if (me->button() == Qt::LeftButton) {
                    m_dragSourceTab = btn;
                    m_dragStartPos = me->globalPos();
                    m_tabDragging = false;
                }
            }
        }
    } else if (event->type() == QEvent::MouseMove) {
        // 分类标签拖拽启动：必须仍是左键按住，避免右键移动误触发拖拽；
        // 且必须是分类标签（isCategoryTab），网格图标有自己的拖拽实现。
        // C2：把 m_dragSourceTab 的非空判断提到最前 —— 常态下它恒为空，
        // 于是每次鼠标移动都能省掉一次 qobject_cast 与一次元属性名查找
        // （property("isCategoryTab") 要按名字查 QMetaObject，比一次指针比较贵得多）。
        if (m_dragSourceTab) {
            if (auto* btn = qobject_cast<QToolButton*>(watched)) {
                if (btn->property("isCategoryTab").toBool()
                    && btn == m_dragSourceTab && !m_tabDragging
                    && (static_cast<QMouseEvent*>(event)->buttons() & Qt::LeftButton)) {
                    auto* me = static_cast<QMouseEvent*>(event);
                    if ((me->globalPos() - m_dragStartPos).manhattanLength() > QApplication::startDragDistance()) {
                        m_tabDragging = true;
                        startTabDrag(btn);
                    }
                }
            }
        }
        // 原有：感知子控件上的鼠标移动并切换边缘光标
        auto* w = static_cast<QWidget*>(watched);
        auto* me = static_cast<QMouseEvent*>(event);
        const QPoint pos = (w == this) ? me->pos() : w->mapTo(this, me->pos());
        updateHoverCursor(pos);
    } else if (event->type() == QEvent::MouseButtonRelease) {
        // 如果没有进入拖拽，只是普通点击，QToolButton 的 clicked 信号正常触发
        if (!m_tabDragging) {
            m_dragSourceTab = nullptr;
        }
    } else if (event->type() == QEvent::DragEnter) {
        // tabBar 接受分类标签拖拽（text=分类名）、图标跨分类拖拽（x-desktopitem-id）、
        // 以及从真实桌面/资源管理器拖入的文件（urls）
        if (watched == m_tabBar) {
            auto* de = static_cast<QDragEnterEvent*>(event);
            if (de->mimeData()->hasFormat(QStringLiteral("application/x-desktopitem-id")) ||
                (de->mimeData()->hasText() && !de->mimeData()->text().isEmpty())) {
                de->acceptProposedAction();
                return true;
            }
            if (de->mimeData()->hasUrls()) {
                // 外部文件：只登记归属，绝不搬动/删除真实文件 → 一律 Copy 语义。
                // 照搬 acceptProposedAction() 会把 Explorer 的 MoveAction 当默认动作接受
                // （2026-09-14 用户投诉：拖拽期间 Windows 在移动文件）。
                de->setDropAction(Qt::CopyAction);
                de->accept();
                return true;
            }
        }
        // 图标网格内接受图标拖拽（同分类重排序 / 移动到当前分类）
        if (watched == m_gridContainer || isGridChild(watched)) {
            auto* de = static_cast<QDragEnterEvent*>(event);
            if (de->mimeData()->hasFormat(QStringLiteral("application/x-desktopitem-id"))) {
                de->acceptProposedAction();
                return true;
            }
            if (de->mimeData()->hasUrls()) {
                de->setDropAction(Qt::CopyAction);
                de->accept();
                return true;
            }
        }
    } else if (event->type() == QEvent::DragMove) {
        // 拖拽过程中实时显示插入位置指示线
        if (watched == m_tabBar) {
            auto* de = static_cast<QDragMoveEvent*>(event);
            if (de->mimeData()->hasFormat(QStringLiteral("application/x-desktopitem-id")) ||
                (de->mimeData()->hasText() && !de->mimeData()->text().isEmpty()) ||
                de->mimeData()->hasUrls()) {
                hideGridInsertMarker();   // 移到标签栏时隐藏网格指示线
                updateInsertMarker(de->pos());
                if (de->mimeData()->hasFormat(QStringLiteral("application/x-desktopitem-id"))) {
                    de->acceptProposedAction();
                } else {
                    // 外部文件一律降级 Copy：只登记归属，绝不搬动真实文件
                    de->setDropAction(Qt::CopyAction);
                    de->accept();
                }
                return true;
            }
        }
        // 图标在网格内拖动：实时显示“将插入到此处”的竖直指示线
        if (watched == m_gridContainer || isGridChild(watched)) {
            auto* de = static_cast<QDragMoveEvent*>(event);
            if (de->mimeData()->hasFormat(QStringLiteral("application/x-desktopitem-id")) ||
                de->mimeData()->hasUrls()) {
                if (!de->mimeData()->hasFormat(QStringLiteral("application/x-desktopitem-id"))) {
                    de->setDropAction(Qt::CopyAction);
                    de->accept();
                } else {
                    de->acceptProposedAction();
                }
                hideInsertMarker();       // 移回网格时隐藏标签栏指示线
                const QPoint localPos = (watched == m_gridContainer)
                    ? de->pos()
                    : m_gridContainer->mapFromGlobal(static_cast<QWidget*>(watched)->mapToGlobal(de->pos()));
                updateGridInsertMarker(localPos);
                de->acceptProposedAction();
                return true;
            }
        }
    } else if (event->type() == QEvent::Drop) {
        if (watched == m_tabBar) {
            auto* de = static_cast<QDropEvent*>(event);
            // 找到被命中的分类按钮（子控件可能是按钮内的 label，向上追溯到 QToolButton）
            auto resolveTabCat = [this](const QPoint& pos) -> QString {
                QWidget* w = m_tabBar->childAt(pos);
                while (w && w != m_tabBar) {
                    if (auto* btn = qobject_cast<QToolButton*>(w)) {
                        return QString::fromUtf8(btn->property("category").toByteArray());
                    }
                    w = w->parentWidget();
                }
                return QString();
            };

            if (de->mimeData()->hasFormat(QStringLiteral("application/x-desktopitem-id"))) {
                const QUuid id = QUuid::fromString(QString::fromUtf8(
                    de->mimeData()->data(QStringLiteral("application/x-desktopitem-id"))));
                const QString sourceCat = QString::fromUtf8(
                    de->mimeData()->data(QStringLiteral("application/x-source-category")));
                const QString targetCat = resolveTabCat(de->pos());
                if (!targetCat.isEmpty() && targetCat != sourceCat) {
                    if (!m_isBoxWindow) {
                        handleStorageDrop(id, sourceCat, targetCat);
                    } else {
                        // 收纳盒窗口：保持原“仅内存重排/跨分类”行为，不搬动真实文件
                        moveItemToCategory(id, sourceCat, targetCat);
                    }
                }
                hideInsertMarker();
                hideGridInsertMarker();
                de->acceptProposedAction();
                return true;
            }
            // 从 Windows 资源管理器/桌面直接拖入的文件：只登记归属，不搬动文件
            if (!m_isBoxWindow && de->mimeData()->hasUrls()) {
                const QString targetCat = resolveTabCat(de->pos());
                if (!targetCat.isEmpty()) {
                    for (const QUrl& url : de->mimeData()->urls()) {
                        if (url.isLocalFile())
                            CategoryStore::setCategory(url.toLocalFile(), targetCat);
                    }
                    onItemFileMovedOut();   // 分类记录已更新，延后刷新视图
                }
                hideInsertMarker();
                hideGridInsertMarker();
                // 显式 Copy：本程序只改分类记录，绝不搬动/删除真实文件。
                // 照搬 acceptProposedAction() 会把 Explorer 的 MoveAction 当默认动作接受，
                // 等于授权系统移动文件（2026-09-14 用户投诉）。
                de->setDropAction(Qt::CopyAction);
                de->accept();
                return true;
            }
            const QString category = de->mimeData()->text();
            if (!category.isEmpty()) {
                handleTabDrop(category, de->pos());
            }
            hideInsertMarker();
            hideGridInsertMarker();
            de->acceptProposedAction();
            return true;
        }

        // 图标在网格内释放：同分类重排序，或从其他分类移入当前分类
        if (watched == m_gridContainer || isGridChild(watched)) {
            auto* de = static_cast<QDropEvent*>(event);
            if (de->mimeData()->hasFormat(QStringLiteral("application/x-desktopitem-id"))) {
                const QUuid id = QUuid::fromString(QString::fromUtf8(
                    de->mimeData()->data(QStringLiteral("application/x-desktopitem-id"))));
                const QString sourceCat = QString::fromUtf8(
                    de->mimeData()->data(QStringLiteral("application/x-source-category")));
                const QPoint localPos = (watched == m_gridContainer)
                    ? de->pos()
                    : m_gridContainer->mapFromGlobal(static_cast<QWidget*>(watched)->mapToGlobal(de->pos()));
                const int targetIndex = targetIndexFromDropPos(localPos);

                if (m_isBoxWindow) {
                    if (sourceCat == m_currentCategory) {
                        reorderItemInCategory(m_currentCategory, id, targetIndex);
                    } else if (!m_currentCategory.isEmpty()) {
                        moveItemToCategory(id, sourceCat, m_currentCategory, targetIndex);
                    }
                } else {
                    const QString sourcePath = sourcePathOf(id, sourceCat);
                    if (sourceCat == m_currentCategory) {
                        reorderItemInCategory(m_currentCategory, id, targetIndex);
                    } else if (!m_currentCategory.isEmpty()) {
                        // A 方案：跨分类拖放只改分类记录，文件不搬动。拖入“其它”即归入“其它”。
                        CategoryStore::setCategory(sourcePath, m_currentCategory);
                        QTimer::singleShot(0, this, &IconGridWindow::requestRefresh);
                    }
                }
                hideGridInsertMarker();
                de->acceptProposedAction();
                return true;
            }
            // 从 Windows 资源管理器/桌面直接拖入网格：只登记归属，不搬动文件
            if (!m_isBoxWindow && de->mimeData()->hasUrls() && !m_currentCategory.isEmpty()) {
                for (const QUrl& url : de->mimeData()->urls()) {
                    if (url.isLocalFile())
                        CategoryStore::setCategory(url.toLocalFile(), m_currentCategory);
                }
                onItemFileMovedOut();   // 分类记录已更新，延后刷新视图
                hideGridInsertMarker();
                // 同上：显式 Copy，绝不授权系统移动文件。
                de->setDropAction(Qt::CopyAction);
                de->accept();
                return true;
            }
        }
    } else if (event->type() == QEvent::DragLeave) {
        // 拖拽离开 tabBar 或网格时隐藏对应的插入指示线
        if (watched == m_tabBar) {
            hideInsertMarker();
        } else if (watched == m_gridContainer || isGridChild(watched)) {
            hideGridInsertMarker();
        }
    } else if (event->type() == QEvent::Leave) {
        if (!m_dragging && !m_resizing && !m_locked && !m_tabDragging) {
            unsetCursor();
        }
    }
    return QWidget::eventFilter(watched, event);
}

// 鼠标移入本窗口 → 悬停显示模式下亮出头部菜单按钮（始终模式下这次调用是幂等的空动作）。
// 用 Enter/Leave 而不是逐像素追踪：Qt 保证父部件在"鼠标移到自己的子部件上"时**不会**收到 Leave
// （进出子部件只在与新位置无关的层级上派发），所以穿梭于标题栏按钮之间不会闪隐。
// 作者：谭征
void IconGridWindow::enterEvent(QEvent* event) {
    m_pointerInside = true;
    applyMenuRevealVisibility();
    QWidget::enterEvent(event);
}

// 离开事件
// 作者：谭征
void IconGridWindow::leaveEvent(QEvent* event) {
    m_pointerInside = false;
    applyMenuRevealVisibility();
    if (!m_dragging && !m_resizing && !m_locked) {
        unsetCursor();
    }
    QWidget::leaveEvent(event);
}

// 鼠标松开事件
// 作者：谭征
void IconGridWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const bool hadInteraction = m_dragging || m_resizing;
        m_dragging = false;
        m_resizing = false;
        m_resizeEdges = Qt::Edges();
        unsetCursor();                             // 释放后复位，交由悬停逻辑重新判定光标
        event->accept();
        // 拖拽移动或缩放结束后通知主窗口保存当前几何
        if (hadInteraction) {
            emit windowGeometryChanged();
        }
    }
    QWidget::mouseReleaseEvent(event);
}

// 为收纳盒窗口提供可关闭标题栏按钮；默认隐藏以保持主窗口外观不变
// 作者：谭征
void IconGridWindow::setCloseButtonVisible(bool visible) {
    if (m_closeBtn) {
        m_closeBtn->setVisible(visible);
    }
}

// 是否强制保证存在“快捷方式”分类；主窗口需要，收纳盒不需要
// 作者：谭征
void IconGridWindow::setForceShortcutCategory(bool force) {
    m_forceShortcutCategory = force;
}

// force快捷方式分类
// 作者：谭征
bool IconGridWindow::forceShortcutCategory() const {
    return m_forceShortcutCategory;
}

// 标识该窗口是否为独立收纳盒窗口，用于区分右键菜单文案等
// 作者：谭征
void IconGridWindow::setIsBoxWindow(bool isBox) {
    m_isBoxWindow = isBox;
}

// 判断盒子窗口
// 作者：谭征
bool IconGridWindow::isBoxWindow() const {
    return m_isBoxWindow;
}

// 当前是否处于折叠状态
// 作者：谭征
bool IconGridWindow::isCollapsed() const {
    return m_gridCollapsed;
}

// 展开尺寸
// 作者：谭征
QSize IconGridWindow::expandedSize() const {
    return QSize(m_expandedWidth, m_expandedHeight);
}

// 设置展开尺寸
// 作者：谭征
void IconGridWindow::setExpandedSize(const QSize& size) {
    if (size.isValid() && size.width() > 0 && size.height() > 0) {
        m_expandedWidth = size.width();
        m_expandedHeight = size.height();
    }
}

// 关闭事件
// 作者：谭征
void IconGridWindow::closeEvent(QCloseEvent* event) {
    // 关闭前通知主窗口保存当前几何
    emit windowGeometryChanged();
    // 退出快捷键路由：否则一个已关闭的窗口仍能消费 F2/Delete（表现为别的程序里按 Delete 没反应）。
    unregisterGridWindow(this);
    QWidget::closeEvent(event);
}
