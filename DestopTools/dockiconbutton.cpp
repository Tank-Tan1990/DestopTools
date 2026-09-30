/*
 * @file dockiconbutton.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "dockiconbutton.h"

#include <QPainter>
#include <QMouseEvent>
#include <QApplication>
#include <QTimer>
#include <QCursor>
#include <QPixmap>
#include <QFontMetrics>
#include <QFontDatabase>
#include <QRect>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QInputMethodEvent>
#include <QHash>
#include <QDateTime>
#include <QPointer>
#include <QtWin>
#include "glassmessagebox.h"
#include "crashtrace.h"
#include "shellops.h"              // 重命名/删除的唯一落盘通道（与收纳盒共用）
#include "diagtrace.h"             // 临时：重命名链路诊断日志
#include "desktopscanner.h"        // isRealShortcut：改后缀后重算“是否真实快捷方式”
#include "desktopmirrorwindow.h"   // raiseAboveBandWindows：内联编辑框脱离 Dock 焦点约束后仍需压在收纳盒/助手之上
#include "editorwatch.h"           // 编辑框“真可见 / 真焦点”探测与强制置顶（与收纳盒图标共用）

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <objbase.h>
#endif

#ifdef Q_OS_WIN
// 取 Windows 桌面图标标题字体（SPI_GETICONTITLELOGFONT），与桌面快捷方式标签字体一致。
// 返回的字号以“逻辑像素”设置（setPixelSize），因为 Windows 桌面标签同样以逻辑像素绘制，
// 在 HiDPI 下由系统统一缩放，故 Dock 标签字体/字号与桌面逐项一致。
static QFont iconTitleFont()
{
    LOGFONTW lf = {0};
    if (SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(lf), &lf, 0)) {
        QFont f(QString::fromWCharArray(lf.lfFaceName));
        int px = qAbs(lf.lfHeight);
        if (px <= 0) px = 12;            // 个别系统返回 0 时兜底为 12 逻辑像素
        f.setPixelSize(px);
        // 字族与字号跟随 Windows 桌面图标标题字体；但 Win32 字重(lf.lfWeight，400=正常/700=粗体)
        // 与 Qt 字重(0–99 刻度，Normal=50/Bold=75)不是同一刻度，旧的 qBound(1, lf.lfWeight, 99)
        // 会把正常的 400 错夹成 99(Black) 导致标签异常粗。故统一强制常规字重(Normal)，
        // 既与桌面标签“非粗体”观感一致，也满足用户“不要粗体”的要求。
        Q_UNUSED(lf.lfWeight);
        f.setWeight(QFont::Normal);
        f.setItalic(lf.lfItalic != 0);
        f.setStyleStrategy(QFont::PreferAntialias);
        return f;
    }
    // 终极兜底：与桌面默认等价的像素字号（12 逻辑像素），避免点阵字号在 HiDPI 下偏大。
    QFont f;
    f.setPixelSize(12);
    return f;
}

// —— 编辑期系统级输入钩子（仅编辑期间存在，收框即卸载） ——
// 为什么必须下沉到系统钩子：
// 1. Dock 主窗口带 WS_EX_NOACTIVATE（Win+D 免疫所必需），重命名编辑框虽为独立顶层窗口，
// 但本进程常不在前台，SetForegroundWindow 会被 Windows 前台锁延后/丢弃 → 编辑框拿不到
// 键盘焦点 → 回车 / Esc 没有接收者，出现“按一次没反应”。
// 2. 落在 Dock 空白处（或其它窗口/面板）的鼠标按下，未必会经 Qt 的事件路由回到本窗口 →
// 应用层守卫也收不到 → “点空白处提交不了”。
// 放到 WH_MOUSE_LL / WH_KEYBOARD_LL 后，判定只依赖“屏幕坐标”和“物理按键”，与窗口激活状态、
// Qt 事件路由、控件命中测试全部无关：
// · 鼠标：任何落在编辑框矩形之外的按下 → 立即结束命名（**不吞事件**，该点击照常派发给目标窗口，
// 所以右键点空白仍然会正常弹出桌面菜单）；
// · 键盘：回车 → 提交；Esc → 取消（吞掉按键，避免被编辑框再处理一次）。
static HHOOK s_kbHook = nullptr;
static HHOOK s_msHook = nullptr;
static DockIconButton* s_hookOwner = nullptr;   // 编辑期间非空；钩子回调据此定位当前编辑项
static bool s_hooksActive = false;              // 句柄非空 ≠ 钩子有效（系统会静默摘除超时的低级钩子）

// 钩子回调里【绝不做重活】：只投递一个 0ms 延时请求，立即返回。
// 原因：低级钩子回调超时会被 Windows 静默摘除（句柄仍非空，于是后续 install 以为“已装”而跳过，
// 结果整段编辑期没有任何钩子 → 回车/点击全都不再提交，表现即“重命名失效”）。
// 同时避免在回调里销毁编辑框/卸载钩子本身（UnhookWindowsHookEx 在自身回调中调用有死锁风险）。
static void postCommit(DockIconButton* owner) {
    if (!owner) return;
    QPointer<DockIconButton> p(owner);
    QTimer::singleShot(0, owner, [p]() { if (p && p->isEditing()) p->commitRename(); });
}

static void postCancel(DockIconButton* owner) {
    if (!owner) return;
    QPointer<DockIconButton> p(owner);
    QTimer::singleShot(0, owner, [p]() { if (p && p->isEditing()) p->cancelRename(); });
}

static void uninstallEditHooks(DockIconButton* owner);   // 前置声明：installEditHooks 换主时需先卸载

static LRESULT CALLBACK dockKeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_hookOwner && s_hookOwner->isEditing()
        && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        const KBDLLHOOKSTRUCT* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        // 输入法合成中（拼音未上屏）一律放行：此时回车是“上屏候选词”、Esc 是“取消合成”，
        // 若被我们吞掉，中文名就永远打不出来。而且合成中编辑框必然持有键盘焦点，
        // 放行后回车的“提交”语义仍会经编辑框自身的 returnPressed 正常生效。
        if (k && !s_hookOwner->imeComposing()) {
            if (k->vkCode == VK_RETURN) { postCommit(s_hookOwner); return 1; }
            if (k->vkCode == VK_ESCAPE) { postCancel(s_hookOwner); return 1; }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static LRESULT CALLBACK dockMouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && s_hookOwner && s_hookOwner->isEditing()
        && s_hookOwner->editorVisible()) {
        switch (wParam) {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN: {
            const MSLLHOOKSTRUCT* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            // 编辑框刚弹出的极短时间内（默认 250ms）不做提交：
            // · 右键图标弹出菜单后，选“重命名”的那一次点击的按下消息可能晚于编辑框显示才到达；
            // · 弹出菜单返回后 Qt 仍可能补投一次同手势的按下事件。
            // 这类“同一次手势的按下”若被当成“用户点了别处”，就会把刚弹出的框立刻关掉 →
            // 表现即“右键重命名失效/一闪而过”。超过该窗口后，点框外一律提交（Windows 语义）。
            if (!s_hookOwner->editorJustOpened()) {
                const QRect r = s_hookOwner->editorNativeScreenRect();
                if (ms && !r.contains(QPoint((int)ms->pt.x, (int)ms->pt.y)))
                    postCommit(s_hookOwner);   // 点在编辑框之外 = 结束命名
            }
            break;
        }
        default:
            break;
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// 低级钩子必须装在拥有消息循环的线程（即 GUI 线程）；本类的所有调用点都在 GUI 线程。
// 每次都重新安装：句柄非空不代表钩子有效（见上面 postCommit 的说明），只以 s_hooksActive 为准。
static void installEditHooks(DockIconButton* owner) {
    if (!owner) return;
    // 换了编辑主体：先彻底卸掉上一轮的钩子，避免残留钩子把输入提交到错误的对象上。
    if (s_hooksActive && s_hookOwner && s_hookOwner != owner) uninstallEditHooks(s_hookOwner);
    s_hookOwner = owner;
    if (s_hooksActive) return;
    HMODULE mod = GetModuleHandleW(nullptr);
    s_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, dockKeyboardHookProc, mod, 0);
    s_msHook = SetWindowsHookExW(WH_MOUSE_LL, dockMouseHookProc, mod, 0);
    s_hooksActive = (s_kbHook != nullptr || s_msHook != nullptr);
}

static void uninstallEditHooks(DockIconButton* owner) {
    if (owner && s_hookOwner && s_hookOwner != owner) return;   // 不是当前主体，勿误卸
    s_hookOwner = nullptr;
    s_hooksActive = false;
    if (s_kbHook) { UnhookWindowsHookEx(s_kbHook); s_kbHook = nullptr; }
    if (s_msHook) { UnhookWindowsHookEx(s_msHook); s_msHook = nullptr; }
}

// —— 编辑期的线程输入挂靠（保持整个编辑期，收框时解除） ——
// Dock 主窗口带 WS_EX_NOACTIVATE（Win+D 免疫所必需），本进程通常不是前台进程。此状态下对编辑框
// 调 SetFocus 会被系统忽略/随即收回 → 编辑框拿不到键盘输入：用户看到的框是“假的焦点”，
// 打字进不去，回车提交的仍是未改动的旧名（表现即“重命名失效”）。
// 把本线程输入队列挂靠到当前前台线程后，SetFocus 才真正生效、按键才会送到编辑框。
// 注意：必须在本线程仍是“未挂靠”状态时建立，并在收框时对称解除，避免与其它线程长期共享输入状态。
static void attachForegroundThread(quintptr* attachedTid) {
    if (!attachedTid || *attachedTid) return;
    HWND fg = ::GetForegroundWindow();
    if (!fg) return;
    DWORD fgPid = 0;
    const DWORD fgTid = GetWindowThreadProcessId(fg, &fgPid);
    const DWORD myTid = GetCurrentThreadId();
    if (!fgTid || fgTid == myTid || fgPid == GetCurrentProcessId()) return;
    if (AttachThreadInput(myTid, fgTid, TRUE)) *attachedTid = (quintptr)fgTid;
}

static void detachForegroundThread(quintptr* attachedTid) {
    if (!attachedTid || !*attachedTid) return;
    AttachThreadInput(GetCurrentThreadId(), (DWORD)(*attachedTid), FALSE);
    *attachedTid = 0;
}
#else
// 非 Windows 平台（本项目仅在 Windows 构建）：钩子/挂靠为空实现，保持调用点无分支。
static void installEditHooks(DockIconButton*) {}
static void uninstallEditHooks(DockIconButton*) {}
static void attachForegroundThread(quintptr*) {}
static void detachForegroundThread(quintptr*) {}
#endif

// 编辑框刚弹出后的“提交宽限期”（毫秒）：期内来自鼠标的“点框外即提交”一律忽略。
// 用于消掉“刚弹出就被同一次手势关掉”的闪框（右键菜单选“重命名”时那一次点击的按下消息，
// 可能晚于编辑框显示才到达；弹出菜单返回后 Qt 也可能补投同手势事件）。
static const qint64 kEditorCommitGateMs = 250;

DockIconButton::DockIconButton(const DesktopItem& item, QWidget* parent)
    : QWidget(parent), m_item(item) {
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    // 与收纳盒/网格视图保持一致：单元格 72x84，图标 48x48
    m_cellW = 72;
    m_iconSize = 48;
    setFixedSize(m_cellW, 84);
    // C1：回收站判定（命名空间 CLSID 优先，显示名兜底）在构造期算一次。
    // 该判定含 2 次 QString 分配，却在拖动中被逐像素反复调用（recycleBinButton() 会对每个
    // Dock 按钮调一遍 isRecycleBin()）。按钮的 item 在生命周期内不变，故一次即可。
    {
        const QString sp = m_item.shellPath.trimmed().toLower();
        m_isRecycleBinItem = (sp == QLatin1String("::{645ff040-5081-101b-9f08-00aa002f954e}"));
        if (!m_isRecycleBinItem) {
            const QString n = m_item.displayName.trimmed().toLower();
            m_isRecycleBinItem = (n == QStringLiteral("回收站") || n == QLatin1String("recycle bin"));
        }
    }
}

DockIconButton::~DockIconButton() {
    // 内联编辑框是无父的顶层窗口：按钮被销毁（如布局重建）时必须一并清掉，
    // 否则它会作为孤儿窗残留在屏幕上。此处不 emit inlineEditFinished，
    // 避免在销毁/重建过程中触发 Dock 的刷新重排（Qt 已通过 QPointer 自动置空 m_editingBtn）。
    // 但编辑标志必须显式复位：QPointer 只负责置空 m_editingBtn 指针，不负责把本对象
    // 留在旧快照/旧容器里的 m_editing 读成 false；若外部恰好还持有本对象并读 isEditing()，
    // 就会卡在"有图标正在编辑"上，连带把 F2 与右键重命名一起锁死。
    // 也不再写 ~DESTRUCT 诊断日志。trace.log 位于用户实际桌面根目录的子目录下
    // （项目部署形态：F:\Desktop\DestopTools\DestopTools\release\），之前 registerChangeNotify
    // 用了 fRecursive=TRUE，曾导致"每只按钮析构 → 写一行日志 → SHCNE_UPDATEITEM 回弹 →
    // refresh() → 重建 → 又一轮析构日志…"的自激循环（Dock 图标"点中即失选"症状的元凶）。
    // 现在 fRecursive=FALSE 已切断回弹，但即便如此，继续在析构里写文件触发的近因机会
    // （SHCN 偶尔也会被回收站、移动操作、上层 shell 扩展诱发），为根除这一类风险，
    // 这里**不再写任何日志**——按 F2 / 右键重命名的可用性比销毁日志更值得保留。
    m_editing = false;
    if (m_editWatchTimer) m_editWatchTimer->stop();   // 按钮被销毁：看护定时器一并停掉
    uninstallEditHooks(this);
    detachForegroundThread(&m_attachedTid);
    if (m_editor) {
        m_editor->hide();            // 先隐藏：deleteLater 要等事件循环，期间不能留“僵尸框”在屏幕上
        m_editor->removeEventFilter(this);
        m_editor->deleteLater();
        m_editor = nullptr;
    }
}

// 编辑框刚弹出（尚未超过提交宽限期）：此时到达的“点框外即提交”一律忽略，
// 避免把同一次手势（弹出菜单的那次点击 / 菜单返回后补投的事件）当成“用户点了别处”而立刻收框。
// 作者：谭征
bool DockIconButton::editorJustOpened() const {
    return m_editing && m_editorShownTick.isValid()
        && m_editorShownTick.elapsed() < kEditorCommitGateMs;
}

// 编辑框在屏幕上的矩形（Win32 物理像素）。低级鼠标钩子给的是物理坐标，故此处用 GetWindowRect 取。
// 作者：谭征
QRect DockIconButton::editorNativeScreenRect() const {
#ifdef Q_OS_WIN
    if (m_editor) {
        RECT rc;
        if (GetWindowRect((HWND)m_editor->winId(), &rc))
            return QRect(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
    }
#endif
    return QRect();
}

// 设置单元尺寸
// 作者：谭征
void DockIconButton::setCellSize(int cellW) {
    m_cellW = qMax(64, cellW);
    // 保持与收纳盒/网格视图一致的 72x84 比例
    setFixedSize(m_cellW, qRound(m_cellW * 84.0 / 72.0));
    update();
}

// 设置图标尺寸
// 作者：谭征
void DockIconButton::setIconSize(int px) {
    m_iconSize = qMax(16, px);
    update();
}

// 设置selected
// 作者：谭征
void DockIconButton::setSelected(bool sel) {
    if (m_selected != sel) {
        m_selected = sel;
        update();
    }
}

// —— 可视命中区：与 paintEvent 的绘制几何共用同一套计算，二者必须一致 ——
// 图标：顶部居中，边长 m_iconSize，上边距 qMax(4, 4% 单元格宽)。
// 作者：谭征
QRect DockIconButton::iconVisualRect() const {
    const int iconS = m_iconSize;
    const int iconX = (width() - iconS) / 2;
    const int iconY = qMax(4, qRound(m_cellW * 0.04));
    return QRect(iconX, iconY, iconS, iconS);
}

// 标签：图标下方留 4px 间隙，横向左右各留 4px（与 paintEvent 的 maxW = w - 8 一致）。
// 作者：谭征
QRect DockIconButton::labelVisualRect() const {
    const QRect ir = iconVisualRect();
    const int labelTop = ir.y() + ir.height() + 4;
    const int labelH = height() - labelTop - 4;
    if (labelH <= 10) return QRect();
    return QRect(4, labelTop, width() - 8, labelH);
}

// 命中判定用「图标 ∪ 标签」的并集矩形（含二者之间的 4px 间隙），与 hitsVisualRect() 完全同源。
// 为什么必须含那条 4px 间隙：图标底边（y = iconY+iconS）与标签顶边（labelTop = +4）之间有一条
// 4px 死区。若按「图标 OR 标签」分别判 contains，落在该死区里的按下会被误判为「点空白」——
// 于是用户在图标下沿/标签上沿附近点一下，就把刚建立的选中清掉，F2 / Delete 立即失效。
// 用并集矩形（hitsVisualRect）则这条死区并入命中区，点击命中与画框多选的口径也统一，
// 不再出现「看得见的地方点了没反应、还把选中清掉」。
// 作者：谭征
bool DockIconButton::hitsVisualArea(const QPoint& localPos) const {
    return hitsVisualRect().contains(localPos);
}

// 画框多选的相交判定用：图标与标签的并集矩形（局部坐标）。
// 与 hitsVisualArea() 共用同一套几何，保证“框到看得见的部分”才选中。
// 作者：谭征
QRect DockIconButton::hitsVisualRect() const {
    QRect r = iconVisualRect();
    const QRect lr = labelVisualRect();
    if (lr.isValid()) r = r.united(lr);
    return r;
}

// —— “拖到回收站即删除”相关 ——
// 回收站判定：优先按命名空间 CLSID（唯一、稳定），显示名只作兜底（本地化/将来改名时仍可识别）。
// C1：结果改为构造期算好的成员（见构造函数），语义与旧实现完全一致，
// 但省掉了拖动期间每次调用 2 次 QString 分配的开销。
// 作者：谭征
bool DockIconButton::isRecycleBin() const {
    return m_isRecycleBinItem;
}

// 说明：Dock 的“拖到回收站”只对真实文件系统项生效，与 F2/Delete 快捷键保持同一套判据。
// 作者：谭征
bool DockIconButton::isDeletableOnDrop() const {
    if (m_item.isSpecial) return false;                                 // 系统图标（此电脑/网络/回收站/控制面板）
    if (m_item.shellPath.isEmpty()) return false;
    if (m_item.shellPath.startsWith(QLatin1String("::{"))) return false; // 个人文件夹等的 CLSID 形式
    // 路径级判据统一收敛到 ShellOps::isSafeToDelete（受保护项、盘根、用户主目录及其上级、
    // 桌面目录本身）。Dock 与收纳盒共用同一份判据，杜绝两边口径分裂。
    return ShellOps::isSafeToDelete(m_item.shellPath);
}

// 拖动悬停在回收站上时的高亮（提示“松手即删除”）。
// 作者：谭征
void DockIconButton::setDropTarget(bool on) {
    if (m_dropTarget == on) return;
    m_dropTarget = on;
    update();
}

// paint事件
// 作者：谭征
void DockIconButton::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // 拖动时整图标半透明，贴近 Windows 桌面拖拽观感
    p.setOpacity(m_dragging ? 0.55 : 1.0);

    const int w = width();
    const int h = height();

    // 选中高亮：Windows 桌面风格半透明蓝圆角矩形（覆盖整个单元格）
    if (m_selected) {
        QColor sel(51, 153, 255, 140);
        p.setPen(Qt::NoPen);
        p.setBrush(sel);
        p.drawRoundedRect(2, 2, w - 4, h - 4, 6, 6);
    }
    // 鼠标悬停：Windows 桌面图标 hover 时的淡蓝半透明底（比选中更淡）
    else if (m_hovered) {
        QColor hover(51, 153, 255, 60);
        p.setPen(Qt::NoPen);
        p.setBrush(hover);
        p.drawRoundedRect(2, 2, w - 4, h - 4, 6, 6);
    }

    // 图标区域：顶部居中，绘制尺寸 = m_iconSize（与 Windows 桌面真实图标等大）。
    // 按设备像素比请求高清位图（源为系统镜像列表 256px JUMBO，缩放到 N*dpr 仍清晰），
    // 再绘制进 N 逻辑像素矩形，使 HiDPI 屏下与桌面图标清晰度一致、不发虚。
    const QRect iconR = iconVisualRect();   // 与 hitsVisualArea() 共用几何，保证“看得见的才算可点”
    const int iconS = iconR.width();
    const int iconX = iconR.x();
    const int iconY = iconR.y();

    // 快捷方式小箭头已随图标一起由 DesktopScanner::loadIcon 经
    // SHGetFileInfo(SHGFI_LINKOVERLAY) 直接取得（与 Windows 桌面角标完全一致），
    // 此处无需再自绘，避免重复叠加或箭头过小。

    // 标签几何：图标下方居中白字 + 细暗阴影，最多两行、超长省略
    // 内联重命名进行中（m_editing）时跳过标签绘制，由叠加的 QLineEdit 接管显示
    const int labelTop = iconY + iconS + 4;
    const int labelH = h - labelTop - 4;
#ifdef Q_OS_WIN
    const QFont f = iconTitleFont();
#else
    QFont f = font();
    f.setPixelSize(qBound(12, qRound(m_cellW * 0.15), 15));
#endif

    // —— D2：静态层缓存（图标 + 标签）——
    // 只在“输入”变化时重建：设备像素尺寸 / 显示名 / 选中态 / 编辑态 / 图标 / 字体。
    // hover、按下、拖动半透明、回收站提示环都不属于本层，仍由下面的即时绘制负责，
    // 因此本缓存不会影响任何交互态外观（拖动半透明由外层 QPainter 的 opacity 统一施加）。
    const qreal dpr = devicePixelRatioF();
    const int cw = qMax(1, qRound(w * dpr));
    const int chh = qMax(1, qRound(h * dpr));
    const QString fontKey = f.key();
    const qint64 iconKey = m_icon.cacheKey();
    const bool staticSame = m_staticKeyValid
        && m_staticKeyW == cw && m_staticKeyH == chh
        && m_staticKeyIcon == iconKey
        && m_staticKeyText == m_item.displayName
        && m_staticKeyFont == fontKey
        && m_staticKeySelected == m_selected
        && m_staticKeyEditing == m_editing;
    if (!staticSame) {
        m_staticKeyValid = true;
        m_staticKeyW = cw;
        m_staticKeyH = chh;
        m_staticKeyIcon = iconKey;
        m_staticKeyText = m_item.displayName;
        m_staticKeyFont = fontKey;
        m_staticKeySelected = m_selected;
        m_staticKeyEditing = m_editing;

        m_staticLayer = QPixmap(cw, chh);
        m_staticLayer.setDevicePixelRatio(dpr);
        m_staticLayer.fill(Qt::transparent);
        QPainter sp(&m_staticLayer);
        sp.setRenderHint(QPainter::Antialiasing, true);
        sp.setRenderHint(QPainter::TextAntialiasing, true);
        sp.setRenderHint(QPainter::SmoothPixmapTransform, true);
        sp.setFont(f);

        // 图标区域：按设备像素比请求高清位图（源为系统镜像列表 256px JUMBO，缩放到 N*dpr 仍清晰），
        // 再绘制进 N 逻辑像素矩形，使 HiDPI 屏下与桌面图标清晰度一致、不发虚。
        if (!m_icon.isNull()) {
            const int pmSize = qMax(1, qRound(iconS * dpr));
            const QPixmap pm = m_icon.pixmap(pmSize, pmSize);
            if (!pm.isNull()) sp.drawPixmap(iconX, iconY, iconS, iconS, pm);
        }

        if (!m_editing && labelH > 10) {
            const QFontMetrics fm(f);
            const int maxW = w - 8;
            const QString text = m_item.displayName;
            QString line1 = text, line2;
            if (fm.horizontalAdvance(text) > maxW) {
                // 简单二分截断：先取首行可容纳的前缀，剩余作第二行（第二行省略号）
                int take = text.length();
                while (take > 1 && fm.horizontalAdvance(text.left(take)) > maxW) --take;
                line1 = text.left(take);
                QString rest = text.mid(take);
                if (fm.horizontalAdvance(rest) > maxW)
                    rest = fm.elidedText(rest, Qt::ElideRight, maxW);
                line2 = rest;
            }

            auto drawLabel = [&](const QRect& r, const QString& t) {
                if (m_selected) {
                    // Windows 桌面选中：标签不透明蓝底 + 纯白字（无阴影）
                    sp.setPen(Qt::NoPen);
                    sp.setBrush(QColor(0, 120, 215));
                    sp.drawRoundedRect(r.adjusted(-2, -1, 2, 1), 3, 3);
                    sp.setPen(Qt::white);
                    sp.drawText(r, Qt::AlignHCenter | Qt::AlignVCenter, t);
                } else {
                    // 1px 细阴影，增强在复杂壁纸上的可读性，同时保持清晰
                    sp.setPen(QColor(0, 0, 0, 180));
                    sp.drawText(r.adjusted(1, 1, 1, 1), Qt::AlignHCenter | Qt::AlignVCenter, t);
                    sp.setPen(Qt::white);
                    sp.drawText(r, Qt::AlignHCenter | Qt::AlignVCenter, t);
                }
            };

            if (line2.isEmpty()) {
                drawLabel(QRect(4, labelTop, maxW, labelH), line1);
            } else {
                drawLabel(QRect(4, labelTop, maxW, labelH / 2), line1);
                drawLabel(QRect(4, labelTop + labelH / 2, maxW, labelH / 2), line2);
            }
        }
    }
    p.drawPixmap(0, 0, m_staticLayer);

    // 拖动悬停在回收站上：整格画一圈醒目提示环（在图标之后绘制，保证压在图标之上仍然清晰）。
    // 用偏红的高亮而非普通的选中蓝，明确提示“松手 = 删除”这个不可逆动作。
    if (m_dropTarget) {
        QPen pen(QColor(232, 72, 72, 235), 2);
        p.setPen(pen);
        p.setBrush(QColor(232, 72, 72, 45));
        p.drawRoundedRect(QRectF(1.5, 1.5, w - 3.0, h - 3.0), 7, 7);
    }
}

// 进入事件
// 作者：谭征
void DockIconButton::enterEvent(QEvent* /*event*/) {
    m_hovered = true;
    update();
}

// 离开事件
// 作者：谭征
void DockIconButton::leaveEvent(QEvent* /*event*/) {
    m_hovered = false;
    update();
}

// 标题栏本身不承载任何编辑交互，不会与输入/选择冲突。
// 作者：谭征
void DockIconButton::mousePressEvent(QMouseEvent* event) {
    // 落在单元格内、却不在“图标 / 标签”可视区上的按下 = 点在 Dock 空白处：
    // 单元格 72x84 远大于图标 48x48 + 标签，若照单全收，用户在图标旁的空白按下会被当成
    // “点了这个图标”而保持/转移到选中态 —— 表现即“点 Dock 空白清不掉选中”。
    // 这里 ignore 掉事件，Qt 会把它转交给父窗口（DesktopMirrorWindow），由其清空选中并开始框选。
    if (!hitsVisualArea(event->pos())) {
        event->ignore();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        m_pressedLeft = true;
        m_pressGlobal = event->globalPos();
        m_grabOffset = event->pos();
        m_dragging = false;
    }
    // 一律上报 Dock 窗口，由窗口统一决策：左键选中(含 Ctrl/Shift/多选)、右键确保选中并弹原生菜单。
    emit iconPressed(this, event->globalPos(), event->buttons(), event->modifiers());
    QWidget::mousePressEvent(event);
}

// 鼠标移动事件
// 作者：谭征
void DockIconButton::mouseMoveEvent(QMouseEvent* event) {
    // 丢失释放自愈：若左键已物理抬起但本按钮仍以为在按下/拖动（拖动被系统提前中止、或释放事件
    // 路由丢失），立即复位并对称通知窗口解除拖动态——否则窗口的 m_dragActive 会永久卡住。
    if (m_pressedLeft && !(event->buttons() & Qt::LeftButton)) {
        m_pressedLeft = false;
        if (m_dragging) {
            m_dragging = false;
            emit dragEnd(this, event->globalPos() - m_grabOffset);
        }
        emit iconReleased();
        QWidget::mouseMoveEvent(event);
        return;
    }
    if (m_pressedLeft && !m_dragging) {
        if ((event->globalPos() - m_pressGlobal).manhattanLength() > QApplication::startDragDistance())
            m_dragging = true;
    }
    if (m_dragging && parentWidget()) {
        // 跟手移动按钮（在本窗口内局部移动）
        QPoint newLocal = parentWidget()->mapFromGlobal(event->globalPos() - m_grabOffset);
        move(newLocal);
        // 上报窗口：让同组其它已选中图标同步位移（Windows 多选整体拖动）
        emit dragMove(this);
    }
    QWidget::mouseMoveEvent(event);
}

// 鼠标松开事件
// 作者：谭征
void DockIconButton::mouseReleaseEvent(QMouseEvent* event) {
    // 与 mousePressEvent 对称：这次手势不是从本按钮可视区开始的（按下已被放行给 Dock 空白处理）
    // → 释放同样放行，避免把父窗口的框选释放阶段截在自己的矩形里。
    if (!m_pressedLeft && !hitsVisualArea(event->pos())) {
        event->ignore();
        return;
    }
    if (m_pressedLeft && event->button() == Qt::LeftButton) {
        m_pressedLeft = false;
        if (m_dragging) {
            m_dragging = false;
            // 拖动结束：上报自身最终全局左上角，窗口据此把整组（若多选）位置回写真实桌面
            QPoint newGlobalTopLeft = event->globalPos() - m_grabOffset;
            emit dragEnd(this, newGlobalTopLeft);
        }
        // 无论真拖动还是普通单击，左键手势已结束：对称通知窗口复位拖动态。
        emit iconReleased();
    }
    QWidget::mouseReleaseEvent(event);
}

// 鼠标双击click事件
// 作者：谭征
void DockIconButton::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        m_pressedLeft = false;
        emit iconReleased();
        launch();
        emit activated();
    }
    QWidget::mouseDoubleClickEvent(event);
}

// 启动
// 作者：谭征
void DockIconButton::launch() {
#ifdef Q_OS_WIN
    const QString sp = m_item.shellPath;
    if (sp.startsWith(QLatin1String("::"))) {
        // 特殊命名空间项（我的电脑/回收站/网络）：用 explorer 打开对应 CLSID
        ShellExecuteW(nullptr, L"open", L"explorer.exe",
                      sp.toStdWString().c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(nullptr, L"open", sp.toStdWString().c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
#endif
}

// 应用级“点别处即提交”守卫（装在 qApp 上）：见 cpp 中的实现说明。
// 作者：谭征
bool DockIconButton::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_editor && event->type() == QEvent::FocusIn) {
        m_editorFocused = true;   // 已真正拿到键盘焦点：无需再补抢
    }
    // 焦点被收回：记下来，编辑期看护定时器会再抢一次（“假焦点”是打不进字的直接原因）。
    if (watched == m_editor && event->type() == QEvent::FocusOut) {
        m_editorFocused = false;
        DiagTrace::log(QStringLiteral("[dockbtn] editor FocusOut elapsed=%1ms")
                           .arg(m_editorShownTick.isValid() ? m_editorShownTick.elapsed() : -1));
    }
    // 跟踪输入法合成状态：系统键盘钩子据此决定是否放行回车/Esc（见 dockKeyboardHookProc）。
    if (watched == m_editor && event->type() == QEvent::InputMethod) {
        QInputMethodEvent* ime = static_cast<QInputMethodEvent*>(event);
        m_imeComposing = !ime->preeditString().isEmpty();
    }
    // Esc：立即结束内联重命名。本实现的改名只在“提交”时落盘，未提交即等于原名不变，
    // 因此 Esc 直接收框即为 Windows 语义（新建项保留默认名、已有项保留原名），且一次生效
    // （不依赖任何 clearFocus → editingFinished 的被动路径）。
    // 说明：正常情况下 Esc 已被系统键盘钩子拦截，这里是钩子安装失败时的兜底路径。
    if (watched == m_editor && event->type() == QEvent::KeyPress) {
        QKeyEvent* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            cancelInlineRename();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

// 说明：编辑框弹出后的“被动失焦”一律不提交（见 editingFinished 处理）。原先这里有一个
// kEditorFocusGraceMs 宽限期用来区分“抖动失焦”与“用户点别处”，现已由架构层面消除：
// 提交只走 Enter / Esc / 点框外三条明确的用户意图通道，被动失焦只补抢焦点。

// 强制把 h 置为前台窗口并收键盘焦点：越过 Windows 焦点窃取防护（非前台进程 SetForegroundWindow
// 会被忽略），保证内联重命名框一次就能直接输入，无需点第二次。
// 关键：SetForegroundWindow / SetFocus 都是异步消息，且必须在“线程输入队列仍挂靠前台线程”时生效。
// 旧实现在 AttachThreadInput 之后 60ms 就解绑，导致前台切换/焦点消息在解绑后被前台锁收回，
// 编辑框拿不到键盘焦点——表现为“左键先选中再右键一次成功，直接右键要两次”，
// 更严重的是“框弹出来了但打不进字，回车提交的是旧名”，看起来就像重命名失效。
// 现在把挂靠交给编辑期统一管理（attachForegroundThread / detachForegroundThread），此处不再解绑。
static void forceForegroundWindow(HWND h, quintptr* attachedTid) {
    if (!h) return;
    // 自愈兜底（2026-09-13）：同收纳盒侧 —— 应用级过滤器会给“非挂件顶层窗口”加上
    // WS_EX_NOACTIVATE，编辑框一旦带该样式就永远无法成为前台窗口（SetForegroundWindow /
    // SwitchToThisWindow 被系统拒绝），表现即“框出来了但打不进字、回车提交旧名”。
    // 过滤器侧已豁免输入窗口（治本），这里再摘一次作为兜底，任何时序下都能自愈。
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_NOACTIVATE) {
        SetWindowLongPtrW(h, GWL_EXSTYLE, ex & ~WS_EX_NOACTIVATE);
        SetWindowPos(h, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }
    attachForegroundThread(attachedTid);
    AllowSetForegroundWindow(ASFW_ANY);
    // SwitchToThisWindow：无视前台锁的强抢前台（专用于本进程自己弹出的编辑器）
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        typedef void (WINAPI* PSwitchToThisWindow)(HWND, BOOL);
        if (auto pSwitch = reinterpret_cast<PSwitchToThisWindow>(
                GetProcAddress(u32, "SwitchToThisWindow")))
            pSwitch(h, TRUE);
    }
    ::SetForegroundWindow(h);
    ::SetFocus(h);                 // 同步把键盘焦点设到编辑框
}

// 仅对真实文件系统图标生效；特殊命名空间项（我的电脑/网络/回收站等）不启用。
// 作者：谭征
void DockIconButton::startInlineRename() {
    CrashTrace::markPath("startInlineRename", m_item.shellPath);
    DiagTrace::log(QStringLiteral("[dockbtn] startInlineRename IN name='%1' shellPath='%2' editing=%3 hasEditor=%4 special=%5")
                       .arg(m_item.displayName, m_item.shellPath)
                       .arg(DiagTrace::boolStr(m_editing), DiagTrace::boolStr(m_editor != nullptr),
                            DiagTrace::boolStr(m_item.isSpecial)));
    if (m_editing) {
        // 已存在编辑框（如新建项自动重命名残留）：首次右键“重命名”时重新置前并抢焦点，
        // 避免“点了重命名却没反应、要再点一次”的现象。
        if (m_editor) {
            m_editor->show();
            m_editor->setText(m_item.displayName);
            m_editor->selectAll();
            m_editorShownTick.restart();   // 重新抢焦点，同样给宽限期，避免这次抖动把框收掉
            m_editorFocused = false;       // 重新确认焦点
            installEditHooks(this);        // 确保系统级输入钩子挂在本次编辑上
            DesktopMirrorWindow::raiseAboveBandWindows((HWND)m_editor->winId());
            forceForegroundWindow((HWND)m_editor->winId(), &m_attachedTid);
            // 同样走错峰补抢，确保这次也真的拿到键盘焦点
            for (int delayMs : {0, 40, 120, 250, 500, 900}) {
                QTimer::singleShot(delayMs, this, [this]() {
                    if (m_editing && m_editor && !m_editorFocused) {
                        forceForegroundWindow((HWND)m_editor->winId(), &m_attachedTid);
                        m_editor->activateWindow();
                        m_editor->setFocus();
                    }
                });
            }
            return;   // 已有编辑框：本次只是“重新置前”，不重走下面的新建流程
        }
        // 只置了标志、编辑框却已不在（异常时序残影）：旧实现这里是**静默 return** ——
        // 后果是 F2 与右键菜单的「重命名」**双双永久失效**，必须重启程序才恢复：
        // · F2 被 DesktopMirrorWindow::hotkeyContextOk() 的 isEditing() 挡死；
        // · 右键「重命名」被这个 return 挡死。
        // （收纳盒侧 DesktopIconButton 已改为“复位后继续”，Dock 侧此前漏改，此处对齐。）
        DiagTrace::log(QStringLiteral("[dockbtn] startInlineRename: m_editing && !m_editor -> RESET and continue"));
        m_editing = false;
        m_renameCancelled = false;
        m_imeComposing = false;
    }
    // 特殊命名空间项（我的电脑/网络/回收站/控制面板/个人文件夹）不可重命名
    if (m_item.isSpecial || m_item.shellPath.startsWith(QLatin1String("::{"))) {
        DiagTrace::log(QStringLiteral("[dockbtn] startInlineRename ABORT: special namespace item"));
        return;
    }

    m_editing = true;
    m_renameCancelled = false;
    m_editorFocused = false;         // 每次重新进入编辑态都要重新确认焦点，中途可能已被前台锁收回
    m_imeComposing = false;          // 合成状态随本次编辑重置
    if (!m_editor) {
        // 关键：必须做成“无父的顶层窗口”，不能做 Dock 子控件。
        // Dock 主窗口带 Qt::WindowDoesNotAcceptFocus（Win+D 免疫所必需），其子控件整条
        // 焦点链都不会收到键盘焦点，导致子控件 QLineEdit 即便 setFocus() 也打不进字。
        // 独立 Qt::Window 不受此约束，并复用已验证的 raiseAboveBandWindows 压在收纳盒/
        // 助手之上（仍非所有程序之上，与重命名/新建分类弹框行为一致）。
        m_editor = new QLineEdit(nullptr);
        // 供 DesktopImmunityFilter 识别并豁免（同上：带 WS_EX_NOACTIVATE 的编辑框无法被激活）。
        m_editor->setObjectName(QStringLiteral("InlineRenameEditor"));
        m_editor->setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        m_editor->setFrame(false);
        m_editor->installEventFilter(this);
        // Enter 提交。
        connect(m_editor, &QLineEdit::returnPressed, this, &DockIconButton::commitInlineRename);
        // 失焦提交：只有失焦发生在“宽限期”之后，才认定是用户点到别处。
        // 新建项弹框时本进程并非前台（Dock 是 WS_EX_NOACTIVATE），编辑框 show() 的瞬间
        // Windows 会先派发一轮激活/失焦抖动；若把它当成“点别处”立刻提交，编辑框就一闪而过。
        // 宽限期内改为重新抢回焦点，保证框稳定显示。
        connect(m_editor, &QLineEdit::editingFinished, this, [this]() {
            if (!m_editing) return;
            DiagTrace::log(QStringLiteral("[dockbtn] editingFinished elapsed=%1ms cancelled=%2 focused=%3")
                               .arg(m_editorShownTick.isValid() ? m_editorShownTick.elapsed() : -1)
                               .arg(DiagTrace::boolStr(m_renameCancelled),
                                    DiagTrace::boolStr(m_editorFocused)));
            if (m_renameCancelled) { cancelInlineRename(); return; }
            // 这里【绝不提交】（2026-09-13 定论，与 DesktopIconButton 对齐）。
            // editingFinished 是**被动**信号：窗口重排、前台锁收回、系统激活抖动都会派发它，
            // 用户并没有“点到别处”。旧实现在宽限期之后直接 commitInlineRename()：
            // · 用户还没开始打字 → 新名 == 磁盘旧名 → 提交按“无操作”处理，静默收框；
            // · 下一次 F2/右键重命名时按钮已是 editing=false/hasEditor=false
            // （正是日志里“每次都是 editing=N hasEditor=N”的来源）。
            // 提交只由三个明确的用户意图驱动：Enter（returnPressed / WH_KEYBOARD_LL）、
            // Esc（WH_KEYBOARD_LL → cancelInlineRename）、点框外（WH_MOUSE_LL）。
            // 所以这里只补抢焦点，让框继续留在屏幕上等用户输入。
            if (m_editor) {
                forceForegroundWindow((HWND)m_editor->winId(), &m_attachedTid);
                m_editor->activateWindow();
                m_editor->setFocus();
            }
        });
    }
    // 编辑框几何精确对齐标签绘制：labelTop = iconY + iconS + 4，宽 = w - 8
    const int w = width(), h = height();
    const int iconS = m_iconSize;
    const int iconY = qMax(4, qRound(m_cellW * 0.04));
    const int labelTop = iconY + iconS + 4;
    const int labelH = h - labelTop - 4;
    // 独立顶层窗口：几何用全局坐标（标签在屏幕上的真实位置），与自绘标签像素对齐。
    const QPoint labelGlobal = mapToGlobal(QPoint(4, labelTop));
    m_editor->setGeometry(labelGlobal.x(), labelGlobal.y(), qMax(20, w - 8), qMax(16, labelH));
#ifdef Q_OS_WIN
    m_editor->setFont(iconTitleFont());
#endif
    // 白底黑字、蓝边，选中蓝底白字，贴近 Explorer 行内编辑观感
    m_editor->setStyleSheet(
        "QLineEdit{ background:white; color:black;"
        " border:1px solid rgb(0,120,215);"
        " border-radius:0;"                       // 直角框，不用圆角
        " selection-background-color:rgb(51,153,255); selection-color:white;"
        " padding:0 1px; }");
    m_editor->setText(m_item.displayName);
    m_editor->selectAll();          // 选中全名（含扩展名）
    m_editorShownTick.start();      // 记录显示时刻：随后的短时失焦抖动按“非用户操作”处理
    m_editor->show();
    // ① 清掉 Qt 可能替我们指定的 owner：被 owner 化的窗口会跟随 owner 一起被
    // SetWindowPos(owner, HWND_BOTTOM) 拖到最底层（Dock 每秒都被压底），编辑框会因此
    // 沉到桌面图标层之下 —— 完全不可见，而 Qt 仍报 isVisible()==true。
    EditorWatch::clearOwner((HWND)m_editor->winId());
    // ② 直接 HWND_TOPMOST 置顶（而不是“相对插到 band 窗口之上”）：本项目有一堆周期性 Z 序
    // 改写（parkAllTargetsAtBottom 1s / band 守卫 / topmost 状态机），只有 topmost 层能稳定
    // 不被它们波及。编辑框是短暂输入窗口，置顶是 Windows 行内重命名的正常语义。
    EditorWatch::forceOnTop((HWND)m_editor->winId());
    // 压在收纳盒/助手之上（非所有程序之上），与 GlassInputDialog 一致
    DesktopMirrorWindow::raiseAboveBandWindows((HWND)m_editor->winId());
    // 强制抢前台焦点：越过 Windows 焦点窃取防护，保证首次右键“重命名”即可直接输入，
    // 不再需要点第二次（activateWindow/setFocus 在非前台进程会被静默忽略）。
    // 挂靠线程输入队列的时机在 show() 之后：此时前台窗口仍是“打开菜单前的那个”，挂靠才有效。
    forceForegroundWindow((HWND)m_editor->winId(), &m_attachedTid);
    m_editor->activateWindow();
    m_editor->setFocus();
    // 错峰补抢焦点：Dock 为 WS_EX_NOACTIVATE，本进程通常不在前台，首次 SetForegroundWindow 会被
    // 前台锁延后/丢弃，导致“框出来了但没焦点，必须先点进输入框才能打字/回车”。这里多次补抢，
    // 一旦确认拿到焦点（m_editorFocused）立即停止，避免与用户输入抢焦点。
    for (int delayMs : {0, 40, 120, 250, 500, 900}) {
        QTimer::singleShot(delayMs, this, [this]() {
            if (m_editing && m_editor && !m_editorFocused) {
                forceForegroundWindow((HWND)m_editor->winId(), &m_attachedTid);
                m_editor->activateWindow();
                m_editor->setFocus();
            }
        });
    }
    update();                        // 隐藏自绘标签，避免与编辑框重影
    // 关键：挂上系统级输入钩子。编辑框能否拿到键盘焦点、点击能否经 Qt 路由回到 Dock，都不再由
    // 这两条路决定——回车/Esc 与“点框外即提交”由 WH_KEYBOARD_LL / WH_MOUSE_LL 保证，一次生效。
    installEditHooks(this);
    emit inlineEditStarted(this);    // 通知 Dock：进入编辑态（编辑期间延后桌面刷新）
    // ③ 拉起编辑期看护定时器：每 150ms 复查“真的在屏幕上最上层 + 真的是前台窗口”，
    // 任一条不成立立刻再断言并写日志（见 editorwatch.h）。
    if (!m_editWatchTimer) {
        m_editWatchTimer = new QTimer(this);
        m_editWatchTimer->setInterval(150);
        connect(m_editWatchTimer, &QTimer::timeout, this, &DockIconButton::editorWatchTick);
    }
    m_editWatchCount = 0;
    m_editWatchTimer->start();
    {
        // 证据点：noact=Y 说明编辑框带着 WS_EX_NOACTIVATE（无法被激活 → 打不进字）；
        // fgSelf=N 说明前台仍在别的进程（前台锁会让 SetForegroundWindow 静默失效）；
        // onTop=N 说明编辑框被压在别的窗口之下（Qt 的 isVisible() 完全看不出来）；
        // owner!=0 说明它被 owner 化了，会随 owner 被压底而沉到桌面图标层之下。
        const HWND eh = (HWND)m_editor->winId();
        const EditorWatch::Status st = EditorWatch::probe(eh);
        DiagTrace::log(QStringLiteral("[dockbtn] startInlineRename OK visible=%1 geom=%2,%3 %4x%5 focus=%6 noact=%7 fgSelf=%8 onTop=%9 owner=%10 above=%11")
                           .arg(DiagTrace::boolStr(m_editor->isVisible()))
                           .arg(m_editor->x()).arg(m_editor->y())
                           .arg(m_editor->width()).arg(m_editor->height())
                           .arg(DiagTrace::boolStr(m_editor->hasFocus()))
                           .arg(DiagTrace::boolStr(st.noActivate), DiagTrace::boolStr(st.isForeground))
                           .arg(DiagTrace::boolStr(st.onTopAtCenter))
                           .arg(reinterpret_cast<quintptr>(st.owner))
                           .arg(st.aboveDesc.isEmpty() ? QStringLiteral("-") : st.aboveDesc));
    }
}

// 编辑期看护（与 DesktopIconButton::editorWatchTick 同源）：
// Qt 的 isVisible() 只看 WS_VISIBLE（被压住也返回 true）、hasFocus() 只看内部记账
// （Windows 真前台不是它时也返回 true），两者都无法反映“用户看不见 / 打不进字”。
// 作者：谭征
void DockIconButton::editorWatchTick() {
    if (!m_editing || !m_editor) return;
    ++m_editWatchCount;
    const HWND eh = (HWND)m_editor->winId();
    const EditorWatch::Status st = EditorWatch::probe(eh);

    bool repaired = false;
    if (!st.onTopAtCenter) {
        EditorWatch::forceOnTop(eh);
        repaired = true;
        DiagTrace::log(QStringLiteral("[dockbtn] WATCH !onTop tick=%1 above=%2 -> forceOnTop")
                           .arg(m_editWatchCount).arg(st.aboveDesc));
    }
    if (!st.isForeground && m_editWatchCount <= 20) {
        forceForegroundWindow(eh, &m_attachedTid);
        m_editor->activateWindow();
        m_editor->setFocus();
        repaired = true;
        DiagTrace::log(QStringLiteral("[dockbtn] WATCH !fg tick=%1 -> refocus").arg(m_editWatchCount));
    }
    if (st.owner) {
        EditorWatch::clearOwner(eh);
        repaired = true;
        DiagTrace::log(QStringLiteral("[dockbtn] WATCH owner=%1 cleared").arg(reinterpret_cast<quintptr>(st.owner)));
    }
    if (m_editWatchCount <= 6 || repaired) {
        DiagTrace::log(QStringLiteral("[dockbtn] WATCH tick=%1 onTop=%2 fg=%3 noact=%4 owner=%5 qtFocus=%6")
                           .arg(m_editWatchCount)
                           .arg(DiagTrace::boolStr(st.onTopAtCenter), DiagTrace::boolStr(st.isForeground),
                                DiagTrace::boolStr(st.noActivate))
                           .arg(reinterpret_cast<quintptr>(st.owner))
                           .arg(DiagTrace::boolStr(m_editor->hasFocus())));
    }
}

// cleanupinlineeditor
// 作者：谭征
void DockIconButton::cleanupInlineEditor() {
    const bool wasEditing = m_editing;
    DiagTrace::log(QStringLiteral("[dockbtn] cleanupInlineEditor wasEditing=%1 hasEditor=%2 name='%3'")
                       .arg(DiagTrace::boolStr(wasEditing), DiagTrace::boolStr(m_editor != nullptr))
                       .arg(m_item.displayName));
    m_editing = false;
    m_renameCancelled = false;
    m_imeComposing = false;
    m_editorFocused = false;
    if (m_editWatchTimer) m_editWatchTimer->stop();   // 编辑结束：看护定时器一并停掉
    uninstallEditHooks(this);        // 先卸载钩子：此后任何输入都不再与本编辑态相关
    detachForegroundThread(&m_attachedTid);   // 对称解除线程输入挂靠（编辑期才需要）
    if (m_editor) {
        // 关键：先 hide() 再 deleteLater()。deleteLater 要等事件循环回到创建层级才执行，
        // 若期间（如处于原生菜单嵌套循环）被拖延，屏幕上会残留一个“按钮已认为编辑结束、
        // 却仍然可见且不响应任何操作”的僵尸编辑框——正是“点空白/回车/Esc 全都提交不了”的观感来源。
        m_editor->hide();
        m_editor->removeEventFilter(this);
        m_editor->deleteLater();
        m_editor = nullptr;
    }
    update();                        // 恢复标签绘制
    if (wasEditing) emit inlineEditFinished(this);   // 通知 Dock：编辑结束（补做被延后的刷新）
}

// 取消inline重命名
// 作者：谭征
void DockIconButton::cancelInlineRename() {
    if (!m_editing) return;
    DiagTrace::log(QStringLiteral("[dockbtn] cancelInlineRename name='%1'").arg(m_item.displayName));
    cleanupInlineEditor();
}

// 内联重命名：提交（Enter/失焦）/取消（Esc）。cleanupEditor 负责销毁编辑框并恢复标签绘制。
// 作者：谭征
void DockIconButton::commitInlineRename() {
    if (!m_editing) {
        DiagTrace::log(QStringLiteral("[dockbtn] commitInlineRename SKIP: !m_editing"));
        return;
    }
    const QString oldName = m_item.displayName;     // 提交前先取旧名，用于通知 Dock 更新 m_entries
    const QString newName = m_editor ? m_editor->text().trimmed() : QString();
    CrashTrace::markPath("commitInlineRename", oldName + QStringLiteral(" -> ") + newName);
    // ① 先收框：无论改名成功与否，编辑框都必须立即从屏幕消失。这样即便本次提交是由系统低级
    // 钩子回调触发的（该上下文要求尽快返回，否则钩子会被系统判定超时而丢弃），动作也足够轻量。
    cleanupInlineEditor();
    DiagTrace::log(QStringLiteral("[dockbtn] commit old='%1' new='%2' %3")
                       .arg(oldName, newName,
                            (newName.isEmpty() || newName == oldName) ? QStringLiteral("-> NO-OP")
                                                                      : QStringLiteral("-> rename")));
    // ② 空名或与原名相同：视为无操作（Windows 语义：不改名直接提交＝保持原名、文件不丢）
    if (newName.isEmpty() || newName == oldName) return;
    // ③ 再落盘改名
    performRename(oldName, newName);
}

// 落盘后可再通知 Dock；失败提示走队列消息框，绝不在钩子回调里弹模态窗口。
// 作者：谭征
void DockIconButton::performRename(const QString& oldName, const QString& newName) {
#ifdef Q_OS_WIN
    CrashTrace::markPath("performRename:enter", m_item.shellPath);
    // 落盘统一走 ShellOps（IShellFolder::SetNameOf）—— 与收纳盒图标是同一条通道。
    // 为什么要抽出共用：两端各写一套 SetNameOf 极易出现“一边能改名、一边静默失败”的口径分裂，
    // 而重命名失败若被当成成功，界面会显示一个磁盘上并不存在的名字（比崩溃更难排查）。
    CrashTrace::mark("performRename:SetNameOf");
    QString err;
    if (ShellOps::renameInPlace(m_item.shellPath, newName, (void*)winId(), &err)) {
        // 落盘成功：① 立即更新本按钮的显示名与 shellPath（无需等异步 SHChangeNotify，
        // 否则要拖动才刷新；且 shellPath 若不同步，本按钮后续的右键菜单/二次重命名
        // 会拿到已失效的旧路径而静默失败）；② 同步通知 Dock 更新其 m_entries。
        m_item.displayName = newName;
        const QFileInfo fi(m_item.shellPath);
        if (fi.isAbsolute()) {
            // 改后缀会连带改变“类型”：路径、目标、是否快捷方式都必须一起重算，
            // 否则后续图标/右键菜单/自动重命名判定会继续按旧类型走。
            const QString newPath = QDir(fi.path()).filePath(newName);
            m_item.shellPath = newPath;
            m_item.sourcePath = newPath;
            m_item.isShortcut =
                QFileInfo(newPath).suffix().compare(QLatin1String("lnk"), Qt::CaseInsensitive) == 0
                && DesktopScanner::isRealShortcut(newPath);
            // targetPath 必须是解析后的真实目标（取图只认目标文件，见 DesktopScanner::effectiveTarget）
            m_item.targetPath = DesktopScanner::effectiveTarget(m_item.sourcePath, m_item.isShortcut);
        }
        CrashTrace::markPath("performRename:emit-renamed", newName);
        update();
        emit renamed(oldName, newName);
    } else {
        // 失败提示走队列：本次提交可能来自系统钩子回调，绝不能在那里弹模态窗口。
        QPointer<DockIconButton> self(this);
        const QString msg = err.isEmpty()
            ? QStringLiteral("找不到该项，可能已被移动或删除。") : err;
        QTimer::singleShot(0, this, [self, msg]() {
            if (self) GlassMessageBox::warning(self, QStringLiteral("重命名失败"), msg);
        });
    }
#else
    Q_UNUSED(oldName);
    Q_UNUSED(newName);
#endif
}

// Invoked → 文件系统可能已变，调用方自行决定是否重新扫描。
// 作者：谭征
void DockIconButton::showNativeContextMenu(const QPoint& globalPos) {
#ifdef Q_OS_WIN
    CrashTrace::markPath("showContextMenu", m_item.shellPath);
    Q_UNUSED(globalPos)
    // 菜单本体走 ShellOps 的**共用**实现（收纳盒/图标网格走的是同一个函数）：
    // 之前这里与收纳盒各写一份，结果一处改原生、一处改自建 QMenu，两边菜单长得不一样。
    // 菜单项、分组、图标、语言全部由 Shell 生成，与 Windows 桌面完全一致。
    HWND owner = reinterpret_cast<HWND>(winId());
    // TrackPopupMenu 要**物理像素**；菜单弹在光标处，直接取 Win32 光标坐标免去 DPI 换算。
    POINT pt = {0, 0};
    GetCursorPos(&pt);

    const ShellOps::MenuAction act = ShellOps::showNativeContextMenu(
        owner, m_item.shellPath, m_item.displayName, pt.x, pt.y,
        /*interceptDelete=*/false);   // Dock 保持原状：删除交给 Shell 自己执行

    if (act == ShellOps::MenuAction::Rename) {
        // 命中「重命名」且为真实文件/文件夹：改走内联编辑（SetNameOf 落盘），
        // 不调用 Shell 的 InvokeCommand（Dock 非 Shell View，行内编辑无接收方会哑火）。
        // 特殊命名空间项（我的电脑/网络/回收站等）重命名交由原生菜单（通常无此功能）。
        CrashTrace::mark("showContextMenu:rename");
        DiagTrace::log(QStringLiteral("[dock] menu act=1 Rename isSpecial=%1 shellPath='%2'")
                           .arg(DiagTrace::boolStr(m_item.isSpecial)).arg(m_item.shellPath));
        if (!m_item.isSpecial && !m_item.shellPath.startsWith(QLatin1String("::{"))) {
            startInlineRename();
        } else {
            DiagTrace::log(QStringLiteral("[dock] menu Rename BLOCKED: isSpecial or ::{ path"));
        }
    }
    // 其余命令已由 ShellOps 内部 InvokeCommand 执行完（删除/属性/打开方式…），
    // 任何变更都会经 SHChangeNotify 触发 Dock 刷新。
#else
    Q_UNUSED(globalPos)
#endif
}
