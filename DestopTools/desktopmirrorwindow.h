/*
 * @file desktopmirrorwindow.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef DESKTOPMIRRORWINDOW_H
#define DESKTOPMIRRORWINDOW_H

#include <QWidget>
#include <QVector>
#include <QHash>
#include <QPointer>
#include <QList>
#include <QSet>
#include <QPoint>
#include <QPixmap>      // 拖拽浮层：缓存拖动组的图标位图
#include <QString>
#include <QStringList>
#include <functional>   // 跨窗口拖拽落点解析器（Dock → 收纳盒）
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include "desktopitem.h"

class DockIconButton;
class QTimer;
class QResizeEvent;
class QShowEvent;
class QEvent;
class QMouseEvent;
class QPaintEvent;

// 全屏 Dock 层：铺满主屏、始终位于最底层，作为 Windows 桌面的存在。
// 数据源 = NativeListView：启动前（原生桌面仍可见时）跨进程快照 SysListView32，
// 1:1 镜像 Windows 桌面的图标集合与屏幕坐标，确保 Dock 内图标的大小/位置/清晰度与桌面完全一致。
// 交互：双击打开、右键原生菜单（IContextMenu）、左键拖动（回写真实桌面位置）、悬停高亮、Ctrl/Shift 多选、框选。
class DesktopMirrorWindow : public QWidget {
    Q_OBJECT
public:
    enum class SourceMode {
        NativeListView,   // 兼容旧模式：1:1 镜像 Windows 桌面 ListView
        DesktopOrganize   // 桌面整理：从 DesktopScanner + CategoryStore 读取
    };

    // 构造函数：初始化对象
    explicit DesktopMirrorWindow(QWidget* parent = nullptr);
    ~DesktopMirrorWindow();

    // Dock 图标条目（供外部读取 dock 当前图标列表，如「桌面整理」按钮按 dock 内图标分类）
    struct Entry {
        QString displayName;
        QString shellPath;      // 绝对路径 或 "::{CLSID}"
        int imageIndex = -1;   // 系统镜像列表索引（DesktopOrganize 模式下通常为 -1）
        QPoint screenPos;       // 物理屏幕坐标（仅 NativeListView 模式使用原始位置）
        bool special = false;
        bool hasPos = false;    // screenPos 是否来自真实 ListView 有效坐标
        int lvIndex = -1;       // 真实桌面 SysListView32 索引（仅 NativeListView 模式）
        int iconSizePx = 0;     // 该图标在 Windows 桌面上的真实像素尺寸（逐项读取，与桌面逐个一致）
    };

    void startMirror();                 // 启动：铺满主屏、隐藏原生桌面、注册同步、显示
    void stopMirror();                  // 停止：注销同步、还原原生桌面
    void refresh();                     // 重新读取桌面并重建图标布局
    // 清除选中
    void clearSelection();
    // —— 三者统一状态控制（无任何特殊处理）——
    // 三窗口（Dock / 收纳盒 / 桌面助手）一律走同一组 API、同一时机：
    // · 常态：parkAllTargetsAtBottom() 一并 HWND_BOTTOM 压底（不抢焦点）
    // · show desktop 激活期：topmostAllTargets() 一并 HWND_TOPMOST 置顶（免疫 SW_HIDE）
    // 不再为 Dock 单独提供"压底"方法——三者零差异化控制。
    static void parkAllTargetsAtBottom();          // 三窗口一律 HWND_BOTTOM 压底（常态）
    // 一次性把 Dock 自身 EX/STYLE 调整为"抗显示桌面"（WS_EX_NOACTIVATE + 去 WS_MINIMIZEBOX/...），
    // 仅 Dock 独有（收纳盒/助手有自己的 EX 设置路径），与"压底"无关。
    void ensureDockWindowStyle();

    // C：重活期间临时摘掉低级钩子
    // 低级钩子（WH_KEYBOARD_LL / WH_MOUSE_LL）是【系统同步回调到安装线程（本 GUI 线程）】的：
    // 鼠标/键盘输入必须等我们的 hookProc 返回才继续派发。于是「GUI 线程被重活钉住」会直接
    // 表现为「全系统鼠标卡的不动」（超 LowLevelHooksTimeout 还会被静默摘钩）。
    // 任何已知会长时间占用 GUI 线程的操作（主题下发 / 全量重扫 / 退出清理）前后调用这一对
    // 函数，使「操作慢」不再等于「鼠标卡死」。嵌套安全（内部计数）；未处于暂停中的 resume
    // 是空操作，绝不会把 stopMirror 已释放的钩子又装回去。
    static void suspendHotkeyHooks();
    // 恢复hotkeyhooks
    static void resumeHotkeyHooks();

#ifdef Q_OS_WIN
    // 抗“显示桌面”(Win+D) / 任务栏“显示桌面”按钮 / “最小化全部”(Win+M)：
    // 注册需保持常驻的窗口（Dock / 收纳盒 / 桌面助手）。
    // · 守卫 = T5 事件钩子（CLOAKED/HIDE/MINIMIZESTART）即时恢复 + 100ms 轮询兜底。
    // · 不再用 QWidget::isHidden() 作“程序是否主动隐藏”的意图判定——
    // 外壳用 SW_HIDE 隐藏窗口时 Qt 会把 isHidden() 也翻成 true（Qt 不分发起方），
    // 会导致“显示桌面按钮”把窗口藏起后本应复活的窗口被误跳过、永远不恢复。
    // · 改用受控的注册/注销：本程序显式 hide()（用户解散收纳盒/主面板）时调 unregisterCloakTarget，
    // 只在窗口处于“应可见”状态时才复活；常隐的 MainWindow 协调窗口从不注册。
    static void registerCloakTarget(QWidget* w);
    static void unregisterCloakTarget(QWidget* w);  // 用户主动隐藏某窗口时注销，避免被强制复活
    // 供自由函数 guardEventProc（__stdcall）调用的唯一公开入口：
    // 若 hwnd 属于受保护窗口则立即执行恢复（内部有防重入）。
    static void handleShellHideEvent(HWND hwnd);   // 窗口被外壳 SW_HIDE（显示桌面按钮路径）→ 进入 topmost 常驻
    static void handleShellShowEvent(HWND hwnd);   // 窗口被外壳重新 SW_SHOW（退出显示桌面）→ 撤 topmost
    // B8：hwnd 是否为本程序登记的受保护/挂件窗口（O(1) 查表）。
    // guardEventProc 是**全局范围**事件钩子（系统里任何窗口的显示/隐藏/最小化都会回调），
    // 用它做第一道闸门 —— 非本程序窗口立即返回，省掉后续全部字符串/Qt 对象构造。
    static bool isProtectedWindow(HWND hwnd);
    // 公开调试/强制恢复桥（供自由函数与现有前台钩子调用，避免直接访问 private 成员）。
    static void debugLog(const QString& s);
    static void forceRestore();   // 直接触发 restoreAllTargets()（内部有防重入）
    // —— 三窗口 nativeEvent 共用的 Z 序守卫（修复「点击 Dock 一闪而过」的根因）——
    // WM_WINDOWPOSCHANGING 是同步发送消息、不经过消息队列，全局 QAbstractNativeEventFilter
    // 根本看不到它——守卫必须写在每个窗口自己的 nativeEvent 里（这是此前多轮修复无效的原因）。
    // 三个窗口（Dock/收纳盒/助手）的 nativeEvent 均调用本函数，在 z 序变更【应用之前】拦截提层。
    static bool isShowDesktopActive();               // show desktop 状态机是否持有 z 序控制权
    static bool clampBandZOrder(HWND hwnd, WINDOWPOS* wp);   // 常态拦截提层；返回 true=已改写
    // Rainmeter 式 topmost 状态机：进入 show desktop 时三窗口 HWND_TOPMOST 保持可见，
    // 退出时 HWND_NOTOPMOST 撤顶 + Dock 压底。公开 static 便于自由函数 guardEventProc 调用。
    static void topmostAllTargets();      // 三窗口置顶（topmost 天然免疫 show desktop 的 SW_HIDE）
    static void untopmostAllTargets();    // 三窗口撤顶恢复常态 Z 序（Dock 额外压回 HWND_BOTTOM）
    static bool isForegroundRealApp();    // 前台是否为"真实程序窗口"（非桌面、非自身）→ 判定退出 show desktop
    // 将 h 置于所有非 Dock 的 band 窗口（收纳盒/桌面助手）之上、但仍低于其它正常程序。
    // 供设置中心等弹框使用：先在 band 集合里求出最上层的那一个作唯一参照，再相对插入一次
    // （不解引用 HWND_TOP/TOPMOST），故 h 落在全部 band 窗口之上、且不遮挡普通程序。
    static void raiseAboveBandWindows(HWND h);

    // 长期存活窗口（快速搜索窗）的「层级看护」：仅当 h 确实被某个 band 窗口压住时才修正，
    // 已经在 band 之上则不做任何事。返回 true = 本次执行了修正。
    static bool ensureLayerAboveBandWindows(HWND h);
#endif

    // 设置源模式
    void setSourceMode(SourceMode mode);
    SourceMode sourceMode() const { return m_sourceMode; }

    // 获取 dock 当前所有图标条目（供「桌面整理」按钮按 dock 内图标分类，而非扫描 Windows 桌面文件系统）
    QVector<Entry> entries() const { return m_entries; }

    // 收纳盒已加载完成后，将其包含的全部图标从 Dock 显示中移除（隐藏，不删真实文件）。
    // 传入的 shellPath 集合（绝对路径，大小写不敏感）对应的 Entry 不再生成 Dock 按钮。
    void setHiddenShellPaths(const QSet<QString>& paths);
    // 从隐藏集合中移除指定路径（解散分类时调用），使对应图标重新在 Dock 上显示。
    void removeHiddenShellPaths(const QSet<QString>& paths);
    QSet<QString> hiddenShellPaths() const { return m_hiddenShellPaths; }

    // 快捷操作：设置中心「快捷操作」页的两个复选（2026-09-24 接通）
    // 两个开关的消费点全在**鼠标钩子回调之后的那一拍**（Dock 是 WS_EX_NOACTIVATE 的底层窗口，
    // 空白处鼠标事件未必回到 Qt，必须走 WH_MOUSE_LL 通道，历史同因）。钩子路径禁做文件 I/O
    // （读设置 = 读 INI），故取值缓存成普通成员：构造期读一次 + ThemeManager::settingChanged 热更新。
    // ① QuickActions/hideIconsOnDoubleClick —— 双击桌面空白处 → 隐藏/显示桌面图标（Dock 镜像层）
    // ② QuickActions/drawBoxOnBlank         —— 桌面空白处拉框 → 松手按框新建收纳盒并收起框内图标
    // 热更新入口（供构造成员之外的显式同步用；settingChanged 已在构造函数里接好）。
    void setQuickActionFlags(bool hideIconsOnDoubleClick, bool drawBoxOnBlank);

    // 双击桌面空白：隐藏 / 显示桌面（本镜像层）的全部图标。隐藏态在内存里，重启即恢复显示。
    void toggleDesktopIconsVisible();
    bool desktopIconsHidden() const { return m_desktopIconsHidden; }

    // —— 跨窗口拖拽：收纳盒图标 → Dock（落点即显示位置）——
    // 把指定 shellPath 图标的“坐标真值”设成：单元格中心落在 globalDropPos（逻辑屏幕坐标）处。
    // 用途 = 收纳盒 → Dock 拖放的“松手点就是图标显示点”：松手后该图标必须原地出现，
    // 而不是跳回它被收进收纳盒之前的旧桌面位置。
    // 只改坐标（三项，全是内存/位置数据，绝不碰任何文件）：
    // ① m_entries[i].screenPos / hasPos —— layoutButtonsNative() 就是按它摆放按钮的；
    // ② m_snapshotEntries —— 原生桌面被 SW_HIDE 后 LVM_GETITEMPOSITION 读数会塌缩到 (0,0)，
    // 快照是后续 refresh() 的唯一坐标真相源，不同步就会被下一次刷新打回旧位置；
    // ③ 真实桌面 SysListView32 的图标位置（LVM_SETITEMPOSITION）——与既有
    // 「拖动 Dock 图标 = 移动真实桌面图标」完全同一语义，保证 Dock 与真实桌面不脱节。
    // 刻意**不**触发重排：调用方（MainWindow）紧接着就会用 setHiddenShellPaths() 让该项
    // 从隐藏集合里出来，那一次本身就会重排；这里若也重排会在同一拍里重建两代按钮
    // （旧的 deleteLater 尚未回收）造成瞬间叠影。
    void placeDockIconAtCursor(const QString& shellPath, const QPoint& globalDropPos);

    // 盒→Dock 拖放落点后由 MainWindow 调用：让刚落下的图标立即处于选中态，并重新武装
    // 快捷键作用域（拖动期间“按下”发生在收纳盒 → s_hkLastClickOnDock 被清成 false；
    // 且 layoutButtons 的选中接力只找回“旧选中”，新落下的图标不在集合里）。
    // 若不处理，用户松手后直接按 F2/Delete 会因 “empty selection / last click not on dock” 失效。
    void selectIconByShellPathAndArm(const QString& shellPath);

    // —— 跨窗口拖拽：Dock 图标 → 收纳盒（落点解析器）——
    // 为什么用回调而不是信号槽：onIconDragEnd() 必须在**同一次调用里同步**知道
    // “这一松手是否已被收纳盒接收”，才能决定要不要继续走原有的两条落盘路径
    // （拖到回收站 = 删除 / 回写真实桌面图标位置）。信号槽默认直连虽也同步，
    // 但槽的 void 返回值无法把决策回传给调用方，故这里用带返回值的解析器。
    // 参数：被拖动的 shellPath 列表、对应显示名列表、松手时的**逻辑屏幕坐标**。
    // 返回 true = 已由收纳盒接管（Dock 不再做任何落盘动作）。
    using BoxDropResolver = std::function<bool(const QStringList& shellPaths,
                                               const QStringList& displayNames,
                                               const QPoint& globalPos)>;
    // 设置盒子投放resolver
    void setBoxDropResolver(BoxDropResolver fn) { m_boxDropResolver = std::move(fn); }

    // 拖动过程中光标是否正悬停在某个收纳盒上（由 MainWindow 的悬停判定驱动）。
    // 效果：把光标换成“可放置/移动”的形状 —— Dock 拖动是自定义跟手移动（不走 QDrag），
    // 没有系统提供的落点光标，不给反馈时用户分不清“这里能放”还是“这里放不进去”。
    // 拖动收尾（abort/finish）会自动复位，调用方无需自己清。
    void setDragOverBox(bool on);

    // —— 跨窗口拖拽：图标浮层（被收纳盒盖住时补画到最上层）——
    // Dock 的拖动是**自定义跟手移动**（按钮本体 move()，不走 QDrag），而 Dock 分层窗恒在最底、
    // 收纳盒在其上 → 图标拖进盒子会被整块盖住，只剩光标可见。MainWindow 在拖拽悬停时把
    // 「当前被遮挡的屏幕区域」下发过来，Dock 只补画这一块（遮挡区之外仍由真实按钮绘制，
    // 浮层整块画就会与真实图标叠成重影）。传空矩形 = 未被遮挡（浮层隐藏）。
    // 收尾（abort / finishDrag）由 Dock 自己复位，调用方无需清理。
    void setDragGhostClip(const QRect& globalClipRect);

    // 清除桌面整理分类库（CategoryStore）的全部持久化记录。
    // 调用后所有文件回到“未分类”，Dock 将与当前真实桌面目录保持一致。
    static void clearCategoryStore();

    // 清除 Dock 相关的本地持久化数据：分类库 categories.ini，以及可能存在的 Dock 快照组。
    // 调用后下次启动会从当前真实桌面重新镜像生成图标，避免陈旧数据导致 Dock 与桌面不一致。
    static void clearDockPersistedData();

    // 把 Dock 当前镜像的全部图标（显示名 / shellPath / 图标索引 / 屏幕坐标 / 大小 / 系统项标记）
    // 写入本地持久化数据（DockSnapshot 组），作为下次启动或异常时回退的真实桌面集合。
    void saveDockSnapshot() const;
    // Dock 自定义图标位置覆盖的持久化：用户在 Dock 内拖动的图标位置（仅 Dock 内生效，不入真实桌面）。
    void saveCustomDockPos() const;
    // 加载自定义Dock位置
    void loadCustomDockPos();

    // dock 复位：清空 dock 内图标，临时还原原生桌面以重读最新图标位置，
    // 重拍快照后重新隐藏原生桌面，并按 Windows 桌面最新位置重排 dock 图标。
    void resetDock();

    // 由 DockIconButton 调用：在 DesktopOrganize 模式下不再重定位真实桌面图标，仅保留接口兼容。
    void repositionDesktopIcon(const QString& shellPath, const QPoint& globalTopLeft);

    // 把"逻辑坐标（Dock 窗口/屏幕逻辑像素）下的图标左上角"吸附到最近的网格格子，
    // 使 Dock 内拖动 / 盒→Dock 落下的图标与上下左右图标行列对齐（贴近 Windows 桌面“对齐网格”）。
    // 网格 = 真实桌面图标网格（步长 m_gridStepX/Y，原点 = ListView 原点折算逻辑坐标）。
    QPoint snapToGridLogical(const QPoint& logicalTopLeft) const;

    signals:
    // Dock 首次镜像就绪（界面已显示 + 数据已读取 + 图标已布局完成）后发射一次，
    // 供 MainWindow 在 Dock 完全加载完成后再触发收纳盒加载，确保 Dock 先于收纳盒就绪。
    void firstMirrorReady();
    // Dock 内联重命名已落盘（oldPath/newPath 均为桌面下绝对路径，正斜杠）。
    // 供 MainWindow 同步收纳盒/网格里该项的路径与分类记录：改名（尤其改后缀）后若不通知，
    // 收纳盒内的项仍指向旧路径，会表现为“盒子里的图标点不动/下次整理时归属丢失”。
    void itemRenamedOnDesktop(const QString& oldPath, const QString& newPath);
    // Dock 内图标被删除（Delete / D 快捷键，走系统回收站删除，与资源管理器“删除”同一条路径）。
    // paths 为归一化（正斜杠）绝对路径。供 MainWindow 清理以路径为键的残留：m_allItems、
    // CategoryStore 分类记录、Dock 隐藏集合 —— 不清就会留下指向已删文件的孤儿记录。
    void itemsDeletedOnDesktop(const QStringList& paths);
    // 拖动 Dock 图标过程中，光标实时位置（逻辑屏幕坐标）。供 MainWindow 判定当前是否悬停在
    // 某个收纳盒窗口上，并让该窗口亮起“松手即收进此处”的提示框。
    void iconDragHoverChanged(const QPoint& globalPos);
    // 本次图标拖动已结束（无论落点在哪）。供 MainWindow 清除所有收纳盒窗口的悬停提示框。
    void iconDragFinished();
    // 「在桌面空白处绘制创建收纳盒」：用户在桌面空白处拉出的框松手后发射。
    // globalRect  —— 框的逻辑屏幕坐标（松手即可按此几何摆放新盒窗口）；
    // shellPaths  —— 框内图标（与「从 Dock 拖图标进收纳盒」同一取法：跳过 "::" 系统虚拟项）；
    // displayNames—— 与 shellPaths 一一对应的显示名。
    // MainWindow 收到后新建收纳盒（几何 = globalRect），并把这批图标收进它的默认分类。
    void boxRegionDrawn(const QRect& globalRect, const QStringList& shellPaths,
                        const QStringList& displayNames);

    protected:
    // 鼠标按下事件
    void mousePressEvent(QMouseEvent* event) override;
    // 鼠标移动事件
    void mouseMoveEvent(QMouseEvent* event) override;
    // 鼠标松开事件
    void mouseReleaseEvent(QMouseEvent* event) override;
    // paint事件
    void paintEvent(QPaintEvent* event) override;
    // 缩放事件
    void resizeEvent(QResizeEvent* event) override;
    // 显示事件
    void showEvent(QShowEvent* event) override;
    // change事件
    void changeEvent(QEvent* event) override;
    // 原生事件
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;
    // 应用级“点别处即提交”守卫（装在 qApp 上）：见 cpp 中的实现说明。
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    // 由 DockIconButton 上报的鼠标交互，统一在此决策选中/菜单/拖动
    void onIconPressed(DockIconButton* self, const QPoint& globalPos,
                      Qt::MouseButtons buttons, Qt::KeyboardModifiers mods);
    // 响应图标拖拽移动
    void onIconDragMove(DockIconButton* self);
    // 响应图标拖拽结束
    void onIconDragEnd(DockIconButton* self, const QPoint& finalGlobalTopLeft);
    void onIconReleased();                 // 左键手势结束（含普通单击/双击）：复位拖动态
    // 由 DockIconButton 在内联重命名落盘成功后上报，用于同步 m_entries 的显示名与 shellPath
    void onItemRenamed(const QString& oldName, const QString& newName);
    // 由 DockIconButton 上报内联重命名开始/结束：编辑期间延后桌面刷新，避免重建按钮销毁编辑框
    void onInlineEditStarted(DockIconButton* btn);
    // 响应inline编辑finished
    void onInlineEditFinished(DockIconButton* btn);

private:
    void readEntries();                 // 根据 m_sourceMode 读取数据源
    void readEntriesFromOrganize();     // 桌面整理数据源
    void readEntriesFromListView();     // Windows ListView 数据源（旧）
    Entry readItemRemote(void* hLV, void* hProc, int i); // 跨进程读单项

    // 在隐藏原生桌面前先快照真实图标位置，避免隐藏后 LVM_GETITEMPOSITION 失效
    struct NativeSnapshot {
        int desktopIconSize = 32;
        int cellW = 80;
        double dpiFactor = 1.0;
        long lvOriginX = 0;
        long lvOriginY = 0;
        QHash<QString, QPoint> positions; // 显示名 -> 物理屏幕坐标
    };
    // take原生快照
    bool takeNativeSnapshot();
    NativeSnapshot m_snapshot;
    QVector<Entry> m_snapshotEntries;   // 原生桌面可见时拍下的权威图标集合（含真实坐标），Dock 的唯一真相源
    void layoutButtons();               // 依据 m_entries 摆放 DockIconButton
    void layoutButtonsGrid();           // DesktopOrganize 模式：网格排列
    void layoutButtonsNative();         // NativeListView 模式：按 Windows 位置
    void applyAutoRename();             // 布局完成后让“新建项”按钮进入内联重命名态（按 shellPath 定位，抗重建）
    // 提交当前所有处于内联重命名态的项（对齐 Windows：点空白/点其它图标/弹菜单前都先结束命名）。
    // 直接遍历 m_buttons 判定 isEditing()，不依赖 m_editingBtn 这一路追踪，避免极端时序下漏提交。
    void commitActiveRename();
    // 原生菜单关闭后补做菜单期间被延后的刷新（若此时正在重命名则继续延后，交由编辑结束再补）。
    void flushPendingRefresh();
    void buildNameMap();                // 枚举桌面 IShellFolder 建立显示名->shellPath 映射
    void* bestListView();               // 选“图标最多且可见”的桌面 ListView 句柄
    QString resolveShellPath(const QString& displayName);      // 由显示名解析出 shellPath（含特殊项 CLSID）
    void updateSnapshotPosition(const QString& shellPath, const QPoint& physPos); // 拖动后回写快照位置
    int queryDesktopIconSize();  // 查询 Windows 桌面真实图标像素尺寸（逻辑像素）
    // 注册change通知
    void registerChangeNotify();
    // 注销change通知
    void unregisterChangeNotify();
    // 显示桌面空菜单
    void showDesktopEmptyMenu(const QPoint& globalPos);
    // 隐藏原生桌面
    void hideNativeDesktop();
    // 还原原生桌面
    void restoreNativeDesktop();

    // —— Dock 全局快捷键：F2 重命名 / Delete(或 D) 删除，对齐 Windows 桌面图标 ——
    // 为什么必须下沉到系统级低级钩子：Dock 主窗口带 WS_EX_NOACTIVATE + Qt::WindowDoesNotAcceptFocus
    // （Win+D 免疫所必需），它永远拿不到键盘焦点 —— Qt 的 keyPressEvent / 菜单加速键在本窗口上
    // 从不触发。只有 WH_KEYBOARD_LL 能无关窗口激活状态地看到物理按键，判定只依赖“按键 + 最近一次
    // 鼠标交互落在哪里”，与窗口激活、Qt 事件路由完全无关。
    // 防误触（全部满足才响应并吞键，否则一律放行）：
    // ① Dock 正在运行且持钩；② 有选中项；③ 不在内联重命名/原生菜单中；
    // ④ 最近一次鼠标按下落在 Dock（或桌面外壳面）上；⑤ 无 Ctrl/Shift/Alt/Win 修饰键。
    void ensureHotkeyHooks();                 // 安装（自愈：先卸后装，规避低级钩子被静默摘除）
    // 松开hotkeyhooks
    void releaseHotkeyHooks();
    bool hotkeyContextOk() const;             // 当前是否处于“Dock 面”上下文
    int  hotkeyAction(int vk) const;          // 0=非本快捷键 1=F2 重命名 2=删除
    void triggerRenameShortcut();             // F2
    void triggerDeleteShortcut();             // Delete / D

    // —— 拖到回收站 = 删除（对齐 Windows 桌面：把图标拖到回收站上松手即删除） ——
    // 关键点：**不能让图标在回收站上叠着**。落点判定必须基于“可视图标矩形是否压上回收站图标”，
    // 命中则走与 Delete 键完全相同的删除通道（SHFileOperation → 回收站，可还原），
    // 未命中才回写真实桌面位置；若命中了却一个都没删成（全是受保护项），必须把整组位置还原。
    DockIconButton* recycleBinButton() const;  // 定位回收站图标（拖放落点）
    bool dragOverRecycleBin() const;           // 本次拖动是否压在回收站上（仅对“可删除项”成立）
    void updateDropTargetHighlight(bool force = false);   // 拖动中刷新回收站的高亮提示态
    // 回收站满/空状态跟随 Windows 桌面：SHChangeNotifyRegister 只监听桌面文件夹（非递归），
    // 收不到回收站（特殊命名空间项）的状态翻转通知，故用轻量轮询兜底。状态未翻转时零开销，
    // 翻转时重取回收站真实图标并刷新 Dock 按钮。
    void pollRecycleBinState();
    void restoreDragSnapshot();                // 位置还原（拖动未生效时用，避免图标摞在一起）
    void abortActiveDrag();                    // 复位拖动态（冻结标志 + 拖动组/快照裸指针），防悬垂
    // 拖拽浮层四件套：抓位图（每轮拖动只抓一次）/ 按遮挡区更新位置与显隐 / 隐藏 / 收尾复位。
    void ensureDragGhostFrames();
    // 更新拖拽浮层
    void updateDragGhost();
    // 隐藏拖拽浮层
    void hideDragGhost();
    void resetDragGhost();   // 三条收尾路径（finishDrag / abortActiveDrag / 起拖前）共用
    // 删除一组 Dock 项（SHFileOperation，与资源管理器“删除”同一条路径 → 进回收站可还原）。
    // 返回是否真的删掉了至少一项；受保护项（系统图标/个人文件夹等）自动跳过。
    bool deleteDockItems(const QVector<DockIconButton*>& targets);
#ifdef Q_OS_WIN
    // A1：不再各自 SetWindowsHookEx 安装低级钩子，改为注册到进程级
    // LowLevelHookManager —— 全进程只保留 1 个 WH_KEYBOARD_LL + 1 个 WH_MOUSE_LL，
    // 由 Dock 与收纳盒共享（原来各装一套 = 最多 4 个钩子，每次鼠标移动走两遍回调）。
    static bool hotkeyKeyConsumer(int vk, void* ctx);                    // true = 已消费（吞键）
    // hotkey鼠标consumer
    static void hotkeyMouseConsumer(WPARAM msg, LPARAM lParam, void* ctx);
    // 该屏幕点是否属于 “Dock 面”：落在 Dock 覆盖范围内，且该点上的窗口是本进程 Dock 自身/子控件，
    // 或属于桌面外壳面（Progman/WorkerW/SHELLDLL_DefView —— Dock 正是取代桌面的存在）。
    // 其它进程的真实程序窗口覆盖于此 → 返回 false（那一击属于那个程序，快捷键一律放行）。
    bool nativeHitTestIsDock(POINT pt) const;
#endif

    // —— 选中集合管理（支持 Ctrl/Shift 多选 + 画框多选），贴近 Windows 桌面 ——
    void selectExclusive(DockIconButton* b);   // 清空其它，仅选中 b，设锚点
    void addToSelection(DockIconButton* b);    // 加入集合（Ctrl 加选）
    void toggleSelection(DockIconButton* b);   // 在集合中切换（Ctrl 点已选中项）
    void rangeSelect(DockIconButton* b);       // 从锚点到 b 全选（Shift）

    // —— 画框多选（橡皮筋），对齐 Windows 桌面 ——
    // 双通道驱动，且都汇进同一套状态机（幂等）：
    // ① Qt 通道：mousePressEvent/mouseMoveEvent/mouseReleaseEvent；
    // ② 系统级 WH_MOUSE_LL 通道：空白处按下后由 16ms 轮询（QCursor::pos + GetAsyncKeyState）
    // 自行推进与收尾。为什么必须有②：Dock 带 WS_EX_NOACTIVATE 且常年 HWND_BOTTOM，
    // 落在空白处的按下未必经 Qt 的事件路由回到本窗口（与“点空白提交不了重命名”
    // “点空白清不掉选中”同源）—— 只靠 Qt 会出现“框选拖不出来”。
    // 轮询而非依赖 WM_MOUSEMOVE 钩子：钩子回调必须极轻，逐条 move 消息投递会拖垮它。
    void beginRubber(const QPoint& globalPos, bool additive);  // 开始框选（已在进行中则忽略）
    void updateRubber(const QPoint& globalPos);                // 更新框选矩形 + 实时选中
    void endRubber();                                          // 结束框选（幂等，停轮询、清绘制）
    void rubberTick();                                         // 轮询推进（仅编辑期运行）
    // 应用框选选中
    void applyRubberSelection(const QRect& rect, bool additive);
    // 把框选矩形画进本窗口（自绘，不用 QRubberBand）：Dock 是 WA_TranslucentBackground 的透明层，
    // 自绘可避免多出一个子窗口带来的 z 序/裁剪问题，且与图标同一层绘制、不闪。
    void onBlankPressFromHook(const QPoint& physPt);           // 钩子通道：Dock 空白处按下
    // 物理屏幕像素 → 本窗口逻辑坐标（低级钩子给的是物理像素；Qt 用的是逻辑像素）。
    QPoint nativePhysToWindowLogical(const QPoint& phys) const;

    // —— 快捷操作（设置中心「快捷操作」页）——
    // 桌面空白处「双击」判定 + 处理。WH_MOUSE_LL 只送 WM_LBUTTONDOWN/UP，Windows 的
    // WM_LBUTTONDBLCLK 不经低级钩子，故必须自按系统双击参数（GetDoubleClickTime +
    // SM_CX/YDOUBLECLK）判定。只在**空白处**计数：点在图标上是"打开文件"，绝不能连坐触发。
    // 返回 true = 这一击已按"双击空白"处理（调用方不要再清选中/起框）。
    bool handleBlankDoubleClick(const QPoint& physPt);
    // 把 m_desktopIconsHidden 套用到全部图标按钮上。任何一次重排/重建后都要重套
    // （layoutButtons 末尾调用）——否则刷新一次就把隐藏态冲掉（表现为"隐藏一下又自己回来了"）。
    void applyDesktopIconsHiddenState();

    // 全局点是否落在某个图标的“可视区”（图标/标签）上；否则视为 Dock 空白处。
    // 不能用 childAt()：单元格 72x84 远大于图标+标签，落在单元格内边距上的点 childAt() 也会
    // 返回该按钮，于是“点图标旁的空白”被误判成“点图标”，选中状态永远清不掉。
    DockIconButton* iconAtGlobal(const QPoint& globalPos) const;
    // 点 Dock 空白处 → 清空选中（若有）。鼠标侧统一入口：Qt 的 mousePressEvent（左键/右键空白）
    // 与系统级鼠标钩子（左键/右键按下）都会调用它。为什么要钩子兜底：Dock 带 WS_EX_NOACTIVATE，
    // 落在空白处的按下未必会经 Qt 的事件路由回到本窗口（此前“点空白提交不了重命名”同源）。
    void clearSelectionOnBlankPress(const QPoint& globalPos);

    void dumpDebug(const QString& tag); // 写调试日志

#ifdef Q_OS_WIN
    // —— 抗“显示桌面”(Win+D)/“最小化全部”(Win+M) 守卫（T5 事件钩子为主 + 100ms 轮询兜底）——
    static QHash<HWND, QPointer<QWidget>> s_cloakTargets;  // 常驻窗口：HWND -> QWidget（意图判定用）
    static HWINEVENTHOOK s_cloakHook;     // EVENT_OBJECT_CLOAKED  ：Win+D 的 DWM 外壳斗篷
    static HWINEVENTHOOK s_hideHook;      // EVENT_OBJECT_HIDE     ：SW_HIDE 隐藏路径
    static HWINEVENTHOOK s_minimizeHook;  // EVENT_SYSTEM_MINIMIZESTART：Win+M 最小化
    static HWINEVENTHOOK s_showHook;      // EVENT_OBJECT_SHOW     ：窗口重新显示（退出显示桌面按钮路径）
    static QTimer* s_pollTimer;           // 100ms 轮询兜底（事件漏报时补救）
    static bool s_restoring;              // 防重入：恢复过程自身会再次触发 HIDE/CLOAKED 事件
    // —— Dock 快捷键（F2 / Delete）全局钩子状态 ——
    // A1：HHOOK 已改由进程级 LowLevelHookManager 统一持有（与收纳盒共享），
    // 本类不再保存句柄，只用 s_hkActive 记录“Dock 是否已注册到共享钩子管理器”。
    static bool  s_hkActive;
    static QPointer<DesktopMirrorWindow> s_hkOwner;   // 当前持钩的 Dock（QPointer 防悬垂）
    static bool  s_hkLastClickOnDock;     // 快捷键的作用域标志（见 hotkeyContextOk 说明）
    // Rainmeter 式 topmost 状态机核心：轮询驱动「前台=桌面→置顶 / 前台=真实程序→撤顶」的进出边沿，
    // 并兜底 Win+M 最小化还原与 show desktop 期间窗口被误藏后的重新显示。
    static void restoreAllTargets(bool force = false);
#endif

    QVector<DockIconButton*> m_buttons;
    QHash<QString, QString> m_nameToShell;  // 显示名 -> shellPath
    QVector<Entry> m_entries;
    QByteArray m_entriesDigest;             // 上一次 m_entries 的指纹（displayName|shellPath|special|hasPos 序列化）；
                                            // refresh() 用它判定条目集合是否真的变了——若未变就跳过 layoutButtons
                                            // （即跳过"销毁并重建全部 DockIconButton"），从而保住 m_selection/m_anchor 不丢，
                                            // 也为意外的多源刷新风暴（如 SHCN 递归监听时的自激循环）兜底。
    QSet<QString> m_hiddenShellPaths;       // 已被收纳盒“收走”的图标（归一化小写绝对路径），Dock 不再显示
    // Dock 自定义图标位置覆盖：lowercased shellPath -> 物理屏幕坐标。用户在 Dock 内拖动图标只改这里，
    // 绝不写回真实 Windows 桌面；复位(resetDock)时清空。layoutButtonsNative 优先用此覆盖摆放。
    QHash<QString, QPoint> m_customDockPos;
    QSet<QString> m_autoRenamedPaths;       // 已自动进入过重命名态的新建项 shellPath（归一化，防刷新反复弹框）
    QSet<QString> m_autoRenameQueue;        // 待自动重命名的 shellPath（布局完成后统一触发，抗按钮重建）
    QPointer<DockIconButton> m_editingBtn;  // 当前正在内联重命名的按钮（非空则延后桌面刷新；QPointer 防悬垂）
    // A3：应用级事件过滤器不再常驻。它只为“点编辑框之外即提交重命名”服务，
    // 故改为内联重命名期间 install、编辑全部结束后 remove（进程内所有控件的所有事件都要过
    // 一次虚调用，常驻是纯浪费）。用独立标志而非直接判 m_editingBtn，便于在异常时序下保守地
    // 保持挂载（宁可多留，不可漏提交）。
    bool m_appFilterInstalled = false;
    bool m_refreshPending = false;          // 编辑期间被延后的刷新请求
    // 原生右键菜单（空白处/图标上）弹出期间为 true。菜单是原生模态循环，Qt 定时器仍可能在循环内
    // 被触发 → refresh/layoutButtons 会 deleteLater 掉“正在弹菜单的那个按钮”，其 HWND 随之销毁，
    // 而菜单命令返回后还要在该按钮上 startInlineRename()（this 已失效）→ 右键“重命名”静默失效。
    // 因此菜单期间一律延后重排，菜单关闭后再补做。
    bool m_menuOpen = false;
    QTimer* m_autoRenameTimer = nullptr;    // 自动重命名的重试定时器：竞态下首轮未弹出时补弹
    int m_autoRenameRetry = 0;              // 已重试次数（上限保护，避免死循环）
    // 会话基线是否已登记（只做一次）：程序启动后**首次拿到有效桌面条目**时，把此刻已存在的一切
    // 记入 m_autoRenamedPaths —— "本次会话之前就存在"的项永不自动弹重命名框。没有它的话，
    // 桌面上遗留的「新建文本文档.txt」每次启动都会被当成新建项，逐个弹出重命名框【用户 2026-09-22】。
    bool m_autoRenameArmed = false;

    double m_dpiFactor = 1.0;
    int m_cellW = 80;                   // 逻辑单元格宽（遗留/显示密度参考，勿用于吸附）
    // 真实桌面图标网格步长（逻辑像素，由 LVM_GETITEMSPACING 真实间距折算），用于 Dock 内拖动吸附对齐。
    double m_gridStepX = 84.0;          // 水平步长（相邻图标左上角间距）
    double m_gridStepY = 96.0;          // 垂直步长
    int m_desktopIconSize = 32;         // 桌面真实图标像素尺寸（Dock 图标按此绘制）
    long m_lvOriginX = 0;               // 真实 ListView 工作区原点（仅 NativeListView 模式）
    long m_lvOriginY = 0;
    void* m_hLV = nullptr;              // SysListView32 句柄（仅 NativeListView 模式）
    unsigned long m_changeId = 0;
    bool m_mirroring = false;

    QList<DockIconButton*> m_selection;  // 当前选中图标集合（替换单一 m_selectedBtn）
    DockIconButton* m_anchor = nullptr;  // Shift 范围选锚点

    // —— 画框多选状态 ——
    bool m_rubbering = false;
    bool m_rubberAdditive = false;                 // 本次画框是否 Ctrl 加选（不清基线集合）
    QPoint m_rubberStart;                          // 框选起点（窗口局部坐标）
    QRect m_rubberRect;                            // 当前框选矩形（窗口局部坐标，自绘）
    QList<DockIconButton*> m_rubberBase;           // Ctrl 加选时的基线集合（框选开始时拍下）
    QTimer* m_rubberTimer = nullptr;               // 16ms 轮询：不依赖 Qt 鼠标事件路由地推进/收尾画框
    // 本次画框的语义（见 beginRubber）：true = 「在桌面空白处绘制创建收纳盒」——松手时按框
    // 新建收纳盒并收起框内图标，框内**不做**选中（避免"拉个框建盒子"顺带选中一堆图标）。
    // Ctrl 拉框恒为框选多选（保留多选/删除/拖动这条既有能力，是本开关的逃生口）。
    bool m_rubberCreatesBox = false;
    // 双击那一击不再起框：钩子与 Qt 两条通道都会来调 beginRubber，双击的第二击若起框，
    // 用户会在"隐藏了桌面图标"后看到一个残留选择框。置真后被 endRubber()/下一次按下复位。
    bool m_suppressRubber = false;

    // —— 快捷操作开关（设置中心「快捷操作」页；构造期读一次 + settingChanged 热更新）——
    bool m_quickHideIconsOnDblClick = true;   // ① 双击桌面空白处隐藏桌面图标
    bool m_quickDrawBoxOnBlank = true;        // ② 在桌面空白处绘制创建收纳盒
    bool m_desktopIconsHidden = false;        // 桌面图标当前是否处于"双击隐藏"态（会话内有效）
    unsigned int m_lastBlankDownTick = 0;     // 双击判定：上次"空白处按下"的 GetTickCount
    QPoint m_lastBlankDownPhys;               // 双击判定：上次"空白处按下"的物理像素坐标

    QVector<DockIconButton*> m_dragGroup; // 当前拖动涉及的图标（单选=自身，多选=整组）
    QVector<QPair<DockIconButton*, QPoint>> m_dragSnapshot; // 拖动起点时各组按钮的局部坐标
    DockIconButton* m_dragPrimary = nullptr;
    QPoint m_dragPrimaryStart;           // 主拖拽按钮的起始局部坐标（组同步位移的基准）
    QPoint m_dragStartGlobal;            // 拖动起点（全局坐标）
    bool m_dragActive = false;
    bool m_dragOverBox = false;          // 拖动中光标是否悬停在某个收纳盒上（驱动 setDragOverBox 的光标反馈）
    // 本次手势是否真的发出过“拖动悬停”通知（= iconDragHoverChanged 至少发过一次）。
    // 收尾时只有它为 true 才需要通知外部清理 —— 否则每次单击 Dock 图标（press 无条件置 m_dragActive）
    // 都会触发一轮“遍历所有收纳盒窗口清指示线”的无谓开销。由 abortActiveDrag / finishDrag 复位。
    bool m_dragHoverNotified = false;
    // C1：拖动期间“是否压在回收站上”的判定缓存。
    // 回收站按钮在整段拖动中不会移动，拖动组成员也不会变 —— 而原实现是【每个鼠标 move 都重算】：
    // recycleBinButton() 要对每个按钮调 isRecycleBin()（每次 2 次 QString 分配），
    // dragOverRecycleBin() 又对每个成员调 isDeletableOnDrop() → ShellOps::isSafeToDelete()
    // （含 QFileInfo::exists 文件系统 stat + QStandardPaths shell 路径查询）。
    // 拖动 move 是逐像素到达的（可达数百 Hz），这些全部改为在拖动开始时算一次。
    DockIconButton* m_dragRecycleBin = nullptr;   // 拖动开始时的回收站按钮（不存在则空）
    QVector<bool> m_dragDeletable;                // 与 m_dragGroup 对齐的“可拖删”标志
    qint64 m_lastDropHighlightMs = 0;             // 回收站高亮的节流时间戳（同 m_lastHoverEmitMs）
    BoxDropResolver m_boxDropResolver;   // 跨窗口拖拽落点解析器（由 MainWindow 注册）
    qint64 m_lastHoverEmitMs = 0;        // 拖动中悬停信号的节流时间戳（避免逐像素发信号）

    // —— 拖拽浮层（见 setDragGhostClip）——
    QWidget* m_dragGhost = nullptr;      // 置顶浮层；无父对象（顶层窗），析构时显式 delete
    QRect m_dragGhostClip;               // 屏幕坐标：当前被收纳盒遮挡的区域（空 = 未遮挡 → 浮层隐藏）
    bool m_dragGhostFramesReady = false; // 本次拖动的图标位图是否已抓取（每轮拖动重抓一次）
    QSize m_dragGhostSize;               // 拖动组包围盒尺寸（拖动期间恒定，故浮层只需 move 不需 resize）
    QPoint m_dragGhostBBoxOffset;        // 包围盒左上角相对 m_dragGroup.first() 左上角的偏移
    QVector<QPair<QPoint, QPixmap>> m_dragGhostFrames;   // (相对包围盒左上角的偏移, 图标位图)

    QTimer* m_refreshTimer = nullptr;
    QTimer* m_bottomTimer = nullptr;   // 兜底：周期性把 Dock 压到 Z 序最底层
    QTimer* m_recyclePollTimer = nullptr;   // 周期轮询回收站满/空状态，翻转时刷新 Dock 回收站图标
    bool m_recycleBinKnown = false;    // 是否已取到首次回收站状态（首拍不刷新，只记账）
    bool m_recycleBinEmpty = true;     // 上次已知的回收站空/满状态
    QSize m_lastSize;
    SourceMode m_sourceMode = SourceMode::NativeListView;  // 1:1 镜像 Windows 桌面
    bool m_firstReadyEmitted = false;  // 保证 firstMirrorReady 仅发射一次
};

#endif // DESKTOPMIRRORWINDOW_H
