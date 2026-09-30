/*
 * @file mainwindow.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "mainwindow.h"
#include "icongridwindow.h"
#include "assistantwindow.h"
#include "desktopmirrorwindow.h"
#include "sidepanelwidget.h"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include "desktopscanner.h"
#include "settingsmanager.h"
#include "categorystore.h"
#include "shellops.h"        // 粘贴 / 粘贴快捷方式：桌面命名空间原生动词
#include "crashtrace.h"      // 跨窗口拖拽："落点在收纳盒上→否决删除分支"埋点
#include "diagtrace.h"       // 跨窗口拖拽：盒 → Dock 的落点判定诊断日志
#include "theme.h"
#include "windowsnap.h"      // 顶部留白：恢复窗口几何时不许贴住工作区顶边
#include "settingcenterdialog.h"
#include "glassmessagebox.h"
#include "glassinputdialog.h"
#include "filesearchwidget.h"   // 「快速搜索」文件搜索窗口
#include "shutdowntimerwindow.h"   // 「定时关机」窗口
#include "wallpaperwindow.h"         // 「壁纸」窗口
#include <QMessageBox>
#include <QCloseEvent>
#include <QMenu>
#include <QAction>
#include <QLineEdit>
#include <QCursor>
#include <QDesktopServices>
#include <QUrl>
#include <QProcess>
#include <QStandardPaths>
#include <QScreen>
#include <QSet>
#include <QTimer>
#include <QPointer>       // 跨窗口拖拽：延迟执行的 lambda 里持有收纳盒窗口需防野指针
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QSettings>
#include <QUuid>         // 收纳盒持久化：稳定唯一 id

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ShellAPI.h>   // ShellExecuteW（注册表以管理员身份启动）
#endif

// 特殊命名空间项（我的电脑/回收站/网络）归入的常驻分类名
static const QString kSystemCategory = QStringLiteral("系统");

#ifdef Q_OS_WIN
// 定位承载桌面图标的 SysListView32 控件。层级：Progman/WorkerW -> SHELLDLL_DefView -> SysListView32。
// 仅隐藏这一层（而非整个 Progman），可保留桌面右键菜单，同时把全部桌面图标一次性藏掉。
static HWND findDesktopListView() {
    HWND prog = FindWindowW(L"Progman", nullptr);
    HWND def = prog ? FindWindowExW(prog, nullptr, L"SHELLDLL_DefView", nullptr) : nullptr;
    if (!def) {
        // Win10/11 下可见桌面有时挂在 WorkerW 下，枚举顶层窗口定位 SHELLDLL_DefView
        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            wchar_t cls[64] = {0};
            GetClassNameW(hwnd, cls, 64);
            if (wcscmp(cls, L"WorkerW") == 0) {
                HWND dv = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
                if (dv) { *reinterpret_cast<HWND*>(lParam) = dv; return FALSE; }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&def));
    }
    return def ? FindWindowExW(def, nullptr, L"SysListView32", nullptr) : nullptr;
}

// （选择性隐藏方案已撤销：恢复为整屏 SW_HIDE 接管 + 系统壁纸 Dock，见 hideNativeDesktop/restoreNativeDesktop）

// 作者：谭征
void MainWindow::hideNativeDesktop() {
    // 整屏隐藏原生桌面图标层。SW_HIDE 是对整个控件的一次性隐藏，不写任何单项 LVIS_HIDDEN，
    // 因此不会触发 LVS_AUTOARRANGE 重排 -> 不刷 Explorer、不关已打开文件夹窗口。
    HWND lv = (m_desktopListView && IsWindow((HWND)m_desktopListView))
                  ? (HWND)m_desktopListView : findDesktopListView();
    if (!lv) return;
    m_desktopListView = lv;
    ShowWindow(lv, SW_HIDE);
}

// 还原原生桌面
// 作者：谭征
void MainWindow::restoreNativeDesktop() {
    HWND lv = (m_desktopListView && IsWindow((HWND)m_desktopListView))
                  ? (HWND)m_desktopListView : findDesktopListView();
    if (lv) ShowWindow(lv, SW_SHOW);
    m_desktopListView = lv;
}

// 切换原生桌面
// 作者：谭征
void MainWindow::toggleNativeDesktop() {
    HWND lv = (m_desktopListView && IsWindow((HWND)m_desktopListView))
                  ? (HWND)m_desktopListView : findDesktopListView();
    if (!lv) return;
    m_desktopListView = lv;
    if (IsWindowVisible(lv)) ShowWindow(lv, SW_HIDE);
    else                     ShowWindow(lv, SW_SHOW);
}

// 助手面板顶到普通程序之上再被压回的「一闪而过」。show desktop 期放行。
// 作者：谭征
bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, long* result) {
    MSG* msg = static_cast<MSG*>(message);
    // Explorer 重启会重建桌面 SysListView32；事件驱动地重新隐藏，无需轮询定时器。
    // 用字面量避免依赖未导出的壳钩子宏（WM_SHELLHOOK=0x0406, HSHELL_WINDOWCREATED=1）。
    if (msg->message == 0x0406 && msg->wParam == 1) {
        HWND created = (HWND)msg->lParam;
        wchar_t cls[64] = {0};
        GetClassNameW(created, cls, 64);
        if (wcscmp(cls, L"SysListView32") == 0) {
            HWND parent = GetParent(created);
            if (parent) {
                wchar_t pcls[64] = {0};
                GetClassNameW(parent, pcls, 64);
                if (wcscmp(pcls, L"SHELLDLL_DefView") == 0) {
                    ShowWindow(created, SW_HIDE);
                    m_desktopListView = created;
                }
            }
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

// 启动无需提权的外部工具（记事本/计算器/命令行/文件管理等），像 Win+R「运行」对话框那样
// 由 shell 前台激活其窗口，避免被本程序接管层压住或静默失败。workDir 为空则沿用当前目录；
// 返回是否成功发起（<=32 表示 ShellExecute 失败）。 不用 SetWindowPos(HWND_TOPMOST)：
// 本程序自身可能也有 topmost 接管层，互设 topmost 反而会把工具窗口盖住（现象=点了“打不开”）。
static bool launchTool(const QString& program, const QString& workDir,
                       const QString& params = QString()) {
    const HINSTANCE r = ShellExecuteW(nullptr, L"open",
                                     reinterpret_cast<LPCWSTR>(program.utf16()),
                                     params.isEmpty() ? nullptr
                                                      : reinterpret_cast<LPCWSTR>(params.utf16()),
                                     workDir.isEmpty() ? nullptr
                                                      : reinterpret_cast<LPCWSTR>(workDir.utf16()),
                                     SW_SHOWNORMAL);
    return (reinterpret_cast<INT_PTR>(r) > 32);
}
#endif

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("桌面助手 - 协调窗口"));
    // 协调窗口本身不显示，只用来管理两个子窗口。
    // 用 Qt::Tool（对应 Win32 WS_EX_TOOLWINDOW）确保这个 1×1 隐藏管理器：
    // ① 不占任务栏按钮；② 不出现在 Alt+Tab 任务切换里。
    // 其子窗口 m_gridWindow / m_assistantWindow 以 Qt::Window 创建，仍各自拥有
    // 正常的任务栏按钮（Qt 会为它们加 WS_EX_APPWINDOW），不受此处影响。
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_ShowWithoutActivating);
    resize(1, 1);

    // 任意子窗口（整理窗口/收纳盒/桌面助手）关闭都不自动退出程序，
    // 仅在菜单“退出”时显式 qApp->quit()
    QApplication::setQuitOnLastWindowClosed(false);

    QFont themeFont = Theme::appFont();
    if (!themeFont.family().isEmpty()) {
        QApplication::setFont(themeFont);
    }

    // 注意：桌面挂件窗口必须以 nullptr 为父（独立顶层窗口），绝不能以隐藏的
    // MainWindow 为父。否则它们会成为“被隐藏所有者拥有的工具窗口”，在 Win32 下
    // 随所有者一起被隐藏、显式 show() 也救不回——表现为“收纳盒/桌面助手不显示”。
    m_gridWindow = new IconGridWindow(nullptr);
    // 跨窗口拖拽：收纳盒 → Dock。主整理窗口与每个独立收纳盒窗口都要装（见 createNewBox）。
    installBoxDropResolver(m_gridWindow);
    // 读取持久化的“快捷方式”强制策略：若用户曾在主窗口解散“快捷方式”，
    // 重启后保持解散状态，不再强制补回该分类
    {
        SettingsManager sm;
        m_gridWindow->setForceShortcutCategory(
            sm.loadValue(QStringLiteral("mainForceShortcutCategory"), true).toBool());
    }
    m_assistantWindow = new AssistantWindow(nullptr);

    connect(m_gridWindow, &IconGridWindow::menuRequested,
            this, &MainWindow::showCategoryMenu);
    connect(m_gridWindow, &IconGridWindow::requestNewCategory,
            this, &MainWindow::onRequestNewCategory);
    connect(m_gridWindow, &IconGridWindow::requestNewBox,
            this, [this]() { createNewBox(); });
    connect(m_gridWindow, &IconGridWindow::categoryChanged,
            this, &MainWindow::onCategoryChanged);
    connect(m_gridWindow, &IconGridWindow::requestDeleteCategory,
            this, &MainWindow::deleteCategory);
    connect(m_gridWindow, &IconGridWindow::requestRenameCategory,
            this, &MainWindow::renameCategoryByName);
    connect(m_gridWindow, &IconGridWindow::requestRefresh,
            this, [this]() { refreshDesktop(); });
    // 网格/收纳盒内图标被删除或重命名后，同样要同步本地持久化数据：
    // 分类库（path → 分类）以绝对路径为键，Dock 侧有一条同步链路，网格侧必须有对等的一条，
    // 否则会出现“在收纳盒里删掉的文件，重启后又在盒子里 / 点不动”这类幽灵记录。
    connect(m_gridWindow, &IconGridWindow::itemsDeleted,
            this, &MainWindow::onDockItemsDeleted);
    connect(m_gridWindow, &IconGridWindow::itemRenamed,
            this, &MainWindow::onDockItemRenamed);
    connect(m_assistantWindow, &AssistantWindow::searchTextChanged,
            this, &MainWindow::onSearchChanged);
    connect(m_assistantWindow, &AssistantWindow::toolTriggered,
            this, &MainWindow::onToolTriggered);
    // 「快速搜索」文件搜索窗口：常驻单例，由 快速搜索 按钮与助手面板搜索框共同唤起。
    m_fileSearch = new FileSearchWidget(this);
    // 「定时关机」窗口：常驻单例。任务调度器活在本窗口内（1s 心跳），即使窗口从未被打开，
    // 已保存的任务也会在到点时执行。
    m_shutdownTimer = new ShutdownTimerWindow(this);
    m_wallpaper = new WallpaperWindow(this);   // 「壁纸」：更换桌面壁纸窗口（常驻单例）
    connect(m_assistantWindow->panel(), &SidePanelWidget::requestFileSearch,
            m_fileSearch, &FileSearchWidget::showSearch);
    if (m_assistantWindow->panel()) {
        connect(m_assistantWindow->panel(), &SidePanelWidget::requestCreateNewBox,
                this, [this]() { createNewBox(nullptr); });
        connect(m_assistantWindow->panel(), &SidePanelWidget::requestDockReset,
                this, [this]() { if (m_dock) m_dock->resetDock(); });
        connect(m_assistantWindow->panel(), &SidePanelWidget::requestToggleNativeDesktop,
                this, [this]() { toggleNativeDesktop(); });
        connect(m_assistantWindow->panel(), &SidePanelWidget::mainWindowVisibilityChanged,
                this, &MainWindow::applyAssistantVisibility);
    }

    // 两个子窗口发生移动、缩放、折叠/展开或关闭时，防抖保存几何到本地
    m_geometrySaveTimer = new QTimer(this);
    m_geometrySaveTimer->setSingleShot(true);
    connect(m_geometrySaveTimer, &QTimer::timeout, this, &MainWindow::saveLayout);
    connect(m_gridWindow, &IconGridWindow::windowGeometryChanged,
            this, &MainWindow::requestSaveLayout);
    connect(m_assistantWindow, &AssistantWindow::windowGeometryChanged,
            this, &MainWindow::requestSaveLayout);

    // 确保程序任何退出路径都会保存当前窗口布局
    connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::saveLayout);
    // 退出时还原原生桌面图标层（整屏 SW_SHOW），避免桌面图标永久消失
    connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::restoreNativeDesktop);

    loadLayout();
    positionWindows();

    // 全屏 Dock 镜像层：必须最先加载运行（先于收纳盒/桌面助手），作为 Windows 桌面的
    // 存在并始终位于最底层。Dock 界面/数据/图标先完全加载完成，收纳盒再加载。
    // 数据源改为 NativeListView：1:1 镜像真实桌面的 SysListView32 图标位置，
    // 保证 Dock 内图标位置、大小、清晰度均与 Windows 桌面一致。
    m_dock = new DesktopMirrorWindow(nullptr);
    m_dock->setSourceMode(DesktopMirrorWindow::SourceMode::NativeListView);
    connect(qApp, &QCoreApplication::aboutToQuit, m_dock, &DesktopMirrorWindow::stopMirror);
    // Dock 首次镜像就绪（界面显示 + 数据读取 + 图标布局完成）后，再让收纳盒从 Dock 读取图标加载，
    // 严格保证 Dock 先于收纳盒就绪（满足“dock 先加载完成，再加载收纳盒”）。
    connect(m_dock, &DesktopMirrorWindow::firstMirrorReady,
            this, [this]() { refreshDesktop(false, false); restoreUserBoxes(); });
    // Dock 内联重命名落盘后同步收纳盒/网格侧的路径与分类记录（改后缀同样适用）：
    // 不同步的话，收纳盒里那一项仍指向已不存在的旧路径（点不动），
    // 且分类库里旧路径记录成孤儿，重启/整理时该项的归属会丢失。
    connect(m_dock, &DesktopMirrorWindow::itemRenamedOnDesktop,
            this, &MainWindow::onDockItemRenamed);
    // Dock 内 Delete 快捷键删除（文件已进回收站）后，清理收纳盒/网格侧以路径为键的残留记录
    connect(m_dock, &DesktopMirrorWindow::itemsDeletedOnDesktop,
            this, &MainWindow::onDockItemsDeleted);
    // 「在桌面空白处绘制创建收纳盒」（设置中心 → 快捷操作，开关由 Dock 侧消费）：
    // 桌面空白处拉出的框松手 → 按该框新建收纳盒，并把框内图标一并收进去。
    connect(m_dock, &DesktopMirrorWindow::boxRegionDrawn,
            this, &MainWindow::createBoxFromDockRegion);

    // —— 跨窗口拖拽：Dock → 收纳盒 ——
    // 落点解析器（带返回值，必须同步回答"这一松手是否已被收纳盒接管"）：
    // 返回 true 后 Dock 会跳过它自己的两条落盘路径（拖到回收站=删除 / 回写桌面图标位置）。
    m_dock->setBoxDropResolver([this](const QStringList& paths, const QStringList& names,
                                      const QPoint& pos) -> bool {
        const BoxDropTarget t = resolveBoxAt(pos);
        if (t.valid()) {
            // 插入下标：按**松手光标位置**在该分类的图标网格上算出（-1 = 没落在网格上 → 追加末尾）。
            // 与拖动过程中那条指示线共用同一个函数，所以“线画在哪、图标就插到哪”。
            const int idx = t.window ? t.window->externalDropIndexAt(pos) : -1;
            scheduleDockItemsIntoBox(paths, names, t.category, t.window, idx);
            return true;
        }
        // 结构性保险（用户要求：整个拖拽交换过程绝不删除文件）：
        // 松手点落在某个**真实盒子**上、却没能解析出有效分类（例如该盒的分类不参与收纳）时，
        // 仍然把这次拖拽「认领」下来 —— 只返回 true、什么都不做，
        // 绝不让一个「拖到盒子上」的落点掉进回收站删除通道。
        // 判据必须是「真实目标」而非「窗口矩形」：全屏接管时窗口铺满整屏，
        // 用矩形判会把“拖到壁纸上”也误认领，导致 Dock 图标再也无法在桌面重排。
        if (anyRealDropTargetAt(pos)) {
            CrashTrace::mark("dragEnd:boxAreaVeto-deleteSuppressed");
            return true;
        }
        return false;
    });
    // 拖动过程中的悬停提示：让光标下方的收纳盒窗口亮起"松手即收进此处"的边框。
    connect(m_dock, &DesktopMirrorWindow::iconDragHoverChanged,
            this, &MainWindow::onDockIconDragHover);
    connect(m_dock, &DesktopMirrorWindow::iconDragFinished,
            this, &MainWindow::onDockIconDragFinished);
    m_dock->startMirror();

    // 兜底：若 Dock 因异常（如非 Windows 平台或快照始终失败）未发射 firstMirrorReady，
    // 2.5s 后强制以桌面扫描加载收纳盒，避免空壳。
    QTimer::singleShot(2500, this, [this]() {
        if (!m_dock || m_dock->entries().isEmpty()) {
            // C：全量重扫是重活（逐图标跨进程 SendMessageW(VLM_GETITEMW/
            // GETITEMPOSITION) + VirtualAllocEx/ReadProcessMemory，实测数百毫秒）。本定时器
            // 触发时 Dock 的钩子早已在位，若不摘钩，这段重扫会把全系统鼠标一起冻住。
            DesktopMirrorWindow::suspendHotkeyHooks();
            IconGridWindow::suspendGridHooks();
            refreshDesktop(false, false);
            restoreUserBoxes();
            IconGridWindow::resumeGridHooks();
            DesktopMirrorWindow::resumeHotkeyHooks();
        }
    });

    m_gridWindow->show();

#ifdef Q_OS_WIN
    // 把“应常驻”的窗口注册进抗“显示桌面”守卫（Win+D / 任务栏显示桌面按钮 / Win+M 下保持可见）。
    // 守卫 = T5 事件钩子（CLOAKED/HIDE/MINIMIZESTART）即时恢复 + 10ms 轮询兜底。
    // 注意：常隐的 MainWindow 协调窗口【不注册】，避免被误复活；
    // 用户主动 hide() 的窗口（解散主面板/收纳盒）通过 unregisterCloakTarget 注销，同样不会被强制复活。
    // 已撤回 T1 桌面嵌入：SetParent 到 WorkerW 会让 Qt 顶层窗口变桌面子窗口，坐标/裁剪错乱导致窗口不可见。
    DesktopMirrorWindow::registerCloakTarget(m_gridWindow);
#endif

    // 「显示主界面/隐藏主界面」：按上次设置决定助手面板是否显示。
    // 先判后显：避免「先 show 再 hide」的启动闪烁；隐藏时注销 cloak 守卫避免被强制复活。
    // 几何已在上游 loadLayout() 恢复，隐藏窗口在后续被菜单/设置重新点亮时仍在正确位置。
    {
        SettingsManager sm;
        const bool showMain = sm.loadValue(QStringLiteral("General/showMainWindow"), true).toBool();
        if (showMain) {
            m_assistantWindow->show();
#ifdef Q_OS_WIN
            DesktopMirrorWindow::registerCloakTarget(m_assistantWindow);
#endif
        } else {
            DesktopMirrorWindow::unregisterCloakTarget(m_assistantWindow);
            m_assistantWindow->hide();
        }
    }

    // 启动后恢复上次的“全屏收纳”模式（若有）。setFencesMode 会捕获当前浮动几何作为
    // 退出时的还原尺寸，并把窗口铺满工作区、重建为平铺盒子视图。
    {
        SettingsManager sm;
        if (sm.loadValue(QStringLiteral("IconGridWindow/fencesMode"), false).toBool()) {
            m_gridWindow->setFencesMode(true);
        }
    }

#ifdef Q_OS_WIN
    // 注册系统级壳钩子，监听 Explorer 重启后桌面重建 -> 事件驱动重新隐藏（非轮询，不刷 Explorer）
    RegisterShellHookWindow((HWND)winId());
    // 单次延迟隐藏：等 Explorer 完成桌面初始化后再整屏隐藏，规避启动竞态导致的“找不到列表”闪烁。
    // 这是一次性窗口隐藏（非 ListView 写操作），不会引起重排/刷新。
    QTimer::singleShot(600, this, &MainWindow::hideNativeDesktop);
#endif
}

MainWindow::~MainWindow() {
    // C：析构是重活（销毁全屏分层窗 + N 个持 HICON/QPixmap 的图标按钮 +
    // 还原原生桌面图标层，最后还要等系统排空在途的钩子回调）。此刻钩子可能仍在位 ——
    // 低级钩子是系统同步回调到本 GUI 线程的，期间全系统鼠标都会等我们返回（关闭瞬间卡死）。
    // 先摘钩；后面 stopMirror() / unregisterGridWindow() 会正常释放，故末尾的 resume 在
    // owner 已空时自动成为空操作。
    DesktopMirrorWindow::suspendHotkeyHooks();
    IconGridWindow::suspendGridHooks();
    // 桌面挂件窗口已改为独立顶层窗口（nullptr 父），不再随 MainWindow 自动销毁，
    // 必须在此显式释放，避免泄漏；同时 stopMirror 确保还原原生桌面图标层。
    if (m_dock) {
        m_dock->stopMirror();
        delete m_dock;
        m_dock = nullptr;
    }
    if (m_gridWindow) {
        delete m_gridWindow;
        m_gridWindow = nullptr;
    }
    if (m_assistantWindow) {
        delete m_assistantWindow;
        m_assistantWindow = nullptr;
    }
    // 其余“新建收纳盒”窗口：正常关闭流程已 deleteLater，此处清理仍在列表中的残留。
    for (auto& b : m_boxes) {
        if (b.window) {
            b.window->deleteLater();
            b.window = nullptr;
        }
    }
    m_boxes.clear();
    IconGridWindow::resumeGridHooks();
    DesktopMirrorWindow::resumeHotkeyHooks();
}

// 所有显示器的可用工作区并集，用于判断“已保存窗口几何是否落在某个屏幕内”
// （避免副屏窗口因只比对主屏 availableGeometry 而 intersects 失败、丢失位置回退到主屏）。
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

// 当前鼠标所在的显示器（把新窗口开在用户正在使用的屏幕上）；取不到时回退主屏。
// 作者：谭征
static QScreen* currentScreen() {
    QScreen* s = QApplication::screenAt(QCursor::pos());
    return s ? s : QApplication::primaryScreen();
}

// 位置windows
// 作者：谭征
void MainWindow::positionWindows() {
    // 如果本地已有保存的窗口几何，则优先恢复用户上一次的位置和大小，不再强制居中
    if (m_hasGridGeometry && m_hasAssistantGeometry) {
        return;
    }

    // 默认并排显示在（鼠标所在）屏幕中央附近
    QRect screen = currentScreen()->availableGeometry();
    int gridW = qMin(1000, screen.width() - 360);
    int gridH = qMin(720, screen.height() - 80);
    int assistantW = 320;
    // 桌面助手若上次处于折叠状态，启动时保持紧凑高度，避免展开大片空白
    int assistantH = (m_assistantWindow && m_assistantWindow->panel() && m_assistantWindow->panel()->isCollapsed())
                         ? Theme::collapsedHeight()
                         : gridH;

    int startX = (screen.width() - gridW - assistantW - 8) / 2;
    int startY = (screen.height() - gridH) / 2;

    if (!m_hasGridGeometry && m_gridWindow) {
        // 桌面整理窗口若处于折叠状态，保持紧凑高度，避免重启后被展开成默认高度
        if (m_gridWindow->isCollapsed()) {
            m_gridWindow->resize(gridW, Theme::collapsedHeight());
        } else {
            m_gridWindow->resize(gridW, gridH);
            // 同步展开尺寸，保证折叠/展开回退尺寸与默认布局一致
            m_gridWindow->setExpandedSize(QSize(gridW, gridH));
        }
        m_gridWindow->move(startX, startY);
    }

    if (!m_hasAssistantGeometry && m_assistantWindow) {
        // 桌面助手窗口若处于折叠状态，保持紧凑高度
        if (m_assistantWindow->panel() && m_assistantWindow->panel()->isCollapsed()) {
            m_assistantWindow->resize(assistantW, Theme::collapsedHeight());
        } else {
            m_assistantWindow->resize(assistantW, assistantH);
        }
        m_assistantWindow->move(startX + gridW + 8, startY);
    }
}

// 只有用户点击“桌面整理”按钮时才执行完整整理。
// 作者：谭征
void MainWindow::refreshDesktop(bool resetDisbanded, bool autoClassifyEmpty) {
    // 读取“桌面整理”设置中心的整理策略。
    // - 若用户已打开/保存过“桌面整理”页（Organize/mode 键存在），严格按其设置整理。
    // - 若用户此前已有保存的分类数据（旧版自动分类或用户自定义分类），使用默认整理规则，
    // 使用户无需重新打开设置中心也能按规则生效。
    // - 若什么都没有（全新初次打开），仍使用默认整理规则直接创建分类标签（不再合并到“快捷方式”）。
    SettingsManager sm;
    // 把历史遗留的“未分类”记录一次性并入“其它”，确保旧版数据不会残留隐藏分类。
    CategoryStore::migrateLegacyUncategorized();
    // “桌面整理”按钮触发（resetDisbanded=true）：清空已解散集合，按规则完整重新分类，
    // 等同“重置整理”——即便用户此前解散了全部分类，点击该按钮也会重新按规则创建分类标签。
    if (resetDisbanded) {
        sm.saveValue(QStringLiteral("IconGridWindow/disbandedCategories"), QString());
        sm.sync();
    }
    const QVariant modeValue = sm.loadValue(QStringLiteral("Organize/mode"), QVariant());
    const bool hasOrganizeSettings = !modeValue.isNull();

    // 无整理设置时，迁移/锁定为默认规则模式：若存在旧版保存分类且无自定义重命名，
    // 清掉残留空分类记录；并写入默认规则模式（mode=0），使后续刷新持续按规则整理，
    // 不再回退到“快捷方式”。（即便用户从未打开过设置中心，首启也使用默认规则。）
    if (!hasOrganizeSettings) {
        const QString renameStr = sm.loadValue(QStringLiteral("IconGridWindow/categoryRenames"), QString()).toString();
        if (renameStr.isEmpty()) {
            sm.saveValue(QStringLiteral("IconGridWindow/categoryOrder"), QString());
            sm.saveValue(QStringLiteral("IconGridWindow/categoryNames"), QString());
        }
        sm.saveValue(QStringLiteral("Organize/mode"), 0);
        sm.saveValue(QStringLiteral("Organize/fixedPartition"), 0);
        sm.sync();
    }

    // A 方案（360 式）：桌面文件始终留在原位置，分类只记录在 CategoryStore 数据库里。
    // 扫描桌面文件，category 取自数据库；无记录则按设置实时整理（规则模式 / 固定分区模式），
    // 并将新分类落盘，保证“实时整理”开关生效后新文件立即进入对应分区。
    // 启动/刷新阶段绝不搬运文件，因此不会重现此前崩溃。
    // 收纳盒数据源：优先从 Dock 镜像层读取真实桌面图标（与 dock 完全一致，含公共桌面项），
    // 不再直接扫描 Windows 桌面文件系统。系统虚拟项（我的电脑/网络/控制面板/回收站/个人文件夹）
    // 不纳入收纳盒（用户要求：系统图标不要扫描），仅 dock 内常驻显示。
    // dock 尚未就绪（启动竞态）或取数为空时，回退到扫描桌面文件系统，避免网格空壳。
    QVector<DesktopItem> raw;
    if (m_dock) {
        const auto dockEntries = m_dock->entries();
        if (!dockEntries.isEmpty()) {
            raw.reserve(dockEntries.size());
            for (const auto& e : dockEntries) {
                // 跳过系统命名空间项（shellPath 以 ::{ 开头，special=true），收纳盒不扫描系统图标。
                // 防御性检测：个人文件夹在桌面上显示名=用户名（如"TanK"），若 resolveShellPath
                // 未能返回 CLSID 而返回了真实用户目录路径，也一并跳过。
                if (e.special || e.shellPath.startsWith(QStringLiteral("::{"))) continue;
                static const QString userDir = QDir::toNativeSeparators(QDir::homePath()).toLower();
                if (QDir::toNativeSeparators(e.shellPath).toLower().startsWith(userDir)) continue;
                DesktopItem it;
                it.displayName = e.displayName;
                it.shellPath  = e.shellPath;   // 真实绝对路径（Dock 经 COM 命名空间解析，含公共桌面项）
                it.sourcePath = e.shellPath;
                it.isShortcut = e.shellPath.endsWith(QStringLiteral(".lnk"), Qt::CaseInsensitive);
                // targetPath 必须是**解析后的真实目标**，不能填 .lnk 自身：
                // SHDefExtractIcon/IExtractIcon 对 .lnk 本体取不到图标（实测 96/256 全为空），
                // 只能靠目标文件取到对应尺寸的真图；填成 .lnk 会一路掉到系统镜像列表兜底，
                // 那里只有 48px（EXTRALARGE），而 QToolButton 不会放大不足的位图 ——
                // 结果就是“大图标”档只有格子放大、图标还是 48px。
                it.targetPath = DesktopScanner::effectiveTarget(it.sourcePath, it.isShortcut);
                it.isSpecial  = false;
                raw.append(it);
            }
        }
    }
    if (raw.isEmpty()) {
        // 兜底：dock 未就绪时仍要保证网格有内容（仅启动首帧，dock 就绪后自动改走 dock 数据源）。
        DesktopScanner scanner;
        raw = scanner.scan();
    }
    // 一次性读入整张分类映射，内存里查，避免对每个文件都重新读整份 INI（N 次读盘）。
    // 按**小写**建索引：Windows 路径大小写不敏感；历史版本曾把小写路径写库（如粘贴副本
    // `f:/desktop/xxx - 副本.txt`），而 QHash 查找区分大小写 → 漏配 → 副本被当“未分类”
    // 从 m_allItems 剔除（B 重新冒回 Dock、C 消失/重启丢失）。小写化后统一匹配。
    QHash<QString, QString> catMap;
    {
        const QHash<QString, QString> rawMap = CategoryStore::readMap();
        catMap.reserve(rawMap.size());
        for (auto it = rawMap.constBegin(); it != rawMap.constEnd(); ++it)
            catMap.insert(it.key().toLower(), it.value());
    }

    // 按“桌面整理”设置中心的模式/规则对未分类文件做实时归类
    const int organizeMode = hasOrganizeSettings ? modeValue.toInt() : 0;
    const QVector<OrganizeRule> rules = hasOrganizeSettings
        ? DesktopScanner::loadRules(sm) : DesktopScanner::defaultRules();
    static const QStringList partitions = {
        QStringLiteral("目录"), QStringLiteral("文档"), QStringLiteral("压缩"),
        QStringLiteral("图片"), QStringLiteral("快捷方式"), QStringLiteral("网址"),
        QStringLiteral("视频"), QStringLiteral("音频"), QStringLiteral("其它")
    };
    const int fixedIdx = sm.loadValue(QStringLiteral("Organize/fixedPartition"), 0).toInt();
    const QString fixedCategory = partitions.value(fixedIdx, QStringLiteral("其它"));

    QHash<QString, QString> updates;
    m_allItems.clear();
    // 提前计算已解散集合（含“其它”被解散的情况），分类归类与“应用已解散”都依赖它
    const QSet<QString> disbanded = disbandedNameSet();
    for (auto& it : raw) {
        const QString normalizedSource = QDir::fromNativeSeparators(it.sourcePath);
        QString cat = catMap.value(normalizedSource.toLower(), QString());   // catMap 以小写建索引
        // 无记录（尚未归类）的文件按当前设置实时整理；已有“其它”兜底记录的文件也重新评估，
        // 这样当用户在设置中心启用“快捷方式/网址/视频/音频”等规则后，之前被错误归入“其它”
        // 的文件会自动迁出到对应分类。不匹配任何启用规则的文件仍留在“其它”。
        // autoClassifyEmpty=false 时（程序启动）跳过实时整理：只加载已持久化分类，避免用户
        // 解散的分类在重启后被规则扫描重新带出；只有点击“桌面整理”按钮才执行完整整理。
        if (autoClassifyEmpty && (cat.isEmpty() || cat == CategoryStore::desktopCategory())) {
            QString newCat;
            if (hasOrganizeSettings && organizeMode == 1) {
                newCat = fixedCategory;
            } else {
                newCat = DesktopScanner::classifyWithRules(it, rules);
            }
            // “其它”已解散：不在任何规则内的文件不再归入兜底盒，保持为桌面散件
            // （不显示在任何收纳盒，也不写入分类记录），刷新/重启后不会被自动重建。
            if (newCat == CategoryStore::desktopCategory() && disbanded.contains(CategoryStore::desktopCategory())) {
                newCat.clear();
            }
            if (newCat != cat) {
                cat = newCat;
                if (!cat.isEmpty()) updates.insert(it.sourcePath, cat);
                // cat 为空（未归类）时不写入记录，文件保持为桌面散件
            }
        }
        it.category = cat;
        if (!cat.isEmpty()) m_allItems[cat].append(it);
    }

    // 粘贴意图兜底：SHChangeNotify 触发的 refreshDesktop(false,false)（autoClassifyEmpty=false）
    // 会在 onPasteWatchTick 把副本写入 categories.ini 之前/之间运行，把“尚未归类”的副本从
    // m_allItems 剔除（变成 Dock 散件 → B 不隐藏、C 不落盘）。这里用粘贴意图把这批文件强制归回
    // 目标分类：仅当该项确实尚未归类时才覆盖（已被映射/规则归类的尊重既有归属），并随 updates 落盘。
    // 消费后即从 m_pasteIntents 移除，避免下次刷新重复处理；文件已不存在的意图同样清掉。
    if (!m_pasteIntents.isEmpty()) {
        // 隔离：粘贴目标若是用户盒，则把意图路由到该盒 box.items，绝不写 m_allItems / CategoryStore，
        // 否则默认盒会凭分类名“长出”同名分类。
        const bool pasteToDefault = (m_pasteTarget == m_gridWindow);
        OrganizerBox* pasteBox = nullptr;
        if (!pasteToDefault) {
            for (auto& b : m_boxes) { if (b.window == m_pasteTarget) { pasteBox = &b; break; } }
        }
        for (auto it = raw.begin(); it != raw.end(); ++it) {
            const QString key = QDir::fromNativeSeparators(it->sourcePath).toLower();
            auto pit = m_pasteIntents.find(key);
            if (pit == m_pasteIntents.end()) continue;
            const QString intentCat = pit.value();
            if (it->category.isEmpty()) {
                if (pasteToDefault || !pasteBox) {
                    it->category = intentCat;
                    updates.insert(it->sourcePath, intentCat);
                    m_allItems[intentCat].append(*it);
                } else {
                    // 用户盒：加入本盒（已在盒内则跳过，避免与 onPasteWatchTick 重复添加）。
                    bool has = false;
                    auto vit = pasteBox->items.constFind(intentCat);
                    if (vit != pasteBox->items.constEnd()) {
                        for (const auto& d : *vit)
                            if (!d.sourcePath.isEmpty()
                                && QDir::fromNativeSeparators(d.sourcePath).compare(key, Qt::CaseInsensitive) == 0)
                                { has = true; break; }
                    }
                    if (!has) {
                        DesktopItem di = *it; di.category = intentCat;
                        pasteBox->items[intentCat].append(di);
                    }
                }
            }
            m_pasteIntents.erase(pit);
        }
    }

    // 把所有变化（新增/迁出）一次性落盘，避免每次刷新都对整个 INI 做多次写操作
    if (!updates.isEmpty()) {
        CategoryStore::applyUpdates(updates);
    }

    // 启动/初始加载：若本地持久化分类为空（m_allItems 为空），则按规则把 dock 内全部图标
    // 归类进收纳盒——即用户要求“收纳盒先读本地持久化数据，为空则从 dock 读取全部图标”。
    // 此时 dock 的全部图标被收进收纳盒，后续会被统一从 dock 显示中隐藏。
    // 但仅在用户**从未手动管理过归属**时才这么做（Organize/autoFillAll 默认 true）：
    // 用户把图标逐个拖回 Dock 后，收纳盒本来就应该是空的；若此时又执行这条兜底，
    // 全部图标会被重新收回盒子里 —— 用户观感就是“拖出去的图标，一重启又回来了”。
    // 因此任何一次显式拖拽（Dock ⇄ 收纳盒）都会把该键置 false，使“空收纳盒”成为合法状态。
    const bool autoFillAll = sm.loadValue(QStringLiteral("Organize/autoFillAll"), true).toBool();
    if (m_allItems.isEmpty() && !raw.isEmpty() && autoFillAll) {
        for (auto& it : raw) {
            QString newCat;
            if (hasOrganizeSettings && organizeMode == 1) {
                newCat = fixedCategory;
            } else {
                newCat = DesktopScanner::classifyWithRules(it, rules);
            }
            // “其它”已解散：无规则匹配的文件不进入兜底盒，保持为桌面散件
            if (newCat == CategoryStore::desktopCategory() && disbanded.contains(CategoryStore::desktopCategory())) {
                newCat.clear();
            }
            if (!newCat.isEmpty()) {
                it.category = newCat;
                updates.insert(it.sourcePath, newCat);
                m_allItems[newCat].append(it);
            }
        }
        if (!updates.isEmpty()) CategoryStore::applyUpdates(updates);
    }

    // 在“应用已解散”之前，先把持久化的重命名映射与用户自定义分类名应用回扫描结果。
    // 关键顺序：原实现把改名还原放在 setItems 内、applyDisbanded 之后执行，导致“重命名后的
    // 分类被解散”在刷新/重启后失效——解散记录指向改名后的名字，而此时 m_allItems 仍是规则
    // 原名，匹配不到，分类便被改名逻辑重新带出。现在先改名/补自定义分类，再剔除已解散项。
    // 把持久化的重命名映射与用户自定义分类名应用到扫描结果（items）上；
    // 必须在 setItems 之前执行，否则会作用在网格窗口尚未填充的空 m_allItems 上而失效。
    m_gridWindow->applyPersistedCategories(m_allItems, disbanded);
    // 应用已解散分类：扫描会重新生成规则分类，这里把用户主动解散的分类剔除，
    // 使其内图标并入兜底分类，保证“解散”在重启/刷新后持续生效（不再被扫描重新带出）。
    applyDisbandedCategories();

    // 收纳盒不再并入系统命名空间项（我的电脑/网络/控制面板/回收站/个人文件夹）：
    // 这些虚拟项由 dock 镜像层常驻显示，用户要求收纳盒“系统图标不要扫描”，故此处不再并入“系统”分类。

    // 收纳盒与主窗口互斥持有同一文件：把当前已装入任意收纳盒窗口的文件从主窗口剔除，
    // 避免“桌面整理/刷新”按分类库（盒子分类名仍记录在 CategoryStore）把这些文件又带回主窗口，
    // 与收纳盒窗口重复显示。启动阶段（收纳盒尚未重载）此处 m_boxes 为空，不剔除，
    // 由 restoreUserBoxes() 在 refreshDesktop 之后统一处理。
    if (!m_boxes.isEmpty()) {
        QSet<QString> boxPaths;
        for (const OrganizerBox& box : m_boxes) {
            if (!box.window) continue;
            const QMap<QString, QVector<DesktopItem>>& items = box.window->items();
            for (auto it = items.constBegin(); it != items.constEnd(); ++it)
                for (const DesktopItem& di : it.value())
                    boxPaths.insert(QDir::fromNativeSeparators(di.sourcePath).toLower());
        }
        if (!boxPaths.isEmpty()) {
            for (auto it = m_allItems.begin(); it != m_allItems.end(); ++it) {
                QVector<DesktopItem>& vec = it.value();
                for (int k = vec.size() - 1; k >= 0; --k) {
                    const QString key = QDir::fromNativeSeparators(vec[k].sourcePath).toLower();
                    if (!key.isEmpty() && boxPaths.contains(key)) vec.removeAt(k);
                }
            }
            for (auto it = m_allItems.begin(); it != m_allItems.end();) {
                if (it.value().isEmpty() && !IconGridWindow::isHiddenCategory(it.key()))
                    it = m_allItems.erase(it);
                else ++it;
            }
        }
    }

    m_gridWindow->setItems(m_allItems);
    // 网格窗口已在 setItems() 中恢复上次持久化选中的分类（IconGridWindow/currentCategory）；
    // 直接用恢复后的值作为当前分类，避免用 firstKey 覆盖掉已保存的选中项。
    m_currentCategory = m_gridWindow->currentCategory();
    m_gridWindow->setCurrentCategory(m_currentCategory);

    // 收纳盒加载完成后，把收纳盒包含的全部图标从 Dock 显示中隐藏（不删真实文件）：
    // 收集 m_allItems 所有 sourcePath 交给 Dock 的隐藏集合（大小写不敏感匹配），
    // setHiddenShellPaths 会立即重排 Dock 按钮，被收走的图标不再出现在 Dock 上。
    // 同时把 Dock 当前镜像的全部图标（图标/位置/大小等）持久化到本地，作为下次启动回退。
    // 必须走 resetDockHiddenFromAllItems()（= m_allItems ∪ 「已归档」集合），
    // 绝不在这里自行只用 m_allItems 重算隐藏集合：粘贴落盘会触发 SHChangeNotify → 本函数，
    // 若此处覆盖掉「已归档」记录，被复制的源图标就会重新冒回 Dock（用户报告的 bug）。
    if (m_dock) {
        resetDockHiddenFromAllItems();
        m_dock->saveDockSnapshot();
    }
    // 清理已归档集合中磁盘上已不存在的文件（源文件被删除后不再需要隐藏）。
    CategoryStore::pruneFiledDockPaths();
}

// 收纳盒侧的路径同步：删除＝剔除条目，改名＝原地换路径与显示名。
// 每个受影响的收纳盒窗口都要重新 setItems，否则界面上仍是旧数据
// （用户“删了但盒子里还在”的观感就来自这里）。
// 作者：谭征
bool MainWindow::syncBoxItemPaths(const QSet<QString>& removedNorm,
                                  const QString& oldNorm, const QString& newPath) {
    bool anyBoxChanged = false;
    for (auto& box : m_boxes) {
        bool changed = false;
        for (auto it = box.items.begin(); it != box.items.end(); ++it) {
            QVector<DesktopItem>& vec = it.value();
            for (int i = vec.size() - 1; i >= 0; --i) {
                const QString key = QDir::fromNativeSeparators(vec[i].sourcePath).toLower();
                if (key.isEmpty()) continue;
                if (!removedNorm.isEmpty() && removedNorm.contains(key)) {
                    vec.removeAt(i);
                    changed = true;
                    continue;
                }
                if (!oldNorm.isEmpty() && key == oldNorm
                    && !newPath.isEmpty()) {
                    vec[i].sourcePath = newPath;
                    vec[i].shellPath = newPath;
                    // 改名后 .lnk 目标要重解析（改后缀会改变“是否快捷方式”，也必须跟着重算）
                    vec[i].isShortcut = QFileInfo(newPath).suffix().compare(QLatin1String("lnk"),
                                                                          Qt::CaseInsensitive) == 0;
                    vec[i].targetPath = DesktopScanner::effectiveTarget(newPath, vec[i].isShortcut);
                    vec[i].displayName = QFileInfo(newPath).fileName();
                    changed = true;
                }
            }
        }
        if (changed) {
            anyBoxChanged = true;
            if (box.window) box.window->setItems(box.items);
        }
    }
    return anyBoxChanged;
}

// Dock 内联重命名（含“改后缀”）落盘后，把收纳盒/网格侧的状态同步到新路径。
// 为什么必须同步：分类记录与收纳盒项都以“绝对路径”为键，改名后不迁移就会留下指向旧路径的
// 孤儿数据 —— 收纳盒里那一项点不动、点“桌面整理”或重启后该项会因“查不到分类记录”而被
// 重新规则归类，用户在盒子里放好的归属结果丢失。
// 作者：谭征
void MainWindow::onDockItemRenamed(const QString& oldPath, const QString& newPath) {
    if (oldPath.isEmpty() || newPath.isEmpty()) return;
    const QString oldNorm = QDir::fromNativeSeparators(oldPath);
    if (oldNorm.compare(QDir::fromNativeSeparators(newPath), Qt::CaseInsensitive) == 0) return;

    // 延迟一拍再处理：本槽由 DockIconButton::performRename() 经信号**同步**调入，
    // 直接重建网格/重发隐藏集合会在该按钮自己的成员函数栈上触发 Dock 重排（回收该按钮）。
    // 延时后既能脱离该栈，也能让“改名落盘 → UI 同步”在同一帧内完成。
    QTimer::singleShot(0, this, [this, oldPath, newPath, oldNorm]() {
        bool changed = false;
        for (auto it = m_allItems.begin(); it != m_allItems.end(); ++it) {
            for (auto& item : it.value()) {
                if (QDir::fromNativeSeparators(item.sourcePath)
                        .compare(oldNorm, Qt::CaseInsensitive) != 0) continue;
                item.sourcePath = newPath;
                item.shellPath = newPath;
                item.targetPath = newPath;   // 下面按 isShortcut 重新解析（见 isShortcut 赋值处）
                // displayName 必须一起换新：本函数结尾会用本 m_allItems **重新下发**给网格
                // （setItems），只改路径不改名字的话，网格重建时按钮又按这里的旧 displayName
                // 画出旧名 —— 用户看到的就是“改名明明成功了，界面刷新一下名字又复位”。
                item.displayName = QFileInfo(newPath).fileName();
                // 改后缀会改变类型与图标（.txt → .jpg 等），缓存必须失效后重取，
                // 否则重建出的按钮会沿用旧图标（loadIconNow 只在 icon 为空时才取图）。
                item.icon = QIcon();
                item.systemImageIndex = -1;
                item.isShortcut = QFileInfo(newPath).suffix().compare(QLatin1String("lnk"),
                                                                     Qt::CaseInsensitive) == 0
                                  && DesktopScanner::isRealShortcut(newPath);
                // targetPath 必须是解析后的真实目标：改名/改后缀后重新解析（取图只认目标文件）
                item.targetPath = DesktopScanner::effectiveTarget(item.sourcePath, item.isShortcut);
                changed = true;
            }
        }
        // 分类库记录跟着改名迁移（旧记录删除，避免孤儿残留）
        const QString cat = CategoryStore::categoryOf(oldPath);
        if (!cat.isEmpty()) {
            CategoryStore::setCategory(newPath, cat);
            CategoryStore::clearCategory(oldPath);
        }
        // 收纳盒侧的图标集合同样以路径为键，必须一起迁移并刷新窗口
        const QString oldKey = oldNorm.toLower();
        syncBoxItemPaths(QSet<QString>(), oldKey, newPath);
        if (!changed) return;

        m_gridWindow->setItems(m_allItems);
        // setItems 会恢复上次持久化选中的分类，这里同步回当前分类，避免选中项被覆盖
        m_currentCategory = m_gridWindow->currentCategory();
        m_gridWindow->setCurrentCategory(m_currentCategory);
        saveLayout();   // 本地持久化：收纳盒状态由 m_allItems 派生，改名后立即重存

        // 隐藏集合按新路径重发：保持“已收进收纳盒的图标不在 Dock 上显示”的一致性
        // （统一走 resetDockHiddenFromAllItems，避免覆盖「已归档」集合）
        if (m_dock) {
            resetDockHiddenFromAllItems();
        }
    });
}

// Dock 内图标被 Delete 快捷键删除后（真实文件已进回收站）清理收纳盒/网格侧的残留记录。
// 为什么必须清理：m_allItems 与 CategoryStore 都以“绝对路径”为键，不清理就会留下指向已删文件的
// 孤儿记录 —— 收纳盒里那一项点不动（文件已不存在），重启后也不会消失（记录仍在，只是永远打不开）。
// 作者：谭征
void MainWindow::onDockItemsDeleted(const QStringList& paths) {
    if (paths.isEmpty()) return;
    QSet<QString> norm;
    norm.reserve(paths.size());
    for (const QString& p : paths) {
        if (!p.isEmpty()) norm.insert(QDir::fromNativeSeparators(p).toLower());
    }
    if (norm.isEmpty()) return;

    // 延迟一拍再处理：本槽由 DesktopMirrorWindow::triggerDeleteShortcut() 经信号**同步**调入，
    // 直接重建网格/重发隐藏集合会在该函数自己的栈上触发 Dock 重排（同 onDockItemRenamed 的原因）。
    QTimer::singleShot(0, this, [this, paths, norm]() {
        // ① m_allItems：移除所有键命中的条目（大小写不敏感）
        bool changed = false;
        for (auto it = m_allItems.begin(); it != m_allItems.end(); ++it) {
            QVector<DesktopItem>& vec = it.value();
            for (int i = vec.size() - 1; i >= 0; --i) {
                const QString key = QDir::fromNativeSeparators(vec[i].sourcePath).toLower();
                if (!key.isEmpty() && norm.contains(key)) { vec.removeAt(i); changed = true; }
            }
        }
        // ② CategoryStore：清掉这些路径的分类记录（与改名同步同一手法；CategoryStore 内部
        // 对路径做正斜杠归一化后再匹配，故直接传原路径即可）。
        for (const QString& p : paths) CategoryStore::clearCategory(p);
        // ③ 收纳盒侧：剔除条目并刷新对应的收纳盒窗口（否则界面里那一项仍然挂着）
        syncBoxItemPaths(norm, QString(), QString());

        // ④ 本地持久化：收纳盒状态由 m_allItems 派生，删除后必须重存一次，
        // 否则重启后又会按旧记录把它带回来。
        saveLayout();
        if (!changed) return;
        m_gridWindow->setItems(m_allItems);
        // setItems 会恢复上次持久化选中的分类，这里同步回当前分类，避免选中项被覆盖
        m_currentCategory = m_gridWindow->currentCategory();
        m_gridWindow->setCurrentCategory(m_currentCategory);

        // ⑤ 隐藏集合重发：被删路径必须从隐藏集合里消失，否则将来同名的项会被误隐藏
        // （统一走 resetDockHiddenFromAllItems，避免覆盖「已归档」集合）
        if (m_dock) {
            resetDockHiddenFromAllItems();
        }
    });
}

// 跨窗口拖拽：Dock ⇄ 收纳盒
// 需求：
// ① Dock 内图标用左键拖到收纳盒 → Dock 隐藏该图标、收纳盒新增对应图标；
// ② 收纳盒内图标用左键拖到 Dock → 收纳盒删除该图标、Dock 恢复显示该图标。
// 实现要点（两条链路都走"精确增量"，不走 refreshDesktop 的全量重建，原因见头文件说明）：
// · 归属真值 = CategoryStore（path → 分类），与既有整理逻辑同一份数据；
// · Dock 可见性 = m_dock 的隐藏集合，其权威来源就是 m_allItems 的全部路径；
// · 因此"载入/移出"只需改 CategoryStore + m_allItems，再重算隐藏集合下发即可。

// 按 m_allItems 重算 Dock 隐藏集合并下发（与 onDockItemsDeleted 内那段同源）。
// 作者：谭征
void MainWindow::resetDockHiddenFromAllItems() {
    if (!m_dock) return;
    QSet<QString> hidden;
    hidden.reserve(m_allItems.size());
    for (auto it = m_allItems.constBegin(); it != m_allItems.constEnd(); ++it) {
        for (const auto& item : it.value()) {
            if (!item.sourcePath.isEmpty())
                hidden.insert(QDir::fromNativeSeparators(item.sourcePath).toLower());
        }
    }
    // 一并纳入「已归档（隐藏于 Dock）」集合中的源文件：复制语义粘贴后，
    // 被复制的源图标也要从 Dock 立即消失（文件仍物理留在桌面）。
    const QStringList filed = CategoryStore::filedDockPaths();
    // 收纳盒（默认盒之外的用户盒）内的图标同样要从 Dock 隐藏：与默认收纳盒功能一致 ——
    // 文件被归入收纳盒后，真实桌面镜像上不应再显示它。收纳盒条目独立存于 box.items，
    // 重启后会被从 m_allItems 剔除，故这里一并纳入隐藏集合（与 m_allItems、已归档集合取并集）。
    for (const OrganizerBox& box : m_boxes) {
        if (!box.window) continue;
        const QMap<QString, QVector<DesktopItem>>& bi = box.window->items();
        for (auto it = bi.constBegin(); it != bi.constEnd(); ++it) {
            for (const DesktopItem& di : it.value()) {
                if (!di.sourcePath.isEmpty())
                    hidden.insert(QDir::fromNativeSeparators(di.sourcePath).toLower());
            }
        }
    }
    for (const QString& p : filed)
        if (!p.isEmpty()) hidden.insert(QDir::fromNativeSeparators(p).toLower());
    m_dock->setHiddenShellPaths(hidden);
}


// 全部收纳盒窗口：主整理窗口 + 各独立收纳盒窗口。
// C4：改为返回内部缓存的常量引用（原来每次调用都 new 一个 QVector 并逐个拷贝指针，
// 而本函数在拖动悬停期间被反复调用）。缓存与“m_boxes 里非空的 window + m_gridWindow”逐项比对，
// 不一致才重建 —— 盒数量通常个位数，比对代价可忽略，且无需任何“置脏”通知点。
const QVector<IconGridWindow*>& MainWindow::allBoxWindows() const {
    bool same = true;
    int idx = 0;
    for (const auto& b : m_boxes) {
        if (!b.window) continue;
        if (idx >= m_boxWindowsCache.size() || m_boxWindowsCache.at(idx) != b.window) { same = false; break; }
        ++idx;
    }
    if (same) {
        if (m_gridWindow) {
            if (idx + 1 != m_boxWindowsCache.size() || m_boxWindowsCache.at(idx) != m_gridWindow) same = false;
        } else if (idx != m_boxWindowsCache.size()) {
            same = false;
        }
    }
    if (!same) {
        m_boxWindowsCache.clear();
        m_boxWindowsCache.reserve(m_boxes.size() + 1);
        for (const auto& b : m_boxes)
            if (b.window) m_boxWindowsCache.append(b.window);
        if (m_gridWindow) m_boxWindowsCache.append(m_gridWindow);
    }
    return m_boxWindowsCache;
}

// 落点是否落在**真实可放置目标**上（任一收纳盒窗口里真正接得住图标的位置）。
// 历史教训（2026-09-14 修正）：早先用「窗口 frameGeometry 包含」当判据是错的 ——
// 全屏接管模式下收纳盒窗口铺满整个桌面且背景完全透明，于是任何坐标都“落在窗口里”，
// 判定恒为真，后果是：① 盒 → Dock 永远不生效；② Dock 图标拖到壁纸上的常规重排也被误认领而失效。
// 现在把判据下沉到各窗口：全屏/盒子视图＝命中某个真实 FenceBox；窗口化网格视图＝落在窗口内。
// 用途不变：只要松手点真的落在某个盒子上，无论能否解析出分类，
// 都不允许继续落到 onIconDragEnd 的回收站删除分支（「拖拽交换绝不删除文件」的结构性保险）。
// 作者：谭征
bool MainWindow::anyRealDropTargetAt(const QPoint& globalPos) const {
    for (IconGridWindow* w : allBoxWindows()) {
        if (!w || !w->isVisible()) continue;
        if (w->hitRealDropTarget(globalPos)) return true;
    }
    return false;
}

// 屏幕逻辑坐标 → 命中的收纳盒窗口与其分类。
// 顺序：先查独立收纳盒窗口（通常叠在主整理窗口之上，也是用户更可能拖向的小目标），
// 再查主整理窗口。窗口内部的"具体落到哪个分类"由 IconGridWindow::dropCategoryAtGlobal 判定
// —— 全屏盒子视图下它能精确到"命中的那一个盒子"。
MainWindow::BoxDropTarget MainWindow::resolveBoxAt(const QPoint& globalPos) const {
    BoxDropTarget t;
    for (IconGridWindow* w : allBoxWindows()) {
        if (!w || !w->isVisible()) continue;
        const QString cat = w->dropCategoryAtGlobal(globalPos);
        if (cat.isEmpty()) continue;
        t.window = w;
        t.category = cat;
        return t;
    }
    return t;
}

// 统一开关悬停提示：命中的那一个窗口亮起，其余一律熄灭（拖动中光标会不断换目标）。
// 作者：谭征
void MainWindow::setExternalDropHighlightWindow(IconGridWindow* target) {
    for (IconGridWindow* w : allBoxWindows()) {
        if (w) w->setExternalDropHighlight(target != nullptr && w == target);
    }
}

// 拖动悬停（40ms 节流）：① 命中的盒子亮起边框（原有行为）② 在命中窗口的图标网格上显示
// “将插入到此处”的竖直指示线（与盒内拖拽看到的是同一条线）③ 让 Dock 换上“可放置”光标。
// 三者必须一起开关：光标每 40ms 才来一次，若有窗口/光标没被清掉，指示线就会残留在上一次的目标上。
// 作者：谭征
void MainWindow::onDockIconDragHover(const QPoint& globalPos) {
    IconGridWindow* hit = resolveBoxAt(globalPos).window;
    setExternalDropHighlightWindow(hit);
    for (IconGridWindow* w : allBoxWindows()) {
        if (!w) continue;
        if (w == hit) w->updateExternalDropIndicatorAt(globalPos, nullptr);
        else          w->clearExternalDropIndicator();
    }
    if (m_dock) m_dock->setDragOverBox(hit != nullptr);
    // ④ 拖拽图标浮层：Dock 分层窗恒在最底、收纳盒在其上，拖进盒子的图标会被整块盖住 → 把
    // 「当前遮挡区」（屏幕坐标）下发给 Dock，它把被盖住的那一块补画到最上层（见 setDragGhostClip）。
    // 遮挡区与上面的 hit 判定必须分开算：hit 回答的是“能不能放进去”（dropCategoryAtGlobal），
    // 拖到不收东西的盒子上 hit 可能为空，但图标照样被它盖住 —— 遮挡是纯几何事实。
    if (m_dock) {
        QRect occluded;
        for (IconGridWindow* w : allBoxWindows()) {
            if (!w) continue;
            const QRect r = w->externalDropSurfaceRectGlobal(globalPos);
            if (!r.isEmpty()) { occluded = r; break; }
        }
        m_dock->setDragGhostClip(occluded);
    }
}

// 跨窗口拖拽结束：清除所有收纳盒窗口的悬停提示。
// 作者：谭征
void MainWindow::onDockIconDragFinished() {
    setExternalDropHighlightWindow(nullptr);
    // 收尾：所有窗口的插入指示线与 Dock 的光标反馈一并复位（否则会残留在最后一次命中的窗口上）。
    for (IconGridWindow* w : allBoxWindows()) if (w) w->clearExternalDropIndicator();
    if (m_dock) {
        m_dock->setDragOverBox(false);
        m_dock->setDragGhostClip(QRect());   // 拖拽浮层的遮挡区也要清（Dock 侧同时会自行收起浮层）
    }
}

// 给一个收纳盒窗口装上"拖出到 Dock"的落点解析器。
// 由 DesktopIconButton 在拖拽结束后（文件仍在原处）用**松手坐标**回调，逐层判定：
// ① 松手点仍落在某个**真实盒子**上 → 一律不处理。这一条同时挡住三种情况：盒内轻微抖动、
// 跨盒拖动、以及"落在盒子上却没命中有效分类"（如该盒的分类不参与收纳）——
// 后两者绝不能被误当成"拖出到 Dock"。
// 判据用 anyRealDropTargetAt（须真的命中盒子）而非 resolveBoxAt（还会回退到"当前分类"），
// 也**不能**用窗口矩形：全屏接管时窗口铺满整屏，矩形判会让本功能彻底失效。
// ② 松手点真的落在 Dock 镜像面（＝桌面面）上 → 取消分类归属，图标回到 Dock 且落在松手点。
// ③ 其它落点（记事本之类第三方程序接住了这次放置）→ 什么都不做，与改动前行为一致。
// 作者：谭征
void MainWindow::installBoxDropResolver(IconGridWindow* w) {
    if (!w) return;
    w->setDockDropResolver([this](const QString& path, const QString& name, const QPoint& pos) -> bool {
        if (anyRealDropTargetAt(pos)) return false;
        if (!m_dock || !m_dock->isVisible() || !m_dock->frameGeometry().contains(pos)) {
            DiagTrace::log(QStringLiteral("[cross] boxDrop ignored: not on dock gp=%1,%2")
                               .arg(pos.x()).arg(pos.y()));
            return false;
        }
        DiagTrace::log(QStringLiteral("[cross] box->dock name='%1' gp=%2,%3")
                           .arg(name).arg(pos.x()).arg(pos.y()));
        scheduleBoxItemBackToDock(path, name, pos);
        return true;
    });
}

// 用户已手动管理过归属：此后 refreshDesktop 不再执行"收纳盒为空则把 dock 全部图标收进来"
// 的首次启动兜底 —— 否则"把图标逐个拖回 Dock、最后收纳盒为空"会被下一次刷新全盘推翻。
// 作者：谭征
void MainWindow::markBoxAssignmentUserHandled() {
    SettingsManager sm;
    sm.saveValue(QStringLiteral("Organize/autoFillAll"), false);
    sm.sync();
}

// Dock → 收纳盒：把一批 Dock 图标归入某分类。
// 延迟一拍执行：本方法由 DesktopMirrorWindow::onIconDragEnd() 经**同步**解析器回调调入，
// 若就地重建收纳盒、重排 Dock，等于在那次拖动自己的事件处理栈上改动控件树
// （与 onDockItemsDeleted 完全同样的理由 —— 必须等本栈退出、拖动收尾完成后再动）。
// insertIndex：松手光标在目标分类网格上的插入下标（-1 / 越界 = 追加末尾）。
// 作者：谭征
void MainWindow::scheduleDockItemsIntoBox(const QStringList& paths, const QStringList& displayNames,
                                          const QString& category, IconGridWindow* targetWindow,
                                          int insertIndex) {
    if (paths.isEmpty() || category.isEmpty()) return;
    // 用 QPointer 持有目标窗口：真正的改动在事件循环下一拍执行，若这期间用户恰好解散了那个
    // 收纳盒（或程序退出），裸指针就会变成野指针。
    QPointer<IconGridWindow> target = targetWindow;
    QTimer::singleShot(0, this, [this, paths, displayNames, category, target, insertIndex]() {
        // 落点判定放在 lambda 内：target 是 QPointer，若用户在延迟期间解散了该盒，
        // target 自动变 null，避免捕获指向 m_boxes 元素的裸指针因 removeAt 而悬空。
        if (!target) return;
        const bool isDefaultBox = (target == m_gridWindow);
        OrganizerBox* targetBox = nullptr;
        if (!isDefaultBox) {
            for (auto& b : m_boxes) {
                if (b.window == target) { targetBox = &b; break; }
            }
            if (!targetBox) return;   // 目标用户盒已被销毁/解散，放弃本次操作
        }
        // 插入位置：由松手光标位置算出（-1 / 越界 = 追加末尾）。多选整组拖动时依次后移，
        // 保证整组仍按原相对顺序、连着插在光标那一格。
        int insertAt = insertIndex;
        for (int i = 0; i < paths.size(); ++i) {
            const QString path = paths[i];
            if (path.isEmpty()) continue;
            const QString norm = QDir::fromNativeSeparators(path);
            const QString name = (i < displayNames.size() && !displayNames[i].isEmpty())
                                     ? displayNames[i]
                                     : QFileInfo(path).fileName();
            // ① 先摘掉**其它分类**的同路径条目：一个文件只能属于一个收纳盒分类，
            // 不摘就会出现"同一个图标同时出现在两个盒子里"的幽灵项。
            // 目标分类单独在 ③ 处理 —— 它的摘除会改变插入下标，必须与插入一起算。
            for (auto it = m_allItems.begin(); it != m_allItems.end(); ++it) {
                if (it.key() == category) continue;
                QVector<DesktopItem>& vec = it.value();
                for (int k = vec.size() - 1; k >= 0; --k) {
                    if (QDir::fromNativeSeparators(vec[k].sourcePath)
                            .compare(norm, Qt::CaseInsensitive) == 0) {
                        vec.removeAt(k);
                    }
                }
            }
            // ② 构造条目并加入目标分类（字段与 refreshDesktop 从 dock entries 构建的保持一致）
            DesktopItem item;
            item.displayName = name;
            item.sourcePath = path;
            item.shellPath   = path;
            item.category    = category;
            item.isShortcut  = path.endsWith(QStringLiteral(".lnk"), Qt::CaseInsensitive);
            // targetPath = 解析后的真实目标（取图只认目标文件，理由同 refreshDesktop）
            item.targetPath  = DesktopScanner::effectiveTarget(item.sourcePath, item.isShortcut);
            item.isSpecial   = false;

            // ③ 插到光标位置（默认盒 → 共享模型 + 分类库；用户盒 → 只写本盒数据）
            QVector<DesktopItem>* dst = nullptr;
            if (isDefaultBox) {
                // 默认收纳盒：写入共享模型 + 共享分类库（刷新/重启读回）
                CategoryStore::setCategory(path, category);
                dst = &m_allItems[category];
            } else {
                // 用户新建收纳盒：仅写入本盒 box.items，绝不触碰 m_allItems / CategoryStore，
                // 保证默认盒不会“长出”同名分类，解散时也只清本盒数据。若该项曾属于默认盒，
                // 顺手清掉旧的共享分类映射，避免刷新时被默认盒重新收编。
                CategoryStore::clearCategory(path);
                dst = &targetBox->items[category];
            }
            if (dst) {
                // 目标分类里若已有同一路径（极端情况：Dock 隐藏集合尚未同步），先摘掉避免重复；
                // 被摘掉的位置若在插入点之前，插入点要同步前移一格，否则落点会整体偏后。
                for (int k = dst->size() - 1; k >= 0; --k) {
                    if (QDir::fromNativeSeparators((*dst)[k].sourcePath)
                            .compare(norm, Qt::CaseInsensitive) == 0) {
                        dst->removeAt(k);
                        if (insertAt > k) --insertAt;
                    }
                }
                if (insertAt < 0) insertAt = dst->size();
                insertAt = qBound(0, insertAt, dst->size());
                dst->insert(insertAt, item);
                ++insertAt;   // 组内下一项紧挨其后
            }
        }
        markBoxAssignmentUserHandled();

        if (isDefaultBox) {
            // ④' 顺序键必须先落盘：下面 setItems() → applyItemOrder() 是按**磁盘里的顺序**重排的，
            // 若顺序键还是旧的（新插入的路径不在其中），刚插到光标位置的图标会被甩回末尾。
            // 收纳盒窗口没有顺序键（其显示顺序就是传入顺序），无需处理。
            if (m_gridWindow) m_gridWindow->setCategoryOrderedItems(category, m_allItems.value(category));
            // ④ 目标收纳盒窗口：确保该分类出现在它的显示列表里（用户刚往这个盒子拖了东西）。
            // 其余盒子：该分类若本来就在它们的列表里，也同步成最新数据（同一分类可被多个盒显示）。
            for (auto& box : m_boxes) {
                if (!box.window) continue;
                if (box.window == target || box.items.contains(category))
                    box.items[category] = m_allItems.value(category);
                box.window->setItems(box.items);
            }
            // ⑤ 主整理窗口：全量刷新
            if (m_gridWindow) {
                m_gridWindow->setItems(m_allItems);
                m_currentCategory = m_gridWindow->currentCategory();
                m_gridWindow->setCurrentCategory(m_currentCategory);
            }
        } else {
            // 用户盒：重发本盒数据即可（与其它盒、默认盒互不影响）
            if (targetBox->window) targetBox->window->setItems(targetBox->items);
        }
        // ⑥ 持久化 + Dock 隐藏集合重算（新收进来的路径进入隐藏集合 → Dock 上该图标消失）
        saveLayout();
        resetDockHiddenFromAllItems();
        if (m_dock) m_dock->saveDockSnapshot();
        // ⑦ 让刚收进来的图标立即选中并武装快捷键作用域：拖动期间"按下"发生在 Dock，
        // 目标收纳盒的 s_gridHot 未指向它、且重建接力只找回旧选中——不补这一步，
        // 用户松手后直接按 F2/Delete 会失效。取第一个非空路径即可。
        if (target.data()) {
            for (const QString& p : paths) {
                if (p.isEmpty()) continue;
                target->selectIconByShellPathAndArm(p);
                break;
            }
        }
    });
}

// 收纳盒 → Dock：取消该图标的归属，使其重新出现在 Dock 上，且**就出现在松手点**。
// 与 onDockItemsDeleted 完全同构，唯一区别是**不删除真实文件**（文件始终在桌面原位置，
// 只是不再被任何收纳盒持有；这里连位置都是“显示位置”，不是文件位置）。
// globalDropPos = 松手时的逻辑屏幕坐标：图标会以单元格中心对齐到该点（落点即显示位置）。
// 作者：谭征
void MainWindow::scheduleBoxItemBackToDock(const QString& sourcePath, const QString& displayName,
                                           const QPoint& globalDropPos) {
    Q_UNUSED(displayName)
    if (sourcePath.isEmpty()) return;
    QTimer::singleShot(0, this, [this, sourcePath, globalDropPos]() {
        const QString norm = QDir::fromNativeSeparators(sourcePath).toLower();
        // ① m_allItems：移除所有分类里命中该路径的条目（大小写不敏感）
        bool changed = false;
        for (auto it = m_allItems.begin(); it != m_allItems.end(); ++it) {
            QVector<DesktopItem>& vec = it.value();
            for (int i = vec.size() - 1; i >= 0; --i) {
                const QString key = QDir::fromNativeSeparators(vec[i].sourcePath).toLower();
                if (!key.isEmpty() && key == norm) { vec.removeAt(i); changed = true; }
            }
        }
        // ② CategoryStore：清掉分类记录 → 该项回到"未归类"（文件仍在桌面，Dock 会重新显示它）
        CategoryStore::clearCategory(sourcePath);
        markBoxAssignmentUserHandled();

        // ③ 收纳盒侧剔除条目并刷新各盒窗口（否则界面里那一项仍挂着）
        QSet<QString> removed;
        removed.insert(norm);
        syncBoxItemPaths(removed, QString(), QString());
        saveLayout();
        if (m_gridWindow && changed) {
            m_gridWindow->setItems(m_allItems);
            m_currentCategory = m_gridWindow->currentCategory();
            m_gridWindow->setCurrentCategory(m_currentCategory);
        }
        for (auto& box : m_boxes)
            if (box.window) box.window->setItems(box.items);

        // ④ 先写坐标真值，再取消隐藏 —— 顺序是关键（见 placeDockIconAtCursor 的说明）：
        // 先让 Dock 记住"这一项现在应该在松手点"，紧接着那次因隐藏集合变化而触发的重排
        // 就会直接把图标画在落点上，全程只有一次重建，不会出现"先闪旧位置再跳过去"。
        if (m_dock) m_dock->placeDockIconAtCursor(sourcePath, globalDropPos);
        // ⑤ Dock 隐藏集合重算（该路径已不在集合里 → 图标重新出现在 Dock 上，位置即上面写的落点）
        resetDockHiddenFromAllItems();
        if (m_dock) m_dock->saveDockSnapshot();
        // ⑥ 让刚落下的图标立即选中并武装快捷键作用域：拖动期间“按下”发生在收纳盒，
        // Dock 的 s_hkLastClickOnDock 被清 false、且重建接力只找回旧选中——不补这一步，
        // 用户松手后直接按 F2/Delete 会失效（本 bug 的直接来源）。
        if (m_dock) m_dock->selectIconByShellPathAndArm(sourcePath);
    });
}

// organizeandclassify桌面
// 作者：谭征
void MainWindow::organizeAndClassifyDesktop() {
    // 「桌面整理」按钮：按当前设置（规则模式或固定分区模式）重新整理 dock 内图标，
    // 并覆盖写入 CategoryStore；绝不搬运物理文件。
    // 数据收集在主线程（读 dock m_entries），分类+写库放在后台线程。
    if (m_organizeWatcher && m_organizeWatcher->isRunning()) return;   // 防止重复点击导致并发写入

    // ---- 主线程：从 dock 收集原始图标数据 ----
    QVector<DesktopItem> raw;
    if (m_dock) {
        const auto dockEntries = m_dock->entries();
        raw.reserve(dockEntries.size());
        for (const auto& e : dockEntries) {
            // 跳过系统虚拟项（我的电脑 / 网络 / 控制面板 / 回收站 / 个人文件夹）：
            // 它们 shellPath 以 "::{" 开头、special=true，非真实文件，不应参与整理归类。
            // 防御性检测：个人文件夹显示名=用户名时若未被 CLSID 匹配，按用户目录路径兜底过滤。
            if (e.special || e.shellPath.startsWith(QStringLiteral("::{"))) continue;
            static const QString userDir2 = QDir::toNativeSeparators(QDir::homePath()).toLower();
            if (QDir::toNativeSeparators(e.shellPath).toLower().startsWith(userDir2)) continue;
            DesktopItem it;
            it.displayName = e.displayName;
            it.shellPath = e.shellPath;
            it.sourcePath = e.shellPath;
            it.isShortcut = e.shellPath.endsWith(QStringLiteral(".lnk"), Qt::CaseInsensitive);
            // targetPath 必须是解析后的真实目标（同 refreshDesktop，理由见那里的注释）
            it.targetPath = DesktopScanner::effectiveTarget(it.sourcePath, it.isShortcut);
            it.isSpecial = false;
            raw.append(it);
        }
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto task = [raw]() mutable {
        // ---- 后台线程：按设置中心规则/固定分区归类 + 写库 ----
        SettingsManager sm;
        const QVariant modeValue = sm.loadValue(QStringLiteral("Organize/mode"), QVariant());
        const bool hasOrganizeSettings = !modeValue.isNull();

        QVector<DesktopItem> classified = raw;
        if (hasOrganizeSettings && modeValue.toInt() == 1) {
            // 固定分区模式
            static const QStringList partitions = {
                QStringLiteral("目录"), QStringLiteral("文档"), QStringLiteral("压缩"),
                QStringLiteral("图片"), QStringLiteral("快捷方式"), QStringLiteral("网址"),
                QStringLiteral("视频"), QStringLiteral("音频"), QStringLiteral("其它")
            };
            const int fixedIdx = sm.loadValue(QStringLiteral("Organize/fixedPartition"), 0).toInt();
            const QString fixedCategory = partitions.value(fixedIdx, QStringLiteral("其它"));
            for (auto& it : classified) it.category = fixedCategory;
        } else if (!classified.isEmpty()) {
            // 规则模式
            const QVector<OrganizeRule> rules = hasOrganizeSettings
                ? DesktopScanner::loadRules(sm) : DesktopScanner::defaultRules();
            for (auto& it : classified) it.category = DesktopScanner::classifyWithRules(it, rules);
        }

        QHash<QString, QString> updates;
        updates.reserve(classified.size());
        for (const auto& it : classified) {
            updates.insert(it.sourcePath, it.category);
        }
        CategoryStore::applyUpdates(updates);   // 仅一次落盘
    };
    if (!m_organizeWatcher) {
        m_organizeWatcher = new QFutureWatcher<void>(this);
        connect(m_organizeWatcher, &QFutureWatcher<void>::finished, this, [this]() {
            QApplication::restoreOverrideCursor();
            refreshDesktop(true);   // DB write done; rebuild view and resync desktop icon visibility
        });
    }
    m_organizeWatcher->setFuture(QtConcurrent::run(task));
}

// 应用disbanded分类
// 作者：谭征
void MainWindow::applyDisbandedCategories() {
    // A 方案：已解散分类的文件只需清空其 CategoryStore 记录，文件仍在原桌面位置、不丢失。
    // 扫描会按规则重新生成该分类名，这里把它从视图中剔除；解散时不并入任何兜底分类。
    SettingsManager sm;
    const QStringList disbanded = sm.loadValue(QStringLiteral("IconGridWindow/disbandedCategories"), QString())
                                     .toString().split(QLatin1Char(';'), Qt::SkipEmptyParts);
    if (disbanded.isEmpty()) return;

    // 解析 name>fallback 映射（仅用于记录已解散分类名；解散时不并入任何兜底）
    QMap<QString, QString> map;
    for (const QString& pair : disbanded) {
        const int sep = pair.indexOf(QLatin1Char('>'));
        if (sep > 0) map.insert(pair.left(sep), pair.mid(sep + 1));
    }

    for (auto it = map.begin(); it != map.end(); ++it) {
        const QString& name = it.key();
        if (!m_allItems.contains(name)) continue;          // 本次扫描未生成该分类，无需处理
        // 解散任意分类：直接从视图移除，并清空该分类下所有文件的分类记录（文件回到真实桌面，不搬运）。
        m_allItems.remove(name);
        CategoryStore::removeCategory(name);
    }
}

// disbanded名称设置
// 作者：谭征
QSet<QString> MainWindow::disbandedNameSet() const {
    // 解析 IconGridWindow/disbandedCategories（name>fallback;...）得到已解散的分类名集合，
    // 供 applyPersistedCategories 在补回自定义分类时跳过这些已解散项。
    SettingsManager sm;
    const QStringList disbanded = sm.loadValue(QStringLiteral("IconGridWindow/disbandedCategories"), QString())
                                     .toString().split(QLatin1Char(';'), Qt::SkipEmptyParts);
    QSet<QString> set;
    for (const QString& pair : disbanded) {
        const int sep = pair.indexOf(QLatin1Char('>'));
        const QString key = (sep > 0) ? pair.left(sep) : pair;
        if (!key.isEmpty()) set.insert(key);
    }
    return set;
}

// mark分类disbanded
// 作者：谭征
void MainWindow::markCategoryDisbanded(const QString& name, const QString& fallback) {
    if (name.isEmpty()) return;
    SettingsManager sm;
    QStringList pairs = sm.loadValue(QStringLiteral("IconGridWindow/disbandedCategories"), QString())
                           .toString().split(QLatin1Char(';'), Qt::SkipEmptyParts);
    bool found = false;
    for (int i = 0; i < pairs.size(); ++i) {
        const int sep = pairs[i].indexOf(QLatin1Char('>'));
        if (sep > 0 && pairs[i].left(sep) == name) {
            pairs[i] = name + QLatin1Char('>') + fallback;   // 更新合并目标
            found = true;
            break;
        }
    }
    if (!found) pairs.append(name + QLatin1Char('>') + fallback);
    sm.saveValue(QStringLiteral("IconGridWindow/disbandedCategories"), pairs.join(QLatin1Char(';')));
    sm.sync();
}

// unmark分类disbanded
// 作者：谭征
void MainWindow::unmarkCategoryDisbanded(const QString& name) {
    if (name.isEmpty()) return;
    SettingsManager sm;
    QStringList pairs = sm.loadValue(QStringLiteral("IconGridWindow/disbandedCategories"), QString())
                           .toString().split(QLatin1Char(';'), Qt::SkipEmptyParts);
    bool changed = false;
    for (int i = pairs.size() - 1; i >= 0; --i) {
        const int sep = pairs[i].indexOf(QLatin1Char('>'));
        const QString key = (sep > 0) ? pairs[i].left(sep) : pairs[i];
        if (key == name) { pairs.removeAt(i); changed = true; }
    }
    if (changed) {
        sm.saveValue(QStringLiteral("IconGridWindow/disbandedCategories"), pairs.join(QLatin1Char(';')));
        sm.sync();
    }
}

// 响应搜索变化信号
// 作者：谭征
void MainWindow::onSearchChanged(const QString& text) {
    m_gridWindow->setSearchText(text);
}

// 响应工具触发信号
// 作者：谭征
void MainWindow::onToolTriggered(const QString& name) {
    if (name == QStringLiteral("记事本")) {
        if (!launchTool(QStringLiteral("notepad.exe"), QString()))
            QProcess::startDetached(QStringLiteral("notepad.exe"));
    } else if (name == QStringLiteral("计算器")) {
        if (!launchTool(QStringLiteral("calc.exe"), QString()))
            QProcess::startDetached(QStringLiteral("calc.exe"));
    } else if (name == QStringLiteral("命令行")) {
        // 「命令行」：打开 Windows 命令提示符。用 GetSystemDirectory 取真实系统目录（兼容非 C 盘安装）
        // 拼 cmd.exe 绝对路径；像 Win+R 那样由 shell 前台激活，落在用户主目录。
        QString program;
#ifdef Q_OS_WIN
        wchar_t sysBuf[MAX_PATH] = {0};
        if (GetSystemDirectoryW(sysBuf, MAX_PATH))
            program = QDir(QString::fromWCharArray(sysBuf)).filePath(QStringLiteral("cmd.exe"));
#endif
        if (program.isEmpty())
            program = QStringLiteral("cmd.exe");
        const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
        if (!launchTool(program, home))
            QProcess::startDetached(program, QStringList(), home);
    } else if (name == QStringLiteral("文件管理")) {
        if (!launchTool(QStringLiteral("explorer.exe"), QString()))
            QProcess::startDetached(QStringLiteral("explorer.exe"));
    } else if (name == QStringLiteral("磁盘清理")) {
        // 「磁盘清理」：打开 Windows 磁盘清理工具（系统盘 C:），即截图中的
        // 「系统(C:)的磁盘清理」对话框。cleanmgr.exe 基础清理页无需提权；
        // 用 GetSystemDirectory 取真实系统目录（兼容非 C 盘安装），
        // 像 Win+R 那样由 shell 前台激活窗口。「清理系统文件」按钮由 cleanmgr
        // 自行触发 UAC，与本程序权限无关。
        QString program;
#ifdef Q_OS_WIN
        wchar_t sysBuf[MAX_PATH] = {0};
        if (GetSystemDirectoryW(sysBuf, MAX_PATH))
            program = QDir(QString::fromWCharArray(sysBuf)).filePath(QStringLiteral("cleanmgr.exe"));
#endif
        if (program.isEmpty())
            program = QStringLiteral("cleanmgr.exe");
        if (!launchTool(program, QString(), QStringLiteral("/d C:")))
            QProcess::startDetached(program, QStringList{QStringLiteral("/d C:")});
    } else if (name == QStringLiteral("注册表")) {
        // 「注册表」：regedit.exe 清单要求管理员权限，CreateProcess 会拿 ERROR_ELEVATION_REQUIRED(740)
        // 静默失败。必须走 ShellExecute 显式 "runas" verb 触发 UAC；确认后以管理员身份前台打开。
        // （本程序普通权限受 UIPI 限制，无法对提权窗口置前，故依赖 UAC 流程自带的系统前台保证。）
        bool launched = false;
#ifdef Q_OS_WIN
        QString regPath;
        wchar_t sysBuf[MAX_PATH] = {0};
        if (GetSystemDirectoryW(sysBuf, MAX_PATH))
            regPath = QDir(QString::fromWCharArray(sysBuf)).filePath(QStringLiteral("regedit.exe"));
        if (regPath.isEmpty()) {
            const QString windir = qEnvironmentVariable("WINDIR");
            if (!windir.isEmpty()) regPath = windir + QStringLiteral("/regedit.exe");
        }
        if (!regPath.isEmpty()) {
            const HINSTANCE r = ShellExecuteW(nullptr, L"runas",
                                             reinterpret_cast<LPCWSTR>(regPath.utf16()),
                                             nullptr, nullptr, SW_SHOWNORMAL);
            launched = (reinterpret_cast<INT_PTR>(r) > 32);
        }
#endif
        if (!launched) {
            const QString windir = qEnvironmentVariable("WINDIR");
            if (!windir.isEmpty())
                QDesktopServices::openUrl(QUrl::fromLocalFile(windir + QStringLiteral("/regedit.exe")));
        }
    } else if (name == QStringLiteral("锁屏")) {
        QProcess::startDetached(QStringLiteral("rundll32.exe"), QStringList{QStringLiteral("user32.dll"), QStringLiteral("LockWorkStation")});
    } else if (name == QStringLiteral("工具管理")) {
        // 「工具管理」：打开设置中心并直接定位到「小工具」页（小工具增删/排序管理）。
        // 与 openAppearanceSettings 等同模式：局部构造模态 exec，连接建盒请求。
        // 必须 connect quickToolsOrderChanged → 助手面板 updateToolOrder：
        // 此前只有 sidepanelwidget 内部打开设置中心时才连这条信号，从本按钮打开时
        // 漏连，导致「移除/添加/排序小工具」不刷新助手面板（回归）。
        SettingCenterDialog dlg(SettingCenterDialog::TabTools, m_gridWindow);
        connect(&dlg, &SettingCenterDialog::requestCreateNewBox, this, [&dlg, this]() {
            createNewBox(&dlg);
        });
        if (m_assistantWindow && m_assistantWindow->panel()) {
            connect(&dlg, &SettingCenterDialog::quickToolsOrderChanged,
                    m_assistantWindow->panel(), &SidePanelWidget::updateToolOrder);
            connect(&dlg, &SettingCenterDialog::showMainWindowChanged,
                    this, &MainWindow::applyAssistantVisibility);
        }
        dlg.exec();
    } else if (name == QStringLiteral("上网")) {
        // 「上网」：启动系统默认浏览器并打开空白页。
        // 不能用 QDesktopServices::openUrl("about:blank")：about 协议在系统里没有
        // UserChoice 关联，部分 Windows 会弹「需要使用新应用以打开此 about 链接」。
        // 改读注册表 https 关联（UserChoice\ProgId → shell\open\command），拿到浏览器
        // 命令行后把 %1 占位符换成 about:blank 直接启动，绕开协议关联弹窗。
        QString cmdLine;
        {
            QSettings uc(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\https\\UserChoice"),
                         QSettings::NativeFormat);
            const QString progId = uc.value(QStringLiteral("ProgId")).toString();
            if (!progId.isEmpty()) {
                QSettings assoc(QStringLiteral("HKEY_CLASSES_ROOT\\") + progId + QStringLiteral("\\shell\\open\\command"),
                                QSettings::NativeFormat);
                // 注册表默认值：QSettings 里键名为 "."（空名读不到）
                cmdLine = assoc.value(QStringLiteral(".")).toString();
            }
        }
        if (!cmdLine.isEmpty()) {
            cmdLine.replace(QStringLiteral("%1"), QStringLiteral("\"about:blank\""));
            if (QProcess::startDetached(cmdLine)) return;
        }
        // 兜底：https 关联一定存在，经 QDesktopServices 打开（会带一个首页而非空白页）
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.baidu.com")));
    } else if (name == QStringLiteral("截屏")) {
        QProcess::startDetached(QStringLiteral("snippingtool.exe"));
    } else if (name == QStringLiteral("桌面整理")) {
        // 桌面整理按钮：按当前设置中心选择的模式/规则重新整理全部桌面文件。
        // 规则模式（mode=0）按整理规则分类；固定分区模式（mode=1）全部进入固定分区。
        // 整理结果会覆盖写入 CategoryStore，保证收纳盒与 Windows 桌面文件一一对应。
        organizeAndClassifyDesktop();   // 异步整理：后台线程按当前设置覆盖写入 CategoryStore，文件不搬动，搬完自动 refreshDesktop(true)
    } else if (name == QStringLiteral("快速搜索")) {
        // 「快速搜索」：打开全局文件搜索窗口（图2：文件搜索/网页搜索），带入当前助手搜索框关键字。
        const QString kw = m_assistantWindow->panel()->currentSearchText();
        m_fileSearch->showSearch(kw);
    } else if (name == QStringLiteral("定时关机")) {
        // 「定时关机」：打开任务窗口（列表页 / 设置页），任务由窗口内的调度器按 1s 心跳执行。
        m_shutdownTimer->showWindow();
    } else if (name == QStringLiteral("壁纸")) {
        // 「壁纸」：打开更换桌面壁纸窗口（系统壁纸画廊 + 本地图片选择 + 填充方式 + 应用）。
        m_wallpaper->showWindow();
    }
    // 其余快捷工具按钮静默，不弹未实现提示
}

// 响应分类变化信号
// 作者：谭征
void MainWindow::onCategoryChanged(const QString& category) {
    m_currentCategory = category;
    m_assistantWindow->panel()->clearSearch();
}

// 显示分类菜单
// 作者：谭征
void MainWindow::showCategoryMenu() {
    // 菜单可能由主窗口或任意收纳盒窗口发出，通过 sender 区分目标窗口
    auto* target = qobject_cast<IconGridWindow*>(sender());
    if (!target) target = m_gridWindow;
    if (!target) return;

    const bool isMain = (target == m_gridWindow);
    QMap<QString, QVector<DesktopItem>>* itemsMap = nullptr;
    QString* mainCurrent = nullptr;
    int boxIndex = -1;
    if (isMain) {
        itemsMap = &m_allItems;
        mainCurrent = &m_currentCategory;
    } else {
        for (int i = 0; i < m_boxes.size(); ++i) {
            if (m_boxes[i].window == target) {
                itemsMap = &m_boxes[i].items;
                boxIndex = i;
                break;
            }
        }
    }
    if (!itemsMap) return;

    auto currentCat = [&]() -> QString {
        return mainCurrent ? *mainCurrent : target->currentCategory();
    };
    auto setCurrentCat = [&](const QString& cat) {
        if (mainCurrent) *mainCurrent = cat;
        target->setCurrentCategory(cat);
    };

    QWidget* parent = target;
    QMenu menu(parent);
    Theme::applyMenuStyle(&menu);
    menu.setFixedWidth(190);

    // 查看：大图标 / 中等图标 / 小图标 / 列表（当前档位打勾）。
    // 档位由 IconGridWindow 统一实现（单元格尺寸 + 图标绘制像素），并持久化到本地设置。
    // 拍平进主菜单：QMenu 子菜单是独立顶层弹窗，即使各自加 WindowStaysOnTopHint，
    // 鼠标从父菜单移入子菜单时 Qt 的 popup 级联会把它关闭（移动即隐藏）。本程序内所有
    // 上下文菜单子菜单一律拍平，避免跨窗级联关闭。
    {
        const int curView = target->viewMode();
        auto addViewAction = [&](const QString& text, int mode) {
            QAction* act = menu.addAction(text);
            act->setCheckable(true);
            act->setChecked(curView == mode);
        };
        addViewAction(QStringLiteral("大图标"),   IconGridWindow::ViewLarge);
        addViewAction(QStringLiteral("中等图标"), IconGridWindow::ViewMedium);
        addViewAction(QStringLiteral("小图标"),   IconGridWindow::ViewSmall);
        addViewAction(QStringLiteral("列表"),     IconGridWindow::ViewList);
    }
    menu.addSeparator();

    // 排序方式（名称 / 大小 / 类型 / 修改日期）
    menu.addAction(QStringLiteral("名称"));
    menu.addAction(QStringLiteral("大小"));
    menu.addAction(QStringLiteral("类型"));
    menu.addAction(QStringLiteral("修改日期"));
    menu.addSeparator();

    menu.addAction(Theme::icon("refresh"), QStringLiteral("刷新"));
    menu.addSeparator();
    // 全屏收纳（Fences）模式：不再提供“进入全屏收纳”入口（用户要求去掉该菜单项）。
    // 若当前已在全屏模式，仍保留“退出全屏收纳”与“重排收纳盒”，避免无法回到窗口视图。
    if (target->isFencesMode()) {
        menu.addAction(Theme::icon("view_grid"), QStringLiteral("退出全屏收纳"));
        // 盒子可自由拖动/缩放，位置按分类记住；这里提供一键回到自动排布
        menu.addAction(Theme::icon("refresh"), QStringLiteral("重排收纳盒"));
        menu.addSeparator();
    }
    // 粘贴 / 粘贴快捷方式：复用桌面命名空间原生动词（ShellOps::pasteToDesktop）。
    // 剪贴板无文件数据时置灰，与资源管理器/360 桌面助手行为一致。
    const bool canPaste = ShellOps::clipboardHasFileData();
    QAction* actPaste = menu.addAction(Theme::icon("add"), QStringLiteral("粘贴"));
    QAction* actPasteLink = menu.addAction(Theme::icon("home"), QStringLiteral("粘贴快捷方式"));
    actPaste->setEnabled(canPaste);
    actPasteLink->setEnabled(canPaste);
    menu.addSeparator();
    // 「显示主界面/隐藏主界面」：与设置中心常规页共用同一开关（applyAssistantVisibility）。
    // 放到常驻桌面右键菜单，即使助手面板被隐藏也能随时显隐——否则隐藏后设置入口不可达，
    // 再也无法把助手面板显示回来（与 360 桌面助手右键菜单同一思路）。
    QAction* actShowMain = nullptr;
    if (m_assistantWindow) {
        actShowMain = menu.addAction(Theme::icon("view_grid"),
                                     QStringLiteral("显示主界面/隐藏主界面"));
        actShowMain->setCheckable(true);
        actShowMain->setChecked(m_assistantWindow->isVisible());
    }
    menu.addAction(Theme::icon("appearance"), QStringLiteral("改变外观"));
    menu.addAction(Theme::icon("settings"), QStringLiteral("整理规则设定"));
    menu.addSeparator();
    menu.addAction(Theme::icon("rename"), QStringLiteral("重命名"));
    menu.addAction(Theme::icon("delete"), QStringLiteral("解散当前分类"));
    menu.addAction(Theme::icon("close"), QStringLiteral("解散收纳盒"));
    menu.addSeparator();

    // 新建（拍平：新建分类 / 新建收纳盒，见上方注释）
    menu.addAction(Theme::icon("new_category"), QStringLiteral("新建分类"));
    menu.addAction(Theme::icon("new_box"), QStringLiteral("新建收纳盒"));

    // 复刻 360：菜单自身置顶（WindowStaysOnTopHint）即可浮在其它程序之上，
    // 但【绝不提升 owner 窗口】——提升 owner 会横跨 app band 触发 dock 闪烁。
    // AllowSetForegroundWindow 在鼠标事件内调用（输入新鲜）可成功，保住菜单首次点击不被吞。
    menu.setWindowFlag(Qt::WindowStaysOnTopHint, true);
#ifdef Q_OS_WIN
    AllowSetForegroundWindow(GetCurrentProcessId());
#endif
    QAction* chosen = menu.exec(QCursor::pos());
    if (!chosen) return;

    // 显示主界面/隐藏主界面：勾选=显示助手面板，取消=隐藏（与设置中心常规页同一开关）。
    if (chosen == actShowMain && actShowMain) {
        applyAssistantVisibility(chosen->isChecked());
        return;
    }

    const QString text = chosen->text();

    // —— 查看：大图标 / 中等图标 / 小图标 / 列表 ——
    // 主窗口与所有收纳盒窗口共用同一档位（持久化在 IconGridWindow/viewMode），
    // 因此这里对所有已打开的网格窗口一起应用，避免“主窗口换了、盒子还是旧尺寸”。
    if (text == QStringLiteral("大图标") || text == QStringLiteral("中等图标")
        || text == QStringLiteral("小图标") || text == QStringLiteral("列表")) {
        int mode = IconGridWindow::ViewMedium;
        if (text == QStringLiteral("大图标")) mode = IconGridWindow::ViewLarge;
        else if (text == QStringLiteral("小图标")) mode = IconGridWindow::ViewSmall;
        else if (text == QStringLiteral("列表")) mode = IconGridWindow::ViewList;
        target->setViewMode(mode);
        if (m_gridWindow && m_gridWindow != target) m_gridWindow->setViewMode(mode);
        for (const OrganizerBox& box : m_boxes) {
            if (box.window && box.window != target) box.window->setViewMode(mode);
        }
        // 写入本地持久化数据：重启后保持所选档位（默认中等图标）。
        SettingsManager sm;
        sm.saveValue(QStringLiteral("IconGridWindow/viewMode"), mode);
        sm.sync();
        return;
    }

    if (text == QStringLiteral("刷新")) {
        if (isMain) {
            refreshDesktop();
        } else {
            target->setItems(*itemsMap);
        }
        return;
    } else if (text == QStringLiteral("退出全屏收纳")) {
        // 全屏收纳（Fences）模式开关：退出后还原进入前的浮动几何。主窗口状态持久化。
        const bool next = false;
        target->setFencesMode(next);
        if (target == m_gridWindow) {
            SettingsManager sm;
            sm.saveValue(QStringLiteral("IconGridWindow/fencesMode"), next);
            sm.sync();
        }
        saveLayout();
        return;
    } else if (text == QStringLiteral("重排收纳盒")) {
        // 清除所有盒子的自定义位置/尺寸，按图标数量重新流式铺开
        target->resetFenceLayout();
        return;
    } else if (text == QStringLiteral("重命名")) {
        const QString oldName = currentCat();
        if (oldName.isEmpty() || !itemsMap->contains(oldName)) return;
        bool ok = false;
        QString newName = GlassInputDialog::getText(parent, QStringLiteral("重命名分类"),
                                                QStringLiteral("新名称:"), QLineEdit::Normal,
                                                oldName, &ok);
        if (!ok || newName.isEmpty() || newName == oldName) return;
        if (IconGridWindow::isHiddenCategory(newName)) {
            GlassMessageBox::warning(parent, QStringLiteral("保留名称"),
                                 QStringLiteral("'%1' 为系统保留分类名称，不可使用。").arg(newName));
            return;
        }
        if (itemsMap->contains(newName)) {
            GlassMessageBox::warning(parent, QStringLiteral("重复"),
                                 QStringLiteral("分类 '%1' 已存在。").arg(newName));
            return;
        }
        if (isMain) {
            // 主窗口：必须与选项卡右键重命名走同一持久化路径（renameCategory 内部会记录
            // “原始扫描名 -> 新名”映射并写入 IconGridWindow/categoryRenames），否则重启后
            // 扫描重新产生原名、items 仍挂在该名下，改名后的分类要么变成空分类、要么被原名覆盖。
            QVector<DesktopItem> items = m_allItems.take(oldName);
            m_allItems.insert(newName, items);
            if (m_currentCategory == oldName) m_currentCategory = newName;
            m_gridWindow->renameCategory(oldName, newName);
            saveLayout();
        } else {
            // 收纳盒：会话内改名，不持久化到主窗口键；仅更新当前收纳盒自身
            QVector<DesktopItem> items = itemsMap->take(oldName);
            itemsMap->insert(newName, items);
            if (currentCat() == oldName) setCurrentCat(newName);
            target->setItems(*itemsMap);
            target->setCurrentCategory(currentCat());
            if (itemsMap->size() == 1 && boxIndex >= 0) {
                // 收纳盒只有一个分类时，重命名同步更新窗口标题
                m_boxes[boxIndex].name = newName;
                target->setWindowTitle(newName);
            }
        }
    } else if (text == QStringLiteral("解散当前分类")) {
        const QString name = currentCat();
        if (name.isEmpty() || !itemsMap->contains(name)) return;
        if (name == kSystemCategory) {
            GlassMessageBox::warning(parent, QStringLiteral("提示"),
                                 QStringLiteral("“系统”分类（我的电脑/回收站/网络）为常驻项，不可解散。"));
            return;
        }
        if (isMain) {
            // 主窗口：解散分类 → 图标移入“其他”/首个剩余分类（保持归整，不回桌面）
            // 不论主窗口还是收纳盒都不再关闭当前窗口（收纳盒关闭由"解散收纳盒"负责）
            QVector<DesktopItem> items = itemsMap->take(name);
            // 解散分类时，把该分类下图标的 shellPath 从 Dock 隐藏集合中移除，使它们重新在 Dock 上显示
            if (m_dock && !items.isEmpty()) {
                QSet<QString> restore;
                for (const auto& it : items)
                    if (!it.sourcePath.isEmpty())
                        restore.insert(QDir::fromNativeSeparators(it.sourcePath).toLower());
                m_dock->removeHiddenShellPaths(restore);
            }
            QString newCurrent = currentCat();
            QString disbandFallback;   // 解散时图标并入的兜底分类（仅主窗口记录到“已解散”集合）
            if (!itemsMap->isEmpty()) {
                // 仍有剩余分类：图标优先并入“其它”（兜底分类），否则并入首个剩余分类
                QString targetCat = QStringLiteral("其它");
                if (!itemsMap->contains(targetCat)) {
                    targetCat = itemsMap->firstKey();
                }
                if (!items.isEmpty()) (*itemsMap)[targetCat].append(items);
                if (name == newCurrent || !itemsMap->contains(newCurrent)) {
                    newCurrent = targetCat;
                }
                disbandFallback = targetCat;
            } else {
                // 解散的是最后一个分类：不再生成“其他”，当前分类直接置空，避免
                // m_currentCategory 指向已删除的分类，导致下一次解散因找不到分类而无效
                newCurrent = QString();
            }
            // 先清理持久化中的该分类，避免 setItems()/refreshDesktop() 通过 applyPersistedCategories 再次把它补回来
            target->removeCategoryFromPersistence(name);
            // 主窗口：记入“已解散”集合，使规则分类的解散在重启/刷新后持续生效（收纳盒无扫描，无需记录）
            markCategoryDisbanded(name, disbandFallback);

            const bool disbandShortcut = (name == QStringLiteral("快捷方式"));
            const bool becameEmpty = itemsMap->isEmpty();
            const bool prevForce = target->forceShortcutCategory();
            if (disbandShortcut || becameEmpty) {
                // 解散“快捷方式”后不再强制补回；解散最后一个非快捷方式分类后，
                // 若保持原强制策略，setItems 会自动把“快捷方式”补回，导致窗口无法真正变空
                target->setForceShortcutCategory(false);
            }
            target->setItems(*itemsMap);
            if (!disbandShortcut && !becameEmpty) {
                target->setForceShortcutCategory(prevForce);
            }
            // 同步当前分类：setCurrentCat 已同步主窗口 m_currentCategory 与 target 的 currentCategory
            if (IconGridWindow::isHiddenCategory(newCurrent)) {
                newCurrent = target->firstVisibleCategory();
            }
            setCurrentCat(newCurrent);
            saveLayout();
            // 持久化主窗口“快捷方式”是否仍需强制存在，确保重启/刷新后保持解散状态
            SettingsManager sm;
            sm.saveValue(QStringLiteral("mainForceShortcutCategory"), target->forceShortcutCategory());
        } else {
            // 收纳盒：解散分类 → 该分类图标彻底释放回桌面（Dock 重新显示），不再并入任何其他分类。
            DiagTrace::log(QStringLiteral("[release] box-branch name=%1 contains=%2 itemsMapSize=%3")
                               .arg(name).arg(itemsMap->contains(name)).arg(itemsMap->size()));
            QVector<DesktopItem> items = itemsMap->take(name);
            QSet<QString> normPaths;
            for (const auto& it : items)
                if (!it.sourcePath.isEmpty())
                    normPaths.insert(QDir::fromNativeSeparators(it.sourcePath).toLower());
            // releasePathsToDock：从 m_allItems / 各 box.items / CategoryStore 移除并重新计算 Dock
            // 隐藏集合，使这些图标在桌面镜像重新出现；它已 saveLayout，但本盒剩余分类尚未持久化，下面再存一次。
            releasePathsToDock(normPaths);
            target->removeCategoryFromPersistence(name);
            // 当前分类：还有剩余分类则保持/落到首个，否则置空
            QString newCurrent = itemsMap->isEmpty() ? QString()
                                                     : (itemsMap->contains(currentCat()) ? currentCat()
                                                                                         : itemsMap->firstKey());
            if (IconGridWindow::isHiddenCategory(newCurrent)) {
                newCurrent = itemsMap->isEmpty() ? QString() : itemsMap->firstKey();
            }
            target->setItems(*itemsMap);
            setCurrentCat(newCurrent);
            saveLayout();
        }
    } else if (text == QStringLiteral("新建分类")) {
        bool ok = false;
        QString title = GlassInputDialog::getText(parent, QStringLiteral("新建分类"),
                                              QStringLiteral("分类名称:"), QLineEdit::Normal,
                                              QStringLiteral("新分类"), &ok);
        if (!ok || title.isEmpty()) return;
        if (IconGridWindow::isHiddenCategory(title)) {
            GlassMessageBox::warning(parent, QStringLiteral("保留名称"),
                                 QStringLiteral("'%1' 为系统保留分类名称，不可使用。").arg(title));
            return;
        }
        if (itemsMap->contains(title)) {
            GlassMessageBox::warning(parent, QStringLiteral("重复"),
                                 QStringLiteral("分类 '%1' 已存在。").arg(title));
            return;
        }
        itemsMap->insert(title, {});
        setCurrentCat(title);
        target->setItems(*itemsMap);
        target->setCurrentCategory(title);
        if (isMain) saveLayout();
    } else if (text == QStringLiteral("新建收纳盒")) {
        createNewBox();
    } else if (text == QStringLiteral("改变外观")) {
        openAppearanceSettings();
    } else if (text == QStringLiteral("整理规则设定")) {
        openDesktopOrganizeSettings();
    } else if (text == QStringLiteral("粘贴") || text == QStringLiteral("粘贴快捷方式")) {
        // 在收纳盒（或主整理窗口）某个分类里：把 Dock/资源管理器复制的图标
        // 粘贴进来 —— 复用桌面命名空间原生动词，落盘到桌面后归入当前分类。
        const QString cat = currentCat();
        if (cat.isEmpty()) {
            GlassMessageBox::information(parent, QStringLiteral("提示"),
                                     QStringLiteral("请先选择一个分类，再将图标粘贴进来。"));
            return;
        }
        const bool shortcut = (text == QStringLiteral("粘贴快捷方式"));
        pasteIntoCategory(target, itemsMap, boxIndex, cat, shortcut);
        return;
    } else if (text == QStringLiteral("解散收纳盒")) {
        // “解散收纳盒”始终解散当前打开菜单的窗口本身
        disbandBoxWindow(target);
    } else if (text == QStringLiteral("退出")) {
        // A5：本分支按**菜单文本**分发，而"退出"与用户自定义分类名共用同一个
        // 字符串空间 —— 用户只要建一个名为「退出」的分类，从本菜单点它就会【静默退出程序】，
        // 现象正是"程序自己关闭了"且无任何日志、极难定位。
        // 收紧为：仅当该文本**不是当前窗口的分类名**时，才当作内置退出动作。
        // itemsMap 即「分类名 → 图标项」映射，是判定"这是不是一个分类"的权威依据。
        if (itemsMap && itemsMap->contains(text)) {
            DiagTrace::log(QStringLiteral("[menu] 名为「退出」的分类被点击，已拦截误退出"));
            return;
        }
        qApp->quit();
    } else {
        GlassMessageBox::information(parent, QStringLiteral("提示"),
                                 QStringLiteral("功能 \"%1\" 暂未实现。").arg(text));
    }
}


// 粘贴 / 粘贴快捷方式 到收纳盒分类
// 复用桌面命名空间原生动词把剪贴板文件落到桌面，再由轮询检测到新文件后归入当前分类。
// 与「从 Dock 拖图标进收纳盒」(scheduleDockItemsIntoBox) 完全同口径：
// · 以 m_allItems 为唯一真值（主网格 + 各收纳盒共享）；
// · 写 CategoryStore（A 方案：只记 path → 分类，绝不搬动真实文件）；
// · 同步命中的收纳盒窗口 items + 主网格刷新；
// · resetDockHiddenFromAllItems 让新文件从 Dock（桌面镜像）隐藏，只出现在收纳盒里。
// 作者：谭征
void MainWindow::pasteIntoCategory(IconGridWindow* target,
                                   QMap<QString, QVector<DesktopItem>>* itemsMap,
                                   int boxIndex, const QString& category, bool shortcut)
{
    Q_UNUSED(itemsMap)
    Q_UNUSED(boxIndex)
    if (!target || category.isEmpty()) return;

    void* ownerHwnd = nullptr;
#ifdef Q_OS_WIN
    if (target->winId()) ownerHwnd = reinterpret_cast<void*>(target->winId());
#endif

    // 快照：当前桌面根目录的文件集合（粘贴后新增项即落在此处）。
    const QString desk = CategoryStore::desktopPath();
    QSet<QString> before;
    {
        QDir d(desk);
        d.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
        const QFileInfoList list = d.entryInfoList();
        for (const QFileInfo& fi : list)
            before.insert(QDir::fromNativeSeparators(fi.absoluteFilePath()).toLower());
    }

    // 菜单已按剪贴板是否有文件置灰；这里再守一道，避免极端时序下误粘贴。
    if (!ShellOps::clipboardHasFileData()) {
        GlassMessageBox::information(target, QStringLiteral("提示"),
                                 QStringLiteral("剪贴板中没有可粘贴的文件。"));
        return;
    }
    // 复制语义 / 粘贴快捷方式：粘贴会在桌面生成新项（副本文件 或 .lnk），
    // ③② 将其归入当前分类 → 由 ⑥ resetDockHiddenFromAllItems() 从 Dock 隐藏其镜像，
    // 只留在收纳盒显示；源文件 A 始终不归类、不归档 → 继续留在 Dock。
    if (!ShellOps::pasteToDesktop(shortcut ? QStringLiteral("pastelink")
                                          : QStringLiteral("paste"), ownerHwnd)) {
        // 命中原生「粘贴」动词但调用未成功（罕见，如剪贴板被其它进程占用）。
        GlassMessageBox::information(target, QStringLiteral("提示"),
                                 QStringLiteral("粘贴失败，请稍后重试。"));
        return;
    }

    // 启动轮询：原生粘贴是异步的，文件落盘稍后才完成；等到新文件出现即归类。
    m_pasteBefore = before;
    m_pasteCategory = category;
    m_pasteTarget = target;
    m_pasteTicks = 0;
    m_pasteIntents.clear();   // 清掉上一次粘贴遗留的意图（若有未消费项）
    if (!m_pasteWatcher) {
        m_pasteWatcher = new QTimer(this);
        connect(m_pasteWatcher, &QTimer::timeout, this, &MainWindow::onPasteWatchTick);
    }
    m_pasteWatcher->start(120);
}

// 粘贴 / 粘贴快捷方式：轮询等待桌面命名空间原生粘贴落盘后归类到当前分类。
// 作者：谭征
void MainWindow::onPasteWatchTick()
{
    if (!m_pasteTarget || m_pasteWatcher == nullptr) { if (m_pasteWatcher) m_pasteWatcher->stop(); return; }
    m_pasteTicks += 1;

    const QString desk = CategoryStore::desktopPath();
    // key = 归一化小写（仅用于与快照做大小写不敏感比对）；value = **原始大小写绝对路径**。
    // 必须保留原始大小写：CategoryStore/refreshDesktop 以原始大小写路径为键，若这里用小写
    // 入库，则 refreshDesktop 的映射查找（区分大小写）会漏配 → 副本被当“未分类”从 m_allItems
    // 剔除（B 重新冒回 Dock、C 消失），重启后同样加载不了 C。
    QHash<QString, QString> nowOrig;
    {
        QDir d(desk);
        d.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
        const QFileInfoList list = d.entryInfoList();
        for (const QFileInfo& fi : list) {
            const QString abs = fi.absoluteFilePath();
            nowOrig.insert(QDir::fromNativeSeparators(abs).toLower(), abs);
        }
    }

    QList<QString> added;
    for (auto it = nowOrig.constBegin(); it != nowOrig.constEnd(); ++it)
        if (!m_pasteBefore.contains(it.key())) added.append(it.key());

    if (!added.isEmpty()) {
        // 与 scheduleDockItemsIntoBox 同一隔离原则：粘贴进用户盒只写本盒 box.items，
        // 绝不写 m_allItems / CategoryStore，否则默认盒会凭分类名“长出”同名分类。
        const bool pasteToDefault = (m_pasteTarget == m_gridWindow);
        OrganizerBox* pasteBox = nullptr;
        if (!pasteToDefault) {
            for (auto& b : m_boxes) { if (b.window == m_pasteTarget) { pasteBox = &b; break; } }
            if (!pasteBox) { m_pasteWatcher->stop(); return; }   // 目标盒已销毁/解散
        }
        for (const QString& norm : added) {
            // 登记粘贴意图：本文件是本次粘贴产生的新副本，应归入 m_pasteCategory。
            // SHChangeNotify 可能抢在归档前触发 refreshDesktop(false,false) 把未落盘映射的副本
            // 当成“未分类散件”剔除；refreshDesktop 会用此意图把它重新归回目标分类并落盘。
            m_pasteIntents.insert(norm, m_pasteCategory);
            const QString realPath = nowOrig.value(norm, QDir::toNativeSeparators(norm));   // 原始大小写
            // 已在目标分类里（refreshDesktop 的粘贴意图兜底已先行写入）则跳过，避免重复添加。
            bool alreadyInTarget = false;
            if (pasteToDefault) {
                for (auto it = m_allItems.constBegin(); it != m_allItems.constEnd() && !alreadyInTarget; ++it) {
                    for (const auto& it2 : it.value()) {
                        if (!it2.sourcePath.isEmpty()
                            && QDir::fromNativeSeparators(it2.sourcePath).compare(norm, Qt::CaseInsensitive) == 0) {
                            if (it.key() == m_pasteCategory) alreadyInTarget = true; break;
                        }
                    }
                }
            } else {
                auto vit = pasteBox->items.constFind(m_pasteCategory);
                if (vit != pasteBox->items.constEnd()) {
                    for (const auto& it2 : *vit) {
                        if (!it2.sourcePath.isEmpty()
                            && QDir::fromNativeSeparators(it2.sourcePath).compare(norm, Qt::CaseInsensitive) == 0)
                            { alreadyInTarget = true; break; }
                    }
                }
            }
            if (alreadyInTarget) continue;

            // ① 先从其它分类里摘掉同路径条目（一个文件只能属于一个分类）。
            for (auto it = m_allItems.begin(); it != m_allItems.end(); ++it) {
                QVector<DesktopItem>& vec = it.value();
                for (int k = vec.size() - 1; k >= 0; --k) {
                    if (!vec[k].sourcePath.isEmpty()
                        && QDir::fromNativeSeparators(vec[k].sourcePath).compare(norm, Qt::CaseInsensitive) == 0)
                        vec.removeAt(k);
                }
            }
            // ② 构造条目并加入目标分类（字段与 refreshDesktop / scheduleDockItemsIntoBox 一致）。
            DesktopItem item = DesktopScanner::makeItem(realPath);
            item.icon = QIcon();            // 图标由按钮异步加载
            item.systemImageIndex = -1;
            item.category = m_pasteCategory;
            if (pasteToDefault) {
                // 默认收纳盒：写入共享模型 + 共享分类库（刷新/重启读回）
                m_allItems[m_pasteCategory].append(item);
                CategoryStore::setCategory(realPath, m_pasteCategory);
            } else {
                // 用户新建收纳盒：仅写本盒 box.items，并清掉任何旧的共享分类映射，避免刷新被带回。
                CategoryStore::clearCategory(realPath);
                pasteBox->items[m_pasteCategory].append(item);
            }
        }
        markBoxAssignmentUserHandled();

        // ③b 复制语义的「隐藏」由"分类"本身完成，无需额外归档源文件：
        // · 已把粘贴生成的副本归入 m_pasteCategory（默认盒写 m_allItems+CategoryStore，
        // 用户盒写本盒 box.items）；
        // · resetDockHiddenFromAllItems() 把各盒 items 路径并入 Dock 隐藏集
        // → 副本的 Dock 镜像（B）自动隐藏，只在收纳盒显示（C）；
        // · 源文件 A 既不归类也不归档 → 继续留在 Dock 显示（用户要求：不要动源文件）。

        if (pasteToDefault) {
            // ④ 命中的收纳盒窗口（及本身含该分类的盒子）同步显示。
            for (auto& box : m_boxes) {
                if (!box.window) continue;
                if (box.window == m_pasteTarget || box.items.contains(m_pasteCategory))
                    box.items[m_pasteCategory] = m_allItems.value(m_pasteCategory);
                box.window->setItems(box.items);
            }
            // ⑤ 主整理窗口全量刷新。
            if (m_gridWindow) {
                m_gridWindow->setItems(m_allItems);
                m_currentCategory = m_gridWindow->currentCategory();
                m_gridWindow->setCurrentCategory(m_currentCategory);
            }
        } else {
            // 用户盒：重发本盒数据即可（与其它盒、默认盒互不影响）
            if (pasteBox->window) pasteBox->window->setItems(pasteBox->items);
        }
        // ⑥ 持久化 + 让新文件从 Dock（桌面镜像）隐藏，只出现在收纳盒里。
        saveLayout();
        resetDockHiddenFromAllItems();
        if (m_dock) m_dock->saveDockSnapshot();
        // ⑦ 刚收进来的图标立即选中并武装快捷键作用域。
        m_pasteTarget->selectIconByShellPathAndArm(added.first());

        m_pasteWatcher->stop();
        return;
    }

    // 超时（~5s）仍未出现新文件：多半粘贴被系统取消（如文件名冲突用户点了“跳过”且无新增），静默放弃。
    if (m_pasteTicks >= 42) {
        m_pasteWatcher->stop();
    }
}

// 添加新建分类
// 作者：谭征
void MainWindow::addNewCategory() {
    QWidget* parent = m_gridWindow ? static_cast<QWidget*>(m_gridWindow) : static_cast<QWidget*>(this);
    bool ok = false;
    QString title = GlassInputDialog::getText(parent, QStringLiteral("新建分类"),
                                          QStringLiteral("分类名称:"), QLineEdit::Normal,
                                          QStringLiteral("新分类"), &ok);
    if (!ok || title.isEmpty()) return;
    if (m_allItems.contains(title)) {
        GlassMessageBox::warning(parent, QStringLiteral("重复"), QStringLiteral("分类 '%1' 已存在。").arg(title));
        return;
    }
    m_allItems.insert(title, {});
    unmarkCategoryDisbanded(title);   // 用户主动新建同名分类：取消其“已解散”标记，使其可正常显示
    m_currentCategory = title;
    m_gridWindow->setItems(m_allItems);
    m_gridWindow->setCurrentCategory(title);
    m_gridWindow->persistCategoryOrder();   // 持久化新建的分类名称
    saveLayout();
}

// 响应request新建分类
// 作者：谭征
void MainWindow::onRequestNewCategory() {
    auto* target = qobject_cast<IconGridWindow*>(sender());
    if (!target) target = m_gridWindow;
    if (!target) return;

    const bool isMain = (target == m_gridWindow);
    int boxIndex = -1;
    QMap<QString, QVector<DesktopItem>>* itemsMap = isMain ? &m_allItems : nullptr;
    if (!isMain) {
        for (int i = 0; i < m_boxes.size(); ++i) {
            if (m_boxes[i].window == target) {
                itemsMap = &m_boxes[i].items;
                boxIndex = i;
                break;
            }
        }
    }
    if (!itemsMap) return;

    QWidget* parent = target;
    bool ok = false;
    QString title = GlassInputDialog::getText(parent, QStringLiteral("新建分类"),
                                          QStringLiteral("分类名称:"), QLineEdit::Normal,
                                          QStringLiteral("新分类"), &ok);
    if (!ok || title.isEmpty()) return;
    if (IconGridWindow::isHiddenCategory(title)) {
        GlassMessageBox::warning(parent, QStringLiteral("保留名称"),
                             QStringLiteral("'%1' 为系统保留分类名称，不可使用。").arg(title));
        return;
    }
    if (itemsMap->contains(title)) {
        GlassMessageBox::warning(parent, QStringLiteral("重复"),
                             QStringLiteral("分类 '%1' 已存在。").arg(title));
        return;
    }
    itemsMap->insert(title, {});
    unmarkCategoryDisbanded(title);   // 用户主动新建同名分类：取消其“已解散”标记，使其可正常显示
    target->setItems(*itemsMap);
    target->setCurrentCategory(title);
    if (isMain) {
        m_currentCategory = title;
        m_gridWindow->persistCategoryOrder();   // 持久化新建的分类名称
        saveLayout();
    } else if (itemsMap->size() == 1 && boxIndex >= 0) {
        m_boxes[boxIndex].name = title;
        target->setWindowTitle(title);
    }
}

// 重命名当前分类
// 作者：谭征
void MainWindow::renameCurrentCategory() {
    renameCategoryByName(m_currentCategory);
}

// 重命名分类by名称
// 作者：谭征
void MainWindow::renameCategoryByName(const QString& name) {
    if (name.isEmpty() || !m_allItems.contains(name)) return;

    QWidget* parent = m_gridWindow ? static_cast<QWidget*>(m_gridWindow) : static_cast<QWidget*>(this);
    bool ok = false;
    QString title = GlassInputDialog::getText(parent, QStringLiteral("重命名分类"),
                                          QStringLiteral("新名称:"), QLineEdit::Normal,
                                          name, &ok);
    if (!ok || title.isEmpty() || title == name) return;
    if (IconGridWindow::isHiddenCategory(title)) {
        GlassMessageBox::warning(parent, QStringLiteral("保留名称"),
                             QStringLiteral("'%1' 为系统保留分类名称，不可使用。").arg(title));
        return;
    }
    if (m_allItems.contains(title)) {
        GlassMessageBox::warning(parent, QStringLiteral("重复"), QStringLiteral("分类 '%1' 已存在。").arg(title));
        return;
    }

    // 原地重命名指定分类：只修改该分类名称，不会新增额外分类；若不是当前选中项也不强制切换
    const QString oldName = name;
    QVector<DesktopItem> items = m_allItems.take(oldName);
    m_allItems.insert(title, items);
    if (m_currentCategory == oldName) {
        m_currentCategory = title;
    }

    // 同步网格窗口原地改名，不重走 setItems 的“快捷方式”自动补全逻辑，避免重命名“快捷方式”时又多出一个空的“快捷方式”选项卡
    m_gridWindow->renameCategory(oldName, title);
    saveLayout();
}

// 打开appearance设置
// 作者：谭征
void MainWindow::openAppearanceSettings() {
    SettingCenterDialog dlg(SettingCenterDialog::TabAppearance, m_gridWindow);
    connect(&dlg, &SettingCenterDialog::requestCreateNewBox, this, [&dlg, this]() {
        createNewBox(&dlg);
    });
    // 「显示主界面/隐藏主界面」复选框：本入口（收纳盒/桌面右键「改变外观」）此前漏连，
    // 面板已隐藏时从这里打开设置改复选框不会显示回来——与 工具管理 入口保持一致。
    connect(&dlg, &SettingCenterDialog::showMainWindowChanged,
            this, &MainWindow::applyAssistantVisibility);
    dlg.exec();
}

// 打开桌面organize设置
// 作者：谭征
void MainWindow::openDesktopOrganizeSettings() {
    SettingCenterDialog dlg(SettingCenterDialog::TabDesktopOrganize, m_gridWindow);
    connect(&dlg, &SettingCenterDialog::requestCreateNewBox, this, [&dlg, this]() {
        createNewBox(&dlg);
    });
    // 「显示主界面/隐藏主界面」复选框：本入口（收纳盒/桌面右键「整理规则设定」）此前漏连，
    // 面板已隐藏时从这里打开设置改复选框不会显示回来——与 工具管理 入口保持一致。
    connect(&dlg, &SettingCenterDialog::showMainWindowChanged,
            this, &MainWindow::applyAssistantVisibility);
    dlg.exec();
}

// 创建新建盒子
// 作者：谭征
void MainWindow::createNewBox(QWidget* parent) {
    if (!parent) {
        parent = m_gridWindow ? static_cast<QWidget*>(m_gridWindow) : static_cast<QWidget*>(this);
    }
    bool ok = false;
    QString title = GlassInputDialog::getText(parent, QStringLiteral("新建收纳盒"),
                                          QStringLiteral("收纳盒名称:"), QLineEdit::Normal,
                                          QStringLiteral("新收纳盒"), &ok);
    if (!ok || title.isEmpty()) return;

    OrganizerBox box;
    box.id = QUuid::createUuid().toString();
    box.name = title;
    box.items.insert(title, {});

    box.window = new IconGridWindow(nullptr);
    box.window->setWindowTitle(title);
    box.window->setForceShortcutCategory(false);
    box.window->setCloseButtonVisible(false); // 后续新建收纳盒不显示标题栏关闭按钮，通过菜单解散
    box.window->setIsBoxWindow(true);         // 标识为独立收纳盒窗口，右键菜单显示“解散收纳盒”
    box.window->setItems(box.items);
    box.window->setCurrentCategory(title);

    // 收纳盒窗口：关闭/菜单/新建分类/解散/改名/删除/跨窗拖放等全部主窗口侧信号，
    // 与默认收纳盒（主整理窗口）保持一致；统一走 connectBoxWindow。
    connectBoxWindow(box.window);

    // 收纳盒窗口默认尺寸/位置（用户指定）：高度与默认收纳盒（主整理窗口）相同、宽度为其一半；
    // 位置放在默认收纳盒正下方（留 5px 间距），且左边与默认收纳盒对齐；夹在屏幕可用区内。
    // 锁定态：m_locked 默认 false（解锁）且盒窗口不加载主窗口共享锁状态（icongridwindow.cpp
    // 构造期 !m_isBoxWindow 守卫），新建盒锁图标即为"解开"，无需额外处理。
    QRect screen = currentScreen()->availableGeometry();
    QSize bs(480, 720);
    QPoint pos;
    if (m_gridWindow && m_gridWindow->isVisible()) {
        bs = QSize(qMax(320, m_gridWindow->width() / 2), m_gridWindow->height());
        pos = QPoint(m_gridWindow->x(), m_gridWindow->y() + m_gridWindow->height() + 5);
    } else {
        // 默认收纳盒不可见时回落：屏幕居中
        pos = QPoint(screen.x() + qMax(0, (screen.width() - bs.width()) / 2),
                     screen.y() + qMax(0, (screen.height() - bs.height()) / 2));
    }
    // 多个收纳盒依次向右下错位堆叠，避免完全重叠
    for (int i = 0; i < m_boxes.size(); ++i) {
        pos += QPoint(28, 28);
    }
    pos.setX(qBound(screen.x(), pos.x(), screen.x() + qMax(0, screen.width() - bs.width())));
    pos.setY(qBound(screen.y(), pos.y(), screen.y() + qMax(0, screen.height() - bs.height())));
    box.window->setGeometry(pos.x(), pos.y(), bs.width(), bs.height());
    box.window->show();
#ifdef Q_OS_WIN
    // 收纳盒窗口同样登记进抗“显示桌面”守卫，保证“显示桌面”/Win+D/Win+M 下常驻可见。
    DesktopMirrorWindow::registerCloakTarget(box.window);
#endif

    m_boxes.append(box);
}

// 快捷操作②：在桌面空白处绘制创建收纳盒（设置中心「快捷操作」页）
// 消费链条：Dock 空白处按下 → WH_MOUSE_LL 通道起框 → 松手（DesktopMirrorWindow::endRubber）
// → 算出框内图标 → emit boxRegionDrawn → 本函数按框建盒 + 收编框内图标。
// 与「新建收纳盒」共用同一套窗口配置/信号接线/常驻守卫，差别只有两点：
// ① 几何＝用户画出的那个框（不再用"默认盒下方错位堆叠"的推算位置）；
// ② 框内图标直接归入新盒的默认分类 —— 复用「从 Dock 拖图标进收纳盒」那条成熟通道
// （scheduleDockItemsIntoBox：只写本盒数据 + 重算 Dock 隐藏集合 + 落盘），
// 所以那批图标会立刻从桌面消失、只出现在新盒子里，与手动拖拽的结果完全一致。
// 作者：谭征
void MainWindow::createBoxFromDockRegion(const QRect& globalRect, const QStringList& shellPaths,
                                         const QStringList& displayNames) {
    if (globalRect.isEmpty()) return;

    // ⓪ 先弹框确认再建盒：桌面空白处随手拉个框就冒出一个收纳盒太"手滑"（尤其误触时），
    // 先让用户明确点一下「创建收纳盒」；框内有图标时把数量告知，取消则什么都不做
    // （框已消失、选中集合未动，桌面保持原样）。
    // 确认层**就地**出现在刚画出的那个框上（位置与大小都用 globalRect），
    // 视觉上"框原地变成确认层"，确认后才落成真正的收纳盒。
    // 调用上下文安全性：endRubber() 只从事件循环里被调用（rubberTick 的 QTimer::timeout /
    // Qt 鼠标事件 / 钩子路径的 QTimer::singleShot 转发），**不在 WH_MOUSE_LL 回调栈内**，
    // 故此处跑 exec() 嵌套模态循环不会触发低级钩子的超时移除，也不会重入画框
    // （endRubber 幂等，且 m_rubbering/m_rubberRect 已复位）。
    const QString confirmBody = shellPaths.isEmpty()
        ? QStringLiteral("将在所选区域创建一个空的收纳盒。")
        : QStringLiteral("将在所选区域创建一个收纳盒，并把框内的 %1 个图标移入其中。")
              .arg(shellPaths.size());
    if (GlassMessageBox::confirm(this, QStringLiteral("创建收纳盒"), confirmBody,
                                 QStringLiteral("创建收纳盒"), QStringLiteral("取消"),
                                 QStringLiteral("new_box"), globalRect) != QDialog::Accepted) {
        DiagTrace::log(QStringLiteral("[dock] drawBox cancelled by user rect=(%1,%2,%3,%4)")
                           .arg(globalRect.x()).arg(globalRect.y())
                           .arg(globalRect.width()).arg(globalRect.height()));
        return;
    }

    // ① 名称：收纳盒N —— 取"当前未被占用（已有盒名 ∪ 网格分类名）"的最小序号，用户可随后右键改名。
    QSet<QString> used;
    for (const OrganizerBox& b : m_boxes) used.insert(b.name);
    for (auto it = m_allItems.constBegin(); it != m_allItems.constEnd(); ++it) used.insert(it.key());
    QString title;
    for (int i = 1; ; ++i) {
        const QString cand = QStringLiteral("收纳盒%1").arg(i);
        if (!used.contains(cand)) { title = cand; break; }
    }

    OrganizerBox box;
    box.id = QUuid::createUuid().toString();
    box.name = title;
    box.items.insert(title, {});

    box.window = new IconGridWindow(nullptr);
    box.window->setWindowTitle(title);
    box.window->setForceShortcutCategory(false);
    box.window->setCloseButtonVisible(false); // 与「新建收纳盒」一致：不显示标题栏关闭按钮，通过菜单解散
    box.window->setIsBoxWindow(true);         // 独立收纳盒窗口 → 右键菜单显示“解散收纳盒”
    box.window->setItems(box.items);
    box.window->setCurrentCategory(title);
    connectBoxWindow(box.window);

    // ② 几何＝画出的框：夹进屏幕可用区；尺寸不小于 IconGridWindow 的最小尺寸（480×176），
    // 否则标题栏 + 分类栏会把图标区挤没（盒子看着像坏了）。偏移时优先保住框的左上角。
    QRect screen = currentScreen()->availableGeometry();
    QSize bs(qMax(480, globalRect.width()), qMax(200, globalRect.height()));
    bs.setWidth(qMin(bs.width(), screen.width()));
    bs.setHeight(qMin(bs.height(), screen.height()));
    QPoint pos = globalRect.topLeft();
    pos.setX(qBound(screen.x(), pos.x(), screen.x() + qMax(0, screen.width() - bs.width())));
    pos.setY(qBound(screen.y(), pos.y(), screen.y() + qMax(0, screen.height() - bs.height())));
    box.window->setGeometry(pos.x(), pos.y(), bs.width(), bs.height());
    box.window->show();
#ifdef Q_OS_WIN
    // 收纳盒窗口同样登记进抗“显示桌面”守卫（与新建收纳盒路径一致）
    DesktopMirrorWindow::registerCloakTarget(box.window);
#endif

    m_boxes.append(box);

    // ③ 框内图标归入该盒的默认分类；空框照样建盒（只是没有内容）。
    if (!shellPaths.isEmpty()) {
        scheduleDockItemsIntoBox(shellPaths, displayNames, title, box.window, -1);
    } else {
        saveLayout();   // 空盒也要落盘，否则重启即丢
    }
    DiagTrace::log(QStringLiteral("[dock] drawBox->box '%1' rect=(%2,%3,%4,%5) icons=%6")
                       .arg(title).arg(pos.x()).arg(pos.y())
                       .arg(bs.width()).arg(bs.height()).arg(shellPaths.size()));
}

// 收纳盒窗口信号接线：与默认收纳盒（主整理窗口）完全一致
// 作者：谭征
void MainWindow::connectBoxWindow(IconGridWindow* win) {
    if (!win) return;
    connect(win, &IconGridWindow::closeRequested, this, [this, win]() {
        removeBoxByWindow(win);
    });
    connect(win, &IconGridWindow::menuRequested,
            this, &MainWindow::showCategoryMenu);
    connect(win, &IconGridWindow::requestNewCategory,
            this, &MainWindow::onRequestNewCategory);
    connect(win, &IconGridWindow::requestNewBox,
            this, [this]() { createNewBox(); });
    connect(win, &IconGridWindow::requestDeleteCategory,
            this, &MainWindow::deleteBoxCategory);
    connect(win, &IconGridWindow::requestRenameCategory,
            this, &MainWindow::renameBoxCategory);
    // 盒内图标被删除/重命名后，需要清理 m_allItems / 各盒 box.items / Dock 隐藏集合，
    // 否则那项仍留在盒子内存数据里，下一次 setItems 就会把已删文件重新画出来（幽灵项）。
    connect(win, &IconGridWindow::itemsDeleted,
            this, &MainWindow::onDockItemsDeleted);
    connect(win, &IconGridWindow::itemRenamed,
            this, &MainWindow::onDockItemRenamed);
    // 窗口几何（位置/大小）变化 → 防抖保存：收纳盒的几何/折叠态也要入持久化
    connect(win, &IconGridWindow::windowGeometryChanged,
            this, &MainWindow::requestSaveLayout);
    // 收纳盒内切换/重命名选中分类 → 立即防抖保存：让“当前选中分类”入本地持久化
    // （UserBoxPersist::currentCategory 在 saveLayout 里写入），重启后 createBoxFromPersist
    // 会 setCurrentCategory 切回该分类，与默认收纳盒行为一致。
    connect(win, &IconGridWindow::categoryChanged,
            this, &MainWindow::requestSaveLayout);
    // 跨窗口拖拽：收纳盒 → Dock。每个独立收纳盒窗口都必须装，否则"从盒子里把图标拖回 Dock"
    // 只有主整理窗口可用（用户最容易踩的正是小盒子窗口）。
    installBoxDropResolver(win);
}

// 关闭按钮 / 解散后从 m_boxes 移除并注销守卫（不恢复 Dock 隐藏，沿用既有行为）。
// 作者：谭征
void MainWindow::removeBoxByWindow(IconGridWindow* target) {
    if (!target) return;
    for (int i = 0; i < m_boxes.size(); ++i) {
        if (m_boxes[i].window == target) {
#ifdef Q_OS_WIN
            DesktopMirrorWindow::unregisterCloakTarget(target);  // 用户主动关闭：注销避免被强制复活
#endif
            m_boxes[i].window->hide();
            m_boxes[i].window->deleteLater();
            m_boxes.removeAt(i);
            saveLayout();   // 同步持久化：关闭的收纳盒不再写入本地
            break;
        }
    }
}

// 把给定 shellPath（小写）集合从主窗口 m_allItems 中剔除：收纳盒与主窗口互斥持有同一文件，
// 避免同一图标同时出现在主窗口与收纳盒（以及重启后主窗口把盒内文件又带回）。
// 本函数只做“按路径剔除”，【绝不】顺手删除空分类：空分类（用户显式新建的、或把图标
// 全部移入收纳盒后变空的）都必须保留。否则解散一个【非空】收纳盒时，releasePathsToDock
// 会调用本函数，那段“清空后删除空分类”逻辑就会把默认收纳盒里那个与本次操作无关的空分类
// 一并抹掉——这正是「解散非空新盒顺带删掉默认盒空分类」的回归根因。空分类的清理只发生在
// 用户显式「解散分类/解散收纳盒（默认盒）」路径，绝不在此隐式发生。
// 作者：谭征
void MainWindow::stripBoxPathsFromAllItems(const QSet<QString>& paths) {
    if (paths.isEmpty()) return;
    for (auto it = m_allItems.begin(); it != m_allItems.end(); ++it) {
        QVector<DesktopItem>& vec = it.value();
        for (int k = vec.size() - 1; k >= 0; --k) {
            const QString key = QDir::fromNativeSeparators(vec[k].sourcePath).toLower();
            if (!key.isEmpty() && paths.contains(key)) vec.removeAt(k);
        }
    }
}

// 把一组路径从主窗口 m_allItems、各收纳盒 box.items、CategoryStore 中彻底移除，
// 并让它们在 Dock（桌面镜像）重新显示：用于“解散分类 / 解散收纳盒”释放图标回桌面。
// normPaths 为归一化小写 shellPath 集合。
// 作者：谭征
void MainWindow::releasePathsToDock(const QSet<QString>& normPaths) {
    if (normPaths.isEmpty() || !m_dock) return;

    // ① 从主窗口 m_allItems 剔除（避免“解散”的图标又回到默认收纳盒显示）
    stripBoxPathsFromAllItems(normPaths);

    // ② 从各收纳盒 box.items 剔除（同一文件可能同时出现在多个盒子里）
    for (auto& box : m_boxes) {
        if (!box.window) continue;
        bool changed = false;
        QMap<QString, QVector<DesktopItem>>& bi = box.items;
        for (auto it = bi.begin(); it != bi.end(); ++it) {
            QVector<DesktopItem>& vec = it.value();
            for (int k = vec.size() - 1; k >= 0; --k) {
                const QString key = QDir::fromNativeSeparators(vec[k].sourcePath).toLower();
                if (!key.isEmpty() && normPaths.contains(key)) { vec.removeAt(k); changed = true; }
            }
        }
        if (changed) box.window->setItems(bi);
    }

    // ③ 清除分类库路径映射，使 refreshDesktop 不会把已释放的图标重新收编回收纳盒
    for (const QString& p : normPaths)
        CategoryStore::clearCategory(p);

    // ④ 主窗口刷新（去掉已释放图标），并修正当前分类
    if (m_gridWindow) {
        m_gridWindow->setItems(m_allItems);
        QString cur = m_currentCategory;
        if (cur.isEmpty() || !m_allItems.contains(cur) || IconGridWindow::isHiddenCategory(cur))
            cur = m_gridWindow->firstVisibleCategory();
        m_gridWindow->setCurrentCategory(cur);
    }

    // ⑤ Dock 隐藏集合重算：释放的图标不再在任何隐藏集合里 → 桌面镜像重新显示它们
    resetDockHiddenFromAllItems();
    // 双保险：显式再把这些路径从隐藏集剔除一次（即便上面重算因任何时序/归一化差异
    // 把它们重新算进隐藏集，这一步保证最终一定可见）。
    m_dock->removeHiddenShellPaths(normPaths);
    m_dock->saveDockSnapshot();

    saveLayout();
}

// 由持久化结构重建单个收纳盒窗口（不弹对话框）。items 为已按 dock 重建好的分类→条目。
// 作者：谭征
void MainWindow::createBoxFromPersist(const UserBoxPersist& p,
                                      const QMap<QString, QVector<DesktopItem>>& items) {
    OrganizerBox box;
    box.id = p.id.isEmpty() ? QUuid::createUuid().toString() : p.id;
    box.name = p.title;
    box.items = items;
    box.collapsed = p.collapsed;

    box.window = new IconGridWindow(nullptr);
    box.window->setWindowTitle(p.title);
    box.window->setForceShortcutCategory(false);
    box.window->setCloseButtonVisible(false); // 通过菜单解散，标题栏不显示关闭按钮
    box.window->setIsBoxWindow(true);          // 标识为独立收纳盒窗口，右键菜单显示“解散收纳盒”
    box.window->setItems(items);
    // 按本地持久化的分类顺序覆盖（用户拖拽交换过的位置），与默认收纳盒 m_categoryOrder 同口径
    if (!p.categoryOrder.isEmpty())
        box.window->setCategoryOrder(p.categoryOrder);
    if (!p.currentCategory.isEmpty() && items.contains(p.currentCategory))
        box.window->setCurrentCategory(p.currentCategory);

    // 几何（展开态）：落盘几何无效时回落到默认收纳盒正下方、左边对齐（与 createNewBox 同源）
    if (p.geometry.isValid() && p.geometry.width() > 0 && p.geometry.height() > 0) {
        box.window->setGeometry(p.geometry);
    } else {
        QRect screen = currentScreen()->availableGeometry();
        QSize bs(480, 720);
        QPoint pos;
        if (m_gridWindow && m_gridWindow->isVisible()) {
            bs = QSize(qMax(320, m_gridWindow->width() / 2), m_gridWindow->height());
            pos = QPoint(m_gridWindow->x(), m_gridWindow->y() + m_gridWindow->height() + 5);
        } else {
            pos = QPoint(screen.x() + qMax(0, (screen.width() - bs.width()) / 2),
                         screen.y() + qMax(0, (screen.height() - bs.height()) / 2));
        }
        for (int i = 0; i < m_boxes.size(); ++i)
            pos += QPoint(28, 28);
        pos.setX(qBound(screen.x(), pos.x(), screen.x() + qMax(0, screen.width() - bs.width())));
        pos.setY(qBound(screen.y(), pos.y(), screen.y() + qMax(0, screen.height() - bs.height())));
        box.window->setGeometry(pos.x(), pos.y(), bs.width(), bs.height());
    }

    connectBoxWindow(box.window);

    // 折叠态：几何已按展开态恢复，再应用折叠（save=false 避免覆盖已持久化的展开尺寸）
    box.window->setCollapsed(p.collapsed, false);

    box.window->show();
#ifdef Q_OS_WIN
    // 收纳盒窗口同样登记进抗“显示桌面”守卫，保证 Win+D / Win+M 下常驻可见。
    DesktopMirrorWindow::registerCloakTarget(box.window);
#endif
    m_boxes.append(box);
}

// 启动阶段从本地持久化数据重建用户收纳盒（仅一次）。
// 必须在 refreshDesktop 之后调用：先按 dock + 分类库建好主窗口 m_allItems，
// 再把属于收纳盒的文件从主窗口剔除并装进各自的盒子窗口。
// 作者：谭征
void MainWindow::restoreUserBoxes() {
    if (m_boxesRestored) return;
    m_boxesRestored = true;

    SettingsManager sm;
    QVector<UserBoxPersist> saved = sm.loadUserBoxes();
    if (saved.isEmpty()) return;

    // 取 dock 镜像层当前条目（与 refreshDesktop 同源），用于按 shellPath 重建盒子条目。
    QVector<DesktopItem> dockRaw;
    if (m_dock) {
        const auto entries = m_dock->entries();
        static const QString userDir = QDir::toNativeSeparators(QDir::homePath()).toLower();
        for (const auto& e : entries) {
            if (e.special || e.shellPath.startsWith(QStringLiteral("::{"))) continue;
            if (QDir::toNativeSeparators(e.shellPath).toLower().startsWith(userDir)) continue;
            DesktopItem it;
            it.displayName = e.displayName;
            it.shellPath  = e.shellPath;
            it.sourcePath = e.shellPath;
            it.isShortcut = e.shellPath.endsWith(QStringLiteral(".lnk"), Qt::CaseInsensitive);
            it.targetPath = DesktopScanner::effectiveTarget(it.sourcePath, it.isShortcut);
            it.isSpecial  = false;
            dockRaw.append(it);
        }
    }

    // 建立 shellPath(小写) → DesktopItem 的快速索引
    QHash<QString, DesktopItem> dockByPath;
    dockByPath.reserve(dockRaw.size());
    for (const DesktopItem& di : dockRaw)
        dockByPath.insert(QDir::fromNativeSeparators(di.sourcePath).toLower(), di);

    QSet<QString> allBoxPaths;   // 全部盒子的 shellPath（小写），用于从主窗口剔除
    for (const UserBoxPersist& p : saved) {
        QMap<QString, QVector<DesktopItem>> boxItems;
        for (auto cit = p.categories.constBegin(); cit != p.categories.constEnd(); ++cit) {
            QVector<DesktopItem> vec;
            for (const QString& sp : cit.value()) {
                const QString norm = QDir::fromNativeSeparators(sp).toLower();
                if (norm.isEmpty()) continue;
                auto found = dockByPath.find(norm);
                if (found != dockByPath.end()) {
                    vec.append(*found);
                    allBoxPaths.insert(norm);
                }
            }
            boxItems.insert(cit.key(), vec);   // 始终插入：分类即使无图标（空分类）也要保留键名
        }
        // 空分类（无图标）必须恢复：p.categories 只含带图标的分类，而 categoryOrder 完整记录
        // 所有分类名（含空分类），以它兜底补回空分类键，避免重启后丢失。
        for (const QString& cat : p.categoryOrder) {
            if (!cat.isEmpty() && !boxItems.contains(cat))
                boxItems.insert(cat, {});
        }
        createBoxFromPersist(p, boxItems);
    }

    // 从主窗口剔除所有盒条目，避免重复显示
    stripBoxPathsFromAllItems(allBoxPaths);
    // 同时清掉「共享分类库」里属于收纳盒的路径映射：收纳盒条目只存于各盒 box.items，
    // 绝不写入默认盒的 m_allItems / CategoryStore；否则刷新时默认盒会凭分类名“长出”同名分类，
    // 或解散收纳盒时误伤默认盒分类。这里顺手清掉旧版本遗留的脏映射，自愈历史数据。
    for (const QString& p : allBoxPaths)
        CategoryStore::clearCategory(p);

    // 自愈：全局「已解散分类」集合（IconGridWindow/disbandedCategories）本只服务于默认盒，
    // 但旧版本曾把「用户盒分类名」误记进去（disbandBoxWindow 用户盒分支调用 markCategoryDisbanded）。
    // 该集合按分类名建索引、且 applyDisbandedCategories 只作用于默认盒 m_allItems，于是用户盒解散后，
    // 默认盒里同名分类（含空分类）会被反复误删。这里把所有「用户盒分类名」从该集合剔除，
    // 防止历史脏数据继续误伤默认盒；用户盒本身不依赖此集合（数据由 box.items + saveLayout 拥有）。
    {
        QSet<QString> boxCatNames;
        for (const UserBoxPersist& p : saved) {
            for (auto kit = p.categories.constBegin(); kit != p.categories.constEnd(); ++kit)
                if (!kit.key().isEmpty()) boxCatNames.insert(kit.key());
            for (const QString& c : p.categoryOrder)
                if (!c.isEmpty()) boxCatNames.insert(c);
        }
        if (!boxCatNames.isEmpty()) {
            SettingsManager sm;
            QStringList pairs = sm.loadValue(QStringLiteral("IconGridWindow/disbandedCategories"), QString())
                                   .toString().split(QLatin1Char(';'), Qt::SkipEmptyParts);
            bool changed = false;
            for (int i = pairs.size() - 1; i >= 0; --i) {
                const int sep = pairs[i].indexOf(QLatin1Char('>'));
                const QString key = (sep > 0) ? pairs[i].left(sep) : pairs[i];
                if (!key.isEmpty() && boxCatNames.contains(key)) { pairs.removeAt(i); changed = true; }
            }
            if (changed) {
                sm.saveValue(QStringLiteral("IconGridWindow/disbandedCategories"),
                             pairs.isEmpty() ? QString() : pairs.join(QLatin1Char(';')));
                sm.sync();
            }
        }
    }

    if (m_gridWindow) {
        m_gridWindow->setItems(m_allItems);
        QString cur = m_currentCategory;
        if (cur.isEmpty() || !m_allItems.contains(cur) || IconGridWindow::isHiddenCategory(cur))
            cur = m_gridWindow->firstVisibleCategory();
        m_gridWindow->setCurrentCategory(cur);
    }
    resetDockHiddenFromAllItems();
    if (m_dock) m_dock->saveDockSnapshot();
}

// disband末尾盒子
// 作者：谭征
void MainWindow::disbandLastBox() {
    if (m_boxes.isEmpty()) {
        GlassMessageBox::information(m_gridWindow ? static_cast<QWidget*>(m_gridWindow) : this,
                                 QStringLiteral("提示"), QStringLiteral("当前没有可解散的收纳盒。"));
        return;
    }
    OrganizerBox box = m_boxes.takeLast();
    // 解散收纳盒：把该盒内所有分类的图标 shellPath 从 Dock 隐藏集合中移除，恢复 Dock 显示
    if (m_dock && !box.items.isEmpty()) {
        QSet<QString> restore;
        for (auto it = box.items.constBegin(); it != box.items.constEnd(); ++it)
            for (const auto& item : it.value())
                if (!item.sourcePath.isEmpty())
                    restore.insert(QDir::fromNativeSeparators(item.sourcePath).toLower());
        if (!restore.isEmpty()) m_dock->removeHiddenShellPaths(restore);
    }
    if (box.window) {
#ifdef Q_OS_WIN
        DesktopMirrorWindow::unregisterCloakTarget(box.window);  // 用户主动解散：注销避免被强制复活
#endif
        box.window->hide();
        box.window->deleteLater();
    }
}

// disband盒子窗口
// 作者：谭征
void MainWindow::disbandBoxWindow(IconGridWindow* target) {
    if (!target) return;
    if (target == m_gridWindow) {
        // 主整理窗口"解散收纳盒"：把全部已分类图标归位到 Dock 显示，再隐藏主窗口
        if (m_dock && !m_allItems.isEmpty()) {
            QSet<QString> restore;
            for (auto it = m_allItems.constBegin(); it != m_allItems.constEnd(); ++it)
                for (const auto& item : it.value())
                    if (!item.sourcePath.isEmpty())
                        restore.insert(QDir::fromNativeSeparators(item.sourcePath).toLower());
            if (!restore.isEmpty()) m_dock->removeHiddenShellPaths(restore);
        }
        // 同步更新本地持久化数据：清除主窗口全部分类在 CategoryStore 的本地记录，
        // 并记入“已解散”集合，使解散在重启/刷新后持续生效（否则启动阶段“本地分类为空
        // 则从 dock 读取全部图标”会把它们重新收起）；同时清空内存分类与顺序/几何脏数据。
        for (auto it = m_allItems.constBegin(); it != m_allItems.constEnd(); ++it) {
            CategoryStore::removeCategory(it.key());
            markCategoryDisbanded(it.key(), QString());
            m_gridWindow->removeCategoryFromPersistence(it.key());
        }
        m_allItems.clear();
        m_currentCategory.clear();
        if (m_dock) m_dock->saveDockSnapshot();   // 同步 Dock 快照（隐藏集合已更新为可见）
#ifdef Q_OS_WIN
        DesktopMirrorWindow::unregisterCloakTarget(m_gridWindow);  // 用户主动隐藏主面板：注销
#endif
        m_gridWindow->hide();
        return;
    }
    // 用户创建的收纳盒窗口：关闭并清理
    for (int i = 0; i < m_boxes.size(); ++i) {
        if (m_boxes[i].window == target) {
            // 解散收纳盒：盒内所有分类的图标彻底释放回桌面（Dock 重新显示），
            // 并同时从主窗口 m_allItems / 各 box.items / CategoryStore 剔除，不留残留
            // （运行时盒图标也会写入共享的 m_allItems，只清 box.items 会让它们仍留在
            // 主窗口且被 resetDockHiddenFromAllItems 重新隐藏，故必须走 releasePathsToDock）。
            QSet<QString> normPaths;
            for (auto it = m_boxes[i].items.constBegin(); it != m_boxes[i].items.constEnd(); ++it)
                for (const auto& item : it.value())
                    if (!item.sourcePath.isEmpty())
                        normPaths.insert(QDir::fromNativeSeparators(item.sourcePath).toLower());
            releasePathsToDock(normPaths);   // 已 saveLayout 并把图标在 Dock 重新显示
            // 用户盒分类绝不写入全局「已解散分类」集合（IconGridWindow/disbandedCategories）：
            // 该集合按「分类名」建索引，且 applyDisbandedCategories 只作用于默认盒 m_allItems。
            // 若把用户盒分类名记进去，一旦与默认盒某分类（含空分类）同名，下次刷新会把默认盒
            // 那个同名分类一并删掉——这正是「解散新建盒顺带删掉默认盒空分类」的根因。
            // 用户盒数据完全由本盒 box.items + saveLayout(UserBoxPersist) 拥有，解散即 m_boxes.removeAt
            // + saveLayout，不触碰默认盒的全局解散/分类持久化。removeCategoryFromPersistence 对盒窗口
            // 是空操作（m_isBoxWindow 守卫），这里一并省去。
            if (m_dock) m_dock->saveDockSnapshot();   // 同步 Dock 快照（隐藏集合已更新为可见）
#ifdef Q_OS_WIN
            DesktopMirrorWindow::unregisterCloakTarget(m_boxes[i].window);  // 用户主动解散：注销
#endif
            m_boxes[i].window->hide();
            m_boxes[i].window->deleteLater();
            m_boxes.removeAt(i);
            saveLayout();   // 同步持久化：解散的收纳盒不再写入本地
            break;
        }
    }
}

// 删除盒子分类
// 作者：谭征
void MainWindow::deleteBoxCategory(const QString& name) {
    if (name.isEmpty()) return;
    auto* win = qobject_cast<IconGridWindow*>(sender());
    if (!win) return;
    for (int i = 0; i < m_boxes.size(); ++i) {
        if (m_boxes[i].window != win) continue;
        // 收纳盒解散分类：该分类图标彻底释放回桌面（Dock 重新显示），不再并入任何其他分类；
        // 收纳盒窗口本身不关闭（关闭由“解散收纳盒”负责）。
        QVector<DesktopItem> items = m_boxes[i].items.take(name);
        QSet<QString> normPaths;
        for (const auto& it : items)
            if (!it.sourcePath.isEmpty())
                normPaths.insert(QDir::fromNativeSeparators(it.sourcePath).toLower());
        // releasePathsToDock：从 m_allItems / 各 box.items / CategoryStore 移除并重算 Dock 隐藏集合，
        // 使图标在桌面镜像重新出现；它已 saveLayout，但本盒剩余分类尚未持久化，下面再存一次。
        releasePathsToDock(normPaths);
        win->removeCategoryFromPersistence(name);
        // 当前分类：还有剩余分类则保持/落到首个，否则置空
        QString currentCat = m_boxes[i].items.isEmpty() ? QString()
                                                        : (m_boxes[i].items.contains(win->currentCategory())
                                                               ? win->currentCategory()
                                                               : m_boxes[i].items.firstKey());
        if (IconGridWindow::isHiddenCategory(currentCat)) {
            currentCat = m_boxes[i].items.isEmpty() ? QString() : m_boxes[i].items.firstKey();
        }
        win->setItems(m_boxes[i].items);
        win->setCurrentCategory(currentCat);
        saveLayout();
        return;
    }
}

// 重命名盒子分类
// 作者：谭征
void MainWindow::renameBoxCategory(const QString& oldName) {
    auto* win = qobject_cast<IconGridWindow*>(sender());
    if (!win) return;
    for (int i = 0; i < m_boxes.size(); ++i) {
        if (m_boxes[i].window == win) {
            bool ok = false;
            QString newName = GlassInputDialog::getText(win, QStringLiteral("重命名分类"),
                                                    QStringLiteral("新名称:"), QLineEdit::Normal,
                                                    oldName, &ok);
            if (!ok || newName.isEmpty() || newName == oldName) return;
            if (IconGridWindow::isHiddenCategory(newName)) {
                GlassMessageBox::warning(win, QStringLiteral("保留名称"),
                                     QStringLiteral("'%1' 为系统保留分类名称，不可使用。").arg(newName));
                return;
            }
            if (m_boxes[i].items.contains(newName)) {
                GlassMessageBox::warning(win, QStringLiteral("重复"),
                                     QStringLiteral("分类 '%1' 已存在。").arg(newName));
                return;
            }
            QVector<DesktopItem> items = m_boxes[i].items.take(oldName);
            m_boxes[i].items.insert(newName, items);
            m_boxes[i].name = newName;
            m_boxes[i].window->setWindowTitle(newName);
            m_boxes[i].window->setItems(m_boxes[i].items);
            m_boxes[i].window->setCurrentCategory(newName);
            break;
        }
    }
}

// 删除分类
// 作者：谭征
void MainWindow::deleteCategory(const QString& name) {
    if (name.isEmpty()) return;
    if (!m_allItems.contains(name)) return;

    if (name == kSystemCategory) {
        GlassMessageBox::warning(this, QStringLiteral("提示"),
                            QStringLiteral("“系统”分类（我的电脑/回收站/网络）为常驻项，不可解散。"));
        return;
    }

    // A 方案：删除分类只是从程序视图中移除，并清空该分类下文件的分类记录（CategoryStore）。
    // 文件仍在原桌面位置、不丢失，不搬运，也不并入"未分类"视图。
    // "其它"为兜底分类，现在允许解散：解散后不在规则内的文件将作为桌面散件（不进任何收纳盒），
    // 并把"其它"记入已解散集合，刷新/重启后不会被自动重建。
    // 解散前先收集该分类下所有图标的 shellPath，用于恢复 Dock 显示
    QVector<DesktopItem> removedItems = m_allItems.take(name);
    if (m_dock && !removedItems.isEmpty()) {
        QSet<QString> restore;
        for (const auto& it : removedItems)
            if (!it.sourcePath.isEmpty())
                restore.insert(QDir::fromNativeSeparators(it.sourcePath).toLower());
        m_dock->removeHiddenShellPaths(restore);
    }
    CategoryStore::removeCategory(name);
    // 把该分类持久记录到“已解散”集合，使标签页右键“解散”在重启/刷新后持续生效。
    // 此前只有“其它”会记录，导致其它默认规则分类解散后重启又出现。
    markCategoryDisbanded(name, QString());

    // “快捷方式”被解散后不再强制补回
    const bool disbandShortcut = (name == QStringLiteral("快捷方式"));
    if (disbandShortcut) m_gridWindow->setForceShortcutCategory(false);

    m_gridWindow->removeCategoryFromPersistence(name);
    if (m_currentCategory == name || !m_allItems.contains(m_currentCategory)
        || IconGridWindow::isHiddenCategory(m_currentCategory)) {
        m_currentCategory = m_gridWindow->firstVisibleCategory();
    }
    m_gridWindow->setItems(m_allItems);
    m_gridWindow->setCurrentCategory(m_currentCategory);
    saveLayout();
    SettingsManager sm;
    sm.saveValue(QStringLiteral("mainForceShortcutCategory"), m_gridWindow->forceShortcutCategory());
}

// 保存布局
// 作者：谭征
void MainWindow::saveLayout() {
    SettingsManager sm;
    QMap<QString, BoxState> boxes;
    for (auto it = m_allItems.cbegin(); it != m_allItems.cend(); ++it) {
        BoxState state;
        state.title = it.key();
        // 落盘用**基色**（panelBgColor，alpha 恒 255），不要用 Theme::panelBg()：
        // 后者 2026-09-21 起带"界面背景透明度"，写进配置会把当时的透明度烘死，
        // 以后改透明度也改不动这一项。
        state.color = Theme::panelBgColor();
        for (const auto& item : it.value()) {
            state.itemIds.append(item.id.toString());
        }
        boxes.insert(it.key(), state);
    }
    sm.saveBoxes(boxes);
    // 仅当窗口已显示在屏幕上（可见）时才保存位置与大小。
    // 不这样做的话，构造阶段 refreshDesktop() 触发的 saveLayout() 会在窗口尚未 show、
    // 仍停在默认 (0,0) 位置时把用户上次保存的真实位置覆盖掉，导致“位置无法保存”。
    if (m_gridWindow && m_gridWindow->isVisible()) {
        // 全屏收纳模式下窗口铺满工作区，此时几何为全屏；若直接落盘会覆盖用户原有的浮动布局。
        // 改为保存进入全屏前的浮动几何（preFencesGeometry），下次启动先还原浮动布局再进入全屏。
        QRect g = m_gridWindow->isFencesMode()
                      ? m_gridWindow->preFencesGeometry()
                      : m_gridWindow->geometry();
        if (g.isValid()) {
            // gridW 折叠/展开不变；gridH 始终保存展开态高度，避免折叠时把紧凑高度写入
            const int expandedH = m_gridWindow->isCollapsed() ? m_gridWindow->expandedSize().height() : g.height();
            sm.saveValue(QStringLiteral("MainWindow/gridX"), g.x());
            sm.saveValue(QStringLiteral("MainWindow/gridY"), g.y());
            sm.saveValue(QStringLiteral("MainWindow/gridW"), g.width());
            sm.saveValue(QStringLiteral("MainWindow/gridH"), expandedH);
        }
    }
    if (m_assistantWindow && m_assistantWindow->isVisible() && m_assistantWindow->geometry().isValid()) {
        const QRect a = m_assistantWindow->geometry();
        const bool assistantCollapsed = m_assistantWindow->panel() && m_assistantWindow->panel()->isCollapsed();
        const int expandedH = assistantCollapsed ? m_assistantWindow->expandedSize().height() : a.height();
        sm.saveValue(QStringLiteral("MainWindow/assistantX"), a.x());
        sm.saveValue(QStringLiteral("MainWindow/assistantY"), a.y());
        sm.saveValue(QStringLiteral("MainWindow/assistantW"), a.width());
        sm.saveValue(QStringLiteral("MainWindow/assistantH"), expandedH);
    }

    // 保留展开尺寸（折叠/展开切换时用于恢复）
    if (m_assistantWindow) {
        sm.saveValue(QStringLiteral("AssistantWindow/expandedSize"), m_assistantWindow->expandedSize());
    }
    if (m_gridWindow) {
        sm.saveValue(QStringLiteral("IconGridWindow/expandedSize"), m_gridWindow->expandedSize());
    }
    sm.saveValue(QStringLiteral("IconGridWindow/currentCategory"), m_currentCategory);
    sm.saveValue(QStringLiteral("currentCategory"), m_currentCategory);

    // 持久化用户创建的收纳盒：每个盒子的几何、折叠态，以及各分类下的 shellPath 列表。
    // 分类与条目从盒子窗口权威数据（items()）读取，避免内存 items 漂移导致落盘遗漏。
    // 必须在 restoreUserBoxes() 之后才允许写入：启动期 refreshDesktop() 会触发本函数，
    // 而彼时 m_boxes 尚未从磁盘重建（为空），直接用空向量 saveUserBoxes 会把整组 UserBoxes 清空，
    // 紧随其后的 restoreUserBoxes() 读到空 → 已存收纳盒还没读取就被抹掉、永远不恢复。
    if (m_boxesRestored) {
        QVector<UserBoxPersist> persist;
        persist.reserve(m_boxes.size());
        for (const OrganizerBox& box : m_boxes) {
            if (!box.window) continue;
            UserBoxPersist p;
            p.id = box.id;
            p.title = box.window->windowTitle();
            // 折叠态下 geometry() 是折叠后的小矩形；存展开态几何，重载时先还原展开几何再折叠，
            // 避免“折叠两次”把位置算错。
            {
                QRect g = box.window->geometry();
                if (box.window->isCollapsed()) {
                    const QSize exp = box.window->expandedSize();
                    g = QRect(g.x(), g.y(), exp.width(), exp.height());
                }
                p.geometry = g;
            }
            p.collapsed = box.window->isCollapsed();
            p.currentCategory = box.window->currentCategory();
            p.categoryOrder = box.window->categoryOrder();   // 收纳盒分类显示顺序（拖拽交换后的位置）
            const QMap<QString, QVector<DesktopItem>> boxItems = box.window->items();
            for (auto it = boxItems.constBegin(); it != boxItems.constEnd(); ++it) {
                QStringList paths;
                paths.reserve(it.value().size());
                for (const DesktopItem& di : it.value())
                    paths.append(QDir::fromNativeSeparators(di.sourcePath).toLower());
                paths.removeAll(QString());
                // 关键：空分类（paths 为空）也要持久化，否则重启后该空分类丢失；
                // 恢复时由 categoryOrder 兜底重建（setItems/syncCategoryOrder 均支持空分类键）。
                p.categories.insert(it.key(), paths);
            }
            persist.append(p);
        }
        sm.saveUserBoxes(persist);
    }

    sm.sync();
}

// 加载布局
// 作者：谭征
void MainWindow::loadLayout() {
    SettingsManager sm;

    // 恢复桌面整理窗口几何（位置 + 大小）。
    // 采用显式坐标（而非 saveGeometry/restoreGeometry），避免无边框/半透明窗口
    // 在跨会话时 frame 处理不一致导致的位置偏差或失效。
    // 提前读取折叠状态：用于判断“实际可见矩形”的高度（折叠态只有很小高度）
    bool gridCollapsed = sm.loadValue(QStringLiteral("IconGridWindow/gridCollapsed"), false).toBool();

    QVariant gxv = sm.loadValue(QStringLiteral("MainWindow/gridX"), QVariant());
    QVariant gyv = sm.loadValue(QStringLiteral("MainWindow/gridY"), QVariant());
    QVariant gwv = sm.loadValue(QStringLiteral("MainWindow/gridW"), QVariant());
    QVariant ghv = sm.loadValue(QStringLiteral("MainWindow/gridH"), QVariant());
    if (m_gridWindow && !gxv.isNull() && !gyv.isNull() && !gwv.isNull() && !ghv.isNull()
        && gwv.toInt() > 0 && ghv.toInt() > 0) {
        QRect screen = allScreensAvailableGeometry();
        QRect rect(gxv.toInt(), gyv.toInt(), gwv.toInt(), ghv.toInt());
        // 顶部留白（2026-09-24）：收纳盒窗口不许贴住工作区顶边，至少留出一根折叠条的高度；
        // 老配置若落在贴顶位置，这里一次性抬下来（下次保存即为新位置）。
        const int minTop = WindowSnap::topLimitGlobal(rect);
        if (rect.top() < minTop) rect.moveTop(minTop);
        // 用“实际会落点”的矩形判断是否可见：折叠态高度仅 collapsedHeight，
        // 否则会出现“展开态相交、折叠后整窗跑到屏幕外（如 -663,-359）而不可见”的误判。
        const int effH = gridCollapsed ? Theme::collapsedHeight() : ghv.toInt();
        QRect effRect(rect.topLeft(), QSize(gwv.toInt(), effH));
        if (effRect.intersects(screen)) {
            m_gridWindow->setGeometry(rect);
            m_hasGridGeometry = true;
            // gridW/gridH 现在始终保存展开态尺寸；以此作为权威展开尺寸。
            // 若高度异常（旧版本曾把折叠紧凑高度写入），则以 IconGridWindow/expandedSize 兜底。
            QSize restoredSize = rect.size();
            if (restoredSize.height() < 176) {
                QSize fallback = sm.loadValue(QStringLiteral("IconGridWindow/expandedSize"), QSize(960, 720)).toSize();
                restoredSize.setHeight(qMax(fallback.height(), 176));
            }
            m_gridWindow->setExpandedSize(restoredSize);
        }
    }
    // 兜底：几何未恢复时，用落盘展开尺寸补全，保证折叠/展开始终有可用回退尺寸。
    if (m_gridWindow && !m_hasGridGeometry) {
        QSize gridExpanded = sm.loadValue(QStringLiteral("IconGridWindow/expandedSize"), QSize(960, 720)).toSize();
        m_gridWindow->setExpandedSize(gridExpanded);
    }

    // 在几何与展开尺寸恢复后，再应用持久化的折叠状态。
    // 若落盘几何高度异常（旧版本把折叠紧凑高度写入），先恢复到正确的展开尺寸再折叠，
    // 避免把错误高度记录为展开尺寸。
    gridCollapsed = sm.loadValue(QStringLiteral("IconGridWindow/gridCollapsed"), false).toBool();
    if (m_gridWindow) {
        if (gridCollapsed) {
            QSize expandedSize = m_gridWindow->expandedSize();
            m_gridWindow->resize(expandedSize.width(), qMax(expandedSize.height(), 176));
        }
        m_gridWindow->setCollapsed(gridCollapsed, false);
    }

    // 恢复桌面助手窗口几何（位置 + 大小）
    bool assistantCollapsed = sm.loadValue(QStringLiteral("AssistantWindow/collapsed"), false).toBool();
    QVariant axv = sm.loadValue(QStringLiteral("MainWindow/assistantX"), QVariant());
    QVariant ayv = sm.loadValue(QStringLiteral("MainWindow/assistantY"), QVariant());
    QVariant awv = sm.loadValue(QStringLiteral("MainWindow/assistantW"), QVariant());
    QVariant ahv = sm.loadValue(QStringLiteral("MainWindow/assistantH"), QVariant());
    if (m_assistantWindow && !axv.isNull() && !ayv.isNull() && !awv.isNull() && !ahv.isNull()
        && awv.toInt() > 0 && ahv.toInt() > 0) {
        QRect screen = allScreensAvailableGeometry();
        QRect rect(axv.toInt(), ayv.toInt(), awv.toInt(), ahv.toInt());
        // 顶部留白（2026-09-24）：同收纳盒窗口，桌面助手也不许贴住工作区顶边。
        const int minTop = WindowSnap::topLimitGlobal(rect);
        if (rect.top() < minTop) rect.moveTop(minTop);
        // 同收纳盒：用“实际会落点”的矩形（折叠态仅 collapsedHeight）判断可见性，
        // 避免折叠后整窗跑到屏幕外的误判。
        const int effH = assistantCollapsed ? Theme::collapsedHeight() : ahv.toInt();
        QRect effRect(rect.topLeft(), QSize(awv.toInt(), effH));
        if (effRect.intersects(screen)) {
            m_assistantWindow->setGeometry(rect);
            m_hasAssistantGeometry = true;
            // assistantW/assistantH 现在始终保存展开态尺寸；以此为权威展开尺寸。
            QSize restoredSize = rect.size();
            if (restoredSize.height() < 400) {
                QSize fallback = sm.loadValue(QStringLiteral("AssistantWindow/expandedSize"), QSize(320, 720)).toSize();
                restoredSize.setHeight(qMax(fallback.height(), 400));
            }
            m_assistantWindow->setExpandedSize(restoredSize);
        }
    }
    // 兜底：几何未恢复时，用落盘展开尺寸补全
    if (m_assistantWindow && !m_hasAssistantGeometry) {
        QSize assistantExpanded = sm.loadValue(QStringLiteral("AssistantWindow/expandedSize"), QSize(320, 720)).toSize();
        m_assistantWindow->setExpandedSize(assistantExpanded);
    }

    // 在几何与展开尺寸恢复后，再应用持久化的折叠状态。
    // 若落盘几何高度异常，先恢复到正确的展开尺寸再折叠，避免把错误高度记录为展开尺寸。
    assistantCollapsed = sm.loadValue(QStringLiteral("AssistantWindow/collapsed"), false).toBool();
    if (m_assistantWindow) {
        if (assistantCollapsed) {
            QSize expandedSize = m_assistantWindow->expandedSize();
            m_assistantWindow->resize(expandedSize.width(), qMax(expandedSize.height(), 400));
        }
        m_assistantWindow->setPanelCollapsed(assistantCollapsed, false);
    }

    // 优先从新键 IconGridWindow/currentCategory 恢复（与保存逻辑一致、权威），
    // 旧键 currentCategory 仅作为兼容兜底（旧版本遗留数据）。
    QString savedCur = sm.loadValue(QStringLiteral("IconGridWindow/currentCategory"), QString()).toString();
    if (savedCur.isEmpty()) {
        savedCur = sm.loadValue(QStringLiteral("currentCategory"), QStringLiteral("快捷方式")).toString();
    }
    m_currentCategory = savedCur;

    // 将持久化的当前选中分类应用到网格窗口（若仍存在于当前分类集合中），
    // 保证重启后停留在用户上次选中的分类。
    // 系统/未分类为隐藏分类，不应再作为当前选中项。
    if (m_gridWindow && !m_currentCategory.isEmpty() && m_allItems.contains(m_currentCategory)
        && !IconGridWindow::isHiddenCategory(m_currentCategory)) {
        m_gridWindow->setCurrentCategory(m_currentCategory);
    } else if (m_gridWindow && (m_currentCategory.isEmpty() || IconGridWindow::isHiddenCategory(m_currentCategory))) {
        m_currentCategory = m_gridWindow->firstVisibleCategory();
    }
}

// request保存布局
// 作者：谭征
void MainWindow::requestSaveLayout() {
    if (!m_geometrySaveTimer) return;
    // 防抖：500ms 内多次变化只保存一次，避免拖拽/缩放过程中频繁写盘
    m_geometrySaveTimer->start(500);
}

// 关闭事件
// 作者：谭征
void MainWindow::closeEvent(QCloseEvent* event) {
    // C：退出清理是重活（saveLayout 落盘 + 还原原生桌面触发整屏重绘），而此刻
    // Dock / 网格的低级钩子可能仍在位 —— 低级钩子是系统同步回调到本 GUI 线程的，期间全系统
    // 鼠标都会等我们返回 → 这正是「关闭瞬间鼠标卡死」的来源。先摘钩、干完再装回。
    DesktopMirrorWindow::suspendHotkeyHooks();
    IconGridWindow::suspendGridHooks();
    saveLayout();
#ifdef Q_OS_WIN
    restoreNativeDesktop();   // 关闭窗口也还原原生桌面，避免图标残留隐藏
#endif
    IconGridWindow::resumeGridHooks();
    DesktopMirrorWindow::resumeHotkeyHooks();
    event->accept();
}

// 「显示主界面/隐藏主界面」（设置中心常规页）：勾选=显示助手面板，取消=隐藏。
// 隐藏前必须 unregisterCloakTarget：助手窗口在抗“显示桌面”守卫名单里，
// 直接 hide() 会被守卫的事件钩子/轮询立即强制复活。显示时重新注册回守卫。
// 作者：谭征
void MainWindow::applyAssistantVisibility(bool show) {
    if (!m_assistantWindow) return;
    if (show) {
        DesktopMirrorWindow::registerCloakTarget(m_assistantWindow);
        m_assistantWindow->show();
        m_assistantWindow->raise();
    } else {
        DesktopMirrorWindow::unregisterCloakTarget(m_assistantWindow);
        m_assistantWindow->hide();
    }
    SettingsManager sm;
    sm.saveValue(QStringLiteral("General/showMainWindow"), show);
    sm.sync();
}
