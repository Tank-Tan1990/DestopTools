/*
 * @file desktopiconbutton.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "desktopiconbutton.h"
#include "theme.h"
#include "desktopscanner.h"
#include "shellops.h"
#include "glassmessagebox.h"
#include "desktopmirrorwindow.h"   // raiseAboveBandWindows：编辑框是独立顶层窗口，需压在收纳盒/助手之上
#include "diagtrace.h"              // 临时：重命名链路诊断日志
#include "editorwatch.h"           // 编辑框“真可见 / 真焦点”探测与强制置顶（与 Dock 图标共用）
#include "icongridwindow.h"        // 拖拽期冻结视图重建（setIconDragInProgress）
#include <QMouseEvent>
#include <QDrag>
#include <QMimeData>
#include <QApplication>
#include <QScreen>
#include <QDesktopServices>
#include <QUrl>
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QStyle>
#include <QTimer>
#include <QDir>
#include <QPainter>
#include <QFontMetrics>
#include <QCursor>
#include <QGraphicsOpacityEffect>
#include <QPointer>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QInputMethodEvent>
#include <QKeyEvent>

#ifdef Q_OS_WIN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#endif

namespace {

#ifdef Q_OS_WIN
// 取 Windows 桌面图标标题字体（SPI_GETICONTITLELOGFONT）。与 Dock 侧同一来源，
// 保证“收纳盒里的重命名框”和“桌面上的重命名框”字号字形一致。
QFont iconTitleFont()
{
    LOGFONTW lf = {0};
    if (SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(lf), &lf, 0)) {
        QFont f = QString::fromWCharArray(lf.lfFaceName);
        // lfHeight 为负表示字号（像素的负值），直接用它的绝对值作为逻辑像素字号。
        const int px = (lf.lfHeight < 0) ? -lf.lfHeight : (lf.lfHeight > 0 ? lf.lfHeight : 12);
        f.setPixelSize(qBound(9, px, 24));
        return f;
    }
    QFont f;
    f.setPixelSize(12);
    return f;
}

// 编辑期把本线程的输入队列挂靠到当前前台线程：本进程通常不在前台（收纳盒窗口是
// WS_EX_NOACTIVATE），此时 SetForegroundWindow / SetFocus 会被前台锁静默忽略，
// 编辑框拿到的是“假焦点”——打字进不去、回车不生效。挂靠后这些调用才真正生效，
// 且必须**整个编辑期保持挂靠**（提前解挂会让焦点被前台锁收回）。
void attachForegroundThread(quintptr* attachedTid)
{
    if (!attachedTid || *attachedTid) return;
    DWORD fgTid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    if (fgTid && fgTid != GetCurrentThreadId()) {
        if (AttachThreadInput(GetCurrentThreadId(), fgTid, TRUE)) *attachedTid = fgTid;
    }
}

void detachForegroundThread(quintptr* attachedTid)
{
    if (!attachedTid || !*attachedTid) return;
    AttachThreadInput(GetCurrentThreadId(), (DWORD)*attachedTid, FALSE);
    *attachedTid = 0;
}

void forceForegroundWindow(HWND h, quintptr* attachedTid)
{
    if (!h) return;
    // 自愈兜底（2026-09-13）：本程序的应用级 native 过滤器（DesktopImmunityFilter）
    // 会把「非挂件顶层窗口」一律加上 WS_EX_NOACTIVATE，本意是让辅助窗口别抢焦点 ——
    // 但这条规则同样命中了内联重命名编辑框，而带该样式的窗口**永远无法成为前台窗口**：
    // SetForegroundWindow / SwitchToThisWindow 会被系统直接拒绝，于是「框弹出来了、
    // Qt 也报 hasFocus()=true，却打不进字、回车提交的是旧名」，观感就是重命名失效。
    // 过滤器侧已豁免输入窗口（治本）；这里再摘一次样式作为兜底，保证任何时序下编辑框
    // 都能被真正激活：每次错峰补抢焦点都会重跑本函数，因此即使被再次加上也会立刻自愈。
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_NOACTIVATE) {
        SetWindowLongPtrW(h, GWL_EXSTYLE, ex & ~WS_EX_NOACTIVATE);
        SetWindowPos(h, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }
    attachForegroundThread(attachedTid);
    AllowSetForegroundWindow(ASFW_ANY);
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        typedef void (WINAPI* PSwitchToThisWindow)(HWND, BOOL);
        if (auto pSwitch = reinterpret_cast<PSwitchToThisWindow>(
                GetProcAddress(u32, "SwitchToThisWindow")))
            pSwitch(h, TRUE);
    }
    ::SetForegroundWindow(h);
    ::SetFocus(h);
}
#else
QFont iconTitleFont() { QFont f; f.setPixelSize(12); return f; }
void attachForegroundThread(quintptr*) {}
void detachForegroundThread(quintptr*) {}
void forceForegroundWindow(void*, quintptr*) {}
#endif

// 菜单首次点击不被“吞掉”：无框窗口右键不激活顶层窗口，菜单弹出时进程不在前台，
// 第一次点击会被前台锁吃掉。授权本进程前台即可（与分类标签右键菜单同一手段）。
// 用 ASFW_ANY 而不是只授权本进程：菜单弹出自 ShellOps（可能再委托给第三方扩展），
// 放宽授权范围才能保证“第一次点重命名/属性就一定生效”。
void grantForegroundForMenu()
{
#ifdef Q_OS_WIN
    AllowSetForegroundWindow(ASFW_ANY);
#endif
}

}   // namespace

// 编辑会话的系统级输入钩子
// 为什么必须是系统级钩子：本控件所在窗口带 WS_EX_NOACTIVATE（Win+D 免疫所必需），
// **永远拿不到键盘焦点**，因此由“帧它是否能拿到焦点”来决定回车/Esc/点框外是否生效是不可靠的：
// · Enter / Esc 能否送达编辑框 —— 碰运气（本进程不在前台时 SetFocus 会被前台锁静默忽略）；
// · “点框外即提交” 能否经 Qt 路由回本窗口 —— 同样碰运气（Qt 鼠标事件先派发给顶层
// QWidgetWindow，不在编辑框的 parent 链上，不能用对象父子链判定）。
// Dock 侧踩过完全相同的坑，结论一致：编辑期的「回车提交 / Esc 取消 / 点框外即提交」
// 必须下沉到 WH_KEYBOARD_LL + WH_MOUSE_LL。
// 纪律：只在编辑期安装，收框即卸载；回调里只「识别 + 投递 0ms 请求」后立即返回
// （低级钩子回调超时会被 Windows 静默摘除，而句柄仍非空）。
#ifdef Q_OS_WIN
LRESULT CALLBACK editKeyHookProc(int nCode, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK editMouseHookProc(int nCode, WPARAM wParam, LPARAM lParam);

static HHOOK s_editKeyHook = nullptr;
static HHOOK s_editMouseHook = nullptr;
static bool  s_editHooksActive = false;
static DesktopIconButton* s_editOwner = nullptr;   // 当前编辑会话的宿主按钮（仅标识，非拥有）

static void uninstallEditHooks(DesktopIconButton* owner);   // 前置声明：换主时需先卸旧的

static void installEditHooks(DesktopIconButton* owner)
{
    if (!owner) return;
    if (s_editOwner && s_editOwner != owner) uninstallEditHooks(s_editOwner);
    s_editOwner = owner;
    // 先卸后装：句柄非空 ≠ 钩子有效，低级钩子回调超时会被系统静默摘除。
    if (s_editKeyHook)   { UnhookWindowsHookEx(s_editKeyHook);   s_editKeyHook = nullptr; }
    if (s_editMouseHook) { UnhookWindowsHookEx(s_editMouseHook); s_editMouseHook = nullptr; }
    HMODULE mod = GetModuleHandleW(nullptr);
    s_editKeyHook   = SetWindowsHookExW(WH_KEYBOARD_LL, editKeyHookProc,   mod, 0);
    s_editMouseHook = SetWindowsHookExW(WH_MOUSE_LL,    editMouseHookProc, mod, 0);
    s_editHooksActive = (s_editKeyHook != nullptr || s_editMouseHook != nullptr);
}

static void uninstallEditHooks(DesktopIconButton* owner)
{
    if (!owner) return;
    if (s_editOwner && s_editOwner != owner) return;   // 不是当前持有者：勿误卸
    s_editOwner = nullptr;
    s_editHooksActive = false;
    if (s_editKeyHook)   { UnhookWindowsHookEx(s_editKeyHook);   s_editKeyHook = nullptr; }
    if (s_editMouseHook) { UnhookWindowsHookEx(s_editMouseHook); s_editMouseHook = nullptr; }
}

// Enter 提交 / Esc 取消。输入法合成中一律放行（Enter = 确认候选，Esc = 取消候选）。
LRESULT CALLBACK editKeyHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        const KBDLLHOOKSTRUCT* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        DesktopIconButton* owner = s_editOwner;
        if (k && owner && owner->isEditing() && !owner->isImeComposing()) {
            if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000)
                || (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) {
                return CallNextHookEx(nullptr, nCode, wParam, lParam);
            }
            const int vk = static_cast<int>(k->vkCode);
            if (vk == VK_RETURN || vk == VK_ESCAPE) {
                QPointer<DesktopIconButton> p(owner);
                const bool doCommit = (vk == VK_RETURN);
                QTimer::singleShot(0, owner, [p, doCommit]() {
                    if (!p || !p->isEditing()) return;
                    if (doCommit) p->commitInlineRename();
                    else          p->cancelInlineRename();
                });
                return 1;   // 吞掉：该键已由编辑框消费，不再落到前台程序
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// 点编辑框以外即提交（Windows 桌面语义）。**不吞事件**（点击照常派发），
// 几何判定放在 0ms 请求里做，回调本身保持轻量。
LRESULT CALLBACK editMouseHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION
        && (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN
            || wParam == WM_MBUTTONDOWN || wParam == WM_XBUTTONDOWN)) {
        const MSLLHOOKSTRUCT* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        DesktopIconButton* owner = s_editOwner;
        if (ms && owner && owner->isEditing()) {
            const long nx = ms->pt.x, ny = ms->pt.y;   // 低级钩子给的是物理像素
            QPointer<DesktopIconButton> p(owner);
            QTimer::singleShot(0, owner, [p, nx, ny]() {
                if (p && p->isEditing())
                    p->handleEditClickOutside(QPoint(static_cast<int>(nx), static_cast<int>(ny)));
            });
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
#else
static void installEditHooks(DesktopIconButton*) {}
static void uninstallEditHooks(DesktopIconButton*) {}
#endif // Q_OS_WIN

DesktopIconButton::DesktopIconButton(const DesktopItem& item, QWidget* parent)
    : QToolButton(parent), m_item(item) {
    setFixedSize(72, 84);
    setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    setIconSize(QSize(48, 48));
    // 先放占位图标，真实图标稍后由定时器在主线程异步加载，避免大量图标同步取 Shell 导致卡死。
    setIcon(Theme::icon(QStringLiteral("tool_files")));
    setText(item.displayName);
    if (item.isSpecial)
        setToolTip(item.displayName);
    else
        setToolTip(item.displayName + QStringLiteral("\n") + item.sourcePath);
    setAutoRaise(true);
    setStyleSheet(Theme::iconButtonStyle());

    queueIconLoad(this);   // 构造函数末尾入队，等待事件循环空闲时取真实图标
}

DesktopIconButton::~DesktopIconButton() {
    // ① 先摘编辑会话的系统级钩子：本对象即将析构，钩子若仍指向它，紧随其后的回调
    // 就是访问已析构对象（低级钩子回调由系统在任意时刻发起，不受本栈控制）。
    // ② 再补发 editingChanged(false)：窗口的"编辑期冻结重建"记账据此解除。
    // 以前这里只把 m_editing 置 false 而不发信号 —— 一旦"正在编辑的那个按钮"在编辑期间
    // 被重建流程销毁，窗口就永久卡在「有图标正在编辑」上，后果是**两处同时失效且不可恢复**：
    // · F2 被窗口侧 isEditingAnyIcon() 挡死；
    // · 右键菜单的「重命名」被按钮自己的 m_editing 挡死（见 startInlineRename 的静默 return）。
    // 这正是"首次 F2 重命名后，F2 和右键重命名一起失效"的机理。
    // 与 Dock 侧统一：这里也不再写 ~DESTRUCT 诊断日志。原因同 dockiconbutton.cpp：
    // trace.log 位于用户桌面根目录的子目录下，注册桌面变更通知一旦递归，
    // 会和"重建销毁 → 写日志 → SHCN 回弹 → 再重建"形成自激循环，把 CPU 拉满、
    // 把选中集合清空（这正是"Dock 点中即失选"症状的机理）。见 registerChangeNotify() 注释。
    const bool wasEditing = m_editing;
    uninstallEditHooks(this);
    m_editing = false;
    m_editorFocused = false;
    if (m_editWatchTimer) m_editWatchTimer->stop();
    if (wasEditing) emit editingChanged(false);
    // 编辑框是无父顶层窗口，不随本控件析构自动销毁；若在编辑中被重建（refresh/重排），
    // 不显式清理就会在屏幕上留下一个永远收不掉的“幽灵输入框”。
    if (m_editor) {
        detachForegroundThread(&m_attachedTid);
        m_editor->hide();
        m_editor->removeEventFilter(this);
        delete m_editor;
        m_editor = nullptr;
    }
}

QVector<QPointer<DesktopIconButton>> DesktopIconButton::s_iconQueue;
QTimer* DesktopIconButton::s_iconTimer = nullptr;

// 避免一次性为大量图标同步调用 Shell（QFileIconProvider::icon）导致界面卡死。
// 作者：谭征
void DesktopIconButton::queueIconLoad(DesktopIconButton* btn) {
    if (!btn || btn->m_iconQueued) return;
    btn->m_iconQueued = true;
    s_iconQueue.append(QPointer<DesktopIconButton>(btn));
    if (!s_iconTimer) {
        // 定时器随应用程序生命周期存在；每拍按「时间预算」处理若干个，处理完即回到事件循环，
        // 从而保证 UI 始终可响应。
        // B4：间隔 0 → 30ms。0ms 定时器等于「事件循环一有空就跑」，
        // 启动期/大目录刷新期会被切成连续小块，鼠标在启动阶段明显发滞；
        // 30ms 的间隔让事件循环有确定的空档处理输入/绘制。
        // B7：30ms → 15ms。processIconQueue 已改为 8ms 时间预算制（见彼处），
        // 单拍阻塞上限不升反降；预热缓存（g_iconCache）命中后整队一拍清空，间隔不再是瓶颈，
        // 而冷取图（走 Shell 提取 1~20ms/个）的铺满速度提高约一倍，缓解“首次切分类图标逐个冒”。
        s_iconTimer = new QTimer(qApp);
        s_iconTimer->setInterval(15);
        QObject::connect(s_iconTimer, &QTimer::timeout, &DesktopIconButton::processIconQueue);
    }
    if (!s_iconTimer->isActive()) s_iconTimer->start();
}

// process图标队列
// 作者：谭征
void DesktopIconButton::processIconQueue() {
    // B4：batch 8 → 2。单个图标的首取要走 IExtractIcon / SHDefExtractIcon / 系统镜像列表，
    // 未命中缓存时 1~20ms；每拍 8 个就等于把事件循环钉住 40~160ms（启动期与整屏重建时最明显）。
    // B5：batch 2 → 1。GUI 线程同时承载着系统级低级键鼠钩子的同步回调，
    // 「单拍阻塞」直接换成「全系统鼠标的瞬时迟滞」。
    // B7：固定 batch 1 → **8ms 时间预算制**。动机：分类页缓存（HIT-RESORT）落地后，
    // 重访分类不再重建，但**首次**切分类仍是整屏重建 + 整队取图；固定每拍 1 个意味着哪怕全部
    // 命中进程内图标缓存 g_iconCache（QHash 查找，<0.1ms/个），26 个图标也要 26×30ms ≈ 780ms
    // 才铺满 —— 用户观感“图标一个一个慢慢冒”。预算制下：
    // · 缓存命中 ≈ 零成本 → 一拍内清空整队（重访/复用场景瞬时铺满）；
    // · 冷取图 1~20ms/个 → 预算用尽即停，单拍阻塞上限 = 预算 + 单个最慢取图，
    // 不劣于 B5 时代（单拍 1 个最慢 20ms），鼠标流畅度语义不变。
    QElapsedTimer budget;
    budget.start();
    const qint64 budgetMs = 8;
    while (!s_iconQueue.isEmpty()) {
        const int i = s_iconQueue.size() - 1;   // 与原实现一致：从队尾取（后入队先取）
        QPointer<DesktopIconButton> btn = s_iconQueue.at(i);
        s_iconQueue.removeLast();
        if (!btn) continue;                 // 控件已被销毁，跳过（不计时间）
        btn->m_iconQueued = false;
        btn->loadIconNow();
        if (budget.elapsed() >= budgetMs) break;   // 预算用尽，剩余留到下一拍
    }
    if (s_iconQueue.isEmpty() && s_iconTimer) s_iconTimer->stop();
}

// 加载图标now
// 作者：谭征
void DesktopIconButton::loadIconNow() {
    if (m_item.icon.isNull())
        // 箭头缩放经 Theme::shortcutArrowOverlayScale()：「在快捷方式图标上显示箭头」关闭时返回 0
        // → loadIcon / composeShortcutOverlay 原图返回（不叠加角标）。缓存键含该系数，两态不串味。
        m_item.icon = DesktopScanner::loadIcon(m_item, m_iconPx,
                                               Theme::shortcutArrowOverlayScale(m_arrowScale));
    if (!m_item.icon.isNull()) setIcon(m_item.icon);
}

// 选中态

// 作者：谭征
void DesktopIconButton::setSelected(bool sel) {
    if (m_selected == sel) return;
    m_selected = sel;
    update();
}

// 视图尺寸切换（大/中/小图标）：改单元格尺寸 + 图标绘制像素，并按新像素重取一次图标。
// 之所以要重取：图标是按 m_iconPx 走 Shell 取回的（进程内缓存键含尺寸），沿用旧尺寸的位图
// 会被 QToolButton 放大/缩小，放大档会明显发虚。
// 关键：**绝不能在这里同步 loadIconNow()**。一次切换会重建整屏按钮，逐个同步走 Shell 取图
// 会让界面明显卡死（本控件从构造起就用 processIconQueue 分批异步取图，正是为了避开这一点）。
// 正确做法是把 m_item.icon 清空并把按钮重新丢回异步队列；loadIconNow() 读的是调用时刻的
// m_iconPx，所以队列里的任务会自然按新尺寸取图。
// 作者：谭征
void DesktopIconButton::setViewMetrics(int cellW, int cellH, int iconPx) {
    const int px = qBound(16, iconPx, 256);
    const bool pxChanged = (px != m_iconPx);
    m_iconPx = px;
    setIconSize(QSize(px, px));
    setFixedSize(qMax(32, cellW), qMax(32, cellH));
    if (pxChanged) {
        m_item.icon = QIcon();                  // 旧尺寸位图作废
        if (!m_iconQueued) queueIconLoad(this); // 已取过图的按钮需重新入队；已在队列里的会按新尺寸取
    }
    update();
}

// 列表档开关（右键“查看”→ 列表）。几何仍由 setViewMetrics 给出（行宽 / 行高 / 图标像素），
// 本函数只切换绘制形态。基类的 ToolButtonStyle 在本档下不参与绘制（整块自绘），
// 仍同步一份是为了 sizeHint 与可访问性语义不至于和视觉相反。
// 作者：谭征
void DesktopIconButton::setListMode(bool on) {
    if (m_listMode == on) return;
    m_listMode = on;
    setToolButtonStyle(on ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonTextUnderIcon);
    update();
}

// 选中高亮：基底 QSS 背景是全透明的，所以先画一层淡蓝圆角底，再由基类绘制图标与文字，
// 视觉上等价于 Windows 桌面的“选中蓝框”，且不会盖住图标。
// 作者：谭征
void DesktopIconButton::paintEvent(QPaintEvent* event) {
    // 列表档整块自绘（左图标 + 右文字）；否则走 QToolButton 默认的“图标在上、文字在下”。
    if (m_listMode) {
        paintListMode();
        return;
    }
    if (m_selected) {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRect r = rect().adjusted(1, 1, -1, -1);
        p.setPen(QPen(QColor(34, 211, 238, 190), 1));
        p.setBrush(QColor(34, 211, 238, 62));
        p.drawRoundedRect(r, 10, 10);
    }
    QToolButton::paintEvent(event);
}

// 列表档绘制：图标在左、文字在右，整体左对齐 —— 对齐 Windows 资源管理器“列表”视图的观感。
// 为什么整块自绘：QToolButton 的内容布局只有“图标在上、文字在下”和“图标在左、文字在右（整体居中）”
// 两种，且 QSS 不支持内容对齐，做不出“图标贴左 + 文字紧随其后 + 行宽铺满”的列表形态。
// 配色与 Theme::iconButtonStyle() 保持一致（透明底 / 青色悬停 / 白色文字），选中态沿用本控件
// 统一的青色高亮，只把圆角由 10 收成 6 以贴合细长行。
// 作者：谭征
void DesktopIconButton::paintListMode() {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRect r = rect().adjusted(1, 1, -1, -1);

    // —— 背景：选中 > 按下 > 悬停 > 透明 ——
    const bool pressed = isDown();
    if (m_selected) {
        p.setPen(QPen(QColor(34, 211, 238, 190), 1));
        p.setBrush(QColor(34, 211, 238, 62));
        p.drawRoundedRect(r, 6, 6);
    } else if (pressed || underMouse()) {
        p.setPen(pressed ? QPen(Qt::NoPen) : QPen(QColor(34, 211, 238, 51), 1));
        p.setBrush(QColor(34, 211, 238, pressed ? 51 : 31));
        p.drawRoundedRect(r, 6, 6);
    }

    // —— 图标：左侧固定内边距 + 垂直居中，按当前档位像素绘制（列表档 = 小图标尺寸） ——
    const int hPad = 6;
    const int side = qBound(8, m_iconPx, qMax(8, height()));
    const QRect iconRect(hPad, (height() - side) / 2, side, side);
    const QIcon ic = icon();
    if (!ic.isNull()) {
        // 注意第 4 参是 Mode、第 5 参是 State，别把 QIcon::Disabled(Mode) 和 QIcon::Off(State) 混在一个
        // 三元表达式里 —— 两者枚举类型不同，三元会退化成 int 而匹配不到重载（编译期即报 C2665）。
        ic.paint(&p, iconRect, Qt::AlignCenter,
                 isEnabled() ? QIcon::Normal : QIcon::Disabled, QIcon::Off);
    }

    // —— 文字：紧接图标右侧，左对齐 + 垂直居中；超宽用“中间省略”保住扩展名 ——
    const int textLeft = iconRect.right() + 8;
    const int textW = width() - textLeft - hPad;
    if (textW > 4) {
        const QRect textRect(textLeft, 0, textW, height());
        const QFontMetrics fm(font());
        const QString shown = fm.elidedText(m_item.displayName, Qt::ElideMiddle, textW);
        p.setPen(QColor(255, 255, 255));
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, shown);
    }
}

// 右键菜单

// 作者：谭征
void DesktopIconButton::openItem() {
    if (m_item.isSpecial) {
        if (!m_item.launchCommand.isEmpty()) QProcess::startDetached(m_item.launchCommand);
        return;
    }
    const QString path = m_item.launchPath();
    if (!path.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

// 右键菜单：**与 Windows 桌面完全同一套系统原生菜单**（IShellFolder + IContextMenu +
// TrackPopupMenu，全部在 ShellOps::showNativeContextMenu 里，Dock 侧也走同一条通道）。
// 早前这里用的是自建 QMenu（打开 / 重命名 / 删除），问题有三：
// ① 观感与用户熟悉的桌面右键菜单明显不同（无系统图标、无分组、无子菜单箭头）；
// ② 功能残缺：没有「打开方式 / 发送到 / 复制 / 剪切 / 属性」，也没有系统与第三方 Shell
// 扩展（压缩、Git、杀软扫描…）；
// ③ 同一个程序里 Dock 用原生菜单、收纳盒用自建菜单，两边不一致。
// 现在统一为原生菜单，只把两个必须自己接管的动词拦下来：
// · 「重命名」→ 自己做内联编辑（Shell 的 rename 动词在本控件上没有行内编辑接收方，会哑火）；
// · 「删除」  → 走 ShellOps::deleteToRecycleBin，以便同步本地持久化数据（分类记录/顺序）。
// 作者：谭征
void DesktopIconButton::showContextMenu(const QPoint& globalPos) {
    Q_UNUSED(globalPos)
    // 菜单归属窗口用**本控件自己的 HWND**：这是 Win32 里给某个控件弹上下文菜单的惯常写法
    // （菜单消息路由给该控件），也是 Dock 侧原生菜单一直在用、已验证可用的写法。
    // 不用顶层窗口句柄：本窗口带 WS_EX_NOACTIVATE，顶层句柄会牵扯到 z 序守卫与前台激活，
    // 属于没必要引入的未知量。
    HWND owner = reinterpret_cast<HWND>(winId());

    const QString shellPath = !m_item.sourcePath.isEmpty() ? m_item.sourcePath : m_item.shellPath;

    // TrackPopupMenu 的坐标是**物理像素**，而 Qt 的 globalPos 是逻辑坐标（高 DPI 下两者不等）。
    // 菜单本来就该弹在光标处，直接取 Win32 的物理光标坐标，省掉一次易错的 DPI 换算。
    POINT pt = {0, 0};
    GetCursorPos(&pt);

    // 无框窗口右键不激活顶层窗口，进程不在前台时菜单弹出的第一次点击会被前台锁吃掉。
    grantForegroundForMenu();
    // 公告“菜单已弹出”：网格窗口据此冻结重建/重排。TrackPopupMenu 是原生模态循环，
    // 期间的 deleteLater 会被它自己的事件循环执行，正在弹菜单的按钮会被当场销毁。
    emit contextMenuVisibleChanged(true);
    const ShellOps::MenuAction act = ShellOps::showNativeContextMenu(
        owner, shellPath, m_item.displayName, pt.x, pt.y, /*interceptDelete=*/true);
    emit contextMenuVisibleChanged(false);
    DiagTrace::log(QStringLiteral("[btn] menu act=%1 (0=None 1=Rename 2=Delete 3=Invoked) shellPath='%2'")
                       .arg(static_cast<int>(act)).arg(shellPath));

    if (act == ShellOps::MenuAction::Rename) {
        startInlineRename();
    } else if (act == ShellOps::MenuAction::Delete) {
        requestDelete();
    } else if (act == ShellOps::MenuAction::Invoked) {
        // Shell 已执行完某个动词（打开 / 属性 / 剪切 / 发送到 / 压缩 / 第三方扩展…）。
        // 其中一部分会改动桌面内容（压缩新建压缩包、发送到新建快捷方式、第三方“彻底删除”等），
        // 不重新扫描就会留下“磁盘上已经没了，网格里还在”的幽灵图标 —— 比多刷一次更糟。
        emit fileMovedOut();
    }
}

// request删除
// 作者：谭征
void DesktopIconButton::requestDelete() {
    // 再延后一拍：菜单 exec 刚落栈，重建网格会把本按钮 deleteLater；直接 emit 也可行，
    // 但多隔一层事件循环最稳（调用方只需保证不在此栈上访问本对象）。
    QPointer<DesktopIconButton> self(this);
    QTimer::singleShot(0, this, [self]() {
        if (self) emit self->deleteRequested(self.data());
    });
}

// 内联重命名

// 作者：谭征
bool DesktopIconButton::canRename() const {
    return !m_item.isSpecial && ShellOps::isSafeToRename(m_item.sourcePath);
}

// 判断删除
// 作者：谭征
bool DesktopIconButton::canDelete() const {
    return !m_item.isSpecial && ShellOps::isSafeToDelete(m_item.sourcePath);
}

// 仅对真实文件系统图标生效；特殊命名空间项（我的电脑/网络/回收站等）不启用。
// 作者：谭征
void DesktopIconButton::startInlineRename() {
    DiagTrace::log(QStringLiteral("[btn] startInlineRename IN  canRename=%1 isSpecial=%2 src='%3' name='%4' editing=%5 hasEditor=%6")
                       .arg(DiagTrace::boolStr(canRename()), DiagTrace::boolStr(m_item.isSpecial),
                            m_item.sourcePath, m_item.displayName,
                            DiagTrace::boolStr(m_editing), DiagTrace::boolStr(m_editor != nullptr)));
    if (!canRename()) {
        DiagTrace::log(QStringLiteral("[btn] startInlineRename ABORT: canRename()==false"));
        return;
    }
    // 编辑框里预填的名字必须取 **磁盘上的真实文件名**，而不是 m_item.displayName：
    // displayName 只是绘制用的副本，任何一条“只换了路径没换显示名”的同步路径都会让它变陈旧。
    // 预填旧名的后果有两个：用户以为改名没生效；直接回车会被判成“与原名相同＝无操作”而毫无反应。
    QString currentName = m_item.displayName;
    if (!m_item.sourcePath.isEmpty()) {
        const QString onDisk = QFileInfo(m_item.sourcePath).fileName();
        if (!onDisk.isEmpty()) currentName = onDisk;
    }
    if (m_editing) {
        // 已存在编辑框（重复按 F2 / 再次点“重命名”）：重新置前并抢一次焦点即可。
        if (m_editor) {
            m_editor->show();
            m_editor->setText(currentName);
            m_editor->selectAll();
            m_editorShownTick.restart();
            DesktopMirrorWindow::raiseAboveBandWindows((HWND)m_editor->winId());
            forceForegroundWindow((HWND)m_editor->winId(), &m_attachedTid);
            if (!s_editHooksActive) installEditHooks(this);   // 钩子可能已被系统摘除/已卸载
            forceFocusStaggered();
            return;
        }
        // 只置了标志、编辑框却已不在（当前会话残影）：旧实现在这里是**静默 return**，
        // 结果 F2 与右键菜单的「重命名」会双双永久失效 —— 用户观感就是
        // “重命名点了没反应，而且再也恢复不了”。这里复位状态、继续走正常流程。
        m_editing = false;
        m_renameCancelled = false;
        m_imeComposing = false;
    }

    m_editing = true;
    m_editorFocused = false;   // 每次重新进入编辑态都要重新确认焦点（中途可能已被前台锁收回）
    m_renameCancelled = false;
    m_imeComposing = false;
    if (!m_editor) {
        // 关键：必须是“无父的顶层窗口”，不能做本窗口的子控件。
        // 收纳盒/整理窗口带 WS_EX_NOACTIVATE（Win+D 免疫所必需），其整条子控件焦点链都拿不到
        // 键盘焦点，子控件 QLineEdit 即便 setFocus() 也打不进字；独立顶层窗口不受此约束。
        m_editor = new QLineEdit(nullptr);
        // 供 DesktopImmunityFilter 识别并豁免：该过滤器会给“非挂件顶层窗口”加
        // WS_EX_NOACTIVATE，而这个样式会让编辑框永远无法成为前台窗口（打不进字）。
        m_editor->setObjectName(QStringLiteral("InlineRenameEditor"));
        m_editor->setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        m_editor->setFrame(false);
        m_editor->installEventFilter(this);
        connect(m_editor, &QLineEdit::returnPressed, this, &DesktopIconButton::commitInlineRename);
        // 失焦提交：只有发生在“宽限期”之后的失焦才算用户点到别处。
        // 编辑框 show() 的瞬间 Windows 会派发一轮激活/失焦抖动，若当成“点别处”立刻提交，
        // 框会一闪而过；宽限期内改为重新抢回焦点。
        connect(m_editor, &QLineEdit::editingFinished, this, [this]() {
            if (!m_editing) return;
            DiagTrace::log(QStringLiteral("[btn] editingFinished elapsed=%1ms cancelled=%2 focused=%3")
                               .arg(m_editorShownTick.isValid() ? m_editorShownTick.elapsed() : -1)
                               .arg(DiagTrace::boolStr(m_renameCancelled),
                                    DiagTrace::boolStr(m_editorFocused)));
            if (m_renameCancelled) { cancelInlineRename(); return; }
            // 这里【绝不提交】—— 这是“F2 / 右键重命名 无反应”的第 2 个自杀点（2026-09-13 定论）。
            // editingFinished 是**被动**信号：窗口重排、前台锁收回、系统激活抖动、DPI/显示器变化
            // 都会派发它，用户并没有“点到别处”。旧实现在宽限期之后直接 commitInlineRename()：
            // · 用户还没开始打字 → 新名 == 磁盘旧名 → commit 内部按“无操作”return，**一行日志都没有**；
            // · 框被收掉 → 下一次按 F2 时按钮处于 editing=false/hasEditor=false（正是日志里那个
            // “每次 F2 都是 editing=N hasEditor=N”的来源）→ 用户观感永远是“点了没反应”。
            // 提交必须且只能由三个明确的用户意图驱动（这也是本项目已定的架构，见文件头）：
            // · Enter   → QLineEdit::returnPressed / WH_KEYBOARD_LL
            // · Esc     → WH_KEYBOARD_LL → cancelInlineRename()
            // · 点框外   → WH_MOUSE_LL → handleEditClickOutside()
            // 所以这里只做一件事：把焦点抢回来，让框继续留在屏幕上等用户输入。
            if (m_editor) {
                forceForegroundWindow((HWND)m_editor->winId(), &m_attachedTid);
                m_editor->activateWindow();
                m_editor->setFocus();
            }
            return;
        });
    }

    // 编辑框几何精确覆盖标签绘制区，两档对齐各自的文字位置（否则列表档下输入框会跑到行的最左侧，
    // 与它正在编辑的那行文字完全错开）：
    // · 网格档：72×84 的按钮，图标 48 在上、标签在下方 28px → 底部整条；
    // · 列表档：文字在图标右侧、垂直居中 → 与 paintListMode 的 textRect 同位置。
    int bandX = 3, bandY = qMax(0, height() - 32), bandW = qMax(20, width() - 6);
    const int bandH = 28;
    if (m_listMode) {
        bandX = 6 + m_iconPx + 8;
        bandY = qMax(0, (height() - bandH) / 2);
        bandW = qMax(20, width() - bandX - 6);
    }
    const QRect band(bandX, bandY, bandW, bandH);
    const QPoint bandGlobal = mapToGlobal(band.topLeft());
    m_editor->setGeometry(bandGlobal.x(), bandGlobal.y(), band.width(), band.height());
    m_editor->setFont(iconTitleFont());
    // 白底黑字 + 蓝边、直角框，选中蓝底白字：贴近资源管理器行内编辑的观感。
    m_editor->setStyleSheet(QStringLiteral(
        "QLineEdit{ background:white; color:black;"
        " border:1px solid rgb(0,120,215);"
        " border-radius:0;"
        " selection-background-color:rgb(51,153,255); selection-color:white;"
        " padding:0 1px; }"));
    m_editor->setText(currentName);
    m_editor->selectAll();               // 全选（含扩展名），与桌面 F2 行为一致
    m_editorShownTick.start();
    m_editor->show();
    // ① 清掉 Qt 可能替我们指定的 owner：被 owner 化的窗口会跟随 owner 一起被
    // SetWindowPos(owner, HWND_BOTTOM) 拖到最底层（Dock 每秒都被压底），
    // 编辑框会因此沉到桌面图标层之下 —— 完全不可见，而 Qt 仍报 isVisible()==true。
    EditorWatch::clearOwner((HWND)m_editor->winId());
    // ② 直接用 HWND_TOPMOST 置顶，而不是“相对插到 band 窗口之上”：
    // 本项目有一堆周期性 Z 序改写（parkAllTargetsAtBottom 1s / band 守卫 / topmost 状态机），
    // 只有 topmost 层能稳定不被它们波及。编辑框是短暂输入窗口，置顶是正常语义。
    EditorWatch::forceOnTop((HWND)m_editor->winId());
    DesktopMirrorWindow::raiseAboveBandWindows((HWND)m_editor->winId());
    forceForegroundWindow((HWND)m_editor->winId(), &m_attachedTid);
    m_editor->activateWindow();
    m_editor->setFocus();
    forceFocusStaggered();
    // 挂上编辑会话的系统级输入钩子：回车/Esc/点框外提交不再依赖编辑框是否真拿到了键盘焦点。
    installEditHooks(this);
    // ③ 拉起编辑期看护定时器：每 150ms 复查“真的在屏幕上最上层 + 真的是前台窗口”，
    // 任一条不成立立刻再断言（并写日志）。见 editorwatch.h 说明。
    if (!m_editWatchTimer) {
        m_editWatchTimer = new QTimer(this);
        m_editWatchTimer->setInterval(150);
        connect(m_editWatchTimer, &QTimer::timeout, this, &DesktopIconButton::editorWatchTick);
    }
    m_editWatchCount = 0;
    m_editWatchTimer->start();
    // 公告“进入编辑态”：窗口据此冻结重建，避免刚弹出的输入框被一次延后到达的重建带走。
    emit editingChanged(true);
    // 同时记录编辑框的 EX 样式、前台归属与“谁盖在它上面/它的 owner 是谁”，作为根因的直接证据：
    // noact=Y   → 编辑框被应用级过滤器加了 WS_EX_NOACTIVATE（永远无法成为前台窗口，打不进字）；
    // fgSelf=N  → 前台仍是别的进程（前台锁会让 SetForegroundWindow 失效）；
    // onTop=N   → 编辑框被压在别的窗口之下（Qt 的 isVisible() 完全看不出来）；
    // owner!=0  → 它被 owner 化了，会随 owner 被压底而沉到桌面图标层之下。
    const HWND eh = (HWND)m_editor->winId();
    const EditorWatch::Status st = EditorWatch::probe(eh);
    DiagTrace::log(QStringLiteral("[btn] startInlineRename OK   visible=%1 geom=%2,%3 %4x%5 active=%6 focus=%7 noact=%8 fgSelf=%9 onTop=%10 owner=%11 above=%12")
                       .arg(DiagTrace::boolStr(m_editor->isVisible()))
                       .arg(m_editor->x()).arg(m_editor->y())
                       .arg(m_editor->width()).arg(m_editor->height())
                       .arg(DiagTrace::boolStr(m_editor->isActiveWindow()), DiagTrace::boolStr(m_editor->hasFocus()))
                       .arg(DiagTrace::boolStr(st.noActivate), DiagTrace::boolStr(st.isForeground))
                       .arg(DiagTrace::boolStr(st.onTopAtCenter))
                       .arg(reinterpret_cast<quintptr>(st.owner))
                       .arg(st.aboveDesc.isEmpty() ? QStringLiteral("-") : st.aboveDesc));
}

// 编辑期看护：把「编辑框是否真的可见 / 真的持有键盘焦点」变成可观测事实，并自愈。
// Qt 层面（isVisible()/hasFocus()）在这两个问题上都会说谎：
// · isVisible() 只看 WS_VISIBLE，窗口被压在别人之下时照样为 true；
// · hasFocus() 只看 Qt 内部的焦点记账，Windows 真前台不是它的时候照样为 true。
// 作者：谭征
void DesktopIconButton::editorWatchTick() {
    if (!m_editing || !m_editor) return;
    ++m_editWatchCount;
    const HWND eh = (HWND)m_editor->winId();
    EditorWatch::Status st = EditorWatch::probe(eh);

    bool repaired = false;
    // ① 被压在别的窗口之下 → 重新置顶（这是“看不见框”的直接治因）。
    if (!st.onTopAtCenter) {
        EditorWatch::forceOnTop(eh);
        repaired = true;
        DiagTrace::log(QStringLiteral("[btn] WATCH !onTop tick=%1 above=%2 -> forceOnTop")
                           .arg(m_editWatchCount).arg(st.aboveDesc));
    }
    // ② 不是真前台（假焦点：打字进不去）→ 再抢一次。
    // 只在编辑开始后的 3 秒内补抢，避免用户在别处正常操作时被长期抢焦点。
    if (!st.isForeground && m_editWatchCount <= 20) {
        forceForegroundWindow(eh, &m_attachedTid);
        m_editor->activateWindow();
        m_editor->setFocus();
        repaired = true;
        DiagTrace::log(QStringLiteral("[btn] WATCH !fg tick=%1 -> refocus").arg(m_editWatchCount));
    }
    // ③ 被错误 owner 化 → 清掉（否则会被 owner 的压底操作拖着一起沉下去）。
    if (st.owner) {
        EditorWatch::clearOwner(eh);
        repaired = true;
        DiagTrace::log(QStringLiteral("[btn] WATCH owner=%1 cleared").arg(reinterpret_cast<quintptr>(st.owner)));
    }
    if (m_editWatchCount <= 6 || repaired) {
        DiagTrace::log(QStringLiteral("[btn] WATCH tick=%1 onTop=%2 fg=%3 noact=%4 owner=%5 qtFocus=%6")
                           .arg(m_editWatchCount)
                           .arg(DiagTrace::boolStr(st.onTopAtCenter), DiagTrace::boolStr(st.isForeground),
                                DiagTrace::boolStr(st.noActivate))
                           .arg(reinterpret_cast<quintptr>(st.owner))
                           .arg(DiagTrace::boolStr(m_editor->hasFocus())));
    }
}

// 错峰补抢编辑框的键盘焦点。
// 为什么必须补抢：本窗口带 WS_EX_NOACTIVATE，本进程通常不在前台，首次 SetForegroundWindow
// 会被 Windows 的前台锁延后甚至丢弃 —— 用户看到的是“框已经出来了，但打不进字、回车也没用”，
// 而右键菜单「重命名」与 F2 走的是同一条路径，于是两边一起失效。
// 多次补抢，一旦确认真正拿到焦点（编辑框 FocusIn 已到）立刻停止，不与用户输入抢焦点。
// 作者：谭征
void DesktopIconButton::forceFocusStaggered() {
    if (!m_editor) return;
    for (int delayMs : {0, 40, 120, 250, 500, 900}) {
        QTimer::singleShot(delayMs, this, [this]() {
            if (m_editing && m_editor && !m_editorFocused) {
                forceForegroundWindow((HWND)m_editor->winId(), &m_attachedTid);
                m_editor->activateWindow();
                m_editor->setFocus();
            }
        });
    }
}

// 编辑框刚弹出的宽限期：编辑期鼠标钩子据此跳过“打开菜单的那一次手势”
// （右键菜单选「重命名」时，同一次手势的按下消息可能晚于编辑框显示才到达）。
// 作者：谭征
bool DesktopIconButton::editorJustOpened() const {
    return m_editorShownTick.isValid() && m_editorShownTick.elapsed() < 250;
}

// 点编辑框以外即提交。判定用 **Win32 物理坐标 vs GetWindowRect 物理矩形**：
// 钩子给的是物理像素，而 Qt 的 frameGeometry 是逻辑坐标（高 DPI 下两者不等），
// 混用会在缩放屏上出现“框内点击也被判成框外”。也不能用对象父子链判断 ——
// 编辑框是无父顶层窗口，Qt 把鼠标事件先派发给顶层 QWidgetWindow。
// 作者：谭征
void DesktopIconButton::handleEditClickOutside(const QPoint& physScreenPt) {
    if (!m_editing || !m_editor) return;
    if (editorJustOpened()) return;   // 刚弹出：忽略同一次手势带来的按下
#ifdef Q_OS_WIN
    RECT rc = {};
    if (GetWindowRect(reinterpret_cast<HWND>(m_editor->winId()), &rc)) {
        if (physScreenPt.x() >= rc.left && physScreenPt.x() < rc.right
            && physScreenPt.y() >= rc.top && physScreenPt.y() < rc.bottom) {
            return;   // 点在编辑框内：交给编辑框自己（定位光标 / 选中）
        }
    }
#else
    Q_UNUSED(physScreenPt)
#endif
    commitInlineRename();   // 点框外 = 提交（与 Windows 桌面一致）
}

// cleanupeditor
// 作者：谭征
void DesktopIconButton::cleanupEditor() {
    const bool wasEditing = m_editing;
    DiagTrace::log(QStringLiteral("[btn] cleanupEditor wasEditing=%1 hasEditor=%2 name='%3'")
                       .arg(DiagTrace::boolStr(wasEditing), DiagTrace::boolStr(m_editor != nullptr))
                       .arg(m_item.displayName));
    m_editing = false;
    m_renameCancelled = false;
    m_imeComposing = false;
    m_editorFocused = false;
    if (m_editWatchTimer) m_editWatchTimer->stop();   // 编辑结束：看护定时器一并停掉
    uninstallEditHooks(this);      // 先卸钩子：此后任何输入都不再与本编辑态相关
    detachForegroundThread(&m_attachedTid);
    if (m_editor) {
        // 先 hide() 再 deleteLater()：deleteLater 要等事件循环回到创建层级才执行，
        // 若被模态循环拖延，屏幕上会残留一个“已认为编辑结束、却仍可见且不响应”的僵尸框。
        m_editor->hide();
        m_editor->removeEventFilter(this);
        m_editor->deleteLater();
        m_editor = nullptr;
    }
    if (wasEditing) {
        update();
        // 公告“编辑结束”：窗口若因编辑期而冻结了重建（m_pendingRebuild），此时补做。
        emit editingChanged(false);
    }
}

// 取消inline重命名
// 作者：谭征
void DesktopIconButton::cancelInlineRename() {
    if (!m_editing) return;
    DiagTrace::log(QStringLiteral("[btn] cancelInlineRename"));
    m_renameCancelled = true;
    cleanupEditor();
}

// 内联重命名：提交（Enter/失焦）/取消（Esc）。cleanupEditor 负责销毁编辑框并恢复标签绘制。
// 作者：谭征
void DesktopIconButton::commitInlineRename() {
    if (!m_editing) {
        DiagTrace::log(QStringLiteral("[btn] commitInlineRename SKIP: !m_editing"));
        return;
    }
    const QString oldPath = m_item.sourcePath;
    // “原名”以**磁盘上的真实文件名**为准：displayName 是绘制副本，可能已经陈旧。
    // 用陈旧的 displayName 比较，会把“输入框里本来就是磁盘当前名”的情况误判成一次真实改名，
    // 于是走一遍 SetNameOf 同名调用（Shell 直接返回成功但什么都没变），观感就是“重命名没反应”。
    QString oldName = QFileInfo(oldPath).fileName();
    if (oldName.isEmpty()) oldName = m_item.displayName;
    const QString newName = m_editor ? m_editor->text().trimmed() : QString();
    // ① 先收框：无论改名成败，输入框必须立刻消失（否则失败时框会一直挂着）。
    cleanupEditor();
    DiagTrace::log(QStringLiteral("[btn] commit old='%1' new='%2' %3")
                       .arg(oldName, newName,
                            (newName.isEmpty() || newName == oldName) ? QStringLiteral("-> NO-OP")
                                                                      : QStringLiteral("-> rename")));
    // ② 空名 / 与原名逐字相同：视作无操作（Windows 语义：不改名直接提交＝保持原名，文件不丢）。
    if (newName.isEmpty() || newName == oldName) return;

    // ③ 落盘：走 ShellOps（与 Dock 同一条通道 —— IShellFolder::SetNameOf）。
    QString err;
    if (!ShellOps::renameInPlace(oldPath, newName, (void*)winId(), &err)) {
        QPointer<DesktopIconButton> self(this);
        const QString msg = err.isEmpty()
            ? QStringLiteral("重命名失败，文件可能已被移动或删除。") : err;
        QTimer::singleShot(0, this, [self, msg]() {
            if (self) GlassMessageBox::warning(self, QStringLiteral("重命名失败"), msg);
        });
        return;
    }
    // ④ 同步自身状态：显示名与全部路径一起换新，并重算“是否快捷方式/目标”
    // （改后缀会改变类型；不同步会让后续图标、菜单、二次重命名按旧类型走）。
    const QFileInfo fi(oldPath);
    const QString newPath = fi.isAbsolute() ? QDir(fi.path()).filePath(newName) : newName;
    m_item.displayName = newName;
    m_item.sourcePath = newPath;
    m_item.shellPath = newPath;
    m_item.isShortcut = QFileInfo(newPath).suffix().compare(QLatin1String("lnk"), Qt::CaseInsensitive) == 0
                        && DesktopScanner::isRealShortcut(newPath);
    // targetPath 必须是**解析后的真实目标**：改名/改后缀后重解析。取图只认目标文件
    // （SHDefExtractIcon 对 .lnk 本体取不到图标），填 .lnk 会让大图标档只剩 48px 兜底图。
    m_item.targetPath = DesktopScanner::effectiveTarget(m_item.sourcePath, m_item.isShortcut);
    m_item.icon = QIcon();          // 后缀变了图标要重取（缓存键含路径，这里强制重来一次）
    setText(newName);
    setToolTip(newName + QStringLiteral("\n") + newPath);
    setIcon(Theme::icon(QStringLiteral("tool_files")));
    m_iconQueued = false;           // 允许重新入队（首次异步加载可能还没轮到本控件）
    queueIconLoad(this);
    update();
    emit renameCommitted(oldPath, newPath);
}

// 应用级“点别处即提交”守卫（装在 qApp 上）：见 cpp 中的实现说明。
// 作者：谭征
bool DesktopIconButton::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_editor) {
        if (event->type() == QEvent::FocusIn) {
            // 已真正拿到键盘焦点：错峰补抢立即停止，避免与用户输入抢焦点。
            m_editorFocused = true;
        } else if (event->type() == QEvent::FocusOut) {
            // 焦点被收回：看护定时器据此再抢一次（编辑期的假焦点是“打不进字”的直接原因）。
            m_editorFocused = false;
            DiagTrace::log(QStringLiteral("[btn] editor FocusOut elapsed=%1ms")
                               .arg(m_editorShownTick.isValid() ? m_editorShownTick.elapsed() : -1));
        } else if (event->type() == QEvent::InputMethod) {
            // 记录输入法合成状态：合成中按 Esc 是“取消候选词”，不能当成取消重命名。
            auto* ime = static_cast<QInputMethodEvent*>(event);
            m_imeComposing = !ime->preeditString().isEmpty();
        } else if (event->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(event);
            if (ke->key() == Qt::Key_Escape && !m_imeComposing) {
                cancelInlineRename();
                return true;
            }
        }
    }
    return QToolButton::eventFilter(watched, event);
}

// 鼠标

// 作者：谭征
void DesktopIconButton::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragStartPos = event->pos();
        // 「单击选中」移到**释放时**触发（见 mouseReleaseEvent）：按下只记手势与修饰键，
        // 不再立即上抛 selectionRequested。否则拖拽（drag）也是从按下开始，任何一次拖动
        // （哪怕没交换位置）都会因「按下即选中」让图标进入选中态，拖拽结束后残留 ——
        // 表现就是「拖一下图标就高亮、拖多个就多个都高亮、点空白还取消不掉」。
        m_pressMods = event->modifiers();
        m_pressValid = true;
        m_dragging = false;
    } else if (event->button() == Qt::RightButton) {
        // Windows 语义：右键未选中项会先把它选中（再弹菜单），右键已选中项保持整组选中。
        emit selectionRequestedForMenu(this);
        return;
    }
    QToolButton::mousePressEvent(event);
}

// 鼠标移动事件
// 作者：谭征
void DesktopIconButton::mouseMoveEvent(QMouseEvent* event) {
    if (m_item.isSpecial) { QToolButton::mouseMoveEvent(event); return; }  // 特殊项不可拖拽
    if (m_editing) { QToolButton::mouseMoveEvent(event); return; }         // 编辑中不启动拖拽
    if (!(event->buttons() & Qt::LeftButton)) return;
    if ((event->pos() - m_dragStartPos).manhattanLength() < QApplication::startDragDistance()) return;

    // 已确认进入拖拽：取消「按下待选中」并标记拖拽中，释放时不再补一次选中。
    // 这正是「拖动不进入选中态」的关键 —— drag & drop 是移动语义，不是选择语义。
    m_pressValid = false;
    m_dragging = true;
    // 复位「按下」态（2026-09-25）：QToolButton 在 mousePressEvent 里被基类置为 down 态；
    // 随后这里的 QDrag::exec() 会接管鼠标抓取，拖放结束后**松手事件不会送回本按钮**
    // → isDown() 永久为 true → QSS 的 `DesktopIconButton:pressed` 背景一直画着，
    // 视觉上和选中态几乎一样（theme.h: rgba(34,211,238,0.20)）。这就是“拖一个卡一个、
    // 点空白取消不掉、必须逐个点一下才消失”的真正根因——那不是选中，是卡死的按下态。
    setDown(false);

    auto* drag = new QDrag(this);
    auto* mimeData = new QMimeData;
    // 绝不 setUrls / setText —— 2026-09-14 实测的严重缺陷，永久封禁这条路径：
    // 一旦 MIME 里带上 CF_HDROP（文件 URL），这次拖拽就升级成一次「系统级文件拖放」，
    // Explorer 的桌面与任意文件夹窗口都会把它当成「移动文件」接住并**真正落盘**：
    // · 拖到 Dock（＝被镜像覆盖的桌面面）→ 源与目标同目录 → 弹出
    // 「源文件名和目标文件名相同 —— <文件名>」错误框；
    // · 拖到别的文件夹窗口 → 文件被**真的搬走**（用户明令禁止的行为）。
    // 并且那个错误框是**模态**的：用户点「取消」后 QDrag::exec() 才返回，
    // 此刻 QCursor::pos() 取到的是对话框按钮的位置
    // → 现象就是「图标没落在鼠标左键释放的位置」。
    // 只放本程序私有 MIME：外部程序一律不认（fencebox / icongridwindow 的
    // dragEnter / dragMove / drop 全部优先按 x-desktopitem-id 处理），
    // 于是拖拽全程不触碰文件系统，落点也只由本程序按屏幕坐标判定。
    mimeData->setData(QStringLiteral("application/x-desktopitem-id"), m_item.id.toString().toUtf8());
    mimeData->setData(QStringLiteral("application/x-source-category"),
                      property("category").toString().toUtf8());
    drag->setMimeData(mimeData);
    // 最后一道保险（让这条通道不可能被误改回去）：
    // "text/uri-list" 就是 Windows 侧的文件拖放格式（CF_HDROP）。exec() 之前把它彻底剥掉，
    // 即使将来有人在上面的 MIME 组装里误加 setUrls()，这次拖拽也绝不会被 Explorer
    // 当成「文件拖放」而真的搬走文件。当前代码本就不设置它，这行是恒真的 no-op 自检。
    mimeData->removeFormat(QStringLiteral("text/uri-list"));

    // 拖拽光标提示：抓取整个按钮（图标 + 文字）作为半透明拖影，跟随光标移动，
    // 让用户清楚看到正在拖动的是哪一项，而不是只有孤零零的小图标。
    QPixmap btnPix = grab();
    QPixmap dragPix(btnPix.size());
    dragPix.fill(Qt::transparent);
    {
        QPainter p(&dragPix);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setOpacity(0.65);
        p.drawPixmap(0, 0, btnPix);
    }
    drag->setPixmap(dragPix);
    drag->setHotSpot(QPoint(dragPix.width() / 2, dragPix.height() / 2));

    // 拖拽过程中光标的反馈：一律显示「抓握手」。
    // 不给 IgnoreAction 配「禁止」光标 —— 拖出盒子（落在 Dock 镜像面上松手）是有明确
    // 语义的（图标重新出现在 Dock 上），显示禁止符号会让用户误以为「拖不出来」。
    const QPixmap handCursor = QCursor(Qt::ClosedHandCursor).pixmap();
    drag->setDragCursor(handCursor, Qt::MoveAction);
    drag->setDragCursor(handCursor, Qt::IgnoreAction);

    // 把源按钮调暗，提示“此项已被拎起”。松手后网格会重排并重建按钮（deleteLater），
    // 因此这里用 QPointer 保护，避免 exec 返回后在已销毁的 this 上访问。
    auto* liftEffect = new QGraphicsOpacityEffect(this);
    liftEffect->setOpacity(0.4);
    setGraphicsEffect(liftEffect);

    setCursor(Qt::ClosedHandCursor);
    // 拖拽期冻结视图重建（与“菜单期 / 编辑期”同一套记账机制）。
    // 不冻的后果：FenceBox 及其中的按钮就是拖拽源，一旦被 hide / 重父化 / deleteLater，
    // Windows 的 DoDragDrop 会当场结束 —— exec() 提前返回，QCursor::pos() 只取到“中止那一刻”
    // 的位置（通常正好在盒子边缘），使用者看到的就是“鼠标一离开收纳盒图标就落在盒边”。
    if (auto* gw = qobject_cast<IconGridWindow*>(window())) gw->setIconDragInProgress(true);
    QPointer<DesktopIconButton> self = this;
    const Qt::DropAction dropAct = drag->exec(Qt::MoveAction | Qt::CopyAction, Qt::MoveAction);
    if (self) {
        // 双保险：exec() 返回后松手事件同样不会送达（被 OLE 拖放循环消费），
        // 这里再复位一次 down 态。上面 exec() 前那一次覆盖“拖拽被提前中止”的情形，
        // 这一次覆盖正常拖放完成的情形 —— 两处合起来保证 down 态绝不残留。
        self->setDown(false);
        self->setGraphicsEffect(nullptr);
        self->setCursor(Qt::ArrowCursor);
        // 源文件若已不在原路径，说明本次拖拽触发了“收纳/取消收纳”或 Explorer 直接搬走，
        // 通知网格窗口刷新（刷新会延后到事件循环空闲，避免拖动过程中重建控件）。
        if (!QFile::exists(m_item.sourcePath)) {
            emit fileMovedOut();
        } else {
            // 「收纳盒 → Dock」：只要文件仍在原处，就一律把**松手坐标**报上去，由上层按坐标判定归属。
            // 为什么不再要求 dropAct == Qt::IgnoreAction（2026-09-14 修正）：
            // Dock 是全屏铺在桌面上的镜像窗口，而原生桌面图标层是 SW_HIDE 的 —— 落在“桌面面”上的
            // 松手既可能没人接（IgnoreAction），也可能被 OLE 交给下层窗口吞成 Move/Copy。
            // 拿返回值当闸门就会出现“明明拖出来了却什么都没发生”（盒 → Dock 一直不生效的根因）。
            // 改判据后，归属完全由 MainWindow 的解析器按屏幕坐标决定，与 Dock → 盒 用的是同一套机制：
            // · 松手点在任一收纳盒窗口内 → 不处理（窗口内重排 / 跨盒移动自有既有链路）；
            // · 松手点在 Dock 镜像面上 → 取消分类，图标回到 Dock，且就落在松手点（见 placeDockIconAtCursor）；
            // · 其它地方（记事本等外部程序接住了这次放置）→ 什么都不做，行为与改动前完全一致。
            // 安全性：整条链路只改内存分类记录与图标坐标，不创建 / 删除 / 移动 / 改名任何文件。
            // 唯一落点判据：左键此刻是否真的已物理抬起。
            // exec() 返回 ≠ 用户松手 —— 拖拽被系统提前中止（源控件被隐藏/重建、z 序或前台
            // 变化）时它一样会返回，而那时 QCursor::pos() 只是“中止位置”，通常正好落在
            // 盒子边缘。照单全收就会变成“鼠标一离开收纳盒就把图标显示出来”，而不是
            // “左键释放时显示在释放位置”。
#ifdef Q_OS_WIN
            const bool leftButtonUp = !(GetAsyncKeyState(VK_LBUTTON) & 0x8000);
#else
            const bool leftButtonUp = true;
#endif
            const QPoint gp = QCursor::pos();
            if (!leftButtonUp) {
                DiagTrace::log(QStringLiteral("[btn] dragEnd ABORTED lbdown=Y act=%1 name='%2' gp=%3,%4")
                                   .arg(static_cast<int>(dropAct))
                                   .arg(m_item.displayName).arg(gp.x()).arg(gp.y()));
            } else {
                DiagTrace::log(QStringLiteral("[btn] dragEnd act=%1 exists=Y name='%2' gp=%3,%4")
                                   .arg(static_cast<int>(dropAct))
                                   .arg(m_item.displayName).arg(gp.x()).arg(gp.y()));
                emit dragDroppedOutside(m_item.sourcePath, m_item.displayName, gp);
            }
        }
        // 解冻：拖拽（含上面的落点判定）已彻底结束，被推迟的视图重建此刻补做。
        if (auto* gw = qobject_cast<IconGridWindow*>(self->window()))
            gw->setIconDragInProgress(false);
    }
}

// 鼠标双击click事件
// 作者：谭征
void DesktopIconButton::mouseDoubleClickEvent(QMouseEvent* event) {
    QToolButton::mouseDoubleClickEvent(event);
    openItem();
}

// 鼠标松开事件
// 作者：谭征
void DesktopIconButton::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        showContextMenu(event->globalPos());
        return;
    }
    // 「单击选中」落点：左键释放、且按下后从未进入拖拽，才算一次点选。
    // 修饰键用**按下时**记录的（m_pressMods），与 Windows 桌面「按下时判定 Ctrl/Shift」一致；
    // 释放时窗口可能已重建按钮，故用 QPointer 自保护后再上抛，避免在已析构对象上发信号。
    if (event->button() == Qt::LeftButton && m_pressValid && !m_dragging) {
        m_pressValid = false;
        emit selectionRequested(this, m_pressMods.testFlag(Qt::ControlModifier),
                                m_pressMods.testFlag(Qt::ShiftModifier));
    }
    m_pressValid = false;
    m_dragging = false;
    QToolButton::mouseReleaseEvent(event);
}
