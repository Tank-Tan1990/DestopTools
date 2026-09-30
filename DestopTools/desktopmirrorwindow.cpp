/*
 * @file desktopmirrorwindow.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "desktopmirrorwindow.h"
#include "dockiconbutton.h"
#include "desktopscanner.h"
#include "categorystore.h"
#include "crashtrace.h"
#include "shellops.h"      // 删除/重命名的唯一落盘通道（与收纳盒共用）
#include "diagtrace.h"     // 临时：快捷键/重命名链路诊断日志
#include "lowlevelhookmanager.h"   // A1：进程级共享低级键鼠钩子（全进程 2 个钩子，Dock/收纳盒共用）
#include "theme.h"                 // Theme::shortcutArrowOverlayScale（外观开关：快捷方式箭头）
#include "thememanager.h"          // settingChanged 广播：外观开关热更新
#include "settingsmanager.h"       // QuickActions/* 快捷操作开关（构造期读一次）

// E2：快捷键 gate 诊断日志的开关短路。
// DiagTrace::log 的实参在【调用点】就已构造（QStringLiteral(...).arg(...)），即使 log() 内部
// 因开关关闭而立即返回，构造代价也已付出；而这些判据点位于低级钩子回调路径上
// （回调超时会被 Windows 静默摘钩），故统一用本宏包一层：关闭时连参数都不构造。
#define DOCK_HK_TRACE(expr) do { if (DiagTrace::enabled()) DiagTrace::log(expr); } while (0)

#include <QApplication>
#include <QScreen>
#include <QTimer>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QPainter>
#include <QDebug>
#include <QCoreApplication>
#include <QDir>
#include <QSet>
#include <QFileInfo>
#include <QCursor>
#include <QPair>
#include <QCryptographicHash>   // 自激循环兜底：refresh() 用 SHA1 给 m_entries 算指纹，跳过无变化的重建
#include <QDateTime>        // 跨窗口拖拽：悬停信号的节流时间戳
#include <QVariant>         // 外观开关广播的 payload（settingChanged）
#include <string>          // std::wstring（SHFileOperation 的双 null 结尾路径列表）

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <objbase.h>
#include <commctrl.h>
#include <dwmapi.h>

// 自定义窗口消息，用于接收 SHChangeNotifyRegister 的桌面变更通知
static const UINT WM_DESKTOP_NOTIFY = WM_USER + 101;

// Dock 拖拽浮层：把「被收纳盒盖住的拖拽图标」补画到最上层。
// 背景：Dock 的图标拖动是**自定义跟手移动**（按钮本体 move()，不走 QDrag），而 Dock 分层窗
// 恒在最底、收纳盒在其上 → 图标拖进盒子就被整块盖住，只剩光标可见。
// 只画「与收纳盒相交」的那一块（其余留空透明）：遮挡区之外仍由真实按钮绘制，浮层若整块画
// 就会与真实图标叠成重影。故不用窗口 mask（SetWindowRgn 在分层窗上行为未验证），
// 直接在 paintEvent 里 QPainter 裁剪 —— 透明像素自然透出下层。
// 必须不吃输入：置顶浮层若截获鼠标，拖动会当场断在半途（按下时的隐式抓取仍在 Dock 按钮上，
// 但浮层一旦成为光标下的窗口就会抢走后续 move/release）。故 TransparentForInput + 透明鼠标 + 不抢焦点。
namespace {
class DockDragGhost : public QWidget {
public:
    DockDragGhost()
        : QWidget(nullptr, Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint
                             | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput) {
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFocusPolicy(Qt::NoFocus);
        // 挡掉应用级样式表：否则全局 QWidget 规则会给浮层铺上背景色，把下面的收纳盒糊住。
        setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    }
    void setFrames(const QVector<QPair<QPoint, QPixmap>>& frames) {
        m_frames = frames;
        update();
    }
    void setPaintClip(const QRect& localClip) {
        if (m_clip == localClip) return;
        m_clip = localClip;
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        if (m_clip.isEmpty() || m_frames.isEmpty()) return;
        QPainter p(this);
        p.setClipRect(m_clip);
        for (const auto& f : m_frames) p.drawPixmap(f.first, f.second);
    }
private:
    QVector<QPair<QPoint, QPixmap>> m_frames;   // (相对包围盒左上角的偏移, 图标位图)
    QRect m_clip;                               // 只画这一块（浮层局部坐标），其余保持透明
};
}   // namespace

// 定位承载桌面图标的 SysListView32。现代 Windows 活动列表通常挂在某个 WorkerW 下；
// 枚举 Progman 与所有 WorkerW 下的全部 SysListView32，选「图标最多 → 可见 → 在 WorkerW 下」
// 的候选作为唯一真相源。Dock 启动后会把原生桌面 SW_HIDE，此时所有候选都不可见，
// 必须依靠“图标数最多 + 在 WorkerW 下”来锁定真正活动的桌面对话框，否则可能选中
// Progman 下的空壳列表，导致 Dock 图标与 Windows 桌面不一致。
static HWND findBestDesktopListView() {
    struct Cand { HWND hwnd; int count; bool visible; bool underWorker; };
    QVector<Cand> cands;

    auto consider = [&](HWND h, bool worker) {
        if (!h || !IsWindow(h)) return;
        int n = (int)SendMessageW(h, LVM_GETITEMCOUNT, 0, 0);
        RECT r; GetWindowRect(h, &r);
        bool vis = IsWindowVisible(h) && (r.right > r.left) && (r.bottom > r.top);
        cands.append({ h, n, vis, worker });
    };

    HWND hProgman = FindWindowW(L"Progman", L"Program Manager");
    if (hProgman) {
        HWND hShell = FindWindowExW(hProgman, nullptr, L"SHELLDLL_DefView", nullptr);
        if (hShell) consider(FindWindowExW(hShell, nullptr, L"SysListView32", nullptr), false);
    }
    HWND hWorker = nullptr;
    while ((hWorker = FindWindowExW(nullptr, hWorker, L"WorkerW", nullptr)) != nullptr) {
        HWND hShell = FindWindowExW(hWorker, nullptr, L"SHELLDLL_DefView", nullptr);
        if (hShell) consider(FindWindowExW(hShell, nullptr, L"SysListView32", nullptr), true);
    }

    HWND best = nullptr;
    int bestScore = -1;  // 综合评分：count*4 + visible*2 + underWorker*1
    for (const Cand& c : cands) {
        if (c.count <= 0) continue;
        int score = c.count * 4 + (c.visible ? 2 : 0) + (c.underWorker ? 1 : 0);
        if (score > bestScore) {
            bestScore = score;
            best = c.hwnd;
        }
    }
    return best;
}

// 枚举所有承载桌面图标的 SysListView32（Progman 与全部 WorkerW 下），用于整屏隐藏/还原，
// 避免只处理单个句柄导致漏还原、桌面图标永久消失。
static QVector<HWND> enumDesktopLists() {
    QVector<HWND> out;
    auto consider = [&](HWND h) {
        if (h && IsWindow(h)) out.append(h);
    };
    HWND hProgman = FindWindowW(L"Progman", L"Program Manager");
    if (hProgman) {
        HWND hShell = FindWindowExW(hProgman, nullptr, L"SHELLDLL_DefView", nullptr);
        if (hShell) consider(FindWindowExW(hShell, nullptr, L"SysListView32", nullptr));
    }
    HWND hWorker = nullptr;
    while ((hWorker = FindWindowExW(nullptr, hWorker, L"WorkerW", nullptr)) != nullptr) {
        HWND hShell = FindWindowExW(hWorker, nullptr, L"SHELLDLL_DefView", nullptr);
        if (hShell) consider(FindWindowExW(hShell, nullptr, L"SysListView32", nullptr));
    }
    return out;
}

static DesktopMirrorWindow* g_self = nullptr;
static HWND s_dockHwnd = nullptr;   // Dock 自身的 HWND（分层基准：收纳盒/助手插在它之上）
#endif

DesktopMirrorWindow::DesktopMirrorWindow(QWidget* parent)
    : QWidget(parent) {
    // 不使用 WindowStaysOnBottomHint：在 Windows 上它会把窗口压到壁纸之后导致不可见。
    // “始终最底层”改用 show() 后 SetWindowPos(HWND_BOTTOM) + 前台变化钩子实现。
    setWindowFlags(Qt::FramelessWindowHint
                   | Qt::WindowDoesNotAcceptFocus
                   | Qt::Window);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setObjectName(QStringLiteral("DesktopMirrorWindow"));
    g_self = this;

    // 应用级“点别处即提交”守卫：内联重命名打开时，任何落在编辑框之外的鼠标按下都结束命名。
    // Dock 主窗口是 WS_EX_NOACTIVATE，重命名编辑框又是独立顶层窗口；编辑框开启期间，落在 dock
    // 空白处 / 左侧快捷栏 / 收纳盒 / 其它窗口上的点击，未必都会走到 DesktopMirrorWindow::mousePressEvent
    // （被子控件命中、被别的窗口先接收等），表现为“回车/Esc 能提交、鼠标点空白处却不能提交”。
    // 把守卫提到应用层可彻底规避窗口/控件路由差异，与 Windows 桌面行内重命名行为一致。
    // 2026-09-21 回退 A3：此前为省掉一次虚调用改成“仅编辑期挂载”，但卸载判据依赖
    // “遍历 m_buttons 确认无人处于编辑态”，而 m_buttons 在刷新/重建窗口期可能瞬时不含
    // 正在编辑的按钮 → 过滤器被提前摘掉 → “点编辑框外即提交”失效（重命名表现为提交不上去）。
    // 该守卫是重命名这条核心功能的唯一鼠标兜底通道，可靠性优先：恢复构造期常驻挂载。
    qApp->installEventFilter(this);
    m_appFilterInstalled = true;

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    connect(m_refreshTimer, &QTimer::timeout, this, &DesktopMirrorWindow::refresh);

    // 自动重命名重试：新建项弹框依赖“刷新→布局→按钮就绪”的时序，竞态下首轮可能落空，
    // 用一次性定时器补弹（次数有上限），确保第 2 个及以后的新建图标同样能弹出重命名框。
    m_autoRenameTimer = new QTimer(this);
    m_autoRenameTimer->setSingleShot(true);
    m_autoRenameTimer->setInterval(120);
    connect(m_autoRenameTimer, &QTimer::timeout, this, &DesktopMirrorWindow::applyAutoRename);

    // 兜底：周期性把【三窗口】一并压到 Z 序最底层，确保任何来源（任务栏点击/系统切换等）
    // 试图提层都被纠正。show desktop 激活期 parkAllTargetsAtBottom 内部短路，三者不会被打回。
    // B2：1000ms → 3000ms。SetWindowPos(HWND_BOTTOM) 没有“已到位就短路”的语义，
    // 每次都让窗口管理器重算 z 序（对整屏分层窗还可能附带一次合成/重绘）；而真正需要纠偏的
    // 场景（点击任务栏/程序切换导致提层）本身是低频的，3 秒兜底足够。
    m_bottomTimer = new QTimer(this);
    m_bottomTimer->setInterval(3000);
    connect(m_bottomTimer, &QTimer::timeout, this, []() { DesktopMirrorWindow::parkAllTargetsAtBottom(); });

    // 回收站满/空状态跟随 Windows 桌面：回收站是动态图标（满/空两套位图），
    // 但 SHChangeNotifyRegister 只监听桌面文件夹（非递归），收不到回收站这一特殊命名空间项的
    // 状态翻转通知 → 用轻量轮询兜底，翻转才重取图标、翻转前零开销。
    // B3：1500ms → 9000ms。SHQueryRecycleBinW 是 shell 命名空间查询，
    // 回收站条目多时单次可达数十毫秒，而且它【永远在跑】（哪怕回收站从未变过）。
    // 满/空翻转只是「清空或删除后的视觉反馈」，晚几秒刷新完全可感知无损。
    // B4：9000ms → 30000ms。本机实测单次 48~61ms（全盘 SHQueryRecycleBinW），
    // 每 9 秒一次 ≈0.6% 的 GUI 线程常驻占用，且会**恰好在游戏读盘最紧张的时刻**与磁盘 I/O 叠加
    // （机械盘 + 360 抢盘时该调用会更长）；而 30 秒的延迟对"回收站满/空"这一反馈完全无感
    // （只在用户刚清空/刚删除文件时才有可感知差异）。
    m_recyclePollTimer = new QTimer(this);
    m_recyclePollTimer->setInterval(30000);
    connect(m_recyclePollTimer, &QTimer::timeout, this, &DesktopMirrorWindow::pollRecycleBinState);

    QScreen* ps = QApplication::primaryScreen();
    if (ps) setGeometry(ps->geometry());
    m_lastSize = this->size();

    // 加载 Dock 自定义图标位置覆盖（用户在 Dock 内拖动后持久化的值），首次布局优先应用。
    loadCustomDockPos();

    // 外观开关热更新：「在快捷方式图标上显示箭头」（2026-09-22）
    // Dock 图标的箭头是**在 layoutButtons 里取图那一刻**就合成进位图的（经
    // Theme::shortcutArrowOverlayScale → DesktopScanner::loadIcon → composeShortcutOverlay），
    // 所以开关一变必须：① 清掉进程内图标缓存（旧条目还带着相反态）；
    // ② 让按钮重建，重新走一遍取图。
    // ② 不能只调 refresh()：refresh() 对「条目指纹未变」会直接 return（自激循环兜底），
    // 而改外观设置恰恰不改变任何条目 —— 不先清掉指纹就等于什么都没做（用户看到"勾了没反应"）。
    connect(ThemeManager::instance(), &ThemeManager::settingChanged, this,
            [this](const QString& key, const QVariant&) {
                if (key != QLatin1String("Appearance/shortcutArrow")) return;
                DesktopScanner::clearIconCache();
                m_entriesDigest.clear();   // 绕过"条目未变即跳过重建"的指纹短路：这次要的就是重取图标
                refresh();
            });

    // 快捷操作开关（设置中心「快捷操作」页的两个复选，2026-09-24 接通）
    // 消费点都在**鼠标钩子回调之后的那一拍**（Dock 无焦点，空白处鼠标未必回到 Qt，必须走
    // WH_MOUSE_LL 通道），而钩子路径禁做文件 I/O —— 读设置＝读 INI，绝不能放进回调里。
    // 故在此缓存成普通成员：构造期读一次，之后靠 settingChanged 热更新（写盘方是设置中心）。
    {
        SettingsManager sm;
        m_quickHideIconsOnDblClick =
            sm.loadValue(QStringLiteral("QuickActions/hideIconsOnDoubleClick"), true).toBool();
        m_quickDrawBoxOnBlank =
            sm.loadValue(QStringLiteral("QuickActions/drawBoxOnBlank"), true).toBool();
    }
    connect(ThemeManager::instance(), &ThemeManager::settingChanged, this,
            [this](const QString& key, const QVariant& value) {
                if (key == QLatin1String("QuickActions/hideIconsOnDoubleClick")) {
                    m_quickHideIconsOnDblClick = value.toBool();
                } else if (key == QLatin1String("QuickActions/drawBoxOnBlank")) {
                    m_quickDrawBoxOnBlank = value.toBool();
                }
            });
}

// 显式同步两个快捷操作开关（供构造成员之外的调用方使用；设置中心改完会走 settingChanged）。
// 作者：谭征
void DesktopMirrorWindow::setQuickActionFlags(bool hideIconsOnDoubleClick, bool drawBoxOnBlank) {
    m_quickHideIconsOnDblClick = hideIconsOnDoubleClick;
    m_quickDrawBoxOnBlank = drawBoxOnBlank;
}

DesktopMirrorWindow::~DesktopMirrorWindow() {
    stopMirror();
#ifdef Q_OS_WIN
    // A1：本窗口是文件级静态裸指针 g_self 的唯一持有者，析构后若不置空，
    // g_self 立刻变成悬垂指针。它的唯一消费者 debugLog()→dumpDebug() 会被**本窗口之外**
    // 的路径调用（事件钩子 guardEventProc / 状态机 enter·exitDesktop / 其它窗口的静态调用），
    // 那时就是 use-after-free。当前 dumpDebug() 恰好是空实现才没炸 —— 属定时炸弹，
    // 必须在析构时解除引用（只在本对象确实是 g_self 时才清，避免析构顺序交错时误清别人）。
    if (g_self == this) g_self = nullptr;
#endif
    // 拖拽浮层是**无父对象的顶层窗**，不随本窗口析构自动销毁，必须显式 delete
    // （否则切模式/退出时可能留下一层散不掉的浮窗）。
    delete m_dragGhost;
    m_dragGhost = nullptr;
}

// 设置源模式
// 作者：谭征
void DesktopMirrorWindow::setSourceMode(SourceMode mode) {
    if (m_sourceMode == mode) return;
    m_sourceMode = mode;
    refresh();
}

// 调用后所有文件回到“未分类”，Dock 将与当前真实桌面目录保持一致。
// 作者：谭征
void DesktopMirrorWindow::clearCategoryStore() {
    CategoryStore::clearAll();
}

// 开始镜像
// 作者：谭征
void DesktopMirrorWindow::startMirror() {
    m_mirroring = true;   // 供延迟首刷判断"回调到达时镜像是否仍在运行"（见 stopMirror 的落旗）
#ifdef Q_OS_WIN
    QScreen* ps = QApplication::primaryScreen();
    if (ps) setGeometry(ps->geometry());
    m_lastSize = this->size();

    // 关键：在隐藏原生桌面图标层之前，先快照真实 ListView 中每个图标的坐标。
    // 隐藏后 LVM_GETITEMPOSITION 会失效，所以必须在隐藏前完成快照。
    takeNativeSnapshot();

    // 隐藏原生桌面图标层，使本窗口成为唯一的“桌面”可视面。
    hideNativeDesktop();

    show();
    ensureDockWindowStyle();   // 一次性把 Dock EX/STYLE 调好（WS_EX_NOACTIVATE + 去 WS_MINIMIZEBOX/...）
    s_dockHwnd = (HWND)winId(); // 记录 Dock HWND 作为分层基准
    parkAllTargetsAtBottom();   // 分层压底（Dock 最底，收纳盒/助手在其上）
    // 注册 Dock 进抗“显示桌面”守卫（T5 事件钩子即时恢复 + 100ms 轮询兜底）。
    // 绝不做 SetParent 嵌入 WorkerW —— Qt 顶层窗口变桌面子窗口后坐标/裁剪错乱、颜色键透明在子窗口下不渲染，Dock 不可见。
    registerCloakTarget(this);

    // 兜底定时压底，对抗任务栏点击/系统切换可能带来的提层。
    if (m_bottomTimer) m_bottomTimer->start();

    // 回收站满/空状态跟随：启动即拍一次状态（避免等首个 1.5s 才显示正确的满/空图标），
    // 随后由 m_recyclePollTimer 周期轮询、状态翻转才刷新。
    pollRecycleBinState();
    if (m_recyclePollTimer) m_recyclePollTimer->start();

    // 延迟首刷：等本窗口显示、Explorer 稳定后布局。
    // 顺序即修复（「程序打开瞬间鼠标卡死」根治）：
    // 全局快捷键（F2 重命名 / Delete 删除）用的是 WH_KEYBOARD_LL / WH_MOUSE_LL 低级钩子，
    // 而低级钩子是【系统同步回调到安装线程（本 GUI 线程）】的 —— 所有鼠标输入都必须等
    // 我们的 hookProc 返回才会继续派发。线程一旦被堵，全系统鼠标一起僵住；超过
    // LowLevelHooksTimeout（默认约 300ms）还会被 Windows 静默摘钩（项目已知的"F2 用一次
    // 就失效"同源坑）。
    // 而 refresh() 的首刷是重活：takeNativeSnapshot + 逐个桌面图标跨进程
    // SendMessageW(LVM_GETITEMW/GETITEMPOSITION) + VirtualAllocEx/ReadProcessMemory，
    // N 个图标 ≈ 5N 次内核操作，实测数百毫秒。
    // 旧实现把 ensureHotkeyHooks() 放在首刷之前 → 启动瞬间"钩子在位 + 线程被钉住"叠加，
    // 正是打开程序时鼠标卡住不动的原因。故改为：先干重活，重活结束后才能让钩子出现在位。
    // （运行期每次 Dock 交互都会重装钩子自愈，推迟这几百毫秒不影响可用性。）
    QTimer::singleShot(300, this, [this]() {
        // 护栏：若这 300ms 内已经 stopMirror（退出程序 / 切换显示模式），必须整体放弃 ——
        // 否则会在镜像已停止后重新装回低级钩子，并刷新一个已停用的镜像。
        if (!m_mirroring) return;
        refresh();             // 重活：读取桌面图标层内容并布局
        ensureHotkeyHooks();   // 重活结束后再装低级钩子，避免与卡顿窗口重叠
    });
#else
    show();
#endif
}

// 停止镜像
// 作者：谭征
void DesktopMirrorWindow::stopMirror() {
    m_mirroring = false;       // 先落旗：让尚未触发的延迟首刷（连同装钩子）自动放弃
    releaseHotkeyHooks();      // 停止镜像即卸载快捷键钩子（不再代表桌面，绝不继续接管键盘）
    endRubber();               // 收掉可能残留的画框（幂等）：停轮询、清绘制
    resetDragGhost();          // 收掉可能残留的拖拽浮层（切模式/退出时也会走到这里）
#ifdef Q_OS_WIN
    if (m_bottomTimer) m_bottomTimer->stop();
    if (m_recyclePollTimer) m_recyclePollTimer->stop();
    unregisterChangeNotify();
    // A2：stopMirror 此前**不注销自己的 cloak 目标** → s_cloakTargets 里
    // 长期残留一个已销毁窗口的 HWND。Windows 会复用 HWND，一旦该值被复用给别的窗口：
    // · isProtectedWindow() 会把【别人的窗口】当成我们的受保护窗口，全局事件钩子于是
    // 对无关窗口执行 ShowWindow/SetWindowPos/DwmSetWindowAttribute（越权操作他窗口）；
    // · isForegroundRealApp() 也会把它误判成"前台是我们自己的窗口"，导致 show-desktop
    // 状态机撤顶判据失灵。
    // 与 startMirror() 里的 registerCloakTarget(this) 严格配对（unregister 是幂等的）。
    unregisterCloakTarget(this);
    restoreNativeDesktop();
#endif
}

// —— 抗“显示桌面”(Win+D) / “最小化全部”(Win+M) 进程级守卫 ——
// T5：SetWinEventHook 监听三类外壳动作，事件到达即恢复，比轮询更快
// · EVENT_OBJECT_CLOAKED       → Win+D 的 DWM 外壳斗篷
// · EVENT_OBJECT_HIDE          → SW_HIDE 隐藏路径
// · EVENT_SYSTEM_MINIMIZESTART → Win+M / 最小化全部
// 任一受保护窗口被隐藏即 force=true 对全部三窗口一次性复活，消除“逐窗口 hide 事件”的时序竞态。
// A 路线（持续再断言）：100ms 轮询在前台=桌面(WorkerW/Progman)时持续 force 点亮，击败外壳“持续重隐”；
// 用户打开其它程序(前台变走)即停。复活用 SW_SHOWNA（不激活、不退出显示桌面模式）→ 其它窗口保持隐藏(360 效果)。
// 已撤回 T1 桌面嵌入（SetParent 到 WorkerW）：Qt 顶层窗口变桌面子窗口后坐标/裁剪错乱，Dock 不可见。
#ifdef Q_OS_WIN
static const DWORD kDwmwaCloak   = 13;  // DWMWA_CLOAK   ：只写，清除斗篷位用（APP/SHELL/INVISIBLE 一并清除）
static const DWORD kDwmwaCloaked = 14;  // DWMWA_CLOAKED ：只读，查询窗口当前是否被斗篷

QHash<HWND, QPointer<QWidget>> DesktopMirrorWindow::s_cloakTargets;
HWINEVENTHOOK DesktopMirrorWindow::s_cloakHook = nullptr;
HWINEVENTHOOK DesktopMirrorWindow::s_hideHook = nullptr;
HWINEVENTHOOK DesktopMirrorWindow::s_minimizeHook = nullptr;
HWINEVENTHOOK DesktopMirrorWindow::s_showHook = nullptr;
QTimer* DesktopMirrorWindow::s_pollTimer = nullptr;
bool DesktopMirrorWindow::s_restoring = false;
static int s_pollCount = 0;   // 兜底轮询计数（用于每 ~5s 打印一次全窗口状态）

// —— Rainmeter 式 topmost 状态机 ——
// 根因（终局）：Win10 22H2 的 Win+D/显示桌面按钮走【真 SW_HIDE】，外壳把顶层窗口 SW_HIDE；而
// WS_EX_NOACTIVATE（desktopimmunityfilter 设置，用于「点击不抢焦点」）与 SW_SHOWNORMAL 激活复活
// 【根本冲突】——NOACTIVATE 阻止激活 → 无法脱离 hide-set → 激活复活打不过外壳持续重隐。
// 彻底解法（Rainmeter 官方验证）：HWND_TOPMOST 让窗口在 show desktop/Win+M 下【天然不被外壳
// SW_HIDE/最小化】（Windows 硬语义）。仅在 show desktop 期间置顶（此时其它窗口全隐藏、不遮挡），
// 退出立即撤顶恢复常态 Z 序。这同时满足「常驻显示」+「不遮挡其它程序」。
static bool s_desktopActive = false;      // show desktop 激活态：true=三窗口已置顶(HWND_TOPMOST)
static ULONGLONG s_topmostSince = 0;      // 首次进入 topmost 的时刻（GetTickCount64），仅状态 0→1 时记录
static int s_exitStable = 0;              // 前台离开桌面后的稳定计数（连续 N 次轮询 fgRealApp 才撤顶）

// 当前前台是否为桌面（WorkerW/Progman）——“显示桌面”处于激活态的标志（Win+D 路径）。
static bool isForegroundDesktop() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    wchar_t cls[64] = {0};
    GetClassNameW(fg, cls, 64);
    QString c = QString::fromWCharArray(cls);
    return c.contains(QStringLiteral("WorkerW")) || c.contains(QStringLiteral("Progman"));
}

// 公开调试/强制恢复桥（供自由函数与现有前台钩子调用，避免直接访问 private 成员）。
// 作者：谭征
void DesktopMirrorWindow::debugLog(const QString& s) {
    if (g_self) g_self->dumpDebug(s);
}
// force还原
// 作者：谭征
void DesktopMirrorWindow::forceRestore() {
    restoreAllTargets(true);   // 一次性强制点亮全部注册窗口（内部有 s_restoring 防重入）
}

// 前台是否为「真实程序窗口」（非桌面、非我们自己的受保护窗口）——用于判定「退出 show desktop」。
// 作者：谭征
bool DesktopMirrorWindow::isForegroundRealApp() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (s_cloakTargets.contains(fg)) return false;   // 前台是我们自己的受保护窗口
    wchar_t cls[64] = {0};
    GetClassNameW(fg, cls, 64);
    // B2：原来是 QString::fromWCharArray + contains —— 本函数被 s_pollTimer
    // **每 150ms** 调一次，每次都要堆分配一个 QString；类名比较改用宽字符子串查找，
    // 零分配、零拷贝（类名是 "WorkerW"/"Progman"/"ApplicationFrameWindow" 这类纯 ASCII）。
    if (wcsstr(cls, L"WorkerW") || wcsstr(cls, L"Progman")) return false;
    return true;
}

// —— 统一状态转换（所有路径收敛于此，消除多驱动源竞态）——
// 进入 / 退出只在状态 0→1 / 1→0 时各执行一次；其余调用幂等返回。
static void enterDesktop() {
    DesktopMirrorWindow::debugLog(QStringLiteral("enterDesktop: in desktopActive=%1").arg(s_desktopActive?1:0));
    if (s_desktopActive) return;
    s_desktopActive = true;
    s_topmostSince = GetTickCount64();
    s_exitStable = 0;
    DesktopMirrorWindow::debugLog(QStringLiteral("enterDesktop: 0->1"));
    DesktopMirrorWindow::topmostAllTargets();
}
static void exitDesktop() {
    DesktopMirrorWindow::debugLog(QStringLiteral("exitDesktop: in desktopActive=%1 exitStable=%2")
                                  .arg(s_desktopActive?1:0).arg(s_exitStable));
    if (!s_desktopActive) return;
    s_desktopActive = false;
    s_exitStable = 0;
    DesktopMirrorWindow::debugLog(QStringLiteral("exitDesktop: 1->0"));
    DesktopMirrorWindow::untopmostAllTargets();
}

// 置顶全部受保护窗口（进入 show desktop 时调用）。HWND_TOPMOST 让窗口在 show desktop 下
// 天然不被外壳 SW_HIDE/最小化，无需激活、不抢焦点。SWP_SHOWWINDOW 让可能已被 SW_HIDE 的窗口重新点亮。
// 作者：谭征
void DesktopMirrorWindow::topmostAllTargets() {
    for (auto it = s_cloakTargets.begin(); it != s_cloakTargets.end(); ++it) {
        HWND h = it.key();
        if (!h || !IsWindow(h)) continue;
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    debugLog(QStringLiteral("topmostAllTargets n=%1").arg(s_cloakTargets.size()));
}

// 撤顶全部受保护窗口（退出 show desktop 时调用）。
// 关键（避免遮挡其他程序）：三窗口撤 HWND_NOTOPMOST 时【不显式设 hwndInsertAfter】——
// 撤 topmost 后窗口默认回到它加入 topmost 之前的 Z 序位置（仍可能高于普通程序），
// 但 m_bottomTimer (1s) 调 parkAllTargetsAtBottom 把 Dock 压 HWND_BOTTOM 后，
// 收纳盒/助手仍保持 WS_EX_NOACTIVATE（不抢焦点）—— 用户启动其它程序时，那些程序按
// "新窗口 topmost"自动浮到三窗口之上，达成"三窗口在最底、不遮挡"语义。
// 不再统一 HWND_BOTTOM 压底全部三窗口：那会强行提/压 Z 序，触发 Windows "把窗口视作要提层"的副作用，
// 反而让收纳盒/助手被推过 WorkBuddy 之上（这是 05:44 之前分层 SetWindowPos(h, hDock) 错误的根因）。
// 作者：谭征
void DesktopMirrorWindow::untopmostAllTargets() {
    for (auto it = s_cloakTargets.begin(); it != s_cloakTargets.end(); ++it) {
        HWND h = it.key();
        if (!h || !IsWindow(h)) continue;
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    }
    parkAllTargetsAtBottom();   // Dock 立即压底，m_bottomTimer 后续 1s 周期兜底
    debugLog(QStringLiteral("untopmostAllTargets"));
}

// show desktop 状态机是否正持有三窗口的 z 序控制权（true=守卫放行一切 z 变更）。
// 作者：谭征
bool DesktopMirrorWindow::isShowDesktopActive() { return s_desktopActive; }

// —— 三窗口 nativeEvent 共用的 Z 序守卫（「点击 Dock 一闪而过」的根因修复）——
// 现象：360 不运行时，开其它程序后点击 Dock，三窗口被顶到其它程序之上一闪而过，
// 靠 m_bottomTimer(1s) 事后压回 → 可见闪烁。360 运行时它自己钉住桌面层，所以不闪。
// 根因：点击触发系统/Qt 的提层（BringWindowToTop / 激活提层等，插入参照多为 HWND_TOP），
// dock 瞬间高于普通程序，最迟 1s 后才被兜底定时器压回。
// 修复：在 WM_WINDOWPOSCHANGING（z 序变更【应用之前】同步送达本窗口 wndProc→nativeEvent）
// 阶段改写 WINDOWPOS，从源头杜绝「先置顶再压入」。守卫必须写在各窗口的 nativeEvent：
// WM_WINDOWPOSCHANGING 是同步发送消息、不进消息队列，全局 QAbstractNativeEventFilter
// 看不到它——此前把守卫放全局过滤器的多轮修复全部无效，即因如此。
// 规则（仅常态 !s_desktopActive 生效；topmost 状态机持有 z 序时一律放行）：
// · Dock（全屏底层）：唯一合法 z 位=最底。任何非 HWND_BOTTOM 的 z 变更一律改写为
// HWND_BOTTOM（与 m_bottomTimer 压底语义一致，顺带自愈历史残留的错位）。
// · 收纳盒/助手：合法区间=Dock 之上、所有程序之下。冻结「提层」——
// ①HWND_TOP/TOPMOST/NOTOPMOST 顶层提层；
// ②插入参照是「外部真实窗口」foreignIa——点击 Dock 时系统对整组窗口的重排常用
// 外部程序 HWND 作参照，2026-09-10 19:50 实测仅认三个特殊值会漏判导致一闪而过。
// 命中任一即置 SWP_NOZORDER 冻结（保持当前 band 内位置）；下沉 / 组内互指放行。
// 返回 true 表示已改写（调用方照常 return false 走默认处理链，改写即生效）。
// 作者：谭征
bool DesktopMirrorWindow::clampBandZOrder(HWND hwnd, WINDOWPOS* wp) {
#ifdef Q_OS_WIN
    if (!wp || !hwnd || !IsWindow(hwnd)) return false;
    if (wp->flags & SWP_NOZORDER) return false;   // 本次不改 z 序，放行
    if (s_desktopActive) return false;            // show desktop 状态机持有 z 序，放行
    const HWND ia = wp->hwndInsertAfter;
    if (ia == nullptr) return false;              // 无插入参照，非 z 序意图，放行
    const bool isDock = (hwnd == s_dockHwnd);
    if (isDock) {
        if (ia == HWND_BOTTOM) return false;      // 压底：放行（m_bottomTimer/untopmost 路径）
        wp->hwndInsertAfter = HWND_BOTTOM;        // 任何提层/换位 → 一律钉回最底
    } else {
        const bool topRaise = (ia == HWND_TOP) || (ia == HWND_TOPMOST) || (ia == HWND_NOTOPMOST);
        // foreignIa：插入参照既不是 HWND_BOTTOM、也不是本程序三窗口之一（Dock/收纳盒/助手）
        // 的真实窗口 → 视为外部程序正在提层，必须冻结。Dock 已由上面 if 分支处理。
        const bool foreignIa = (ia != HWND_BOTTOM) && (ia != s_dockHwnd)
                               && !s_cloakTargets.contains(ia);
        if (!topRaise && !foreignIa) return false;// 下沉/组内互指：放行
        wp->flags |= SWP_NOZORDER;                // 冻结 z 序：保持当前（桌面 band 内）位置
    }
    wp->flags |= SWP_NOACTIVATE;                  // 不抢焦点
    debugLog(QStringLiteral("zguard hwnd=%1 ia=%2 dock=%3 flags=0x%4")
             .arg((quintptr)hwnd).arg((quintptr)ia).arg(isDock ? 1 : 0)
             .arg((unsigned)wp->flags, 0, 16));
    return true;
#else
    Q_UNUSED(hwnd) Q_UNUSED(wp)
    return false;
#endif
}

// 03:54 版"分层"机制（用户实测"可以，只是会闪烁"）：
// · Dock 单独压到 Z 序最底层（HWND_BOTTOM，壁纸层），其它程序浮起来会自然盖住 Dock，Dock 不遮挡任何人。
// · 收纳盒 / 桌面助手【完全不干预 Z 序】：靠 WS_EX_NOACTIVATE 让其它程序自动浮在它们之上（点击也不抢焦点），
// 这才是 360 桌面助手的"可交互但永远在最底层"语义。任何显式 SetWindowPos(h, dockHwnd) 都会触发 Windows
// "把 h 视作要提层"的副作用，导致收纳盒/助手被推过 WorkBuddy 之上 → 用户看到"三窗口遮挡其他程序"。
// · 启动顺序 = 收纳盒/助手/Dock 都先 show()，Windows 给它们默认的 Z 序；用户启动其它程序后，那些程序按
// "新窗口默认 topmost" 排在最前，自然盖在收纳盒/助手之上 —— 这正是用户认可的"不遮挡"行为。
// show desktop 激活期短路：topmost 状态机正在持有三窗口，强制压底会把它们从免疫态打回。
// 作者：谭征
void DesktopMirrorWindow::parkAllTargetsAtBottom() {
#ifdef Q_OS_WIN
    debugLog(QStringLiteral("parkAllTargetsAtBottom: enter desktopActive=%1 targets=%2 dock=%3")
             .arg(s_desktopActive ? 1 : 0).arg(s_cloakTargets.size()).arg((quintptr)s_dockHwnd));
    if (s_desktopActive) return;   // show desktop 激活期：m_bottomTimer 兜底不压底
    // 只压 Dock 到 HWND_BOTTOM（最底/壁纸层），其余窗口一律不干预。
    if (s_dockHwnd && IsWindow(s_dockHwnd)) {
        // B2：保留原“无条件压底”语义（只把周期从 1000ms 放宽到 3000ms）。
        // 曾加的「!IsWindowVisible 就跳过」已撤回：本函数同时是 exitDesktop()/untopmostAllTargets()
        // 的 z 序复位通道，若此刻 Dock 恰好处于“被外壳 SW_HIDE 但尚未被 restoreAllTargets 复活”
        // 的窗口期，跳过会让 Dock 复活后停在其它程序之上（历史“遮挡其它程序”缺陷）。
        // 一次 SetWindowPos 相对 3s 周期可忽略，可靠性优先。
        SetWindowPos(s_dockHwnd, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
#endif
}

// 把 h（弹框 / 搜索窗等独立顶层窗口）钉在【全部 band 窗口之上】、【其它正常程序之下】。
// 2026-09-16 重写：旧实现是「遍历 s_cloakTargets，逐个 SetWindowPos(h, band)」。
// 只有当集合里**恰好只有一个** band 窗口时才等价于「插到最上面那个之上」；band 一旦多于
// 一个（桌面助手 + 收纳盒 + 若干抽屉），最终位置就取决于 QHash 的迭代顺序：设 z 序自上而下
// 为 [助手, 盒A, 盒B]，若最后插入的参照是 盒B，h 会被插到 盒B 之上、却仍落在 盒A/助手 **之下**
// —— 用户看到的就是「弹框/快速搜索窗被收纳盒或助手盖住」（层级问题，时好时坏取决于哈希序）。
// 改为：先在 band 集合里求出**最上层的那一个**作唯一参照，再插一次，与数量/顺序彻底无关。
// 作者：谭征
void DesktopMirrorWindow::raiseAboveBandWindows(HWND h) {
#ifdef Q_OS_WIN
    if (!h || !IsWindow(h)) return;
    // ① 求最上层 band 窗口。判据：从候选 band 沿 z 序向下（GW_HWNDNEXT）能走到当前 anchor，
    // 说明候选在 anchor 之上，于是候选成为新 anchor。
    HWND anchor = nullptr;
    for (auto it = s_cloakTargets.constBegin(); it != s_cloakTargets.constEnd(); ++it) {
        const HWND band = it.key();
        if (!band || band == s_dockHwnd || !IsWindow(band)) continue;   // Dock 常驻最底，不作参照
        if (!anchor) { anchor = band; continue; }
        for (HWND w = GetWindow(band, GW_HWNDNEXT); w; w = GetWindow(w, GW_HWNDNEXT)) {
            if (w == anchor) { anchor = band; break; }
        }
    }
    if (!anchor) return;                       // 当前没有任何 band 窗口：不干预 z 序
    // ② 已经正好贴在 anchor 之上 → 直接返回。本函数被「错峰多轮」反复调用，幂等即零副作用，
    // 也避免无谓的 z 序写入造成闪烁。
    if (GetWindow(h, GW_HWNDPREV) == anchor) return;
    // 不使用 HWND_TOP/TOPMOST，故 h 仍低于其它正常程序；
    // 各 band 窗口自身的 nativeEvent 守卫（clampBandZOrder）只冻结自身、不干预 h。
    SetWindowPos(h, anchor, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#endif
}

// 给**长期存活**的窗口（快速搜索窗）做「层级看护」用：仅当 h 确实被某个 band 窗口压住时，
// 才把它重新插回 band 之上；已经在全部 band 之上（或当前没有 band 窗口）则**什么都不做**。
// 为什么不直接在定时器里无脑调 raiseAboveBandWindows：那样每次都写一遍 z 序，虽然幂等，
// 但会与「用户刚把别的程序切到前台」这类正常层级变化打架（把窗口从顶部拽回 band 上方）。
// 「只在被压住时才救」既保证窗口永不沉底，又完全不干扰正常 z 序。
// 作者：谭征
bool DesktopMirrorWindow::ensureLayerAboveBandWindows(HWND h) {
#ifdef Q_OS_WIN
    if (!h || !IsWindow(h)) return false;
    // 被压住 = 从 h 向上（GW_HWNDPREV）枚举，途中出现任一**可见的**非 Dock band 窗口。
    // 只认可见窗口：隐藏/已收起的 band 窗口虽然仍在 z 序表里占位，但视觉上盖不住 h，
    // 若把它们也算作「压在上面」，看护会误判并把 h 往下拽、反而可能沉到可见程序之后。
    bool buried = false;
    for (auto it = s_cloakTargets.constBegin(); it != s_cloakTargets.constEnd() && !buried; ++it) {
        const HWND band = it.key();
        if (!band || band == s_dockHwnd || band == h || !IsWindow(band)) continue;
        if (!IsWindowVisible(band)) continue;
        for (HWND w = GetWindow(h, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
            if (w == band) { buried = true; break; }
        }
    }
    if (!buried) return false;
    raiseAboveBandWindows(h);
    return true;
#else
    Q_UNUSED(h)
    return false;
#endif
}

// B8：全局范围事件钩子的第一道闸门 —— 只认本程序登记的受保护/挂件窗口。
// 作者：谭征
bool DesktopMirrorWindow::isProtectedWindow(HWND hwnd) {
    return hwnd && s_cloakTargets.contains(hwnd);
}

// 若 hwnd 属于受保护窗口则立即执行恢复（内部有防重入）。
// 作者：谭征
void DesktopMirrorWindow::handleShellHideEvent(HWND hwnd) {
    if (!s_cloakTargets.contains(hwnd)) return;
    // 显示桌面按钮路径：外壳对窗口逐个 SW_HIDE（前台不一定变 WorkerW，fg=desktop 不触发）。
    // 受保护窗口被外壳隐藏 → 进入 topmost 常驻（topmost 免疫后续 SW_HIDE）。统一走 enterDesktop。
    enterDesktop();
}

// handle外壳显示事件
// 作者：谭征
void DesktopMirrorWindow::handleShellShowEvent(HWND hwnd) {
    if (!s_cloakTargets.contains(hwnd)) return;
    // 退出显示桌面：外壳重新 SW_SHOW 我们窗口。冷静期 500ms 内忽略——那是置顶 SWP_SHOWWINDOW
    // 自己触发的 show 事件，不是真正退出。仅当前台已不是桌面时才撤顶，避免显示桌面持续期误撤。
    if ((GetTickCount64() - s_topmostSince) < 500) return;
    if (!isForegroundDesktop()) {
        exitDesktop();
    }
}

// 注销cloak目标
// 作者：谭征
void DesktopMirrorWindow::unregisterCloakTarget(QWidget* w) {
    if (!w) return;
    HWND h = (HWND)w->winId();
    s_cloakTargets.remove(h);
    DesktopMirrorWindow::debugLog(QStringLiteral("unregisterCloak hwnd=%1").arg((quintptr)h));

    // A6：最后一个受保护目标离开后，这组事件钩子已无任何意义，此前却**从不卸载**。
    // 四个 SetWinEventHook 都是 WINEVENT_OUTOFCONTEXT 的**全局**回调 —— 系统里任意窗口的
    // 显示/隐藏/最小化事件都会同步回调进本进程 GUI 线程（虽有 isProtectedWindow 早退，
    // 但每次仍要跨进程投递 + 查表）。注册侧有 `if (!s_cloakHook)` 去重，卸载侧缺失，
    // 属句柄/回调泄漏。此处与注册严格对称：清空即卸载，并把句柄置空以便下次重新注册。
    if (s_cloakTargets.isEmpty()) {
        if (s_cloakHook)    { UnhookWinEvent(s_cloakHook);    s_cloakHook = nullptr; }
        if (s_hideHook)     { UnhookWinEvent(s_hideHook);     s_hideHook = nullptr; }
        if (s_minimizeHook) { UnhookWinEvent(s_minimizeHook); s_minimizeHook = nullptr; }
        if (s_showHook)     { UnhookWinEvent(s_showHook);     s_showHook = nullptr; }
    }
}

// T5 事件回调。必须是文件级自由函数 + __stdcall(CALLBACK)：静态成员函数在 MSVC 下按 __cdecl
// 处理，与 WINEVENTPROC 的调用约定不匹配（C2664）。
// WINEVENT_OUTOFCONTEXT 下回调在安装钩子的线程（GUI 线程）消息循环中派发，可直接调显示 API；
// 但恢复动作自身会再触发 HIDE/CLOAKED 事件，故用 s_restoring 防重入。
static void CALLBACK guardEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                    LONG idObject, LONG idChild, DWORD, DWORD) {
    // B8：第一道闸门 —— 只处理本程序登记的受保护/挂件窗口。
    // 本组 SetWinEventHook 是**全局范围**（hwndEvent = nullptr）安装的，系统里任何窗口的
    // 显示/隐藏/最小化都会回调到这里，其中 EVENT_OBJECT_SHOW 尤其高频（对话框、菜单、tooltip、
    // 系统弹窗……）。原实现在开头就构造诊断字符串并逐分支判断，白白消耗 GUI 线程时间；
    // 而现在一次 O(1) 的 HWND 查表就能把绝大多数事件挡在门外（旧诊断块已一并删除：
    // dumpDebug 本身是空实现，参数却照旧构造，属纯浪费）。
    if (!DesktopMirrorWindow::isProtectedWindow(hwnd)) return;

    // 前台切换不再自动 enter——fg=WorkerW 会被桌面空闲歧义触发，把三窗口压在所有
    // 图标/普通程序之上（用户报"其他程序打开、点 dock、三窗口遮挡其他程序"）。
    // 进入 topmost 改由 guardEventProc 中 EVENT_OBJECT_HIDE/EVENT_OBJECT_CLOAKED 触发：
    // 只有 Explorer 真在 SW_HIDE 我们自身窗口（Win+D/显示桌面）才进 topmost 免疫态。
    if (event == EVENT_SYSTEM_FOREGROUND) {
        return;
    }

    // 窗口重新显示：外壳结束“显示桌面”重新 SW_SHOW 我们窗口 → 撤 topmost 恢复常态。
    if (event == EVENT_OBJECT_SHOW) {
        if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF) {
            DesktopMirrorWindow::handleShellShowEvent(hwnd);
        }
        return;
    }

    if (event != EVENT_OBJECT_CLOAKED && event != EVENT_OBJECT_HIDE
        && event != EVENT_SYSTEM_MINIMIZESTART) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;  // 过滤子控件事件
    // 受保护窗口被外壳隐藏 → 进入 topmost 常驻（消除逐窗口隐藏事件的竞态）。
    DesktopMirrorWindow::handleShellHideEvent(hwnd);
}

// 只在窗口处于“应可见”状态时才复活；常隐的 MainWindow 协调窗口从不注册。
// 作者：谭征
void DesktopMirrorWindow::registerCloakTarget(QWidget* w) {
    if (!w) return;
    HWND h = (HWND)w->winId();
    if (!h || !IsWindow(h)) return;
    s_cloakTargets.insert(h, QPointer<QWidget>(w));

    // T5：装三个事件钩子（仅限本进程，避免全局开销）
    if (!s_cloakHook) {
        s_cloakHook = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_CLOAKED,
                                      nullptr, guardEventProc,
                                      GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    }
    if (!s_hideHook) {
        s_hideHook = SetWinEventHook(EVENT_OBJECT_HIDE, EVENT_OBJECT_HIDE,
                                     nullptr, guardEventProc,
                                     GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    }
    if (!s_minimizeHook) {
        s_minimizeHook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART,
                                         nullptr, guardEventProc,
                                         GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    }
    if (!s_showHook) {
        s_showHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
                                     nullptr, guardEventProc,
                                     GetCurrentProcessId(), 0, WINEVENT_OUTOFCONTEXT);
    }
    // 诊断：打印各钩子句柄（失败为 NULL）+ 该窗口当前 EX 样式（确认是否被设为 toolwindow）。
    DesktopMirrorWindow::debugLog(
        QStringLiteral("registerCloak hwnd=%1 cloakHook=%2 hideHook=%3 minHook=%4 showHook=%5 ex=0x%6")
        .arg((quintptr)h)
        .arg((quintptr)s_cloakHook)
        .arg((quintptr)s_hideHook)
        .arg((quintptr)s_minimizeHook)
        .arg((quintptr)s_showHook)
        .arg((unsigned long)GetWindowLongPtrW(h, GWL_EXSTYLE), 0, 16));

    // TP-1（A 路线持续再断言）：轮询驱动 topmost 状态机的进出边沿 + Win+M 兜底。
    // 间隔演进：100ms → 10ms（为尽快撤顶，避免 topmost 遮挡普通程序一闪而过）→ 50ms →（现）150ms。
    // B1：本 tick 里最贵的是对每个受保护窗口做跨进程 DwmGetWindowAttribute
    // （要走 DWM 代理，单次亚毫秒~毫秒级）。50ms(20Hz)×3 窗口 ≈ 每秒 60 次同步调用，
    // 持续挤压 GUI 线程 —— 而 GUI 线程同时承载着系统级低级鼠标钩子的同步回调，
    // 于是「轮询空转」会直接表现为「全局鼠标发涩」。撤顶/恢复都是「前台窗口切换」的边沿
    // 动作，150ms 的检测延迟肉眼不可辨；配合 restoreAllTargets 里“常态跳过 DWM 查询”，
    // 常态空转开销降到原来的 1/10 以下。
    if (!s_pollTimer) {
        s_pollTimer = new QTimer(QCoreApplication::instance());
        s_pollTimer->setInterval(150);
        QObject::connect(s_pollTimer, &QTimer::timeout, []() {
            DesktopMirrorWindow::restoreAllTargets(isForegroundDesktop());
        });
        s_pollTimer->start();
    }
}

// 并兜底 Win+M 最小化还原与 show desktop 期间窗口被误藏后的重新显示。
// 作者：谭征
void DesktopMirrorWindow::restoreAllTargets(bool force) {
    if (s_restoring) return;          // 防重入：恢复动作自身会再触发 HIDE/CLOAKED 事件
    s_restoring = true;
    Q_UNUSED(force)                   // 状态机由前台判定驱动，force 参数保留兼容

    // —— Rainmeter 式 topmost 状态机（事件触发：guardEventProc EVENT_OBJECT_HIDE/CLOAKED 已 enter；
    // 轮询仅负责"fgRealApp 稳定后撤顶"和兜底再断言 show 事件）——
    // fgDesktop 不再作为 enter 条件（避免"桌面空闲"歧义——桌面空闲无程序时 fg=WorkerW，
    // 旧逻辑会进 topmost 把三窗口压在所有图标和其他程序之上）。
    // show desktop 期间三窗口被 explorer SW_HIDE/CLOAK → 事件钩子触发
    // guardEventProc -> handleShellHideEvent -> enterDesktop()，已保证免疫。
    const bool fgRealApp = isForegroundRealApp();
    if (fgRealApp && s_desktopActive) {
        // 前台已离开桌面：立即撤顶（不再做 3 次×100ms 防抖，避免 topmost 遮挡普通程序一闪而过）。
        // 冷却阈值 500ms→10ms、防抖计数 3→1：切回桌面后 10ms 内即撤顶。
        if ((GetTickCount64() - s_topmostSince) >= 10) {
            ++s_exitStable;
            if (s_exitStable >= 1) {
                exitDesktop();
            }
        }
    } else {
        // 既不是 fgRealApp 也不是 desktopActive：保持当前状态（防抖计数归零）。
        s_exitStable = 0;
    }

    // B2：原「每 50 拍（≈7.5s）打印全部受保护窗口真实状态」的诊断块已删除。
    // 它每次都要对每个受保护窗口做**跨进程** DwmGetWindowAttribute（要走 DWM 代理，单次
    // 亚毫秒~毫秒级）+ 构造一个长 QString，而它的唯一出口 debugLog→dumpDebug 现在是**空函数**
    // （dock_debug.log 早已停用）—— 纯消耗。需要时把这段搬回来即可（历史版本可查）。
    (void)s_pollCount;

    // 兜底：Win+M（最小化）topmost 不免疫，需单独还原；show desktop 期间窗口被误藏则重新显示。
    // B1 回归修复（2026-09-21）：此前为省开销改成“常态每 5 拍才做一次 DWM 兜底探测”，
    // 但那是**错的**：DWM 遮蔽（DWMWA_CLOAKED）恰恰不改变 WS_VISIBLE —— 被遮蔽的窗口
    // IsWindowVisible() 仍返回 TRUE、IsIconic() 也仍为 FALSE，于是「跳过探测」的那 4 拍里
    // cloaked 恒为 0 → `hidden || cloaked` 判假 → 该窗口一直不被重新点亮；
    // Win+D / 显示桌面退出后表现为三窗口（尤其收纳盒/助手）“不见了”。
    // 正确做法：每拍都探测（探测本身只有 3 个窗口，150ms 一拍 ≈ 6.7 次/秒，
    // 相比改动前的 50ms×3=60 次/秒已降 9 倍，节流收益主要来自间隔放大而非跳过探测）。
    for (auto it = s_cloakTargets.begin(); it != s_cloakTargets.end(); ++it) {
        HWND h = it.key();
        if (!h || !IsWindow(h)) continue;

        const bool iconic = IsIconic(h);
        const bool hidden = !IsWindowVisible(h);
        DWORD cloaked = 0;
        HRESULT hr = DwmGetWindowAttribute(h, kDwmwaCloaked, &cloaked, sizeof(cloaked));
        if (FAILED(hr)) cloaked = 0;

        if (s_desktopActive) {
            // show desktop 期间：topmost 已保证可见；若仍检测到被隐藏（竞态/外壳强重隐），
            // 用 SWP_SHOWWINDOW 重新点亮（不激活、不抢焦点），保持 topmost。
            if (hidden || cloaked) {
                if (cloaked) { BOOL off = FALSE; DwmSetWindowAttribute(h, kDwmwaCloak, &off, sizeof(off)); }
                SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                debugLog(QStringLiteral("topmostReassert hwnd=%1 hidden=%2 cloaked=%3 hr=%4")
                         .arg((quintptr)h).arg(hidden).arg(cloaked).arg((int)hr));
                DiagTrace::log(QStringLiteral("[restore] show-desktop 期间重新点亮 hwnd=%1 hidden=%2 cloaked=%3")
                                   .arg((quintptr)h).arg(hidden).arg(cloaked));
            }
            if (iconic) {
                ShowWindow(h, SW_SHOWNOACTIVATE);   // Win+M 兜底：还原且不抢焦点
            }
        } else {
            // 常态：除 Win+M 最小化（iconic）外，还要处理被外壳 SW_HIDE 的窗口——
            // 用户在 show desktop 期间提前切到其它程序，状态机退出 → 三窗口从 topmost 免疫态
            // 变回普通窗口，外壳的 SW_HIDE 状态保留 → 永远不可见。
            // 必须在常态下用 ShowWindow(SW_SHOWNORMAL) 重新点亮（不激活、不抢焦点），
            // 保证三窗口始终可见。这是「全部隐藏不复活」修复的关键。
            if (iconic) {
                ShowWindow(h, SW_SHOWNOACTIVATE);
                debugLog(QStringLiteral("restoreWinM hwnd=%1").arg((quintptr)h));
                // 诊断：debugLog→dumpDebug 已整体停用，故同时写一条可开关的 DiagTrace。
                // 这里只在**真的发生了恢复**时才记录（低频、且不在低级钩子回调路径上），
                // 排查“Win+D/Win+M 后三窗口不可见”时在 INI 里设 [Diagnostics] traceEnabled=true 打开诊断即可看到。
                DiagTrace::log(QStringLiteral("[restore] WinM 最小化还原 hwnd=%1").arg((quintptr)h));
            }
            if (hidden || cloaked) {
                if (cloaked) { BOOL off = FALSE; DwmSetWindowAttribute(h, kDwmwaCloak, &off, sizeof(off)); }
                ShowWindow(h, SW_SHOWNOACTIVATE);   // 不激活、不抢前台、只点亮
                debugLog(QStringLiteral("restoreHidden hwnd=%1 hidden=%2 cloaked=%3").arg((quintptr)h).arg(hidden).arg(cloaked));
                DiagTrace::log(QStringLiteral("[restore] 外壳隐藏/遮蔽复原 hwnd=%1 hidden=%2 cloaked=%3 desktopActive=%4")
                                   .arg((quintptr)h).arg(hidden).arg(cloaked).arg(s_desktopActive ? 1 : 0));
            }
        }
    }

    s_restoring = false;
}
#endif

// 刷新
// 作者：谭征
void DesktopMirrorWindow::refresh() {
    CrashTrace::mark("refresh:enter");
    // 内联重命名进行中：延后本次刷新。刷新会重建全部 DockIconButton 并销毁正在编辑的项，
    // 导致"新建即重命名"的编辑框一闪即逝。改为记下待刷新标记，编辑结束后由
    // onInlineEditFinished 补做一次 refresh，既保住编辑框又保证最终与真实桌面同步。
    if (m_editingBtn || m_menuOpen) { m_refreshPending = true; return; }
    readEntries();
    // 清理已不存在的自动重命名标记：项被删除/改名后，避免同路径残留导致同名新建项不再弹框。
    if (!m_autoRenamedPaths.isEmpty()) {
        QSet<QString> cur;
        cur.reserve(m_entries.size());
        for (const Entry& e : m_entries)
            cur.insert(QDir::fromNativeSeparators(e.shellPath).toLower());
        m_autoRenamedPaths.intersect(cur);
    }
    dumpDebug(QStringLiteral("refresh:afterRead entries=%1").arg(m_entries.size()));
#ifdef Q_OS_WIN
    // DesktopOrganize 模式下无需等待 ListView；NativeListView 模式下若列表未就绪则重试。
    if (m_sourceMode == SourceMode::NativeListView && m_entries.isEmpty() && !bestListView()) {
        QTimer::singleShot(500, this, &DesktopMirrorWindow::refresh);
        return;
    }
#endif

    // 自激循环兜底（2026-09-13 定论）
    // 之前的 registerChangeNotify() 用了 fRecursive=TRUE，trace.log 又位于用户桌面根目录的子目录下，
    // 两者合在一起形成"写入日志 → SHCNE_UPDATEITEM 回弹 → refresh 重建 → 重建出的按钮析构再写日志 → …"
    // 的自激循环：典型症状就是"点中即失选"（每 500ms 重建销毁一次）。
    // 现在 fRecursive=FALSE 已经切断回弹路径，但**仍要在这里做一道兜底**：
    // 任何外部来源（shell 通知、防抖定时器、其它路径）若在条目未变化时反复调用 refresh，
    // 我们必须避免 layoutButtons() 销毁全部 DockIconButton 并清空 m_selection，否则
    // 用户感知到的现象仍是"点中即失选"。
    // 指纹涵盖：displayName + shellPath + special + hasPos + screenPos + lvIndex + iconSizePx
    // —— 这 7 项决定了一个图标按钮在 Dock 上的所有可见属性；不变就意味着重建结果完全相同，可跳。
    {
        QCryptographicHash h(QCryptographicHash::Sha1);
        for (const Entry& e : m_entries) {
            h.addData(e.displayName.toUtf8());
            h.addData("\x1f", 1);
            h.addData(e.shellPath.toUtf8());
            h.addData("\x1f", 1);
            h.addData(reinterpret_cast<const char*>(&e.special), sizeof(e.special));
            h.addData(reinterpret_cast<const char*>(&e.hasPos),   sizeof(e.hasPos));
            if (e.hasPos) {
                h.addData(reinterpret_cast<const char*>(&e.screenPos), sizeof(e.screenPos));
            }
            h.addData(reinterpret_cast<const char*>(&e.lvIndex),    sizeof(e.lvIndex));
            h.addData(reinterpret_cast<const char*>(&e.iconSizePx), sizeof(e.iconSizePx));
            h.addData("\x1e", 1);
        }
        const QByteArray fp = h.result();
        if (!m_entriesDigest.isEmpty() && fp == m_entriesDigest) {
            // 真实条目未变：不重建、不销毁、不清 m_selection，shell 通知再频繁也不影响选中。
            CrashTrace::mark("refresh:unchanged-skip");
            return;
        }
        m_entriesDigest = fp;
    }

    // 会话基线（只做一次）
    // 程序启动后**首次拿到有效桌面条目**时，把此刻已存在的一切登记为"本次会话之前就存在"。
    // 它们即便名字以「新建 / New 」开头，也不是用户在本次会话里新建的 —— 绝不允许自动进入重命名态，
    // 否则每次启动都会对桌面上遗留的「新建文本文档.txt」逐个弹出重命名框【用户 2026-09-22 反馈：
    // 启动即进入 F2 重命名态，且有几个这样的文件就要点几次才能全部消掉 —— 因为 applyAutoRename()
    // 一次只弹一个，每提交一个才弹下一个】。
    // 判据必须是"本次会话内新增"这一**事实**，而不是名字形状：名字启发对真实存在的「新建…」文件
    // 永远成立（"新建文本文档 (3).txt" 与刚建出来的完全同形），收紧正则也分不出来。
    // 位置特意放在上面 ListView 就绪判定之后：更早的空/半扫描会把基线记成残缺集合，等于没生效。
    if (!m_autoRenameArmed) {
        m_autoRenameArmed = true;
        for (const Entry& e : m_entries)
            m_autoRenamedPaths.insert(QDir::fromNativeSeparators(e.shellPath).toLower());
    }
    layoutButtons();
    dumpDebug(QStringLiteral("refresh:afterLayout buttons=%1").arg(m_buttons.size()));
    CrashTrace::mark("refresh:done");
    // 首次镜像就绪：界面已显示 + 数据已读取 + 图标已布局，发射一次通知 MainWindow 加载收纳盒。
    if (!m_firstReadyEmitted) {
        m_firstReadyEmitted = true;
        emit firstMirrorReady();
    }
}

// —— dock 复位：清除 dock 内图标 + 重新扫描加载 Windows 桌面图标 + 按真实位置重排 ——
// 当前为 NativeListView 模式，启动后原生桌面被 hideNativeDesktop() 隐藏，
// 隐藏态下 LVM_GETITEMPOSITION 会塌缩到 (0,0)，无法读到真实位置。
// 因此复位必须先 restoreNativeDesktop() 让原生桌面可见、重拍权威快照，再 hideNativeDesktop() 恢复接管。
// 作者：谭征
void DesktopMirrorWindow::resetDock() {
#ifdef Q_OS_WIN
    // 0) 复位是用户主动操作：先结束可能正在进行的内联重命名，否则下面的 layoutButtons
    // 会被“编辑中延后重排”的守卫拦下，导致 dock 清空后不重建。
    if (m_editingBtn) m_editingBtn->commitRename();

    // 1) 立即清空 dock 内当前图标，给出“已复位”的即时视觉反馈
    for (DockIconButton* b : m_buttons) b->hide();

    // 2) 还原原生桌面使其可见，才能读到真实坐标
    restoreNativeDesktop();

    // 3) 给窗口管理器极短延时把桌面显示出来（SW_SHOW 同步切换窗口状态，ListView 内部坐标随之恢复），
    // 随后重拍快照 + 重建图标集合 + 按 Windows 最新位置重排 + 重新隐藏原生桌面 + 复位 z 序。
    QTimer::singleShot(60, this, [this]() {
        m_snapshotEntries.clear();     // 清空旧快照，强制 takeNativeSnapshot 全量重读
        m_entries.clear();
        m_customDockPos.clear();       // 清空 Dock 自定义位置覆盖：复位 = 重新镜像真实桌面
        saveCustomDockPos();
        takeNativeSnapshot();           // 重新跨进程扫描 Windows 桌面图标 + 每项真实位置
        readEntries();                 // 以最新快照重建 m_entries（集合与坐标与桌面一致）
        // 复位后网格对齐：把所有图标吸附到各自位置最近的网格格子。
        // 真实桌面图标若曾被手动摆到半格位置，复位镜像会把“不齐”一并带进 Dock；
        // 这里统一吸附到真实桌面网格（与拖动结束 / 盒→Dock 落下同一套 snapToGridLogical）。
        // 同时同步快照 + 自定义覆盖并落盘，保证后续刷新与程序重启都停留在吸附后的位置。
        if (m_sourceMode == SourceMode::NativeListView) {
            for (Entry& e : m_entries) {
                if (!e.hasPos) continue;
                const QPoint logical(qRound(e.screenPos.x() / qMax(0.0001, m_dpiFactor)),
                                     qRound(e.screenPos.y() / qMax(0.0001, m_dpiFactor)));
                const QPoint snapped = snapToGridLogical(logical);
                e.screenPos = QPoint(qRound(snapped.x() * m_dpiFactor),
                                     qRound(snapped.y() * m_dpiFactor));
                updateSnapshotPosition(e.shellPath, e.screenPos);
                const QString k = QDir::fromNativeSeparators(e.shellPath).toLower();
                if (!k.isEmpty()) m_customDockPos[k] = e.screenPos;
            }
            saveCustomDockPos();
        }
        layoutButtons();               // 清空 m_buttons 并逐一按（吸附后的）最新位置重排 dock 图标
        hideNativeDesktop();           // 重新隐藏原生桌面，dock 恢复接管显示
        parkAllTargetsAtBottom();      // 复位 z 序：Dock 压回 HWND_BOTTOM
        dumpDebug(QStringLiteral("resetDock: done buttons=%1").arg(m_buttons.size()));
    });
#else
    refresh();   // 非 Windows 平台无原生桌面概念，退化为普通刷新
#endif
}

// —— COMCTL32 跨进程 HIMAGELIST 安全访问（SEH 包装 + 熔断器）——
// 根因：LVM_GETIMAGELIST 跨进程返回的 HIMAGELIST 是 dub 句柄，只在原生桌面 ListView 仍存活
// 且状态一致时有效；本程序启动后 hideNativeDesktop()、或 explorer 重建 ListView 都会让句柄失效，
// COMCTL32 内部直接踩空 → 访问违例（被上层 SEH 消化＝偶尔卡一下；时机不巧＝进程级致命崩溃，
// 正是「运行一会儿自己关闭」的真凶）。
// ① SEH 包装：异常时返回 false，调用方照原逻辑退注册表兜底（行为完全等价）。
// ② 熔断器：拿到句柄却读不出尺寸 = 失效确证（非"未就绪"，那种根本拿不到 hil）→ 本次进程内永久降级，
// 不再每分钟白跑异常分发、不刷满崩溃日志。
__declspec(noinline)
static bool safeImageListGetIconSize(HIMAGELIST himl, int* cx, int* cy) {
    __try {
        return ImageList_GetIconSize(himl, cx, cy) != FALSE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool s_lvIconSizeUsable = true;   // 一旦确证句柄不可用，本次进程内永久降级注册表兜底

// —— 查询 Windows 桌面真实图标像素尺寸（逻辑像素，与桌面一致）——
// 权威来源：桌面 SysListView32 当前 LVSIL_NORMAL 图像列表的图标尺寸（设备像素），
// 这是 Windows 桌面当前“查看→大/中/小图标”下 Explorer 真实渲染的图标大小。
// 再除以 Qt 主屏 DPR 得到逻辑像素，传给 DockIconButton::setIconSize 后即可与桌面逐项等大。
// 当 ListView 尚未就绪时，退到注册表 HKCU\...\WindowMetrics\Shell Icon Size。
// 作者：谭征
int DesktopMirrorWindow::queryDesktopIconSize() {
#ifdef Q_OS_WIN
    HWND lv = s_lvIconSizeUsable ? findBestDesktopListView() : nullptr;
    if (lv) {
        HIMAGELIST hil = (HIMAGELIST)SendMessageW(lv, LVM_GETIMAGELIST, LVSIL_NORMAL, 0);
        if (hil) {
            int cx = 0, cy = 0;
            if (safeImageListGetIconSize(hil, &cx, &cy) && cx > 0) {
                // 折算为逻辑像素，避免 Qt 在 HiDPI 下再次缩放导致 Dock 图标比桌面大/小
                QScreen* ps = QApplication::primaryScreen();
                const qreal dpr = ps ? ps->devicePixelRatio() : 1.0;
                const int logical = qMax(16, qRound(cx / qMax(qreal(1.0), dpr)));
                dumpDebug(QStringLiteral("queryDesktopIconSize: LV device=%1 dpr=%2 logical=%3")
                              .arg(cx).arg(dpr).arg(logical));
                return logical;
            } else {
                // 拿到句柄却读不出尺寸 = 跨进程 dub 句柄失效的确证（非"未就绪"）→ 本次进程内永久降级
                s_lvIconSizeUsable = false;
                dumpDebug(QStringLiteral("queryDesktopIconSize: LV image list unusable, circuit-break to registry"));
            }
        }
    }

    // 兜底：注册表“Shell Icon Size”
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Control Panel\\Desktop\\WindowMetrics", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        WCHAR buf[64] = {0};
        DWORD len = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, L"Shell Icon Size", nullptr, &type,
                             reinterpret_cast<LPBYTE>(buf), &len) == ERROR_SUCCESS) {
            const int v = QString::fromWCharArray(buf).trimmed().toInt();
            RegCloseKey(hKey);
            if (v >= 16 && v <= 512) {
                dumpDebug(QStringLiteral("queryDesktopIconSize: registry ShellIconSize=%1").arg(v));
                return v;
            }
        }
        RegCloseKey(hKey);
    }
#endif
    return 48;  // 默认中等图标尺寸
}

// —— 数据源：桌面整理 ——
// 作者：谭征
void DesktopMirrorWindow::readEntriesFromOrganize() {
    m_entries.clear();

    // DPI 因子：物理屏幕宽 / 逻辑屏幕宽
    QScreen* ps = QApplication::primaryScreen();
    const int screenPhysW = GetSystemMetrics(SM_CXSCREEN);
    m_dpiFactor = (ps && ps->geometry().width() > 0)
                      ? (double)screenPhysW / ps->geometry().width() : 1.0;
    if (m_dpiFactor <= 0.0) m_dpiFactor = 1.0;

    // 单元格宽度由桌面真实图标尺寸决定：图标与桌面等大，单元格留出标签空间。
    m_desktopIconSize = queryDesktopIconSize();
    m_cellW = qMax(m_desktopIconSize + 40, 64);

    // 一次性读入全部分类映射
    const QHash<QString, QString> catMap = CategoryStore::readMap();

    DesktopScanner scanner;
    QVector<DesktopItem> raw = scanner.scan();
    for (const DesktopItem& it : raw) {
        Entry e;
        e.displayName = it.displayName;
        e.shellPath = it.sourcePath;
        e.imageIndex = it.systemImageIndex;
        e.special = it.isSpecial;
        e.iconSizePx = m_desktopIconSize;   // 逐项记录桌面图标尺寸
        // 记录当前分类，便于调试；显示时不按分类分组，全部平铺
        m_entries.append(e);
    }

    // 特殊命名空间项（我的电脑/回收站/网络）始终显示在 Dock 上
    QVector<DesktopItem> special = DesktopScanner::scanSpecialItems();
    for (const DesktopItem& it : special) {
        Entry e;
        e.displayName = it.displayName;
        e.shellPath = it.sourcePath;   // "::{CLSID}"，DockIconButton::launch 会识别并调用 explorer
        e.imageIndex = it.systemImageIndex;
        e.special = true;
        e.iconSizePx = m_desktopIconSize;   // 逐项记录桌面图标尺寸
        m_entries.append(e);
    }

    // 去重：同一 sourcePath 不重复显示
    QSet<QString> seen;
    QVector<Entry> dedup;
    for (const Entry& e : m_entries) {
        if (seen.contains(e.shellPath)) continue;
        seen.insert(e.shellPath);
        dedup.append(e);
    }
    m_entries = dedup;
}

// —— 快照真实桌面图标位置（ListView 仍可见时调用最可靠） ——
// 作者：谭征
bool DesktopMirrorWindow::takeNativeSnapshot() {
#ifdef Q_OS_WIN
    HWND hLV = findBestDesktopListView();
    if (!hLV) {
        dumpDebug(QStringLiteral("takeNativeSnapshot: no ListView"));
        return false;
    }
    m_hLV = hLV;

    QScreen* ps = QApplication::primaryScreen();
    const int screenPhysW = GetSystemMetrics(SM_CXSCREEN);
    m_dpiFactor = (ps && ps->geometry().width() > 0)
                      ? (double)screenPhysW / ps->geometry().width() : 1.0;
    if (m_dpiFactor <= 0.0) m_dpiFactor = 1.0;

    RECT rc; GetWindowRect(hLV, &rc);
    m_lvOriginX = rc.left;
    m_lvOriginY = rc.top;

    DWORD pid = 0;
    GetWindowThreadProcessId(hLV, &pid);
    HANDLE hProc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE,
                               FALSE, pid);
    if (!hProc) {
        dumpDebug(QStringLiteral("takeNativeSnapshot: OpenProcess failed"));
        return false;
    }

    // 建立 显示名 -> 绝对路径 映射（COM 枚举桌面命名空间，与可见性无关），
    // 用于为每个镜像图标解析出可启动 / 可弹原生菜单的 shellPath。
    buildNameMap();

    const int count = (int)SendMessageW(hLV, LVM_GETITEMCOUNT, 0, 0);
    const DWORD sp = (DWORD)SendMessageW(hLV, LVM_GETITEMSPACING, FALSE, 0);
    const int spcxDevice = (int)(short)LOWORD(sp);
    const int spcyDevice = (int)(short)HIWORD(sp);
    m_desktopIconSize = queryDesktopIconSize();
    // 单元格宽度：与 Windows 桌面图标的真实水平间距（按 Qt DPR 折算逻辑像素）保持一致。
    // 这样 Dock 的图标排布密度和 360 桌面助手 / Windows 桌面完全对齐。
    QScreen* ps2 = QApplication::primaryScreen();
    const qreal dpr = ps2 ? ps2->devicePixelRatio() : 1.0;
    const int spcxLogical = qMax(64, qRound(spcxDevice / qMax(qreal(1.0), dpr)));
    // 同时保证单元格能完整容纳图标 + 两行标签 + 边距
    m_cellW = qMax(spcxLogical, m_desktopIconSize + 40);
    m_cellW = qBound(64, m_cellW, 360);
    // 真实桌面图标网格步长（逻辑像素）：用于 Dock 内拖动结束吸附对齐网格。
    // 取原始真实间距（未经 m_cellW 的“保证容纳”放大），确保吸附后的图标与真实桌面行列一致。
    if (spcxDevice > 0) m_gridStepX = qMax(48.0, (double)qRound(spcxDevice / qMax(qreal(1.0), dpr)));
    if (spcyDevice > 0) m_gridStepY = qMax(48.0, (double)qRound(spcyDevice / qMax(qreal(1.0), dpr)));

    // 直接从当前【可见】的 ListView 读取“显示名 + 图标索引 + 屏幕坐标”，
    // 一次性构成 Dock 的权威图标集合，保证集合与位置都与 Windows 桌面像素级一致。
    m_snapshotEntries.clear();
    int got = 0;
    for (int i = 0; i < count; ++i) {
        Entry e = readItemRemote(hLV, hProc, i);
        if (e.displayName.isEmpty()) continue;
        e.shellPath = resolveShellPath(e.displayName);
        e.special = e.shellPath.startsWith(QLatin1String("::"));
        e.iconSizePx = m_desktopIconSize;   // 逐项记录该桌面图标的真实像素尺寸
        m_snapshotEntries.append(e);
        ++got;
    }
    CloseHandle(hProc);

    m_snapshot.desktopIconSize = m_desktopIconSize;
    m_snapshot.cellW = m_cellW;
    m_snapshot.dpiFactor = m_dpiFactor;
    m_snapshot.lvOriginX = m_lvOriginX;
    m_snapshot.lvOriginY = m_lvOriginY;

    // 详细诊断：列出每个镜像项的显示名、ListView 图像索引、解析后的 shellPath、坐标。
    // 这能直接验证 Dock 读取的集合是否与 Windows 桌面逐项一致。
    for (int i = 0; i < qMin(got, 50); ++i) {
        const Entry& e = m_snapshotEntries.at(i);
        dumpDebug(QStringLiteral("snapshot[%1]: name=%2 idx=%3 pos=(%4,%5) sp=%6 special=%7")
                      .arg(i).arg(e.displayName).arg(e.imageIndex)
                      .arg(e.screenPos.x()).arg(e.screenPos.y())
                      .arg(e.shellPath).arg(e.special));
    }

    dumpDebug(QStringLiteral("takeNativeSnapshot: entries=%1 iconSize=%2 cellW=%3")
                  .arg(m_snapshotEntries.size()).arg(m_desktopIconSize).arg(m_cellW));
    return got > 0;
#else
    return false;
#endif
}

// —— 数据源：Windows 桌面 ListView（1:1 镜像，保留兼容） ——
// 作者：谭征
void DesktopMirrorWindow::readEntriesFromListView() {
#ifdef Q_OS_WIN
    m_entries.clear();

    // 刷新“显示名 -> shellPath”映射：原先只在 takeNativeSnapshot()（启动/复位）里构建，
    // 于是“启动后新建”的项查不到映射，只能靠文件名兜底——而快捷方式的 shell 显示名往往
    // 不等于文件名（Windows 建快捷方式时文件名为“<目标> - 快捷方式.lnk”，显示名却是“<目标>”），
    // 兜底失败 -> shellPath 无效 -> 该图标右键菜单/重命名静默无反应。每次刷新重建即可根治。
    buildNameMap();

    // 1. 若尚未拍下权威快照（多见于启动过早、Explorer 列表尚未就绪），立即补拍一次。
    if (m_snapshotEntries.isEmpty())
        takeNativeSnapshot();

    // 2. 以“原生桌面可见时拍下的快照”为唯一真相源：集合与坐标都与 Windows 桌面一致。
    QVector<Entry> base = m_snapshotEntries;

    // 3. 与原生 ListView 核对，仅用于发现“新增 / 删除”的图标；
    // 已存在图标的位置【绝不】被隐藏后的 ListView 读数覆盖
    // （隐藏后 LVM_GETITEMPOSITION 仍返回成功，但坐标会塌缩到 (0,0)，覆盖会让图标堆在左上角）。
    HWND hLV = findBestDesktopListView();
    if (hLV) {
        m_hLV = hLV;
        QScreen* ps = QApplication::primaryScreen();
        const int screenPhysW = GetSystemMetrics(SM_CXSCREEN);
        double curDpi = (ps && ps->geometry().width() > 0)
                            ? (double)screenPhysW / ps->geometry().width() : 1.0;
        if (curDpi <= 0.0) curDpi = 1.0;
        RECT rc; GetWindowRect(hLV, &rc);
        m_lvOriginX = rc.left;
        m_lvOriginY = rc.top;
        m_dpiFactor = curDpi;

        const int curIcon = queryDesktopIconSize();
        if (curIcon > 0) m_desktopIconSize = curIcon;
        const DWORD sp = (DWORD)SendMessageW(hLV, LVM_GETITEMSPACING, FALSE, 0);
        const int spcxDevice = (int)(short)LOWORD(sp);
        const int spcyDevice = (int)(short)HIWORD(sp);
        if (spcxDevice > 0) {
            QScreen* ps2 = QApplication::primaryScreen();
            const qreal dpr2 = ps2 ? ps2->devicePixelRatio() : 1.0;
            const int spcxLogical = qMax(64, qRound(spcxDevice / qMax(qreal(1.0), dpr2)));
            m_cellW = qMax(spcxLogical, m_desktopIconSize + 40);
            m_cellW = qBound(64, m_cellW, 360);
            // 真实桌面图标网格步长（逻辑像素）：同步刷新，供 Dock 内拖动吸附对齐。
            m_gridStepX = qMax(48.0, (double)qRound(spcxDevice / qMax(qreal(1.0), dpr2)));
            m_gridStepY = qMax(48.0, (double)qRound(spcyDevice / qMax(qreal(1.0), dpr2)));
        }

        DWORD pid = 0;
        GetWindowThreadProcessId(hLV, &pid);
        HANDLE hProc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE,
                                   FALSE, pid);
        if (hProc) {
            const int count = (int)SendMessageW(hLV, LVM_GETITEMCOUNT, 0, 0);
            QSet<QString> liveNames;
            for (int i = 0; i < count; ++i) {
                Entry e = readItemRemote(hLV, hProc, i);
                if (e.displayName.isEmpty()) continue;
                liveNames.insert(e.displayName);
                bool found = false;
                for (const Entry& b : base)
                    if (b.displayName == e.displayName) { found = true; break; }
                if (!found) {
                    // 新增图标：用实时坐标；若恰好为 (0,0)（隐藏列表读数失效）则交由网格补齐。
                    e.shellPath = resolveShellPath(e.displayName);
                    e.special = e.shellPath.startsWith(QLatin1String("::"));
                    e.iconSizePx = m_desktopIconSize;   // 新发现图标沿用桌面当前图标尺寸
                    if (e.hasPos && e.screenPos.x() == 0 && e.screenPos.y() == 0)
                        e.hasPos = false;
                    base.append(e);
                }
            }
            CloseHandle(hProc);
            // 删除已不在桌面上的图标（特殊项也在 liveNames 中，故一并保留）。
            QVector<Entry> filtered;
            for (const Entry& b : base)
                if (liveNames.contains(b.displayName)) filtered.append(b);
            base = filtered;
        }
    }

    // 4. 防御：若绝大多数坐标都是 (0,0)（隐藏列表误读），把这类坐标判为无效改由网格补齐，
    // 防止所有图标堆在屏幕左上角显得“乱”。
    int zero = 0;
    for (const Entry& e : base)
        if (e.hasPos && e.screenPos.x() == 0 && e.screenPos.y() == 0) ++zero;
    if (!base.isEmpty() && zero > base.size() * 0.5) {
        for (Entry& e : base)
            if (e.screenPos.x() == 0 && e.screenPos.y() == 0) e.hasPos = false;
    }

    m_entries = base;
    int pos = 0; for (const Entry& e : m_entries) if (e.hasPos) ++pos;
    dumpDebug(QStringLiteral("NativeListView snapshot=%1 entries=%2 hasPos=%3")
                  .arg(m_snapshotEntries.size()).arg(m_entries.size()).arg(pos));
#else
    Q_UNUSED(m_hLV);
#endif
}

// readentries
// 作者：谭征
void DesktopMirrorWindow::readEntries() {
    if (m_sourceMode == SourceMode::DesktopOrganize)
        readEntriesFromOrganize();
    else
        readEntriesFromListView();
}

// 由显示名解析出可启动 / 可弹原生菜单的 shellPath：
// 特殊命名空间项（此电脑 / 回收站 / 网络等）映射为 CLSID；
// 其余优先用 IShellFolder 枚举得到的"显示名 -> 绝对路径"映射，
// 最后兜底到桌面目录下按显示名（忽略 .lnk 后缀）匹配真实文件。
// 注意：桌面上的"个人文件夹"图标显示名通常是当前 Windows 用户名
// （如 "TanK"），而非"个人文件夹"或"用户的文件"，因此 specials 表需
// 动态注入用户名映射，保证 resolveShellPath 返回 ::{ 开头的 CLSID 路径，
// 使 e.special=true，收纳盒/整理过滤能正确跳过该系统虚拟项。
// 作者：谭征
QString DesktopMirrorWindow::resolveShellPath(const QString& name) {
    static const auto specials = []() {
        QHash<QString, QString> m;
        m.insert(QStringLiteral("此电脑"),     QStringLiteral("::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"));
        m.insert(QStringLiteral("我的电脑"),     QStringLiteral("::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"));
        m.insert(QStringLiteral("回收站"),       QStringLiteral("::{645FF040-5081-101B-9F08-00AA002F954E}"));
        m.insert(QStringLiteral("网络"),         QStringLiteral("::{208D2C60-3AEA-1069-A2D7-08002B30309D}"));
        m.insert(QStringLiteral("用户的文件"),   QStringLiteral("::{59031a47-3f72-44a7-89c5-5595fe6b30ee}"));
        m.insert(QStringLiteral("个人文件夹"),   QStringLiteral("::{59031a47-3f72-44a7-89c5-5595fe6b30ee}"));
        m.insert(QStringLiteral("控制面板"),     QStringLiteral("::{26EE0668-A00A-44D7-9371-BEB064C98683}"));
        m.insert(QStringLiteral("Control Panel"), QStringLiteral("::{26EE0668-A00A-44D7-9371-BEB064C98683}"));
        // 动态注入：桌面"个人文件夹"显示名 = 当前用户名（如 "TanK"）
        const QString userName = QFileInfo(QDir::homePath()).fileName();
        if (!userName.isEmpty() && !m.contains(userName))
            m.insert(userName, QStringLiteral("::{59031a47-3f72-44a7-89c5-5595fe6b30ee}"));
        return m;
    }();
    auto it = specials.find(name);
    if (it != specials.end()) return *it;
    auto nm = m_nameToShell.find(name);
    if (nm != m_nameToShell.end()) return *nm;
    // 兜底 1：在桌面目录中按显示名匹配真实文件（忽略扩展名、大小写不敏感）
    const QString desk = CategoryStore::desktopPath();
    QDir dir(desk);
    const QStringList files = dir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    auto stripExt = [](const QString& f) {
        if (f.endsWith(QLatin1String(".lnk"), Qt::CaseInsensitive) ||
            f.endsWith(QLatin1String(".url"), Qt::CaseInsensitive))
            return f.left(f.size() - 4);
        return f;
    };
    for (const QString& f : files) {
        if (stripExt(f).compare(name, Qt::CaseInsensitive) == 0)
            return desk + QLatin1Char('/') + f;
    }
    // 兜底 2：快捷方式的 shell 显示名常常不等于文件名——Windows 新建快捷方式时文件名为
    // “<目标> - 快捷方式.lnk”，而 shell 显示名是“<目标>”。按“前缀匹配且唯一命中”补一层，
    // 让刚建好的快捷方式也能解析出有效 shellPath（否则右键菜单/重命名会静默无反应）。
    QString hit;
    int hits = 0;
    for (const QString& f : files) {
        const bool isLink = f.endsWith(QLatin1String(".lnk"), Qt::CaseInsensitive) ||
                            f.endsWith(QLatin1String(".url"), Qt::CaseInsensitive);
        if (!isLink) continue;
        if (stripExt(f).startsWith(name, Qt::CaseInsensitive)) { ++hits; hit = f; }
    }
    if (hits == 1) return desk + QLatin1Char('/') + hit;
    // 终极兜底：返回桌面目录下的绝对路径（desk + "/" + name），而非裸显示名。
    // 原因：新建文件/文件夹的创建与 Dock 刷新存在竞态——文件已存在但 entryList 尚未枚举到它时，
    // 若此处返回裸名，后续 SHParseDisplayName(裸名) 会失败，导致 Dock 右键菜单/重命名静默无反应。
    // 返回绝对路径后，SHParseDisplayName 可正常解析，竞态窗口期也能正确弹出菜单/重命名。
    return desk + QLatin1Char('/') + name;
}

// 拖动结束后回写快照中该图标的位置，保证后续刷新仍使用新坐标
// （隐藏后的 ListView 无法再次读取，必须以快照为准）。
// 作者：谭征
void DesktopMirrorWindow::updateSnapshotPosition(const QString& shellPath, const QPoint& physPos) {
    // 键比较按“分隔符归一化 + 大小写不敏感”做：调用方（repositionDesktopIcon / placeDockIconAtCursor）
    // 传入的路径可能来自收纳盒侧的数据，斜杠方向或大小写未必与快照逐字节相同；
    // 用精确比较会静默匹配不到 → 新位置只落在 m_entries、快照仍是旧坐标 → 下一次刷新把图标打回原位。
    const QString key = QDir::fromNativeSeparators(shellPath).toLower();
    for (Entry& e : m_snapshotEntries)
        if (QDir::fromNativeSeparators(e.shellPath).toLower() == key) {
            e.screenPos = physPos;
            e.hasPos = true;
            return;
        }
}

// 清除 Dock 相关的本地持久化数据：分类库 categories.ini，以及可能存在的 Dock 快照组。
// 作者：谭征
void DesktopMirrorWindow::clearDockPersistedData() {
    CategoryStore::clearAll();
    QSettings s(QSettings::IniFormat, QSettings::UserScope,
                QStringLiteral("DestopTools"), QStringLiteral("DestopTools"));
    s.remove(QStringLiteral("DockSnapshot"));
    s.sync();
}

// 把 Dock 当前镜像的全部图标写入本地持久化数据（DockSnapshot 组）。
// 记录字段：显示名 / shellPath / 系统镜像列表索引 / 屏幕坐标(x,y) / 是否系统项 / 是否有真实坐标 /
// SysListView32 索引 / 真实像素尺寸。下次启动或 Windows 重扫失败时可用作回退的真实桌面集合。
// 作者：谭征
void DesktopMirrorWindow::saveDockSnapshot() const {
    QSettings s(QSettings::IniFormat, QSettings::UserScope,
                QStringLiteral("DestopTools"), QStringLiteral("DestopTools"));
    s.beginGroup(QStringLiteral("DockSnapshot"));
    s.beginWriteArray(QStringLiteral("entries"));
    for (int i = 0; i < m_entries.size(); ++i) {
        const Entry& e = m_entries.at(i);
        s.setArrayIndex(i);
        s.setValue(QStringLiteral("displayName"), e.displayName);
        s.setValue(QStringLiteral("shellPath"), e.shellPath);
        s.setValue(QStringLiteral("imageIndex"), e.imageIndex);
        s.setValue(QStringLiteral("x"), e.screenPos.x());
        s.setValue(QStringLiteral("y"), e.screenPos.y());
        s.setValue(QStringLiteral("special"), e.special);
        s.setValue(QStringLiteral("hasPos"), e.hasPos);
        s.setValue(QStringLiteral("lvIndex"), e.lvIndex);
        s.setValue(QStringLiteral("iconSizePx"), e.iconSizePx);
    }
    s.endArray();
    s.setValue(QStringLiteral("count"), m_entries.size());
    s.endGroup();
    s.sync();
}

// 持久化 Dock 自定义图标位置覆盖：用户在 Dock 内拖动后，每个图标（按 shellPath）的“物理屏幕坐标”。
// 仅作用于 Dock 内的摆放，绝不写回真实 Windows 桌面。落盘前剔除已不在当前桌面集合里的孤儿键，
// 避免重启后桌面图标被删却仍残留覆盖导致无谓增长。
// 作者：谭征
void DesktopMirrorWindow::saveCustomDockPos() const {
    QSettings s(QSettings::IniFormat, QSettings::UserScope,
                QStringLiteral("DestopTools"), QStringLiteral("DestopTools"));
    s.beginGroup(QStringLiteral("DockIconOrder"));
    s.remove(QStringLiteral(""));   // 清空旧数据：避免上次多条目保存残留的高位索引（items\N）在本轮少条目时残留，
                                    // 导致 count 与真实索引错位、加载时读不到有效条目。
    QSet<QString> liveKeys;
    for (const Entry& e : m_entries)
        liveKeys.insert(QDir::fromNativeSeparators(e.shellPath).toLower());
    s.beginWriteArray(QStringLiteral("items"));
    int idx = 0;
    for (auto it = m_customDockPos.constBegin(); it != m_customDockPos.constEnd(); ++it) {
        if (!liveKeys.contains(it.key())) continue;   // 丢弃孤儿键
        s.setArrayIndex(idx++);
        s.setValue(QStringLiteral("shellPath"), it.key());
        s.setValue(QStringLiteral("x"), it.value().x());
        s.setValue(QStringLiteral("y"), it.value().y());
    }
    s.endArray();
    s.endGroup();
    s.sync();
}

// 加载自定义Dock位置
// 作者：谭征
void DesktopMirrorWindow::loadCustomDockPos() {
    m_customDockPos.clear();
    QSettings s(QSettings::IniFormat, QSettings::UserScope,
                QStringLiteral("DestopTools"), QStringLiteral("DestopTools"));
    s.beginGroup(QStringLiteral("DockIconOrder"));
    // 用 beginReadArray 读取数组自身记录的长度，绝不依赖手写的 count 键，
    // 规避“残留高位索引 + count 错位”导致有效条目读不到的问题。
    const int n = s.beginReadArray(QStringLiteral("items"));
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        const QString key = QDir::fromNativeSeparators(
            s.value(QStringLiteral("shellPath")).toString()).toLower();
        if (key.isEmpty()) continue;
        const QPoint p(s.value(QStringLiteral("x"), 0).toInt(),
                      s.value(QStringLiteral("y"), 0).toInt());
        m_customDockPos.insert(key, p);
    }
    s.endArray();
    s.endGroup();
}

#ifdef Q_OS_WIN
DesktopMirrorWindow::Entry DesktopMirrorWindow::readItemRemote(void* hLV, void* hProc, int i) {
    Entry e;
    e.lvIndex = i;
    HANDLE hp = (HANDLE)hProc;

    // 在目标进程分配 LVITEMW + 文本缓冲，注入本地结构，SendMessage 读取，再读回。
    void* pLv = VirtualAllocEx(hp, nullptr, sizeof(LVITEMW), MEM_COMMIT, PAGE_READWRITE);
    void* pText = VirtualAllocEx(hp, nullptr, 512 * sizeof(wchar_t), MEM_COMMIT, PAGE_READWRITE);
    void* pPt = VirtualAllocEx(hp, nullptr, sizeof(POINT), MEM_COMMIT, PAGE_READWRITE);
    if (!pLv || !pText || !pPt) {
        if (pLv) VirtualFreeEx(hp, pLv, 0, MEM_RELEASE);
        if (pText) VirtualFreeEx(hp, pText, 0, MEM_RELEASE);
        if (pPt) VirtualFreeEx(hp, pPt, 0, MEM_RELEASE);
        return e;
    }

    LVITEMW lv = {0};
    lv.mask = LVIF_TEXT | LVIF_IMAGE;
    lv.iItem = i;
    lv.iSubItem = 0;
    lv.cchTextMax = 512;
    lv.pszText = (wchar_t*)pText;
    bool ok = true;
    if (!WriteProcessMemory(hp, pLv, &lv, sizeof(LVITEMW), nullptr)) ok = false;
    if (ok && SendMessageW((HWND)hLV, LVM_GETITEMW, 0, (LPARAM)pLv) == FALSE) ok = false;

    LVITEMW lvBack = {0};
    wchar_t buf[512] = {0};
    if (ok && !ReadProcessMemory(hp, pLv, &lvBack, sizeof(LVITEMW), nullptr)) ok = false;
    if (ok && !ReadProcessMemory(hp, pText, buf, 512 * sizeof(wchar_t), nullptr)) ok = false;
    e.imageIndex = ok ? lvBack.iImage : -1;
    e.displayName = ok ? QString::fromWCharArray(buf) : QString();

    POINT ptBack = {0, 0};
    if (ok) {
        LRESULT posOk = SendMessageW((HWND)hLV, LVM_GETITEMPOSITION, i, (LPARAM)pPt);
        if (posOk != FALSE && ReadProcessMemory(hp, pPt, &ptBack, sizeof(POINT), nullptr)) {
            e.screenPos = QPoint((int)(m_lvOriginX + ptBack.x),
                                 (int)(m_lvOriginY + ptBack.y)); // 物理屏幕坐标
            e.hasPos = true;
        }
    }

    VirtualFreeEx(hp, pPt, 0, MEM_RELEASE);
    VirtualFreeEx(hp, pText, 0, MEM_RELEASE);
    VirtualFreeEx(hp, pLv, 0, MEM_RELEASE);

    // shellPath 由上层通过 DesktopScanner 的权威集合按 displayName 匹配，
    // 不再依赖 buildNameMap()（后者在 ListView 隐藏后可能与桌面不同步）。
    return e;
}
#endif

// 构建名称map
// 作者：谭征
void DesktopMirrorWindow::buildNameMap() {
#ifdef Q_OS_WIN
    m_nameToShell.clear();
    const HRESULT cohr = CoInitializeEx(nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninit = (cohr == S_OK);

    IShellFolder* psf = nullptr;
    if (SUCCEEDED(SHGetDesktopFolder(&psf))) {
        IEnumIDList* penum = nullptr;
        if (SUCCEEDED(psf->EnumObjects((HWND)winId(),
                SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &penum))) {
            LPITEMIDLIST child = nullptr;
            ULONG fetched = 0;
            while (penum->Next(1, &child, &fetched) == S_OK && fetched) {
                STRRET str = {0};
                if (SUCCEEDED(psf->GetDisplayNameOf(child, SHGDN_NORMAL, &str))) {
                    QString name;
                    if (str.uType == STRRET_WSTR) {
                        name = QString::fromWCharArray(str.pOleStr);
                        CoTaskMemFree(str.pOleStr);
                    } else {
                        name = QString::fromLocal8Bit(str.cStr);
                    }
                    PWSTR parsed = nullptr;
                    QString shell;
                    if (SUCCEEDED(SHGetNameFromIDList(child, SIGDN_DESKTOPABSOLUTEPARSING, &parsed)) && parsed) {
                        shell = QString::fromWCharArray(parsed);
                        CoTaskMemFree(parsed);
                    }
                    if (!name.isEmpty() && !shell.isEmpty())
                        m_nameToShell.insert(name, shell);
                }
                CoTaskMemFree(child);
            }
            penum->Release();
        }
        psf->Release();
    }
    if (uninit) CoUninitialize();
#endif
}

namespace {
// 新建项命名启发：中文“新建…”，英文“New …”，用于新建后自动进入重命名态（贴近 Explorer）
bool isNewlyCreatedName(const QString& name) {
    return name.startsWith(QStringLiteral("新建"))
        || name.startsWith(QLatin1String("New "), Qt::CaseInsensitive);
}
// 自动重命名的补弹次数上限（每次间隔 m_autoRenameTimer 的 120ms）：覆盖“刷新→布局→按钮就绪”
// 的时序竞态，超过即放弃，避免无限重试。
const int kAutoRenameMaxRetry = 20;
}

// 布局buttons
// 作者：谭征
void DesktopMirrorWindow::layoutButtons() {
    CrashTrace::mark("layoutButtons:enter");
    // 正在内联重命名：任何一次重排都会先把全部 DockIconButton deleteLater 再重建，编辑框会随
    // 按钮一起被销毁（表现即“重命名框一闪而过”）。这里把重排也一并延后——与 refresh() 的守卫
    // 同一原则，且覆盖 setHiddenShellPaths / resizeEvent 等不经 refresh 的重排入口。
    // 原生右键菜单弹出期间同样延后：否则会把“正在弹菜单的按钮”销毁掉，菜单返回后在该按钮上
    // startInlineRename() 就是访问已析构对象（右键“重命名”因此静默失效）。
    // 标记 m_refreshPending，待编辑/菜单结束后由 onInlineEditFinished / onIconPressed 补做。
    if (m_editingBtn || m_menuOpen) { m_refreshPending = true; return; }

    // 重建前先复位拖动态：布局函数会 deleteLater 掉全部按钮，若拖动态（m_dragGroup/m_dragSnapshot/
    // m_dragPrimary）还持有裸指针，松手时 restoreDragSnapshot() 就会去写已释放的堆块。
    CrashTrace::mark("layoutButtons:A-preAbortDrag");
    abortActiveDrag();

    // 选中接力：重建会销毁全部按钮并清空 m_selection（裸指针）。先把当前选中/锚点归一化成
    // shellPath 键，重建后按键找回。否则任何一次重建（文件增删改名 / 盒↔Dock 拖拽 / 回收站还原
    // 触发）都会让 F2/Delete 丢失目标 —— 即本 bug 的直接根因（尤其 onItemRenamed 里那句
    // singleShot(0) layoutButtons：改名提交后紧随其后重建，把刚建立的选中抹掉）。
    CrashTrace::mark("layoutButtons:B-preSelRelay");
    QStringList selKeys;
    QString anchorKey;
    for (DockIconButton* b : m_selection)
        if (b) selKeys << QDir::fromNativeSeparators(b->item().shellPath).toLower();
    if (m_anchor) anchorKey = QDir::fromNativeSeparators(m_anchor->item().shellPath).toLower();

    CrashTrace::mark("layoutButtons:C-preLayout");
    if (m_sourceMode == SourceMode::DesktopOrganize)
        layoutButtonsGrid();
    else
        layoutButtonsNative();
    CrashTrace::mark("layoutButtons:D-postLayout");

    // 重建后按 shellPath 找回选中与锚点（保持原选中顺序；已消失/被收走的项自然匹配不到→丢弃）
    const QSet<QString> want(selKeys.begin(), selKeys.end());
    for (const QString& key : selKeys) {
        for (DockIconButton* b : m_buttons) {
            if (!b) continue;
            if (QDir::fromNativeSeparators(b->item().shellPath).toLower() != key) continue;
            if (m_selection.contains(b)) continue;   // 同一键只接一次
            b->setSelected(true);
            m_selection.append(b);
            break;
        }
    }
    if (!anchorKey.isEmpty()) {
        for (DockIconButton* b : m_buttons) {
            if (!b) continue;
            if (QDir::fromNativeSeparators(b->item().shellPath).toLower() == anchorKey) {
                m_anchor = b;
                break;
            }
        }
    }

    // 新建项自动进入重命名态：此刻 m_buttons 已就绪，延到下一次事件循环触发，
    // 保证编辑框在按钮 show()/布局稳定后再弹出，且按 shellPath 定位不受后续重建影响。
    CrashTrace::mark("layoutButtons:E-preAutoRename");
    if (!m_autoRenameQueue.isEmpty())
        QTimer::singleShot(0, this, &DesktopMirrorWindow::applyAutoRename);

    // 重排/重建会 new 出新按钮并 show()，必须重新套用「双击隐藏桌面图标」的当前状态——
    // 否则任何一次桌面刷新（文件增删/整理/切模式/盒↔Dock 拖拽）都会把隐藏态冲掉，
    // 表现为"双击隐藏后，动一下文件图标又全回来了"。
    applyDesktopIconsHiddenState();
    CrashTrace::mark("layoutButtons:done");
}

// 让队列中的“新建项”按钮进入内联重命名态（图标与重命名框同现）。
// 关键：按 shellPath 在当前 m_buttons 中查找目标按钮，而不是捕获瞬时 DockIconButton*，
// 因此即便布局被再次触发、按钮被销毁重建，仍能命中正确按钮并弹框。
// 只有“编辑框真的弹出来了”才把该项出队，否则保留到下一轮补弹——这样连续新建多个也不会漏弹。
// 作者：谭征
void DesktopMirrorWindow::applyAutoRename() {
    if (m_editingBtn) return;                 // 已有项在编辑：等其结束后由 onInlineEditFinished 补处理
    if (m_autoRenameQueue.isEmpty()) {        // 已无待弹项：收尾
        m_autoRenameRetry = 0;
        if (m_autoRenameTimer) m_autoRenameTimer->stop();
        return;
    }
    bool opened = false;
    for (DockIconButton* b : m_buttons) {
        if (opened) break;                    // 一次只弹一个编辑框
        const QString key = QDir::fromNativeSeparators(b->item().shellPath).toLower();
        if (!m_autoRenameQueue.contains(key)) continue;
        if (b->isEditing()) {                 // 已在编辑态：视为该路径已完成
            m_autoRenameQueue.remove(key);
            continue;
        }
        if (!b->isRenameable()) {             // 特殊命名空间项永不弹框：出队，别浪费重试次数
            m_autoRenameQueue.remove(key);
            continue;
        }
        b->startInlineRename();
        if (b->isEditing()) {                 // 真正弹出编辑框（同步触发 inlineEditStarted）
            m_autoRenameQueue.remove(key);
            opened = true;
        }
    }
    if (opened) return;                       // 其余待弹项留到本次编辑结束后继续
    if (m_autoRenameQueue.isEmpty()) {
        m_autoRenameRetry = 0;
        if (m_autoRenameTimer) m_autoRenameTimer->stop();
        return;
    }
    // 首轮可能落空（按钮尚未就绪 / 正被重建 / 目标路径暂时对不上）：短延时补弹，次数有上限。
    if (m_autoRenameRetry < kAutoRenameMaxRetry) {
        ++m_autoRenameRetry;
        if (m_autoRenameTimer) m_autoRenameTimer->start();
        dumpDebug(QStringLiteral("applyAutoRename: retry=%1 pending=%2")
                      .arg(m_autoRenameRetry).arg(m_autoRenameQueue.size()));
    } else {
        dumpDebug(QStringLiteral("applyAutoRename: give up pending=%1")
                      .arg(m_autoRenameQueue.size()));
        m_autoRenameQueue.clear();
        m_autoRenameRetry = 0;
    }
}

// 直接遍历 m_buttons 判定 isEditing()，不依赖 m_editingBtn 这一路追踪，避免极端时序下漏提交。
// 作者：谭征
void DesktopMirrorWindow::commitActiveRename() {
    CrashTrace::mark("commitActiveRename");
    // 遍历当前按钮集合而不是只看 m_editingBtn：编辑态是按钮自身的状态，m_editingBtn 只是影子追踪，
    // 一旦追踪丢失（按钮被重建/信号时序异常），点空白处就会“提交不了、框一直挂着”。
    // 但对“刚弹出的编辑框”放行不提交（editorJustOpened）：右键菜单选“重命名”的那一次点击，
    // 其按下消息可能晚于编辑框显示才到达，若当成“点了别处”就会把刚弹出的框立刻关掉。
    // 关键：先收集目标、再逐个提交。commitRename() 内部会落盘改名并同步通知本窗口（onItemRenamed），
    // 边遍历 m_buttons 边提交等于在“可能被改动的集合”上迭代，是典型的悬垂/越界来源。
    QVector<DockIconButton*> targets;
    for (DockIconButton* b : m_buttons) {
        if (b && b->isEditing() && !b->editorJustOpened()) targets.append(b);
    }
    if (targets.isEmpty() && m_editingBtn && m_editingBtn->isEditing()
        && !m_editingBtn->editorJustOpened()) {
        targets.append(m_editingBtn);
    }
    for (DockIconButton* b : targets) {
        if (b && b->isEditing()) b->commitRename();
    }
}

// 原生菜单关闭后补做菜单期间被延后的刷新（若此时正在重命名则继续延后，交由编辑结束再补）。
// 作者：谭征
void DesktopMirrorWindow::flushPendingRefresh() {
    if (m_refreshPending && !m_editingBtn) {
        m_refreshPending = false;
        QTimer::singleShot(0, this, &DesktopMirrorWindow::refresh);
    }
}

// 由 DockIconButton 上报内联重命名开始/结束：编辑期间延后桌面刷新，避免重建按钮销毁编辑框
// 作者：谭征
void DesktopMirrorWindow::onInlineEditStarted(DockIconButton* btn) {
    m_editingBtn = btn;
    m_autoRenameRetry = 0;
    if (m_autoRenameTimer) m_autoRenameTimer->stop();
    if (m_refreshTimer) m_refreshTimer->stop();   // 编辑期间不再触发刷新（避免重建按钮销毁编辑框）
    // （应用级“点编辑框外即提交”守卫已在构造函数里常驻挂载，此处无需再挂 —— 见构造期注释。）
}

// 响应inline编辑finished
// 作者：谭征
void DesktopMirrorWindow::onInlineEditFinished(DockIconButton* btn) {
    if (m_editingBtn == btn) m_editingBtn = nullptr;
    // 确定性保险：该项的重命名框已经弹过一次并结束，若它仍是“新建…”默认名，就把当前路径标记为
    // 已自动重命名过。否则一旦它的 shellPath 在后续刷新中被解析成另一种写法（键不同），
    // 会被重新入队 → 框刚提交又被自动弹回来，看起来就像“点空白/回车/Esc 都没提交成功”。
    if (btn && isNewlyCreatedName(btn->item().displayName)) {
        const QString key = QDir::fromNativeSeparators(btn->item().shellPath).toLower();
        if (!key.isEmpty()) m_autoRenamedPaths.insert(key);
        m_autoRenameQueue.remove(key);
    }
    m_autoRenameRetry = 0;
    if (m_refreshPending) {
        // 编辑期间被延后的刷新：补做一次，同步真实桌面
        m_refreshPending = false;
        QTimer::singleShot(0, this, &DesktopMirrorWindow::refresh);
    } else if (!m_autoRenameQueue.isEmpty()) {
        // 还有待弹出的新建项（如连续新建）：继续处理
        QTimer::singleShot(0, this, &DesktopMirrorWindow::applyAutoRename);
    }
    // （应用级守卫常驻，此处不再卸载 —— 见构造函数注释。）
}

// 传入的 shellPath 集合（绝对路径，大小写不敏感）对应的 Entry 不再生成 Dock 按钮。
// 作者：谭征
void DesktopMirrorWindow::setHiddenShellPaths(const QSet<QString>& paths) {
    // 归一化（大小写不敏感）后保存，供 layoutButtons* 在生成 Dock 按钮时跳过这些图标。
    // 仅隐藏 Dock 显示，绝不删除/移动真实文件。
    m_hiddenShellPaths.clear();
    m_hiddenShellPaths.reserve(paths.size());
    for (const QString& p : paths) {
        if (!p.isEmpty()) m_hiddenShellPaths.insert(QDir::fromNativeSeparators(p).toLower());
    }
    layoutButtons();   // 立即按新隐藏集合重排（不重读桌面，避免闪烁）
}

// 从隐藏集合中移除指定路径（解散分类时调用），使对应图标重新在 Dock 上显示。
// 作者：谭征
void DesktopMirrorWindow::removeHiddenShellPaths(const QSet<QString>& paths) {
    // 从隐藏集合中移除指定路径（解散分类时调用），使对应图标重新在 Dock 上显示。
    bool changed = false;
    for (const QString& p : paths) {
        if (!p.isEmpty()) {
            changed |= m_hiddenShellPaths.remove(QDir::fromNativeSeparators(p).toLower()) != 0;
        }
    }
    if (changed) layoutButtons();
}

// 布局buttons网格
// 作者：谭征
void DesktopMirrorWindow::layoutButtonsGrid() {
    for (DockIconButton* b : m_buttons) b->deleteLater();
    m_buttons.clear();
    m_selection.clear();
    m_anchor = nullptr;

    QScreen* ps = QApplication::primaryScreen();
    const QRect geom = ps ? ps->geometry() : QRect(0, 0, 1920, 1080);
    const int marginX = 32;
    const int marginY = 32;
    const int gap = 12;
    // Dock 图标统一使用与收纳盒/网格视图一致的 72x84 单元格、48x48 图标
    const int cellW = 72;
    const int cellH = 84;
    const int iconPx = 48;
    const int cols = qMax(1, (geom.width() - marginX * 2 + gap) / (cellW + gap));

    int row = 0, col = 0;
    for (const Entry& e : m_entries) {
        // 已被收纳盒收走的图标（大小写不敏感归一化匹配）不再生成 Dock 按钮
        if (m_hiddenShellPaths.contains(QDir::fromNativeSeparators(e.shellPath).toLower())) continue;
        DesktopItem it;
        it.displayName = e.displayName;
        it.shellPath = e.shellPath;
        it.systemImageIndex = e.imageIndex;
        it.isSpecial = e.special;
        it.sourcePath = e.shellPath;
        // 类型判定（“改后缀”相关，两处布局函数共用）：只有“真实可解析的 .lnk”才算快捷方式。
        // 把普通文件改名成 .lnk 后它是“假快捷方式”，若仍按快捷方式处理，就会被交给 Shell 的
        // 链接处理器（图标提取 / 上下文菜单），既显示出错误图标，也存在被第三方 Shell 扩展
        // 解析非法数据拖垮进程的风险 → 一律按普通文件处理。
        // 同时补齐 targetPath：原实现恒为空，会让 SHDefExtractIcon 把 .lnk 自身当目标。
        {
            QString lnkTarget;
            const bool realLnk = e.shellPath.endsWith(QLatin1String(".lnk"), Qt::CaseInsensitive)
                              && DesktopScanner::isRealShortcut(e.shellPath, &lnkTarget);
            it.isShortcut = realLnk;
            it.targetPath = realLnk ? lnkTarget : e.shellPath;
        }

        DockIconButton* btn = new DockIconButton(it, this);
        btn->setCellSize(cellW);
        btn->setIconSize(iconPx);
        CrashTrace::markPath("grid:icon-load", it.displayName);
        btn->setIcon(DesktopScanner::loadIcon(
            it, iconPx,
            Theme::shortcutArrowOverlayScale(2.0)));   // Dock：快捷方式箭头放大到 2 倍（关闭该外观开关时为 0 = 不叠加）
        CrashTrace::mark("grid:icon-ok");

        const int lx = marginX + col * (cellW + gap);
        const int ly = marginY + row * (cellH + gap);
        btn->move(lx, ly);

        connect(btn, &DockIconButton::iconPressed, this, &DesktopMirrorWindow::onIconPressed);
        connect(btn, &DockIconButton::dragMove, this, &DesktopMirrorWindow::onIconDragMove);
        connect(btn, &DockIconButton::dragEnd, this, &DesktopMirrorWindow::onIconDragEnd);
        connect(btn, &DockIconButton::iconReleased, this, &DesktopMirrorWindow::onIconReleased);
        connect(btn, &DockIconButton::activated, this, &DesktopMirrorWindow::clearSelection);

        btn->show();
        m_buttons.append(btn);

        connect(btn, &DockIconButton::renamed, this, &DesktopMirrorWindow::onItemRenamed);
        connect(btn, &DockIconButton::inlineEditStarted, this, &DesktopMirrorWindow::onInlineEditStarted);
        connect(btn, &DockIconButton::inlineEditFinished, this, &DesktopMirrorWindow::onInlineEditFinished);
        // 新建项（名以“新建”/“New ”开头）自动进入重命名态，贴近 Explorer。
        // 以 shellPath 为键（而非显示名）并入队，布局完成后统一触发：同名新建项可重复弹框，
        // 且按钮即便被再次重建也能按路径找回，不会因 singleShot 绑定瞬时指针而失效。
        // 「不在 m_autoRenamedPaths 里」才是**本次会话内新增**的判据（会话基线在 refresh() 里
        // 一次登记完成）；名字启发只负责认出 Shell 新建出来、还没改名的默认名。
        // **禁**把门控"简化"成纯名字判断 —— 那样桌面上遗留的「新建文本文档.txt」会在每次启动
        // 被自动弹框改名（2026-09-22 已修，用户明确要求：Dock 内新建仍要自动进重命名态）。
        {
            const QString key = QDir::fromNativeSeparators(e.shellPath).toLower();
            if (isNewlyCreatedName(e.displayName) && !m_autoRenamedPaths.contains(key)) {
                m_autoRenamedPaths.insert(key);
                m_autoRenameQueue.insert(key);
            }
        }

        if (++col >= cols) {
            col = 0;
            ++row;
        }
    }
}

// 由 DockIconButton 在内联重命名落盘成功后上报，用于同步 m_entries 的显示名与 shellPath
// 作者：谭征
void DesktopMirrorWindow::onItemRenamed(const QString& oldName, const QString& newName) {
    if (oldName == newName) return;
    CrashTrace::markPath("onItemRenamed", oldName + QStringLiteral(" -> ") + newName);

    // ① m_entries：更新显示名与同目录新路径，并记录旧/新路径（归一化键）供后续同步使用。
    QString oldKey, newKey, newPath, oldPath;
    for (Entry& e : m_entries) {
        if (e.displayName != oldName) continue;
        oldPath = QDir::fromNativeSeparators(e.shellPath);
        oldKey = oldPath.toLower();
        const QFileInfo fi(e.shellPath);
        newPath = fi.isAbsolute() ? QDir(fi.path()).filePath(newName) : newName;
        e.displayName = newName;
        e.shellPath = newPath;
        break;
    }
    if (newPath.isEmpty()) {
        // 未在 m_entries 命中（如编辑期间已发生过刷新重建）：按桌面目录兜底重建新路径，
        // 否则下面按路径迁移的“隐藏集合 / 映射”会失去目标键。
        newPath = CategoryStore::desktopPath() + QLatin1Char('/') + newName;
    }
    newKey = QDir::fromNativeSeparators(newPath).toLower();

    // ② m_snapshotEntries：必须同步（这是“改后缀后图标跳位”的根因）。
    // 快照是“原生桌面可见时拍下的权威集合 + 坐标”，之后只做增删核对、从不重读坐标。
    // 若不同步，改后缀后下一次刷新会发现“快照里仍是旧名、真实桌面已是新名”，于是把该项判成
    // “旧项已删除 + 新项为新增”；而新增项需从被隐藏的 ListView 读坐标（读数塌缩到 0,0 被判无效），
    // 结果图标丢掉原位置、跳到左上角网格 —— 任何一次改后缀都必经此路。
    for (Entry& s : m_snapshotEntries) {
        if (s.displayName != oldName) continue;
        s.displayName = newName;
        if (!newPath.isEmpty()) s.shellPath = newPath;
        break;
    }

    // ③ 显示名 → shellPath 映射：保证后续右键菜单 / 二次重命名按新名解析到新路径，
    // 否则会拿到已失效的旧路径而静默失败。
    m_nameToShell.remove(oldName);
    if (!newName.isEmpty() && !newPath.isEmpty()) m_nameToShell.insert(newName, newPath);

    // ④ 所有“以路径为键”的集合都要迁移（键为分隔符归一化 + 小写）：
    // · m_hiddenShellPaths：收纳盒已收走的状态必须跟着路径走。否则新路径不在隐藏集合里，
    // 被收进收纳盒的图标会在 Dock 上“复活”，同时收纳盒里那条旧路径记录永远匹配不到真实文件。
    // · m_autoRenamedPaths / m_autoRenameQueue：避免该项被重新判定为“新建项”而反复弹框。
    if (!oldKey.isEmpty() && !newKey.isEmpty() && oldKey != newKey) {
        if (m_hiddenShellPaths.remove(oldKey)) m_hiddenShellPaths.insert(newKey);
        if (m_autoRenamedPaths.remove(oldKey)) m_autoRenamedPaths.insert(newKey);
        if (m_autoRenameQueue.remove(oldKey)) m_autoRenameQueue.insert(newKey);
    } else if (!oldKey.isEmpty()) {
        m_autoRenamedPaths.remove(oldKey);
    }

    // ⑤ 通知 MainWindow 同步收纳盒/网格里该项的路径与分类记录（改名后仍在运行中的收纳盒
    // 若继续持有旧路径，点击会失效、下次整理时归属也会丢）。
    if (!oldPath.isEmpty() && !newPath.isEmpty()
        && oldPath.compare(newPath, Qt::CaseInsensitive) != 0) {
        emit itemRenamedOnDesktop(oldPath, newPath);
    }

    // ⑥ 立即重排（延迟到当前调用栈退出后）：改名是用户可见操作，必须马上显示新名与新图标。
    // 用 singleShot 而不是直接 layoutButtons()，是因为本函数由 DockIconButton::performRename()
    // 经信号同步调入，直接重排会在该按钮自己的成员函数栈上回收它（极端情况 use-after-free）；
    // 延时一拍让 performRename 安全返回，同时保住“即时刷新”的观感。
    if (!m_editingBtn && !m_menuOpen)
        QTimer::singleShot(0, this, &DesktopMirrorWindow::layoutButtons);

    dumpDebug(QStringLiteral("onItemRenamed: %1 -> %2 (%3)")
                  .arg(oldName).arg(newName).arg(newPath));
}

// 布局buttons原生
// 作者：谭征
void DesktopMirrorWindow::layoutButtonsNative() {
    for (DockIconButton* b : m_buttons) b->deleteLater();
    m_buttons.clear();
    m_selection.clear();
    m_anchor = nullptr;

    // 有 ListView 坐标的项按 Windows 桌面原始位置摆放；
    // 未匹配到的项（或坐标失效 fallback）在底部按网格补齐，避免丢失。
    const int marginX = 32;
    const int marginY = 32;
    const int gap = 12;
    // Dock 图标统一使用与收纳盒/网格视图一致的 72x84 单元格、48x48 图标
    const int cellW = 72;
    const int cellH = 84;
    const int iconPx = 48;
    QScreen* ps = QApplication::primaryScreen();
    const QRect geom = ps ? ps->geometry() : QRect(0, 0, 1920, 1080);
    const int cols = qMax(1, (geom.width() - marginX * 2 + gap) / (cellW + gap));
    int gridCol = 0, gridRow = 0;

    auto createButton = [&](const Entry& e, int x, int y) {
        DesktopItem it;
        it.displayName = e.displayName;
        it.shellPath = e.shellPath;
        it.systemImageIndex = e.imageIndex;
        it.isSpecial = e.special;
        it.sourcePath = e.shellPath;
        // 类型判定（“改后缀”相关，两处布局函数共用）：只有“真实可解析的 .lnk”才算快捷方式。
        // 把普通文件改名成 .lnk 后它是“假快捷方式”，若仍按快捷方式处理，就会被交给 Shell 的
        // 链接处理器（图标提取 / 上下文菜单），既显示出错误图标，也存在被第三方 Shell 扩展
        // 解析非法数据拖垮进程的风险 → 一律按普通文件处理。
        // 同时补齐 targetPath：原实现恒为空，会让 SHDefExtractIcon 把 .lnk 自身当目标。
        {
            QString lnkTarget;
            const bool realLnk = e.shellPath.endsWith(QLatin1String(".lnk"), Qt::CaseInsensitive)
                              && DesktopScanner::isRealShortcut(e.shellPath, &lnkTarget);
            it.isShortcut = realLnk;
            it.targetPath = realLnk ? lnkTarget : e.shellPath;
        }

        DockIconButton* btn = new DockIconButton(it, this);
        btn->setCellSize(cellW);
        btn->setIconSize(iconPx);
        CrashTrace::markPath("native:icon-load", it.displayName);
        QIcon ico = DesktopScanner::loadIcon(it, iconPx,
                                             Theme::shortcutArrowOverlayScale(2.0));   // Dock：箭头 ×2（开关关闭时 0）
        btn->setIcon(ico);
        CrashTrace::mark("native:icon-ok");
        if (ico.isNull()) {
            dumpDebug(QStringLiteral("createButton: null icon name=%1 sp=%2 special=%3")
                          .arg(e.displayName).arg(e.shellPath).arg(e.special));
        }
        btn->move(x, y);

        connect(btn, &DockIconButton::iconPressed, this, &DesktopMirrorWindow::onIconPressed);
        connect(btn, &DockIconButton::dragMove, this, &DesktopMirrorWindow::onIconDragMove);
        connect(btn, &DockIconButton::dragEnd, this, &DesktopMirrorWindow::onIconDragEnd);
        connect(btn, &DockIconButton::iconReleased, this, &DesktopMirrorWindow::onIconReleased);
        connect(btn, &DockIconButton::activated, this, &DesktopMirrorWindow::clearSelection);

        btn->show();
        m_buttons.append(btn);

        connect(btn, &DockIconButton::renamed, this, &DesktopMirrorWindow::onItemRenamed);
        connect(btn, &DockIconButton::inlineEditStarted, this, &DesktopMirrorWindow::onInlineEditStarted);
        connect(btn, &DockIconButton::inlineEditFinished, this, &DesktopMirrorWindow::onInlineEditFinished);
        // 新建项（名以“新建”/“New ”开头）自动进入重命名态，贴近 Explorer。
        // 以 shellPath 为键（而非显示名）并入队，布局完成后统一触发：同名新建项可重复弹框，
        // 且按钮即便被再次重建也能按路径找回，不会因 singleShot 绑定瞬时指针而失效。
        // 「不在 m_autoRenamedPaths 里」才是**本次会话内新增**的判据（会话基线在 refresh() 里
        // 一次登记完成）；名字启发只负责认出 Shell 新建出来、还没改名的默认名。
        // **禁**把门控"简化"成纯名字判断 —— 那样桌面上遗留的「新建文本文档.txt」会在每次启动
        // 被自动弹框改名（2026-09-22 已修，用户明确要求：Dock 内新建仍要自动进重命名态）。
        {
            const QString key = QDir::fromNativeSeparators(e.shellPath).toLower();
            if (isNewlyCreatedName(e.displayName) && !m_autoRenamedPaths.contains(key)) {
                m_autoRenamedPaths.insert(key);
                m_autoRenameQueue.insert(key);
            }
        }
    };

    // 第一轮：有真实坐标（或被 Dock 自定义覆盖）的项按对应屏幕位置摆放。
    // 优先用 m_customDockPos 覆盖（用户在 Dock 内拖动过的图标），否则用 Windows 桌面真实坐标。
    for (const Entry& e : m_entries) {
        const QString key = QDir::fromNativeSeparators(e.shellPath).toLower();
        QPoint effPos = e.screenPos;
        bool effHasPos = e.hasPos;
        auto cit = m_customDockPos.constFind(key);
        if (cit != m_customDockPos.constEnd()) { effPos = cit.value(); effHasPos = true; }
        if (!effHasPos) continue;
        // 已被收纳盒收走的图标不再生成 Dock 按钮
        if (m_hiddenShellPaths.contains(key)) continue;
        const int lx = qRound(effPos.x() / m_dpiFactor);
        const int ly = qRound(effPos.y() / m_dpiFactor);
        createButton(e, lx, ly);
    }

    // 第二轮：无真实坐标的项按网格补齐在底部
    for (const Entry& e : m_entries) {
        if (e.hasPos) continue;
        // 已被收纳盒收走的图标不再生成 Dock 按钮
        if (m_hiddenShellPaths.contains(QDir::fromNativeSeparators(e.shellPath).toLower())) continue;
        const int lx = marginX + gridCol * (cellW + gap);
        const int ly = marginY + gridRow * (cellH + gap);
        createButton(e, lx, ly);
        if (++gridCol >= cols) {
            gridCol = 0;
            ++gridRow;
        }
    }
}

// 清除选中
// 作者：谭征
void DesktopMirrorWindow::clearSelection() {
    for (DockIconButton* s : m_selection) if (s) s->setSelected(false);
    m_selection.clear();
    m_anchor = nullptr;
}

// 全局点 → 命中的图标按钮。只有落在“图标 / 标签可视区”内才算命中；
// 单元格 72x84 里图标只有 48x48，落在内边距上的点必须算“空白”。
// 为什么不用 childAt()：childAt() 只看控件矩形，会把点图标旁的空白也判成“点了按钮”，
// 于是用户在空白处按下时选中态既清不掉、又可能被转移到别的项上。
// 作者：谭征
DockIconButton* DesktopMirrorWindow::iconAtGlobal(const QPoint& globalPos) const {
    const QPoint local = mapFromGlobal(globalPos);
    for (DockIconButton* b : m_buttons) {
        if (!b || b->isHidden()) continue;
        if (!b->geometry().contains(local)) continue;
        if (b->hitsVisualArea(b->mapFromParent(local))) return b;
    }
    return nullptr;
}

// 点 Dock 空白处 → 清空选中。鼠标侧的统一入口：Qt 的 mousePressEvent（左键 / 右键空白分支）
// 与系统级鼠标钩子（左键 / 右键按下）都会调。
// 之所以要双通道：Dock 带 WS_EX_NOACTIVATE，落在空白处的按下未必会经 Qt 的事件路由回到本窗口
// （与“点空白提交不了内联重命名”同源），只靠 Qt 会出现“点空白清不掉选中”。
// 两个早退条件保证不干扰已被其它语义接管的鼠标：
// · 原生右键菜单弹出期间（m_menuOpen）——那段时间鼠标属于菜单；
// · 内联重命名进行中——鼠标属于编辑框（点框外由编辑期钩子负责提交）。
// 作者：谭征
void DesktopMirrorWindow::clearSelectionOnBlankPress(const QPoint& globalPos) {
    if (m_menuOpen) return;
    for (DockIconButton* b : m_buttons)
        if (b && b->isEditing()) return;
    if (m_selection.isEmpty()) return;          // 本就没有选中项 → 无需处理
    if (iconAtGlobal(globalPos)) return;        // 点的是图标可视区 → 由 onIconPressed 决策
    DiagTrace::log(QStringLiteral("[dock] clearSelectionOnBlankPress sel=%1 g=(%2,%3)")
                       .arg(m_selection.size()).arg(globalPos.x()).arg(globalPos.y()));
    clearSelection();
}

// —— 选中集合管理（Ctrl/Shift 多选 + 框选），贴近 Windows 桌面交互 ——
// 作者：谭征
void DesktopMirrorWindow::selectExclusive(DockIconButton* b) {
    for (DockIconButton* s : m_selection) if (s != b) s->setSelected(false);
    m_selection.clear();
    m_selection.append(b);
    b->setSelected(true);
    m_anchor = b;
}

// 添加到选中
// 作者：谭征
void DesktopMirrorWindow::addToSelection(DockIconButton* b) {
    if (!m_selection.contains(b)) {
        m_selection.append(b);
        b->setSelected(true);
    }
}

// 切换选中
// 作者：谭征
void DesktopMirrorWindow::toggleSelection(DockIconButton* b) {
    if (m_selection.contains(b)) {
        m_selection.removeAll(b);
        b->setSelected(false);
        if (m_anchor == b) m_anchor = m_selection.isEmpty() ? nullptr : m_selection.last();
    } else {
        m_selection.append(b);
        b->setSelected(true);
        m_anchor = b;
    }
}

// 范围选择
// 作者：谭征
void DesktopMirrorWindow::rangeSelect(DockIconButton* b) {
    if (!m_anchor) { selectExclusive(b); return; }
    const int ia = m_buttons.indexOf(m_anchor);
    const int ib = m_buttons.indexOf(b);
    if (ia < 0 || ib < 0) { selectExclusive(b); return; }
    const int lo = qMin(ia, ib), hi = qMax(ia, ib);
    for (DockIconButton* s : m_selection) s->setSelected(false);
    m_selection.clear();
    for (int i = lo; i <= hi; ++i) {
        DockIconButton* x = m_buttons[i];
        x->setSelected(true);
        m_selection.append(x);
    }
}

// 画框多选（橡皮筋）
// 与 Windows 桌面一致的语义：
// · 空白处按下即清空选中，并起框；
// · 拖动过程中**实时重算**选中集合（框到的选中、框离的取消，而不是只增不减）；
// · 按住 Ctrl 起框为“加选”，基线集合（起框时的选中）全程保留；
// · 松手保持当前集合；点一下不拖（1×1 框）＝只清空，不选中任何项。
// 为什么命中几何用 hitsVisualRect()（图标+标签并集）而不是按钮矩形：
// 单元格 72×84 里图标只有 48×48，用单元格判定会出现“框还没碰到图标，旁边那项就被选中了”。
// 作者：谭征
void DesktopMirrorWindow::applyRubberSelection(const QRect& rect, bool additive) {
    QList<DockIconButton*> hits;
    hits.reserve(m_buttons.size());
    for (DockIconButton* b : m_buttons) {
        if (!b || b->isHidden()) continue;
        const QRect vis = b->hitsVisualRect().translated(b->pos());   // 局部 → 窗口坐标
        if (!vis.isValid()) continue;
        if (rect.intersects(vis)) hits.append(b);
    }
    if (additive) {
        QList<DockIconButton*> merged = m_rubberBase;
        for (DockIconButton* b : hits) if (!merged.contains(b)) merged.append(b);
        hits = merged;
    }
    // 只改差异：本函数在拖动期间每 16ms 跑一次，全量 setSelected+update 会让整屏图标反复重绘。
    for (DockIconButton* b : m_buttons) {
        if (!b) continue;
        const bool want = hits.contains(b);
        if (b->isSelected() != want) b->setSelected(want);
    }
    m_selection = hits;
}

// 轮询而非依赖 WM_MOUSEMOVE 钩子：钩子回调必须极轻，逐条 move 消息投递会拖垮它。
// 作者：谭征
void DesktopMirrorWindow::beginRubber(const QPoint& globalPos, bool additive) {
    if (m_rubbering) return;                 // 双通道（Qt / 系统钩子）都会调，必须幂等：
                                             // 后到的通道不能把起点重置掉（否则框会从“当前点”重新起）
    if (m_suppressRubber) return;            // 这一击已被判为"双击桌面空白"（见 handleBlankDoubleClick）
    if (m_menuOpen) return;                  // 原生菜单弹出期鼠标属于菜单
    for (DockIconButton* b : m_buttons)      // 内联重命名期鼠标属于编辑框（点框外由编辑期钩子提交）
        if (b && b->isEditing()) return;

    // 本次框的语义：默认＝「在桌面空白处绘制创建收纳盒」（设置中心「快捷操作」页）；
    // Ctrl 拉框恒为框选多选 —— 多选（F2/Delete/拖动）这条既有能力必须留一个不改配置的逃生口，
    // 且 Ctrl 本来就是"加选"修饰键，语义自洽。
    m_rubberCreatesBox = (m_quickDrawBoxOnBlank && !additive);

    m_rubbering = true;
    m_rubberAdditive = additive;
    m_rubberStart = mapFromGlobal(globalPos);
    m_rubberRect = QRect(m_rubberStart, QSize(1, 1));
    m_rubberBase.clear();
    if (additive)
        m_rubberBase = m_selection;          // Ctrl 加选：基线保留，框选只做并集

    // 16ms 轮询推进：不依赖 Qt 鼠标事件路由。见头文件说明（Dock 是 WS_EX_NOACTIVATE 的底层窗口，
    // 空白处的按下/移动/释放未必回到 Qt；只靠 mouseMoveEvent 会出现“框拖不出来、松手不结束”）。
    if (!m_rubberTimer) {
        m_rubberTimer = new QTimer(this);
        m_rubberTimer->setInterval(16);
        connect(m_rubberTimer, &QTimer::timeout, this, &DesktopMirrorWindow::rubberTick);
    }
    m_rubberTimer->start();
    update(m_rubberRect);
}

// 更新框选
// 作者：谭征
void DesktopMirrorWindow::updateRubber(const QPoint& globalPos) {
    if (!m_rubbering) return;
    const QPoint cur = mapFromGlobal(globalPos);
    const QRect r = QRect(m_rubberStart, cur).normalized();
    if (r == m_rubberRect) return;           // 位置没变就不重绘（轮询 16ms 下这是常态）
    const QRect dirty = r.united(m_rubberRect).adjusted(-2, -2, 2, 2);
    m_rubberRect = r;
    // 画框建盒模式下不选中图标：框内的图标会在松手时被收进新盒（此时高亮选中只会误导，
    // 且那批按钮随后就被重建/隐藏，选中集合留着反而成了悬垂来源）。
    if (!m_rubberCreatesBox) applyRubberSelection(r, m_rubberAdditive);
    update(dirty);
}

// 结束框选
// 作者：谭征
void DesktopMirrorWindow::endRubber() {
    m_suppressRubber = false;                             // 松手即解除"这一击不起框"
    if (!m_rubbering && m_rubberRect.isNull()) return;    // 幂等：Qt 与钩子两条通道都会调
    const QRect dirty = m_rubberRect.adjusted(-2, -2, 2, 2);
    const bool createsBox = m_rubberCreatesBox;
    const QRect drawn = m_rubberRect;                     // 窗口局部逻辑坐标（松手前先留一份）
    m_rubbering = false;
    m_rubberRect = QRect();
    m_rubberBase.clear();
    m_rubberCreatesBox = false;
    if (m_rubberTimer) m_rubberTimer->stop();
    update(dirty);

    // 快捷操作②：按拉出的框新建收纳盒
    // 太小视为"点了一下/手抖"（阈值 48×36 逻辑像素）：否则每次单击桌面都会冒出一个空盒子。
    // 与 Windows 框选一致，框"碰到"即可（interacts），不要求整个图标落在框内。
    if (!createsBox) return;
    if (drawn.width() < 48 || drawn.height() < 36) return;

    QStringList paths, names;
    for (DockIconButton* b : m_buttons) {
        if (!b || b->isHidden()) continue;
        const QRect vis = b->hitsVisualRect().translated(b->pos());   // 局部 → 窗口坐标
        if (!vis.isValid() || !drawn.intersects(vis)) continue;
        // 与「从 Dock 拖图标进收纳盒」同一取法：系统虚拟项（回收站/我的电脑…，shellPath 以 "::"
        // 开头）没有真实路径，不参与收纳。
        const QString sp = b->item().shellPath;
        if (sp.isEmpty() || sp.startsWith(QLatin1String("::"))) continue;
        paths << sp;
        names << b->item().displayName;
    }
    if (!paths.isEmpty()) clearSelection();   // 这批图标马上离开 Dock，选中集合先清干净

    const QRect globalRect(mapToGlobal(drawn.topLeft()), drawn.size());
    DOCK_HK_TRACE(QStringLiteral("[dock] drawBox rect=(%1,%2,%3,%4) icons=%5")
                      .arg(globalRect.x()).arg(globalRect.y())
                      .arg(globalRect.width()).arg(globalRect.height()).arg(paths.size()));
    emit boxRegionDrawn(globalRect, paths, names);
}

// 画框轮询：每 16ms 读一次真实光标与左键状态，自行推进/收尾。
// 这样即使 Qt 一个鼠标事件都没收到（空白处按下被外壳吃掉），画框照样能用、也能正常结束。
// 作者：谭征
void DesktopMirrorWindow::rubberTick() {
#ifdef Q_OS_WIN
    if (!m_rubbering) { if (m_rubberTimer) m_rubberTimer->stop(); return; }
    updateRubber(QCursor::pos());            // Qt 的 QCursor::pos() 已是逻辑坐标（含 DPI 换算）
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) endRubber();   // 左键已抬起
#else
    if (m_rubberTimer) m_rubberTimer->stop();
#endif
}

// 物理屏幕像素 → 全局逻辑坐标（供 iconAtGlobal / clearSelectionOnBlankPress 使用，二者都按
// mapFromGlobal 期望“全局”坐标）。
// 低级钩子（WH_MOUSE_LL）给的 MSLLHOOKSTRUCT.pt 是物理像素，而 Qt 的鼠标/几何都是逻辑像素：
// 只需除以设备像素比即得全局逻辑坐标。DPI 虚拟化（dpr==1、钩子给的已是虚拟化坐标）与
// PerMonitorV2（dpr=缩放比、钩子给物理坐标）两种情况下本式都成立。
// 不得再加 geometry().topLeft()：iconAtGlobal 内部 mapFromGlobal 会自行减去窗口原点，若这里
// 先加原点，会互相抵消成“把全局逻辑点当成窗口局部点”用，主屏非 (0,0) / 多屏时整体偏移，
// 导致钩子把“点图标”误判为“点空白”并清空刚建立的选中（F2 / Delete 连带失效的根因）。
// 作者：谭征
QPoint DesktopMirrorWindow::nativePhysToWindowLogical(const QPoint& phys) const {
    qreal dpr = devicePixelRatioF();
    if (dpr <= 0) dpr = 1.0;
    return QPoint(qRound(phys.x() / dpr), qRound(phys.y() / dpr));
}

// 钩子通道：Dock 空白处按下 → 清选中 + 起框。
// 判据全部基于几何（iconAtGlobal 是纯几何的图标可视区命中测试），与 Qt 事件是否到达无关，
// 所以即使 Qt 通道稍后也收到同一次按下，两个通道的结果一致、幂等。
// 作者：谭征
void DesktopMirrorWindow::onBlankPressFromHook(const QPoint& physPt) {
    if (!isVisible()) return;                // Dock 未显示 → 不介入
    const QPoint g = nativePhysToWindowLogical(physPt);
    DockIconButton* hit = iconAtGlobal(g);
    if (hit) {                               // 点在图标可视区 → 属于该图标的选中/拖动，不起框
        m_lastBlankDownTick = 0;             // 打断"空白→图标→空白"这种伪双击对（双击只在空白处成立）
        DiagTrace::log(QStringLiteral("[dock] blankFromHook HIT name='%1' g=(%2,%3)")
                           .arg(hit->item().displayName).arg(g.x()).arg(g.y()));
        return;
    }
    // 快捷操作①：双击桌面空白 → 隐藏/显示桌面图标。命中则这一击不再清选中/起框。
    if (handleBlankDoubleClick(physPt)) return;
    // Ctrl 加选：与 Windows 一致，按住 Ctrl 起框不清空已有选中。
#ifdef Q_OS_WIN
    const bool additive = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
#else
    const bool additive = false;
#endif
    if (!additive) clearSelectionOnBlankPress(g);
    beginRubber(g, additive);
}

// 快捷操作①：双击桌面空白处 → 隐藏 / 显示桌面图标
// 为什么必须自己判双击：WH_MOUSE_LL（低级钩子）只送 WM_LBUTTONDOWN/WM_LBUTTONUP，
// Windows 的 WM_LBUTTONDBLCLK 是**窗口消息**，永远不会经过低级钩子，所以系统不会替我们识别。
// 判据用系统参数（GetDoubleClickTime + SM_CXDOUBLECLK/SM_CYDOUBLECLK），与 Explorer 同口径。
// 只在"空白处"计数（见调用点：点在图标上会先把计数清零），因此双击图标仍然是"打开文件"。
// 作者：谭征
bool DesktopMirrorWindow::handleBlankDoubleClick(const QPoint& physPt) {
#ifdef Q_OS_WIN
    const unsigned int now = GetTickCount();
    const int halfW = qMax(2, GetSystemMetrics(SM_CXDOUBLECLK) / 2);
    const int halfH = qMax(2, GetSystemMetrics(SM_CYDOUBLECLK) / 2);
    const bool dbl = (m_lastBlankDownTick != 0)
                     && (now - m_lastBlankDownTick) <= static_cast<unsigned int>(GetDoubleClickTime())
                     && qAbs(physPt.x() - m_lastBlankDownPhys.x()) <= halfW
                     && qAbs(physPt.y() - m_lastBlankDownPhys.y()) <= halfH;
    m_lastBlankDownTick = now;
    m_lastBlankDownPhys = physPt;
    if (!m_quickHideIconsOnDblClick || !dbl) {
        // 非双击（普通按下）：顺手解除上一击可能残留的"不起框"标记 —— 万一那次双击的
        // WM_LBUTTONUP 没到达（外壳抢消息等极端时序），也不至于让"拉框建盒/框选"永久失效。
        m_suppressRubber = false;
        return false;
    }
    m_lastBlankDownTick = 0;      // 三连击不重复触发（必须再点两下才翻一次）
    m_suppressRubber = true;      // 这一击不再起框（Qt 通道同拦，见 beginRubber）
    endRubber();                  // 收掉可能残留的框（幂等）
    clearSelectionOnBlankPress(nativePhysToWindowLogical(physPt));
    toggleDesktopIconsVisible();
    DOCK_HK_TRACE(QStringLiteral("[dock] dblclick blank → desktop icons hidden=%1")
                      .arg(m_desktopIconsHidden ? 1 : 0));
    return true;
#else
    Q_UNUSED(physPt)
    return false;
#endif
}

// 隐藏 / 显示桌面图标（＝本镜像层的全部图标按钮）。隐藏态只在内存里：重启即恢复显示，
// 与"双击切换"的临时语义一致，也不会让用户在下次开机时困惑"图标去哪了"。
// 作者：谭征
void DesktopMirrorWindow::toggleDesktopIconsVisible() {
    m_desktopIconsHidden = !m_desktopIconsHidden;
    applyDesktopIconsHiddenState();
}

// （layoutButtons 末尾调用）——否则刷新一次就把隐藏态冲掉（表现为"隐藏一下又自己回来了"）。
// 作者：谭征
void DesktopMirrorWindow::applyDesktopIconsHiddenState() {
    if (m_desktopIconsHidden) {
        // 隐藏图标前先清选中：按钮接着要 setVisible(false)，留着选中集合只会变成一组点不到的
        // "幽灵选中"（F2/Delete 仍以它们为目标），也避免后续重建时按 shellPath 接力找回。
        clearSelection();
    }
    for (DockIconButton* b : m_buttons) {
        if (!b) continue;
        b->setVisible(!m_desktopIconsHidden);
    }
    update();
}

// Dock 键盘快捷键：F2 重命名 / Delete(或 D) 删除
// 目标：Dock 上选中图标后，操作与 Windows 桌面图标**完全一致** —— F2 进入重命名、
// Delete 删除到回收站；没有选中项时两个键照常交给前台程序，Dock 不抢键盘。
// 为什么必须走系统级低级钩子（而不是 Qt 的 keyPressEvent）：
// Dock 主窗口带 WS_EX_NOACTIVATE + Qt::WindowDoesNotAcceptFocus（Win+D 免疫所必需），
// 它**永远拿不到键盘焦点** —— keyPressEvent / QShortcut / 菜单加速键在它身上从不触发。
// 与内联重命名同一策略：把判定下沉到 WH_KEYBOARD_LL，只看物理按键与屏幕坐标，
// 与窗口激活状态、焦点归属、Qt 事件路由统统无关。
// 作用域（防误删的关键）：快捷键只在“Dock 面”上生效。由 WH_MOUSE_LL 维护
// s_hkLastClickOnDock = 最近一次鼠标按下是否落在 Dock（或桌面外壳面）上。
// 于是：一旦在别的程序窗口里点过一下，F2/Delete 立刻回归那个程序（我们连处理都不做），
// 不会出现“在浏览器里按 Delete 反把桌面文件删了”。
#ifdef Q_OS_WIN
// A1：两个 HHOOK 不再由本类持有 —— 低级键鼠钩子改由进程级
// LowLevelHookManager 统一安装与分发（全进程只 2 个钩子，Dock 与收纳盒共享）。
// 本类只保留“是否已注册 / 谁是 owner / 最近一次按下是否在 Dock”这三项语义状态。
bool DesktopMirrorWindow::s_hkActive = false;
QPointer<DesktopMirrorWindow> DesktopMirrorWindow::s_hkOwner;
bool DesktopMirrorWindow::s_hkLastClickOnDock = false;

// 该屏幕点是否属于 “Dock 面”。三种情形：
// · 点上的窗口属于本进程，且就是 Dock 自身或其子控件        → 是（正常命中 Dock 图标/空白）；
// · 点上的窗口是桌面外壳面（Progman/WorkerW/SHELLDLL_DefView）→ 是（Dock 正是取代桌面的存在，
// 全屏铺满且常在最底层，命中桌面外壳与命中 Dock 是同一语义，取决于 z 序细节故两者都认）；
// · 点上的窗口是其它进程的真实程序窗口（浏览器/资源管理器…）→ 否，那一击属于那个程序。
// 作者：谭征
bool DesktopMirrorWindow::nativeHitTestIsDock(POINT pt) const {
    const HWND self = (HWND)winId();
    if (!self || !IsWindowVisible(self)) return false;
    RECT rc = {};
    if (!GetWindowRect(self, &rc)) return false;
    if (!PtInRect(&rc, pt)) return false;          // 不在 Dock 覆盖范围内（如点到了副屏）
    const HWND h = WindowFromPoint(pt);
    if (!h) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId())
        return h == self || IsChild(self, h) != FALSE;
    wchar_t cls[64] = {0};
    if (!GetClassNameW(h, cls, 63)) return false;
    const QString c = QString::fromWCharArray(cls);
    return c.compare(QLatin1String("Progman"), Qt::CaseInsensitive) == 0
        || c.compare(QLatin1String("WorkerW"), Qt::CaseInsensitive) == 0
        || c.compare(QLatin1String("SHELLDLL_DefView"), Qt::CaseInsensitive) == 0;
}

// 鼠标钩子：① 维护“最近一次按下是否落在 Dock 上”（快捷键作用域）；
// ② 空白处按下 → 清选中（左键还会起画框）；③ 左键抬起 → 收画框。
// 不吞事件（点击照常派发，Dock 的选中/菜单/拖动逻辑完全不受影响）。
// 为什么画框也要走这里：Dock 是 WS_EX_NOACTIVATE + 常年 HWND_BOTTOM，空白处的
// 按下/移动/释放未必经 Qt 事件路由回到本窗口 —— 只靠 mouseMoveEvent 会出现“框拖不出来”。
// 钩子只负责“投递 0ms 请求 + 记录抬起”，推进与收尾由 Dock 内的 16ms 轮询完成（见 rubberTick）。
// 作者：谭征
void DesktopMirrorWindow::hotkeyMouseConsumer(WPARAM wParam, LPARAM lParam, void* /*ctx*/) {
    // A1：nCode 过滤已由 LowLevelHookManager 统一完成，这里只按消息类型分发（鼠标从不吞事件）。
    {
        switch (wParam) {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN: {
            const MSLLHOOKSTRUCT* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            DesktopMirrorWindow* w = s_hkOwner;    // QPointer：对象销毁后自动为空
            s_hkLastClickOnDock = (w && ms) ? w->nativeHitTestIsDock(ms->pt) : false;
            if (s_hkLastClickOnDock && ms && (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN)) {
                const long nx = ms->pt.x, ny = ms->pt.y;   // 低级钩子给的是物理像素
                const bool left = (wParam == WM_LBUTTONDOWN);
                QPointer<DesktopMirrorWindow> p(w);
                QTimer::singleShot(0, w, [p, nx, ny, left]() {
                    if (!p) return;
                    const QPoint phys(nx, ny);
                    if (left) {
                        // 左键：一个入口同时负责“清选中 + 起画框”；点在图标可视区上时内部自会早退，
                        // 交给该图标的选中/拖动逻辑（判据是纯几何的，与 Qt 事件是否到达无关，故幂等）。
                        p->onBlankPressFromHook(phys);
                    } else {
                        // 右键：只清选中（随后弹出的原生菜单由 Qt 侧负责）。
                        p->clearSelectionOnBlankPress(p->nativePhysToWindowLogical(phys));
                    }
                });
            }
            break;
        }
        case WM_LBUTTONUP: {
            // 左键抬起 → 收画框。不判落点：画框可能拖到屏幕任何位置，且 endRubber() 幂等。
            DesktopMirrorWindow* w = s_hkOwner;
            if (w) {
                QPointer<DesktopMirrorWindow> p(w);
                QTimer::singleShot(0, w, [p]() { if (p) p->endRubber(); });
            }
            break;
        }
        default:
            break;
        }
    }
}

// 键盘钩子：只做“识别 + 投递 0ms 请求”，绝不在回调里做重活。
// 低级钩子回调超时会被 Windows 静默摘除（句柄仍非空），所以在回调中不查文件、不建对象、不弹窗。
// 作者：谭征
bool DesktopMirrorWindow::hotkeyKeyConsumer(int vk, void* /*ctx*/) {
    // A1：nCode 与“仅 KEYDOWN/SYSKEYDOWN”过滤已由 LowLevelHookManager 统一完成。
    DesktopMirrorWindow* w = s_hkOwner;
    if (w) {
        const int act = w->hotkeyAction(vk);
        if (act != 0) {
            QPointer<DesktopMirrorWindow> p(w);
            if (act == 1) QTimer::singleShot(0, w, [p]() { if (p) p->triggerRenameShortcut(); });
            else          QTimer::singleShot(0, w, [p]() { if (p) p->triggerDeleteShortcut(); });
            return true;   // 吞掉：该键已由 Dock 消费，不再落到前台程序
        }
    }
    return false;
}

// 键 → 动作映射。非本快捷键（或上下文不符）返回 0 → 钩子一律放行。
// 作者：谭征
int DesktopMirrorWindow::hotkeyAction(int vk) const {
    if (vk != VK_F2 && vk != VK_DELETE && vk != 'D') return 0;
    const bool ok = hotkeyContextOk();
    DOCK_HK_TRACE(QStringLiteral("[dock] hotkeyAction vk=%1 gate=%2")
                      .arg(vk).arg(DiagTrace::boolStr(ok)));
    if (!ok) return 0;
    return (vk == VK_F2) ? 1 : 2;
}

// 上下文判定：全部条件满足才认为“这一键是发给 Dock 的”。
// E2：判据顺序按“代价从低到高、常见拒绝原因靠前”重排（各判据无副作用，
// AND 语义完全等价）。原顺序把 m_selection / s_hkLastClickOnDock 放在遍历 m_buttons 与
// nativeHitTestIsDock 之后 —— 而“没有选中项”“最近一次点击不在 Dock 上”是最常见的拒绝路径，
// 却要先付一次全按钮遍历 + 一套 Win32 命中测试，属于纯浪费。
// 作者：谭征
bool DesktopMirrorWindow::hotkeyContextOk() const {
    if (!s_hkActive || s_hkOwner != this) { DOCK_HK_TRACE(QStringLiteral("[dock] ctx FAIL: hooks inactive/not owner")); return false; }   // 未持钩（Dock 未运行）→ 不响应
    if (!isVisible()) { DOCK_HK_TRACE(QStringLiteral("[dock] ctx FAIL: !isVisible")); return false; }
    // —— 两条 O(1) 且最常见的拒绝原因前置，尽早短路 ——
    if (m_selection.isEmpty()) { DOCK_HK_TRACE(QStringLiteral("[dock] ctx FAIL: empty selection")); return false; }              // 没有选中项 → 无动作可做
    if (!s_hkLastClickOnDock) { DOCK_HK_TRACE(QStringLiteral("[dock] ctx FAIL: last click not on dock")); return false; }               // 最近一次交互不在 Dock/桌面上 → 不抢键盘
    if (m_menuOpen) { DOCK_HK_TRACE(QStringLiteral("[dock] ctx FAIL: m_menuOpen")); return false; }          // 原生右键菜单弹出期间：交给菜单自己（菜单里也有删除/重命名）
    if (m_editingBtn) { DOCK_HK_TRACE(QStringLiteral("[dock] ctx FAIL: m_editingBtn")); return false; }        // 内联重命名期间：按键属于编辑框（Delete 删字符、F2 无意义）
    // —— 重型判定放最后：只有“看起来真能响应”时才遍历按钮 / 做 Win32 命中测试 ——
    for (DockIconButton* b : m_buttons)    // 编辑态兜底：m_editingBtn 只是影子指针，极端时序可能落后
        if (b && b->isEditing()) return false;
    // 光标当前也必须落在 Dock 面上（“焦点跟随鼠标”语义）。两个条件必须同时成立：
    // · 只看“最近点击在 Dock”：用户点完 Dock 再按 Win+E 打开资源管理器、在里面按 Delete，
    // 那一下会被本钩子抢走并删掉桌面文件（严重误伤）；
    // · 只看“光标在 Dock 上”：用户在别处点过、只是把鼠标扫过桌面再按 Delete 同样会误删。
    POINT cur = {};
    if (!GetCursorPos(&cur) || !nativeHitTestIsDock(cur)) { DOCK_HK_TRACE(QStringLiteral("[dock] ctx FAIL: cursor not on dock")); return false; }
    // 带修饰键一律放行：Ctrl/Shift/Alt/Win 组合属于系统或其它程序（Ctrl+C 复制等），Dock 不介入。
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000)
        || (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        || (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000))
        return false;
    return true;
}

// C：重活期间临时摘钩的嵌套计数。>0 时 ensureHotkeyHooks() 一律不装钩 ——
// 低级钩子是【系统同步回调到本 GUI 线程】的，线程被重活钉住时全系统鼠标都会等我们返回。
// 有了这道护栏，「操作慢」不再等于「鼠标卡死」。
static int s_hkSuspendDepth = 0;

// ④ 最近一次鼠标按下落在 Dock（或桌面外壳面）上；⑤ 无 Ctrl/Shift/Alt/Win 修饰键。
// 作者：谭征
void DesktopMirrorWindow::ensureHotkeyHooks() {
    if (s_hkSuspendDepth > 0) return;   // 暂停中：不装钩（重活结束由 resumeHotkeyHooks 统一恢复）
    // 每次调用都“先卸后装”。句柄非空 ≠ 钩子有效：低级钩子回调超时会被 Windows 静默摘除
    // （句柄仍非空，于是后续 install 会误以为“已装”而跳过 → 整段时间快捷键失效）。
    // 本函数挂在“每一次 Dock 交互”（图标按下 / 空白按下 / 启动镜像）上，等于每次要用之前
    // 都保证钩子真的在，属于自愈式安装；三次 Win32 调用的开销可忽略。
    // 注意必须把“最近一次按下在 Dock 上”的作用域标志**跨重装保留**：本函数正是在那次按下的
    // 处理过程中被调用的，若在重装时清空，紧接着按 F2/Delete 就会被判成上下文不符而失效。
    const bool keepArmed = (s_hkOwner == this) && s_hkLastClickOnDock;
    // A1：改为注册到进程级共享钩子管理器。subscribe() 对同一 token 会
    // 原地刷新回调并 reinstall()（= 原来的“先卸后装”自愈），因此不会重复累积钩子。
    lowLevelHooks().subscribe(this,
                              &DesktopMirrorWindow::hotkeyKeyConsumer,
                              &DesktopMirrorWindow::hotkeyMouseConsumer,
                              this);
    // A1 回归修复（2026-09-21）：闸门取“仍在注册名单里”，**不能**取 active()。
    // 共享钩子是全进程一起摘的（嵌套计数），而两侧 resume 是分别调用的：
    // suspend(Dock)→suspend(Grid)→resume(Grid)→resume(Dock) 这个序下，Dock 自己的
    // ensureHotkeyHooks() 会在 m_suspendDepth 尚为 1 时被调用 → reinstall() 跳过
    // → active() 返回 false → 旧码把 s_hkActive 清成 false 且把 s_hkOwner 置空
    // → 从此 Dock 的 F2/Delete/重命名 静默失效（且不会自愈）。
    // registered() 与暂态摘钩无关，摘钩期间本来也收不到按键，故对闸门是正确语义。
    const bool reg = lowLevelHooks().registered(this);
    s_hkActive = reg;
    if (reg) {
        s_hkOwner = this;
        s_hkLastClickOnDock = keepArmed;
    } else {
        s_hkOwner = nullptr;         // 未注册成功：不认领 owner（与原“只有装钩成功才认领”一致）
    }
    DiagTrace::log(QStringLiteral("[dock] ensureHotkeyHooks key=%1 mouse=%2 active=%3 registered=%4 owner=%5 lastClick=%6 keepArmed=%7 consumers=%8 mouseMode=%9")
                       .arg((qulonglong)lowLevelHooks().keyHook(), 0, 16)
                       .arg((qulonglong)lowLevelHooks().mouseHook(), 0, 16)
                       .arg(DiagTrace::boolStr(s_hkActive))
                       .arg(DiagTrace::boolStr(reg))
                       .arg(DiagTrace::boolStr(s_hkOwner == this))
                       .arg(DiagTrace::boolStr(s_hkLastClickOnDock))
                       .arg(DiagTrace::boolStr(keepArmed))
                       .arg(lowLevelHooks().consumerCount())
                       // 鼠标承载方式：RawInput = 异步不阻塞系统输入链（2026-09-24 起的正解）；
                       // WH_MOUSE_LL 只在 Raw Input 注册失败时出现（降级）。
                       .arg(lowLevelHooks().mouseViaRawInput() ? QStringLiteral("RawInput")
                                                               : QStringLiteral("WH_MOUSE_LL")));
}

// 松开hotkeyhooks
// 作者：谭征
void DesktopMirrorWindow::releaseHotkeyHooks() {
    if (s_hkOwner && s_hkOwner != this) return;   // 不是当前持钩者 → 勿误卸
    s_hkOwner = nullptr;
    s_hkLastClickOnDock = false;
    s_hkActive = false;
    // A1：从共享管理器注销本 token；最后一个消费者注销时管理器自动 UnhookWindowsHookEx。
    lowLevelHooks().unsubscribe(this);
}

// C：重活前摘钩（嵌套安全）。刻意【保留】s_hkOwner 与 s_hkLastClickOnDock ——
// resume 时 ensureHotkeyHooks() 会据 (s_hkOwner==this)&&s_hkLastClickOnDock 自动还原
// keepArmed，快捷键「最近一次按下是否在 Dock 上」的作用域不会因为摘钩而丢失。
// 作者：谭征
void DesktopMirrorWindow::suspendHotkeyHooks() {
    if (s_hkSuspendDepth++ > 0) return;        // 已在暂停中：只加计数
#ifdef Q_OS_WIN
    // A1：全进程摘钩（管理器内部同样是嵌套计数，收纳盒的 suspend 与本处互不干扰）。
    lowLevelHooks().suspend();
    s_hkActive = false;
    DiagTrace::log(QStringLiteral("[dock] suspendHotkeyHooks 摘钩（重活期间不再拦截键鼠）"));
#endif
}

// C：重活后重新装钩。只有「确实处于暂停中」才恢复；未暂停时是空操作，
// 避免误把 releaseHotkeyHooks()（如 stopMirror 已释放）之后的状态又给装回去。
// 作者：谭征
void DesktopMirrorWindow::resumeHotkeyHooks() {
    if (s_hkSuspendDepth == 0) return;         // 未暂停 → 什么都不做
    if (--s_hkSuspendDepth > 0) return;        // 仍有外层暂停
#ifdef Q_OS_WIN
    lowLevelHooks().resume();                  // A1：管理器计数归零 → 恢复共享钩子
    // A1 回归修复（2026-09-21）说明：此处判据仍是 s_hkOwner，但它已不会再被“误清空”。
    // 原因是 ensureHotkeyHooks() 的闸门已由 active() 改为 registered()：
    // 在 suspend(Dock)→suspend(Grid)→resume(Grid)→resume(Dock) 这个序下，
    // 本函数与 Dock 的 ensureHotkeyHooks() 都可能在 m_suspendDepth > 0 时被调用，
    // 旧码用 active() 判“装钩成功与否”会把 s_hkOwner 清成空，于是这里直接 return、
    // 闸门永不恢复；现在 registered() 只看“是否还在名单里”，与暂态摘钩无关，
    // 故 s_hkOwner 保持有效，本函数仍能正确刷新闸门。
    // 注意本函数是 static：不能用 this / 隐式成员调用，必须经 s_hkOwner 指针调用。
    if (!s_hkOwner) return;                    // 从未装过 / 已随 stopMirror 释放 → 不重装
    s_hkOwner->ensureHotkeyHooks();
#endif
}

// F2：对齐 Windows 桌面 —— 对“当前项”进入内联重命名。Dock 没有焦点矩形这一概念，
// 取选中集合里的锚点（最后一次单击选中的项）；多选时同样只改这一项（Windows 亦然）。
// 作者：谭征
void DesktopMirrorWindow::triggerRenameShortcut() {
    CrashTrace::mark("hotkey:F2-rename");
    DockIconButton* target = nullptr;
    if (m_anchor && m_selection.contains(m_anchor)) target = m_anchor;
    else if (!m_selection.isEmpty()) target = m_selection.last();
    DiagTrace::log(QStringLiteral("[dock] triggerRenameShortcut sel=%1 target=%2 renameable=%3")
                       .arg(m_selection.size())
                       .arg(target ? target->item().displayName : QStringLiteral("<null>"))
                       .arg(target ? DiagTrace::boolStr(target->isRenameable()) : QStringLiteral("-")));
    if (!target) return;
    if (!target->isRenameable()) return;   // 特殊命名空间项（此电脑/回收站…）不可重命名
    commitActiveRename();                  // 保险：若还有别的项在编辑，先收干净再开新的
    target->startInlineRename();
}

// Delete / D：走 Shell 的删除（与资源管理器“删除”同一条路径 → 进回收站）。
// 特殊命名空间项（此电脑/网络/回收站等）一律跳过，绝不删桌面以外的任何东西。
// 作者：谭征
void DesktopMirrorWindow::triggerDeleteShortcut() {
    CrashTrace::mark("hotkey:Del-delete");
    if (m_selection.isEmpty()) return;
    commitActiveRename();                  // 先结束命名，再按最新 shellPath 收集（改名会换路径）
    deleteDockItems(QVector<DockIconButton*>(m_selection.begin(), m_selection.end()));
}

// 删除一组 Dock 项：Delete 快捷键与“拖到回收站”两条路共用，保证行为完全一致。
// 受保护项（系统命名空间图标、个人文件夹、不可删除的目标）一律跳过 —— 判据集中在
// DockIconButton::isDeletableOnDrop()，避免两处各写一套出现口径不一致的漏网。
// 返回是否真的删掉了至少一项：调用方据此决定“拖动是否要还原位置”。
// 作者：谭征
bool DesktopMirrorWindow::deleteDockItems(const QVector<DockIconButton*>& targets) {
    if (targets.isEmpty()) return false;

    QStringList reportPaths;               // 绝对路径（归一化为正斜杠），交给 ShellOps 与上报
    QVector<DockIconButton*> toHide;
    QSet<QString> seen;
    for (DockIconButton* b : targets) {
        if (!b) continue;
        if (!b->isDeletableOnDrop()) continue;
        const QFileInfo fi(b->item().shellPath);
        const QString abs = fi.absoluteFilePath();
        const QString key = QDir::fromNativeSeparators(abs).toLower();
        if (seen.contains(key)) continue;
        seen.insert(key);
        reportPaths << QDir::fromNativeSeparators(abs);
        toHide.append(b);
    }
    if (reportPaths.isEmpty()) return false;

    // 落盘统一走 ShellOps（与收纳盒图标同一条通道）：FOF_ALLOWUNDO 进回收站、可还原，
    // 受保护项（系统项/盘根/用户主目录及上级/桌面目录本身）在 ShellOps 内部再拦一次。
    if (!ShellOps::deleteToRecycleBin(reportPaths, (void*)winId())) return false;
    // 立即隐藏已删除的按钮：拖放删除时若等 80ms 后的 refresh 才消失，用户会看到图标
    // 仍然“摞在回收站上”——正是要消除的观感。隐藏后 refresh 重建时该项已不在 m_entries 中，
    // 不会被重新画出来，所以这里只做即时反馈、不承担“移除”职责。
    for (DockIconButton* b : toHide) if (b) b->hide();
    // 同步 Dock 自身的权威集合：m_snapshotEntries 是 NativeListView 模式下刷新的唯一真相源。
    // 若不在这里清掉已删除项，一旦某次刷新拿不到 ListView 句柄（读不到 liveNames，整段“核对
    // 增删”被跳过），被删的图标就会原地“复活”——与改后缀改名后必须同步 snapshot 同一个道理。
    {
        QSet<QString> gone;
        for (const QString& p : reportPaths) gone.insert(p.toLower());
        auto stripGone = [&gone](QVector<Entry>& v) {
            QVector<Entry> keep;
            keep.reserve(v.size());
            for (const Entry& e : v) {
                if (!gone.contains(QDir::fromNativeSeparators(e.shellPath).toLower()))
                    keep.append(e);
            }
            v = keep;
        };
        stripGone(m_snapshotEntries);
        stripGone(m_entries);
    }
    clearSelection();
    emit itemsDeletedOnDesktop(reportPaths);
    // 等 Explorer 完成删除落盘后再对齐一次（change notify 也会触发刷新，这里只是兜底更快）。
    QTimer::singleShot(80, this, &DesktopMirrorWindow::refresh);
    return true;
}
#else
// ④ 最近一次鼠标按下落在 Dock（或桌面外壳面）上；⑤ 无 Ctrl/Shift/Alt/Win 修饰键。
// 作者：谭征
void DesktopMirrorWindow::ensureHotkeyHooks() {}
// 松开hotkeyhooks
// 作者：谭征
void DesktopMirrorWindow::releaseHotkeyHooks() {}
// hotkey上下文ok
// 作者：谭征
bool DesktopMirrorWindow::hotkeyContextOk() const { return false; }
// hotkeyaction
// 作者：谭征
int  DesktopMirrorWindow::hotkeyAction(int) const { return 0; }
// 进程内的 WH_KEYBOARD_LL 统一接管（见 icongridwindow.cpp）；下面三个入口即钩子的落点。
// 作者：谭征
void DesktopMirrorWindow::triggerRenameShortcut() {}
// trigger删除快捷方式
// 作者：谭征
void DesktopMirrorWindow::triggerDeleteShortcut() {}
// 返回是否真的删掉了至少一项；受保护项（系统图标/个人文件夹等）自动跳过。
// 作者：谭征
bool DesktopMirrorWindow::deleteDockItems(const QVector<DockIconButton*>&) { return false; }
#endif

// —— 由 DockIconButton 上报的鼠标交互，统一决策 ——
// 作者：谭征
void DesktopMirrorWindow::onIconPressed(DockIconButton* self, const QPoint& globalPos,
                                         Qt::MouseButtons buttons, Qt::KeyboardModifiers mods) {
    // 点任意图标前先提交正在进行的内联重命名（对齐 Windows：点别处即完成命名）。
    // 包括点到“正在被重命名的那个图标”本身——Windows 中点击该项也会结束命名，
    // 之前刻意排除 self 会让用户误以为“点图标不能提交”。
    commitActiveRename();
    // 每次与 Dock 交互都重装一次快捷键钩子（自愈：低级钩子可能已被系统静默摘除），
    // 让“选中图标 → F2 / Delete”这条路随时可用。
    ensureHotkeyHooks();
    if (buttons & Qt::RightButton) {
        // 右键：确保 self 在选中集合（不在则 exclusive 选中），再弹原生菜单。
        // 菜单是原生模态循环，期间 Qt 定时器仍可能在循环内触发 refresh/layoutButtons；
        // 用 m_menuOpen 把重排挡住，避免“正在弹菜单的按钮”被销毁（菜单返回后还要在它上面
        // startInlineRename，一旦 this 失效就是右键“重命名”静默失效）。
        if (!m_selection.contains(self)) selectExclusive(self);
        m_menuOpen = true;
        self->showContextMenu(globalPos);
        m_menuOpen = false;
        flushPendingRefresh();
        return;
    }
    // 左键
    if (mods & Qt::ControlModifier) {
        toggleSelection(self);
    } else if (mods & Qt::ShiftModifier) {
        if (m_anchor) rangeSelect(self);
        else selectExclusive(self);
    } else {
        if (!(m_selection.contains(self) && m_selection.size() > 1)) {
            // 单击未处于多选中集合的图标：独占选中
            selectExclusive(self);
        }
        // 否则保持现有多选中集合，便于整体拖动
    }
    DiagTrace::log(QStringLiteral("[dock] onIconPressed sel=%1 anchor=%2 lastClick=%3 hkOwner=%4")
                       .arg(m_selection.size())
                       .arg(m_anchor ? m_anchor->item().displayName : QStringLiteral("<null>"))
                       .arg(DiagTrace::boolStr(s_hkLastClickOnDock))
                       .arg(DiagTrace::boolStr(s_hkOwner == this)));
    // 准备拖动组：self 已处于多选中集合则整组拖动，否则仅 self
    m_dragPrimary = self;
    m_dragStartGlobal = globalPos;
    m_dragGroup.clear();
    if (m_selection.contains(self) && m_selection.size() > 1)
        m_dragGroup = QVector<DockIconButton*>(m_selection.begin(), m_selection.end());
    else
        m_dragGroup.append(self);
    m_dragSnapshot.clear();
    for (DockIconButton* b : m_dragGroup)
        m_dragSnapshot.append(qMakePair(b, b->pos()));
    // 组同步位移必须以“主拖拽按钮”为基准。注意 m_dragGroup 的顺序来自 m_selection（选中顺序），
    // 主拖拽按钮未必是第 0 个 —— 之前的实现假设 snapshot.first() 就是主按钮，导致从画框多选中
    // 拖动其中任意一个非首项时，整组纹丝不动（只有被拖的那一个跟手），
    // 也就无法把它拖到回收站上完成“整组删除”。
    m_dragPrimaryStart = self->pos();
    // C1：拖动期间“是否压在回收站上”的判定只与拖动组几何有关，而回收站按钮
    // 在整段拖动中不会移动 —— 这里一次性算好（回收站指针 + 每个成员的可拖删标志）。
    // 否则每个鼠标 move 都要跑 recycleBinButton()（对全部按钮做 isRecycleBin）与
    // isDeletableOnDrop()（含 QFileInfo::exists 与 QStandardPaths 查询），拖动跟手度直接受损。
    m_dragRecycleBin = recycleBinButton();
    m_dragDeletable.clear();
    m_dragDeletable.reserve(m_dragGroup.size());
    for (DockIconButton* b : m_dragGroup)
        m_dragDeletable.append(b ? b->isDeletableOnDrop() : false);
    m_lastDropHighlightMs = 0;
    // 拖拽浮层复位：本轮拖动的图标位图必须重抓（拖动组可能与上一轮不同），遮挡区也等新的下发。
    resetDragGhost();
    m_dragActive = true;
}

// 响应图标拖拽移动
// 作者：谭征
void DesktopMirrorWindow::onIconDragMove(DockIconButton* self) {
    if (m_dragSnapshot.isEmpty() || !m_dragActive) return;
    if (self != m_dragPrimary) return;                 // 仅主拖拽按钮驱动同步
    const QPoint delta = self->pos() - m_dragPrimaryStart;
    for (const auto& pr : m_dragSnapshot) {
        DockIconButton* b = pr.first;
        if (!b || b == self) continue;
        b->move(pr.second + delta);
    }
    updateDropTargetHighlight();     // 实时提示“松手即删除”（压在回收站上时高亮它）
    // 拖拽浮层跟手：被收纳盒遮住的那块图标由置顶浮层补画（遮挡区每 40ms 由 MainWindow 下发，
    // 这里负责位置；未被遮挡时该调用只是隐藏浮层，零开销）。
    updateDragGhost();

    // 跨窗口拖拽：把光标当前位置上报给 MainWindow，由它判定是否悬停在某个收纳盒窗口上，
    // 并让那个窗口亮起“松手即收进此处”的提示框。
    // 为什么要节流到 ~40ms：拖动的 move 事件是逐像素到达的（可达数百 Hz），
    // 每次都发信号会让 MainWindow 在拖动期间反复遍历窗口列表算命中，白白拖慢拖动跟手度。
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - m_lastHoverEmitMs >= 40) {
        m_lastHoverEmitMs = nowMs;
        m_dragHoverNotified = true;      // 收尾时要通知外部清理（见成员注释）
        emit iconDragHoverChanged(QCursor::pos());
    }
}

// 拖动悬停在收纳盒上 → 光标切成“可放置”形状（Qt::DragMoveCursor）。
// 为什么必须自己做：Dock 的拖动是**自定义跟手移动**（按钮本体 move()，不走 QDrag），
// 没有系统提供的落点光标反馈 —— 光标维持抓握手时，用户无法分辨“这里松手能收进去”
// 还是“这里放不进去会弹回桌面”。复位统一走 on=false（abort / finishDrag 都会调）。
// 作者：谭征
void DesktopMirrorWindow::setDragOverBox(bool on) {
    if (m_dragOverBox == on) return;
    m_dragOverBox = on;
    if (on) setCursor(Qt::DragMoveCursor);
    else    unsetCursor();
}

// —— 拖拽浮层：把被收纳盒盖住的拖拽图标补画到最上层（配合 DockDragGhost）——
// 作者：谭征
void DesktopMirrorWindow::setDragGhostClip(const QRect& globalClipRect) {
    if (m_dragGhostClip == globalClipRect) return;
    m_dragGhostClip = globalClipRect;
    updateDragGhost();
}

// 抓取拖动组各图标的位图（相对拖动组包围盒左上角的偏移 + 位图）。每轮拖动只抓一次：
// grab() 是整控件重绘，逐帧抓会明显拖慢跟手度。
// 用「包围盒」而非「首个按钮」定位：多选拖动时 m_dragGroup 的顺序来自选中集合的迭代顺序，
// first() 未必是最左上的那个（与 m_dragPrimary 未必是首项同源的坑）。
// 作者：谭征
void DesktopMirrorWindow::ensureDragGhostFrames() {
    if (m_dragGhostFramesReady) return;
    m_dragGhostFramesReady = true;
    m_dragGhostFrames.clear();
    m_dragGhostSize = QSize();
    m_dragGhostBBoxOffset = QPoint();
    if (m_dragGroup.isEmpty()) return;

    QRect bbox;
    bool any = false;
    for (DockIconButton* b : m_dragGroup) {
        if (!b) continue;
        const QRect r(b->mapToGlobal(QPoint(0, 0)), b->size());
        bbox = any ? bbox.united(r) : r;
        any = true;
    }
    QWidget* anchor = m_dragGroup.first();
    if (!any || bbox.isEmpty() || !anchor) return;

    m_dragGhostBBoxOffset = bbox.topLeft() - anchor->mapToGlobal(QPoint(0, 0));
    m_dragGhostSize = bbox.size();
    m_dragGhostFrames.reserve(m_dragGroup.size());
    for (DockIconButton* b : m_dragGroup) {
        if (!b) continue;
        m_dragGhostFrames.append(qMakePair(b->mapToGlobal(QPoint(0, 0)) - bbox.topLeft(), b->grab()));
    }
}

// 位置 / 裁剪更新。仅在「本次拖动」且「确实被遮挡」时才显示 —— 未被遮挡时真实按钮本来就正常可见，
// 浮层一出现反而会与它叠成重影。
// 作者：谭征
void DesktopMirrorWindow::updateDragGhost() {
    if (!m_dragActive || m_dragGhostClip.isEmpty() || m_dragGroup.isEmpty()) {
        hideDragGhost();
        return;
    }
    ensureDragGhostFrames();
    QWidget* anchor = m_dragGroup.first();
    if (!anchor || m_dragGhostFrames.isEmpty() || m_dragGhostSize.isEmpty()) {
        hideDragGhost();
        return;
    }
    // 组内按钮是整组同步位移的 → 「包围盒当前位置 = 锚点当前位置 + 起抓时的相对偏移」。
    const QRect bbox(anchor->mapToGlobal(QPoint(0, 0)) + m_dragGhostBBoxOffset, m_dragGhostSize);
    const QRect inter = bbox.intersected(m_dragGhostClip);
    if (inter.isEmpty()) { hideDragGhost(); return; }

    if (!m_dragGhost) m_dragGhost = new DockDragGhost();
    auto* ghost = static_cast<DockDragGhost*>(m_dragGhost);
    ghost->setFrames(m_dragGhostFrames);
    // 尺寸在整段拖动中恒定 → 这里只触发 move（不 resize），逐帧开销与闪烁都可忽略。
    ghost->setGeometry(bbox);
    ghost->setPaintClip(inter.translated(-bbox.topLeft()));
    if (!ghost->isVisible()) {
        ghost->show();
        ghost->raise();   // 置于最上：本窗是 TOPMOST 层级，收纳盒的 raiseAboveBandWindows 推不动它
    }
}

// 隐藏拖拽浮层
// 作者：谭征
void DesktopMirrorWindow::hideDragGhost() {
    if (m_dragGhost && m_dragGhost->isVisible()) m_dragGhost->hide();
}

// 收尾复位：隐藏 + 丢弃遮挡区与位图。三条路径（finishDrag / abortActiveDrag / 起拖前）共用，
// 避免漏掉某一条而「浮层残留」或「复用到上一轮的位图」。
// 作者：谭征
void DesktopMirrorWindow::resetDragGhost() {
    hideDragGhost();
    m_dragGhostClip = QRect();
    m_dragGhostFramesReady = false;
    m_dragGhostFrames.clear();
    m_dragGhostSize = QSize();
    m_dragGhostBBoxOffset = QPoint();
}

// 定位回收站图标。按 CLSID/显示名匹配（见 DockIconButton::isRecycleBin）。
// 作者：谭征
DockIconButton* DesktopMirrorWindow::recycleBinButton() const {
    for (DockIconButton* b : m_buttons)
        if (b && b->isRecycleBin()) return b;
    return nullptr;
}

// 回收站满/空状态跟随 Windows 桌面：状态翻转即重取真实图标并刷新 Dock 回收站按钮。
// 状态未翻转时直接返回（零开销）；首拍只记账不刷新（此时按钮可能尚未布局，且初始图标
// 已由 loadIcon 在布局时取到，若首拍就刷新反而可能覆盖掉尚未完成的布局时序）。
// 作者：谭征
void DesktopMirrorWindow::pollRecycleBinState() {
#ifdef Q_OS_WIN
    const bool empty = ShellOps::recycleBinIsEmpty();
    if (m_recycleBinKnown && empty == m_recycleBinEmpty) return;   // 状态未翻转，零开销
    m_recycleBinKnown = true;
    m_recycleBinEmpty = empty;
    DockIconButton* rb = recycleBinButton();
    if (!rb) return;   // 回收站按钮尚未布局出来（首刷前），下一轮再试
    rb->setIcon(DesktopScanner::recycleBinIcon(48));
    dumpDebug(QStringLiteral("recycleBin state=%1").arg(empty ? QStringLiteral("empty") : QStringLiteral("full")));
#else
    Q_UNUSED(m_recycleBinKnown)
    Q_UNUSED(m_recycleBinEmpty)
#endif
}

// 本次拖动是否压在回收站上。
// 判据用“可视图标矩形相交”，不用鼠标坐标：拖动时按钮本就跟着光标走，用按钮几何更直观，
// 也避免到处换算 grabOffset / DPI。相交面积需达到图标面积的 1/4 才算“压上去”，
// 防止边缘轻擦就触发删除这种不可逆动作。
// 作者：谭征
bool DesktopMirrorWindow::dragOverRecycleBin() const {
    // C1：拖动中一律用拖动开始时拍好的缓存 —— 回收站按钮不动、成员归属不变，
    // 逐像素重算 QFileInfo::exists / QStandardPaths / QString 分配毫无意义。
    const bool useCache = m_dragActive && m_dragRecycleBin
                          && m_dragDeletable.size() == m_dragGroup.size();
    DockIconButton* bin = useCache ? m_dragRecycleBin : recycleBinButton();
    if (!bin) return false;
    // 拖动组里包含回收站本身时一律不触发（回收站是受保护项，既不能删也不该自我高亮）
    for (DockIconButton* b : m_dragGroup)
        if (b == bin) return false;
    const QRect binIcon = bin->iconVisualRect().translated(bin->pos());
    for (int i = 0; i < m_dragGroup.size(); ++i) {
        DockIconButton* b = m_dragGroup.at(i);
        if (!b) continue;
        const bool deletable = useCache ? m_dragDeletable.at(i) : b->isDeletableOnDrop();
        if (!deletable) continue;                       // 系统图标/个人文件夹不参与
        const QRect icon = b->iconVisualRect().translated(b->pos());
        const QRect inter = icon.intersected(binIcon);
        if (inter.isEmpty()) continue;
        if (inter.width() * inter.height() * 4 >= icon.width() * icon.height()) return true;
    }
    return false;
}

// 更新投放目标highlight
// 作者：谭征
void DesktopMirrorWindow::updateDropTargetHighlight(bool force) {
    // C1：本函数在拖动中被逐像素调用（可达数百 Hz），而它要遍历拖动组做矩形
    // 相交、并在命中时 raise() 回收站按钮。与旁边的 iconDragHoverChanged 同款处理：节流到 40ms
    // ——高亮提示是给人看的，25Hz 已经足够顺滑。
    // force=true（收尾/还原路径）时绕过节流，保证高亮被确定性清除，不会因节流窗口而残留。
    if (m_dragActive && !force) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (m_lastDropHighlightMs != 0 && nowMs - m_lastDropHighlightMs < 40) return;
        m_lastDropHighlightMs = nowMs;
    }
    DockIconButton* bin = m_dragActive ? m_dragRecycleBin : recycleBinButton();
    if (!bin) return;
    const bool on = dragOverRecycleBin();
    bin->setDropTarget(on);
    // 高亮期间把回收站提到子控件最上层：拖动中的图标是不透明的 48px 图块，
    // 否则提示环会被它盖住，用户看不到“松手会删除”。
    if (on) bin->raise();
}

// 还原拖拽快照
// 作者：谭征
void DesktopMirrorWindow::restoreDragSnapshot() {
    for (const auto& pr : m_dragSnapshot)
        if (pr.first) pr.first->move(pr.second);
    updateDropTargetHighlight(true);     // 还原后落点已不成立，顺手清掉高亮（force：绕过节流确保清干净）
}

// 复位拖动态：清空冻结标志与拖动组/快照（裸指针）。任何会 deleteLater 掉按钮的路径
// （layoutButtons*）以及左键手势结束（onIconReleased）都必须调用，避免拖动态残留悬垂指针，
// 也避免“普通单击后 m_dragActive 永久为 true”的冻结泄漏。
// 作者：谭征
void DesktopMirrorWindow::abortActiveDrag() {
    const bool wasActive = m_dragActive;
    m_dragActive = false;
    m_dragGroup.clear();
    m_dragSnapshot.clear();
    m_dragPrimary = nullptr;
    // C1：清掉拖动期缓存（回收站指针 + 可拖删标志），避免下次拖动前的窗口期误用旧值。
    m_dragRecycleBin = nullptr;
    m_dragDeletable.clear();
    // 光标反馈也必须复位：否则拖动被中止后光标会卡在“可放置”形状上。
    if (m_dragOverBox) setDragOverBox(false);
    resetDragGhost();   // 拖拽浮层一并收起（拖动被中止 / 例行复位都不能留下残留的浮窗）
    // 复位要通知外部：MainWindow 据此清掉收纳盒里的“将插入到此处”指示线与高亮框。
    // 真拖动的收尾走 onIconDragEnd（那里自己也 emit），这里是“拖动被中止 / 释放事件丢失”的兜底路径
    // —— 少了这一发，指示线会留在最后一次悬停过的收纳盒上（松手后仍显示不消失）。
    // 判 wasActive + m_dragHoverNotified 是为了让 layoutButtons 等“例行复位”与“普通单击”
    // 都不触发通知（前者 m_dragActive 早已 false，后者从未发过悬停）。
    if (wasActive && m_dragHoverNotified) emit iconDragFinished();
    m_dragHoverNotified = false;
}

// DockIconButton::iconReleased —— 左键手势结束（含未位移普通单击、双击）。真拖动的收尾走
// onIconDragEnd，这里只负责把“可能尚未发生拖动的预备态”复位，与按下时无条件置位形成对称。
// 作者：谭征
void DesktopMirrorWindow::onIconReleased() {
    if (!m_dragActive) return;
    abortActiveDrag();
    flushPendingRefresh();   // 若期间有被延后的刷新，此刻补做（与 finishDrag 对齐）
}

// 响应图标拖拽结束
// 作者：谭征
void DesktopMirrorWindow::onIconDragEnd(DockIconButton* self, const QPoint& /*finalGlobalTopLeft*/) {
    Q_UNUSED(self);

    // 收尾（所有返回路径共用）：还原位置观感 + 清空拖动状态 + 通知外部“本次拖动已结束”。
    // 位置真值始终以 m_entries / 快照为准，随后的重排会用真值重建整屏，
    // 这里的还原只负责“松手瞬间”的正确观感（避免图标摞在落点处）。
    auto finishDrag = [this]() {
        restoreDragSnapshot();
        if (DockIconButton* bin = recycleBinButton()) bin->setDropTarget(false);
        m_dragSnapshot.clear();
        m_dragActive = false;
        m_dragGroup.clear();
        m_dragPrimary = nullptr;
        // C1：清掉拖动期缓存（下一轮拖动由 onIconPressed 重新拍）。
        m_dragRecycleBin = nullptr;
        m_dragDeletable.clear();
        if (m_dragOverBox) setDragOverBox(false);   // 光标反馈复位（见 setDragOverBox 注释）
        resetDragGhost();                           // 拖拽浮层复位（隐藏 + 丢弃位图与遮挡区）
        m_dragHoverNotified = false;
        emit iconDragFinished();
    };

    // ① 跨窗口落点：收纳盒
    // 必须排在回收站判定**之前**：两者都是“改变归属”的落点语义，但收纳盒是用户显式指定的
    // 目标 —— 拖到盒子上不该被误判成“拖到回收站删除”。
    // 系统虚拟项（回收站/我的电脑/网络…，shellPath 以 "::" 开头）没有真实文件路径，不参与收纳。
    if (m_boxDropResolver) {
        QStringList paths, names;
        for (const auto& pr : m_dragSnapshot) {
            if (!pr.first) continue;
            const QString sp = pr.first->item().shellPath;
            if (sp.isEmpty() || sp.startsWith(QLatin1String("::"))) continue;
            paths << sp;
            names << pr.first->item().displayName;
        }
        if (!paths.isEmpty() && m_boxDropResolver(paths, names, QCursor::pos())) {
            CrashTrace::mark("dragEnd:droppedToBox");
            // 已由收纳盒接管：既不删除、也不回写桌面图标位置。
            // 该图标随后会因“隐藏集合新增了它”而从 Dock 上消失（见 MainWindow 的处理）。
            finishDrag();
            return;
        }
    }

    // ② 落点在回收站上 → 删除，而不是把图标摞在回收站上（对齐 Windows 桌面）
    // 只有“可删除项”会被送进删除通道；系统图标与个人文件夹既不删、也不允许被移到回收站上叠着。
    if (dragOverRecycleBin()) {
        CrashTrace::mark("dragEnd:dropOnRecycleBin");
        QVector<DockIconButton*> targets;
        for (const auto& pr : m_dragSnapshot)
            if (pr.first && pr.first->isDeletableOnDrop()) targets.append(pr.first);
        deleteDockItems(targets);
        // 无论是否删成都还原整组位置：删成时让组内“受保护项”回到原位（它们并没有被删除），
        // 一个都没删成时更不能把图标摞在回收站上。
        finishDrag();
        return;
    }

    // ③ 常规：把整组（若多选）新位置记录到 Dock 自定义覆盖（仅 Dock 内生效，不写真实桌面）
    for (const auto& pr : m_dragSnapshot) {
        DockIconButton* b = pr.first;
        // 拖动结束吸附到最近网格：与上下左右图标行列对齐（贴近 Windows 桌面“对齐网格”）。
        const QPoint curGlobalTopLeft = snapToGridLogical(mapToGlobal(b->pos()));
        repositionDesktopIcon(b->item().shellPath, curGlobalTopLeft);
    }
    finishDrag();   // 先清空拖动态（含 dragSnapshot），再延迟重排，避免遍历已销毁按钮
    QTimer::singleShot(0, this, &DesktopMirrorWindow::layoutButtons);
}

// 把逻辑坐标下的图标左上角吸附到最近的网格格子。
// 网格 = 真实桌面图标网格：步长 m_gridStepX/Y（逻辑像素），原点 = ListView 窗口原点折算逻辑坐标
// （m_lvOriginX/Y / m_dpiFactor）。未拖动图标都按真实桌面坐标摆放，本就对齐到此网格，
// 故拖动 / 盒→Dock 落下的图标吸附到同一网格，即与上下左右图标行列对齐。
// 作者：谭征
QPoint DesktopMirrorWindow::snapToGridLogical(const QPoint& p) const {
    const double stepX = (m_gridStepX > 1.0) ? m_gridStepX : 84.0;
    const double stepY = (m_gridStepY > 1.0) ? m_gridStepY : 96.0;
    const double oX = m_lvOriginX / qMax(0.0001, m_dpiFactor);   // 网格原点（逻辑像素）
    const double oY = m_lvOriginY / qMax(0.0001, m_dpiFactor);
    const double rx = p.x() - oX;
    const double ry = p.y() - oY;
    const int gx = qRound(rx / stepX);
    const int gy = qRound(ry / stepY);
    return QPoint(qRound(oX + gx * stepX), qRound(oY + gy * stepY));
}

// 由 DockIconButton 调用：在 DesktopOrganize 模式下不再重定位真实桌面图标，仅保留接口兼容。
// 作者：谭征
void DesktopMirrorWindow::repositionDesktopIcon(const QString& shellPath,
                                                const QPoint& globalTopLeft) {
    const QString key = QDir::fromNativeSeparators(shellPath).toLower();
    if (key.isEmpty()) return;

    if (m_sourceMode == SourceMode::DesktopOrganize) {
        // 桌面整理模式下 Dock 图标按网格排列，拖动不写入 Windows 桌面位置，
        // 仅触发刷新以还原位置，避免与真实桌面产生不一致。
        Q_UNUSED(globalTopLeft);
        QTimer::singleShot(60, this, &DesktopMirrorWindow::refresh);
        return;
    }

    // 默认（NativeListView）模式：Dock 内拖动只改变 Dock 自身图标位置，
    // 绝不写回真实 Windows 桌面图标位置。记录物理屏幕坐标覆盖并存盘，
    // 随后（调用方延迟）按新位置重排 Dock 图标；真实桌面图标保持原位不动。
    const int physX = qRound(globalTopLeft.x() * m_dpiFactor);
    const int physY = qRound(globalTopLeft.y() * m_dpiFactor);
    m_customDockPos[key] = QPoint(physX, physY);
    saveCustomDockPos();
}

// 收纳盒 → Dock 的“落点即显示位置”：把某个图标的坐标真值设成“单元格中心 = 松手点”。
// 与 repositionDesktopIcon 同源（同一套 DPI / ListView 原点换算），差别只有两点：
// ① 同时更新 m_entries 的坐标 —— layoutButtonsNative() 就是按 e.screenPos 摆放按钮的，
// 不同步的话紧接着那次重排仍按旧坐标建按钮，图标会先闪在旧位置再跳过去；
// ② 不做 60ms 延迟 refresh —— 调用方随后就用 setHiddenShellPaths() 触发了一次即时重排。
// 全程只写“位置”，不创建 / 删除 / 移动 / 改名任何文件（用户明确要求：拖拽交换不碰文件）。
// 作者：谭征
void DesktopMirrorWindow::placeDockIconAtCursor(const QString& shellPath,
                                                const QPoint& globalDropPos) {
#ifdef Q_OS_WIN
    if (shellPath.isEmpty()) return;
    const QString key = QDir::fromNativeSeparators(shellPath).toLower();
    if (key.isEmpty()) return;

    // 单元格尺寸与 layoutButtonsNative 保持一致（Dock 图标一律 72x84 单元格、48x48 图标）。
    const int cellW = 72;
    const int cellH = 84;
    // 松手点对齐到“单元格中心”：与 DesktopIconButton 拖影的 hotspot（抓图中心）一致，
    // 松手瞬间图标不跳位；再夹取到 Dock 范围内，避免拖到屏幕边缘时图标被顶到屏幕外。
    QPoint topLeft = globalDropPos - QPoint(cellW / 2, cellH / 2);
    // 盒→Dock 落下也吸附到最近网格：与上下左右图标行列对齐（贴近 Windows 桌面“对齐网格”）。
    topLeft = snapToGridLogical(topLeft);
    const QRect r = rect();
    topLeft.setX(qBound(r.left(), topLeft.x(), qMax(r.left(), r.right()  - cellW + 1)));
    topLeft.setY(qBound(r.top(),  topLeft.y(), qMax(r.top(),  r.bottom() - cellH + 1)));

    // 逻辑 → 物理像素（与 repositionDesktopIcon 完全同一公式）。
    const int physX = qRound(topLeft.x() * m_dpiFactor);
    const int physY = qRound(topLeft.y() * m_dpiFactor);

    // ① 内存条目：hasPos=true 让它走“按真实坐标摆放”的第一轮，而不是被丢到底部网格里。
    for (Entry& e : m_entries) {
        if (QDir::fromNativeSeparators(e.shellPath).toLower() != key) continue;
        e.screenPos = QPoint(physX, physY);
        e.hasPos = true;
        break;
    }
    // ② 快照：refresh() → readEntriesFromListView() 只认快照里的坐标，必须同步，
    // 否则下一次刷新会把图标打回它被收进收纳盒之前的旧位置。
    updateSnapshotPosition(shellPath, QPoint(physX, physY));

    // ③ Dock 内位置覆盖：默认模式（NativeListView）下也不再写回真实 Windows 桌面，
    // 只记“Dock 自定义位置覆盖”（与 repositionDesktopIcon 一致）——Dock 内位置独立于桌面。
    // DesktopOrganize 模式本就不写桌面，此处统一处理。
    m_customDockPos[key] = QPoint(physX, physY);
    saveCustomDockPos();
#else
    Q_UNUSED(shellPath)
    Q_UNUSED(globalDropPos)
#endif
}

// 盒→Dock 拖放落点后调用：让刚落下的图标立即处于选中态，并重新武装快捷键作用域。
// 背景：拖动期间“按下”发生在收纳盒（盒按钮），Dock 的 WH_MOUSE_LL 在那一下会把
// s_hkLastClickOnDock 置 false；Dock 虽在 setHiddenShellPaths → layoutButtons 里做了选中接力，
// 但接力只找回 Dock“原有选中”，新落下的图标不在集合里。两者叠加 → 松手后直接按 F2/Delete
// 会被 hotkeyContextOk 以 “empty selection / last click not on dock” 挡下。
// 作者：谭征
void DesktopMirrorWindow::selectIconByShellPathAndArm(const QString& shellPath) {
    if (shellPath.isEmpty()) return;
    const QString key = QDir::fromNativeSeparators(shellPath).toLower();
    DockIconButton* target = nullptr;
    for (DockIconButton* b : m_buttons) {
        if (!b) continue;
        if (QDir::fromNativeSeparators(b->item().shellPath).toLower() == key) { target = b; break; }
    }
    DiagTrace::log(QStringLiteral("[dock] selectIconByShellPathAndArm found=%1 name='%2'")
                       .arg(DiagTrace::boolStr(target != nullptr))
                       .arg(target ? target->item().displayName : shellPath));
    if (target) selectExclusive(target);
    // 松手落在 Dock 面上 = 一次“与 Dock 的交互”：把作用域标志重新置真，让随后的 F2/Delete
    // 立即命中。先重装钩子、再置真：ensureHotkeyHooks 内部按 keepArmed=(owner==this && flag)
    // 保留旧标志，若装钩失败或 owner 尚未指向本窗口会把 flag 清回 false —— 故置真必须放在
    // 重装之后，保证无论哪种分支最终都为 true（直到下一次落在别处的按下把它清掉）。
    ensureHotkeyHooks();
    s_hkLastClickOnDock = true;
}

// —— 应用级点击守卫：内联重命名打开时，任何“落在编辑框之外”的鼠标按下都立即提交 ——
// 为什么不能只靠 DesktopMirrorWindow::mousePressEvent：
// Dock 主窗口带 Qt::WindowDoesNotAcceptFocus / WS_EX_NOACTIVATE，重命名编辑框是一个独立顶层窗口。
// 编辑框开启期间，落在 dock 空白处、左侧快捷栏、收纳盒或其它窗口上的鼠标按下，未必都会走到本窗口的
// mousePressEvent（可能被某个子控件先命中、被别的窗口接收，或该窗口本身不具备接收条件），于是出现
// “回车 / Esc 能提交，但鼠标点 dock 空白处提交不了”。把判定放到应用层（qApp 事件过滤器）后，
// 只要“按下了鼠标且落点不在编辑框内”就一律提交，不受窗口/控件路由影响，完全对齐 Windows 桌面。
// 作者：谭征
bool DesktopMirrorWindow::eventFilter(QObject* watched, QEvent* event) {
    const QEvent::Type t = event->type();
    if (t == QEvent::MouseButtonPress || t == QEvent::MouseButtonDblClick) {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        const QPoint gp = me->globalPos();
        // 唯一判据是“按下点的屏幕坐标是否落在编辑框矩形内”，不再看事件接收对象。
        // 原因：鼠标事件在 Qt 中会先派发给顶层 QWidgetWindow（parent 为空、不在编辑框 parent 链上），
        // 用对象判据会把“点进编辑框”误判成“点别处”→ 一按下就提交、框根本没法用。
        // 先收集目标再提交：提交会销毁编辑框并可能连锁触发信号/刷新，避免在遍历中改动状态。
        QVector<DockIconButton*> targets;
        for (DockIconButton* b : m_buttons) {
            if (b && b->isEditing() && !b->editorJustOpened() && !b->editorQtRectContains(gp))
                targets.append(b);
        }
        if (m_editingBtn && m_editingBtn->isEditing() && !m_editingBtn->editorJustOpened()
            && !m_editingBtn->editorQtRectContains(gp) && !targets.contains(m_editingBtn)) {
            targets.append(m_editingBtn);
        }
        for (DockIconButton* b : targets) {
            if (b && b->isEditing()) b->commitRename();
        }
    }
    return QWidget::eventFilter(watched, event);
}

// 标题栏本身不承载任何编辑交互，不会与输入/选择冲突。
// 作者：谭征
void DesktopMirrorWindow::mousePressEvent(QMouseEvent* event) {
    // 任何落在本窗口上的按下都先提交正在进行的重命名（对齐 Windows：点别处即完成命名）。
    // 必须放在“是否点在图标上”的早退之前——否则点到非按钮的覆盖层（例如残留的框选橡皮筋）
    // 会漏提交。
    commitActiveRename();
    // 命中判定用“图标/标签可视区”（iconAtGlobal）而非 childAt()：
    // 按钮单元格 72x84 远大于图标 48x48 + 标签，childAt() 会把“点在图标旁的空白”也算在按钮头上
    // → 空白按下被当成点中该项，选中状态永远清不掉。只有真点在图标可视区上才交给子控件。
    if (iconAtGlobal(event->globalPos())) {
        QWidget::mousePressEvent(event);
        return;
    }
    // 空白处按下同样算“与 Dock 交互”：重装一次快捷键钩子（自愈），
    // 让框选出的选中集合也能立刻用 F2 / Delete。
    ensureHotkeyHooks();
    if (event->button() == Qt::RightButton) {
        // 空白处右键：先清掉选中，再弹出桌面空白处原生菜单（查看/排序/刷新/新建等）——
        // 与 Windows 桌面一致（右键空白会取消选择）。
        // 注意顺序：清选中必须在 showDesktopEmptyMenu() **之前**同步完成。showDesktopEmptyMenu
        // 是原生模态循环（TrackPopupMenu），期间 m_menuOpen 为 true，系统级鼠标钩子那条兜底
        // 通道会在循环内被 m_menuOpen 挡掉 → 只能靠这里的同步清理保证生效。
        clearSelectionOnBlankPress(event->globalPos());
        // 同上：菜单期间挡住重排（菜单里的“新建”会触发刷新，若在循环内重建按钮，
        // 随后的自动重命名框会挂在不存在的按钮上）。
        m_menuOpen = true;
        showDesktopEmptyMenu(event->globalPos());
        m_menuOpen = false;
        flushPendingRefresh();
    } else if (event->button() == Qt::LeftButton) {
        // 空白处左键：清空选中并开始画框多选（对齐 Windows 桌面）。
        // Ctrl 加选时不清空已有选中（基线保留，框选只做并集）。
        const bool additive = event->modifiers().testFlag(Qt::ControlModifier);
        if (!additive) clearSelectionOnBlankPress(event->globalPos());
        beginRubber(event->globalPos(), additive);
    }
    event->accept();
    QWidget::mousePressEvent(event);
}

// 鼠标移动事件
// 作者：谭征
void DesktopMirrorWindow::mouseMoveEvent(QMouseEvent* event) {
    if (m_rubbering) updateRubber(event->globalPos());
    QWidget::mouseMoveEvent(event);
}

// 鼠标松开事件
// 作者：谭征
void DesktopMirrorWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (m_rubbering) endRubber();
    QWidget::mouseReleaseEvent(event);
}

// paint事件
// 作者：谭征
void DesktopMirrorWindow::paintEvent(QPaintEvent* /*event*/) {
    // 透明背景：仅绘制图标（由子 DockIconButton 完成），壁纸透过本窗口透出。
    // 例外：画框多选的选择框由本窗口自绘（见下）——本窗口是 WA_TranslucentBackground 的透明层，
    // 自绘可避免额外子窗口带来的 z 序/裁剪问题，也不会出现“框被图标挡住的整块不透明区”。
    if (!m_rubbering || m_rubberRect.isNull()) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    if (m_rubberCreatesBox) {
        // 「在桌面空白处绘制创建收纳盒」：用当前主题强调色 + 框内提示，明确"这是在建盒，不是框选"。
        const QColor ac = Theme::accentCyan();
        QColor fill = ac; fill.setAlpha(46);
        QColor line = ac; line.setAlpha(200);
        p.fillRect(m_rubberRect, fill);
        p.setPen(QPen(line, 1));
        p.setBrush(Qt::NoBrush);
        const QRect rr = m_rubberRect.adjusted(0, 0, -1, -1);
        p.drawRect(rr);
        if (rr.width() > 130 && rr.height() > 44) {
            p.setPen(QColor(255, 255, 255, 225));
            p.drawText(rr.adjusted(8, 6, -8, 0), Qt::AlignLeft | Qt::AlignTop,
                       QStringLiteral("松开即创建收纳盒"));
        }
        return;
    }
    // Windows 10 桌面的框选观感：淡蓝半透明填充 + 略深一点的细边框。
    p.fillRect(m_rubberRect, QColor(51, 153, 255, 56));
    p.setPen(QPen(QColor(51, 153, 255, 190), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(m_rubberRect.adjusted(0, 0, -1, -1));
}

// 缩放事件
// 作者：谭征
void DesktopMirrorWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (size() != m_lastSize) {
        m_lastSize = size();
        // 分辨率/工作区变化后重排（防抖）
        QTimer::singleShot(200, this, &DesktopMirrorWindow::refresh);
    }
}

// 显示事件
// 作者：谭征
void DesktopMirrorWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    QScreen* ps = QApplication::primaryScreen();
    if (ps) {
        setGeometry(ps->geometry());
        m_lastSize = size();   // 避免 showEvent 触发的 resize 误判为分辨率变化而自刷新
    }
    parkAllTargetsAtBottom();   // 三窗口一并压底（Dock showEvent 后可能被系统提层，强制恢复）
    // 若原生窗口被 Qt 重建（winId 变化），重新注册桌面变更通知，保证同步不失效
    unregisterChangeNotify();
    registerChangeNotify();
}

// change事件
// 作者：谭征
void DesktopMirrorWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::WindowStateChange) {
        // “显示桌面”按钮或 Win+D 可能把 Dock 最小化；这里立即恢复，保持 Dock 始终显示。
        if (windowState() & Qt::WindowMinimized) {
            QTimer::singleShot(0, this, &DesktopMirrorWindow::showNormal);
        }
    }
    QWidget::changeEvent(event);
}

// 助手面板顶到普通程序之上再被压回的「一闪而过」。show desktop 期放行。
// 作者：谭征
bool DesktopMirrorWindow::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    MSG* msg = static_cast<MSG*>(message);
    if (msg->message == WM_DESKTOP_NOTIFY) {
        // 桌面增删改名/属性变化：防抖刷新（250ms）
        m_refreshTimer->start(250);
        return true;
    }
    if (msg->message == WM_APP + 1) {
        // 系统线程投递到 GUI 线程：统一恢复全部被“显示桌面”隐藏的常驻窗口
        restoreAllTargets();
        if (result) *result = 0;
        return true;
    }
    if (msg->message == WM_SYSCOMMAND) {
        // 拒绝最小化、最大化、关闭等系统命令，尤其防止“显示桌面”按钮触发最小化。
        WPARAM cmd = msg->wParam & 0xFFF0;
        if (cmd == SC_MINIMIZE || cmd == SC_MAXIMIZE || cmd == SC_CLOSE) {
            if (result) *result = 0;
            return true;
        }
    }
    if (msg->message == WM_SIZE) {
        // 若系统仍试图最小化 Dock（如“显示桌面”按钮的某些路径），立即恢复。
        if (msg->wParam == SIZE_MINIMIZED) {
            QTimer::singleShot(0, this, &DesktopMirrorWindow::showNormal);
            if (result) *result = 0;
            return true;
        }
    }
    // WM_WINDOWPOSCHANGING：常态下由 clampBandZOrder 在 z 序变更【应用之前】拦截提层
    // （点击/Qt show/系统激活把 Dock 顶到普通程序之上 →「一闪而过」的根因修复）。
    // show desktop 激活期（s_desktopActive）守卫放行，topmost 状态机的 HWND_TOPMOST
    // 不受影响（enterDesktop 先置状态再置顶，时序保证）。
    if (msg->message == WM_WINDOWPOSCHANGING) {
        WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(msg->lParam);
        clampBandZOrder(msg->hwnd, wp);   // 已在应用前改写 hwndInsertAfter/SWP_NOZORDER
        wp->flags |= SWP_NOACTIVATE;      // 不抢焦点
        return false;                     // 走默认处理链
    }
    if (msg->message == WM_MOUSEACTIVATE) {
        // 点击本窗口不激活、不抢前台，保持最底层
        if (result) *result = MA_NOACTIVATE;
        return true;
    }
    if (msg->message == WM_ACTIVATE || msg->message == WM_ACTIVATEAPP) {
        // 拒绝任何激活请求，保持最底层
        if (result) *result = 0;
        return true;
    }
    if (msg->message == WM_SETFOCUS) {
        // 焦点交还真实桌面（Progman），本窗口永持焦点
        HWND hProgman = FindWindowW(L"Progman", L"Program Manager");
        if (hProgman) SetFocus(hProgman);
        if (result) *result = 0;
        return true;
    }
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

// 注册change通知
// 作者：谭征
void DesktopMirrorWindow::registerChangeNotify() {
#ifdef Q_OS_WIN
    // 必须传非递归（fRecursive=FALSE）！
    // 项目部署形态：程序自身与运行时日志位于 `F:\Desktop\DestopTools\DestopTools\release\`
    // （用户实际桌面根 = `CSIDL_DESKTOP` = `F:\Desktop`）。若设为 TRUE，**任何写入子目录文件的动作
    // 都会以 SHCNE_UPDATEITEM 形式回弹到这个监听**，进而触发 refresh() → layoutButtons() 销毁所有
    // DockIconButton，于是变成一个 **500ms 周期的自激重建循环**：写入诊断日志 → SHCN 收到通知 →
    // 250ms 防抖 → refresh 重建图标 → 重建出的图标在析构时又写日志 → 再 SHCN → ……
    // 直接外部症状：Dock 图标"点中立刻失选"（每 500ms 重建销毁一次）、CPU 高负载、trace.log 暴涨。
    // 设为 FALSE 后：
    // 用户在桌面**根目录**上新建/重命名/删除/粘贴文件 → 仍会收到通知（Dock 镜像刷新）
    // 用户/程序修改**子目录**下的文件（如我们的日志、构建产物）→ 不再回弹通知
    PIDLIST_ABSOLUTE pid = nullptr;
    if (FAILED(SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOP, &pid)) || !pid)
        return;
    SHChangeNotifyEntry entry = {0};
    entry.pidl = pid;
    entry.fRecursive = FALSE;   // 见上方注释：不能递归，否则与自身日志写入形成自激循环
    m_changeId = SHChangeNotifyRegister(
        (HWND)winId(),
        SHCNRF_ShellLevel | SHCNRF_InterruptLevel,
        SHCNE_CREATE | SHCNE_DELETE | SHCNE_RENAMEITEM | SHCNE_MKDIR |
        SHCNE_RMDIR | SHCNE_ATTRIBUTES | SHCNE_UPDATEITEM | SHCNE_INTERRUPT |
        SHCNE_ASSOCCHANGED,
        WM_DESKTOP_NOTIFY, 1, &entry);
    // pid 由 SHChangeNotifyRegister 内部接管，无需 CoTaskMemFree
#endif
}

// 注销change通知
// 作者：谭征
void DesktopMirrorWindow::unregisterChangeNotify() {
#ifdef Q_OS_WIN
    if (m_changeId) {
        SHChangeNotifyDeregister(m_changeId);
        m_changeId = 0;
    }
#endif
}

// 显示桌面空菜单
// 作者：谭征
void DesktopMirrorWindow::showDesktopEmptyMenu(const QPoint& globalPos) {
#ifdef Q_OS_WIN
    const HRESULT cohr = CoInitializeEx(nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninit = (cohr == S_OK);
    IShellFolder* psf = nullptr;
    if (SUCCEEDED(SHGetDesktopFolder(&psf))) {
        IContextMenu* pcm = nullptr;
        if (SUCCEEDED(psf->GetUIObjectOf((HWND)winId(), 0, nullptr,
                                         IID_IContextMenu, nullptr,
                                         reinterpret_cast<void**>(&pcm)))) {
            HMENU hMenu = CreatePopupMenu();
            if (hMenu && SUCCEEDED(pcm->QueryContextMenu(
                    hMenu, 0, 1, 0x7FFF, CMF_EXPLORE))) {
                // 授权本进程前台：菜单首次点击不被吞（无需提升 Dock，避免 dock 闪烁，复刻 360）。
                AllowSetForegroundWindow(GetCurrentProcessId());
                const UINT id = TrackPopupMenu(hMenu,
                    TPM_RETURNCMD | TPM_RIGHTBUTTON, globalPos.x(), globalPos.y(),
                    0, (HWND)winId(), nullptr);
                if (id > 0) {
                    CMINVOKECOMMANDINFOEX cmi;
                    ZeroMemory(&cmi, sizeof(cmi));
                    cmi.cbSize = sizeof(cmi);
                    cmi.fMask = CMIC_MASK_UNICODE;
                    cmi.hwnd = (HWND)winId();
                    cmi.lpVerbW = MAKEINTRESOURCEW(id - 1);
                    cmi.lpVerb = MAKEINTRESOURCEA(id - 1);
                    cmi.nShow = SW_SHOWNORMAL;
                    pcm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&cmi));
                    // “新建”等会落到真实桌面 -> SHChangeNotify -> 本窗口刷新
                }
                DestroyMenu(hMenu);
            }
            pcm->Release();
        }
        psf->Release();
    }
    if (uninit) CoUninitialize();
#endif
}

// 因此不会触发 LVS_AUTOARRANGE 重排，也就不会刷 Explorer、不会关掉已打开的文件夹窗口。
// 作者：谭征
void DesktopMirrorWindow::hideNativeDesktop() {
#ifdef Q_OS_WIN
    QVector<HWND> lists = enumDesktopLists();
    if (!lists.isEmpty()) m_hLV = lists.first();
    for (HWND lv : lists) ShowWindow(lv, SW_HIDE);
#endif
}

// 还原原生桌面
// 作者：谭征
void DesktopMirrorWindow::restoreNativeDesktop() {
#ifdef Q_OS_WIN
    QVector<HWND> lists = enumDesktopLists();
    for (HWND lv : lists) ShowWindow(lv, SW_SHOW);
#endif
}

// 一次性设置 Dock 自身的窗口样式（WS_EX_NOACTIVATE + 去 WS_MINIMIZEBOX/MAXIMIZEBOX/THICKFRAME），
// 让"显示桌面"按钮无法把 Dock 当作普通窗口最小化/隐藏。Z 序压底已统一由 parkAllTargetsAtBottom 处理。
// 注意：EX/STYLE 设置只对 Dock 自身做一次（无副作用），不要重复做。
// 作者：谭征
void DesktopMirrorWindow::ensureDockWindowStyle() {
#ifdef Q_OS_WIN
    HWND h = (HWND)winId();
    if (!h || !IsWindow(h)) return;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (!(ex & WS_EX_NOACTIVATE)) {
        SetWindowLongW(h, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
        SetWindowPos(h, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }
    LONG style = GetWindowLongW(h, GWL_STYLE);
    LONG unwanted = WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME;
    if (style & unwanted) {
        SetWindowLongW(h, GWL_STYLE, style & ~unwanted);
        SetWindowPos(h, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }
#endif
}

// best列表视图
// 作者：谭征
void* DesktopMirrorWindow::bestListView() {
#ifdef Q_OS_WIN
    return findBestDesktopListView();
#else
    return nullptr;
#endif
}

// dumpdebug
// 作者：谭征
void DesktopMirrorWindow::dumpDebug(const QString& tag) {
    // 测试日志已停用：原实现用 QFile 写 dock_debug.log，在 WINEVENTPROC 钩子回调上下文做文件 I/O 会崩溃。
    // 现已停用，dock_debug.log 不再生成，崩溃消除。z 序守卫逻辑（clampBandZOrder 等）不受影响。
    Q_UNUSED(tag);
}
