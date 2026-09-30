/*
 * @file icongridwindow.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef ICONGRIDWINDOW_H
#define ICONGRIDWINDOW_H

#include <QWidget>
#include <QMap>
#include <QHash>
#include <QVector>
#include <QList>
#include <QStringList>
#include <QPointer>
#include <functional>   // 跨窗口拖拽：Dock 落点解析器
#include "desktopitem.h"

QT_BEGIN_NAMESPACE
class QHBoxLayout;
class QGridLayout;
class QScrollArea;
class QToolButton;
class QLabel;
class QMouseEvent;
class QMenu;
class QCloseEvent;
class QFrame;
class QResizeEvent;
class QShowEvent;
class QGraphicsDropShadowEffect;
class QLineEdit;
class QTimer;
QT_END_NAMESPACE

class FenceBox;       // 全屏收纳（Fences）模式下的分类盒子
class SettingsManager;  // 前向声明：applyPersistedCategories 以引用方式使用该类型
class DesktopIconButton; // 网格内的单个图标按钮
class PressOutsideCancelFilter; // 独立的「点空白取消选中」全局过滤器（见 icongridwindow.cpp）

// 左侧无边框窗口：分类标签 + 图标网格
class IconGridWindow : public QWidget {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit IconGridWindow(QWidget* parent = nullptr);
    ~IconGridWindow() override;

    // 设置条目
    void setItems(const QMap<QString, QVector<DesktopItem>>& items);
    // 返回当前窗口持有全部分类 → 图标（权威来源）；收纳盒持久化时据此读取分类与条目
    const QMap<QString, QVector<DesktopItem>>& items() const { return m_allItems; }
    // 设置搜索文本
    void setSearchText(const QString& text);
    // 设置当前分类
    void setCurrentCategory(const QString& cat);
    // 当前分类
    QString currentCategory() const;
    // 分类显示顺序（与 m_categoryOrder 同口径）：getter 供收纳盒持久化读取；
    // setter 在重建盒子时按本地持久化的顺序覆盖（过滤掉已不存在的分类、补齐新增分类）。
    QStringList categoryOrder() const { return m_categoryOrder; }
    // 设置分类顺序
    void setCategoryOrder(const QStringList& order);
    // 重命名分类
    void renameCategory(const QString& oldName, const QString& newName);

    // —— “查看”视图模式：大图标 / 中等图标 / 小图标 / 列表（右键“查看”子菜单） ——
    // 三档图标尺寸决定网格单元格尺寸与图标绘制像素；列表模式在此之上强制单列。
    // 档位写入本地持久化数据（IconGridWindow/viewMode），默认“中等图标”。
    enum ViewMode { ViewLarge = 0, ViewMedium = 1, ViewSmall = 2, ViewList = 3 };
    void setViewMode(int mode);              // 切换视图模式并重建网格（非法值回落到中等图标）
    int viewMode() const { return m_viewMode; }
    // 某视图模式对应的单元格尺寸与图标像素（出参）。列表模式沿用中等图标尺寸，
    // 单列由 computeGridColumns() 负责。
    static void iconViewMetrics(int mode, int& cellW, int& cellH, int& iconPx);

    // C：重活期间临时摘掉低级钩子
    // 与 DesktopMirrorWindow::suspendHotkeyHooks 同一口径：低级钩子是系统同步回调到本 GUI
    // 线程的，线程被重活钉住时全系统鼠标都会等我们返回。任何长任务前后调用这一对函数，
    // 使「操作慢」不再等于「鼠标卡死」。嵌套安全；未处于暂停中的 resume 是空操作。
    static void suspendGridHooks();
    // 恢复网格hooks
    static void resumeGridHooks();

    // 为收纳盒窗口提供可关闭标题栏按钮；默认隐藏以保持主窗口外观不变
    void setCloseButtonVisible(bool visible);
    // 是否强制保证存在“快捷方式”分类；主窗口需要，收纳盒不需要
    void setForceShortcutCategory(bool force);
    // force快捷方式分类
    bool forceShortcutCategory() const;
    // 标识该窗口是否为独立收纳盒窗口，用于区分右键菜单文案等
    void setIsBoxWindow(bool isBox);
    // 判断盒子窗口
    bool isBoxWindow() const;
    // 当前是否处于折叠状态
    bool isCollapsed() const;
    // 展开尺寸
    QSize expandedSize() const;
    // 设置展开尺寸
    void setExpandedSize(const QSize& size);
    // 设置折叠/展开状态。save 为 true 时表示由用户交互触发，会持久化并通知主窗口；
    // 为 false 时仅更新内部状态与界面，供启动恢复阶段使用，避免覆盖已持久化的展开尺寸。
    void setCollapsed(bool collapsed, bool save = false);

    // 全屏收纳（Fences）模式：窗口铺满工作区，所有分类以半透明盒子形式平铺在壁纸上。
    // 文件不搬动，跨盒拖拽只改分类记录（A 方案）。
    void setFencesMode(bool on);
    bool isFencesMode() const { return m_fencesMode; }
    // 清除所有盒子的自定义位置/尺寸，回到自动流式排布（右键菜单“重排收纳盒”）
    void resetFenceLayout();
    // 进入全屏前的浮动窗口几何，供退出全屏时还原（也用于持久化，避免落盘全屏几何）
    QRect preFencesGeometry() const { return m_preFencesGeom; }
    // 返回第一个非隐藏分类；没有可见分类时返回空字符串
    QString firstVisibleCategory() const;
    // 不显示为分类的类型：系统（走 Dock）、未分类（按用户要求去掉该分类显示）
    static bool isHiddenCategory(const QString& cat);

    // —— 图标选中 / 快捷键（与 Dock 图标同一套操作语义） ——
    // F2 重命名 / Delete·D 删除：本窗口带 WS_EX_NOACTIVATE（Win+D 免疫所必需），
    // **永远拿不到键盘焦点**，keyPressEvent / QShortcut 从不触发，所以快捷键由
    // 进程内的 WH_KEYBOARD_LL 统一接管（见 icongridwindow.cpp）；下面三个入口即钩子的落点。
    void triggerRenameShortcut();
    // trigger删除快捷方式
    void triggerDeleteShortcut();
    bool hotkeyGateOk() const;          // 钩子上下文判定：是否该由本窗口消费这一键
    bool isEditingAnyIcon() const;      // 是否有图标正在内联重命名
    QVector<DesktopIconButton*> allIconButtons() const;   // 本窗口全部图标（网格视图 + 全屏盒子）
    void commitActiveRename();          // 提交本窗口内正在进行的内联重命名
    // 清除选中
    void clearSelection();
    void blankPressFromHook(const QPoint& physScreenPt);   // 系统级鼠标钩子的“点空白”入口
    // 独立全局过滤器的落点（见 PressOutsideCancelFilter）：qApp 级鼠标按下时，
    // 若本窗口有选中项且落点不是图标/改名框/菜单，则清空选中（跨窗口取消多选）。
    // 从 eventFilter 剥离出来，避免 qApp 过滤器让窗口内逻辑（尤其 MouseMove 的 mapTo）
    // 对全进程窗口执行 —— 那是「启动即崩溃」的根因。
    void handleGlobalPressCancel(const QPoint& globalPos);
    // 图标在全局
    DesktopIconButton* iconAtGlobal(const QPoint& globalPos) const;
    // 跨窗口拖放（Dock → 本窗口）落点后由 MainWindow 调用：让刚收进来的图标立即处于选中态，
    // 并重新武装快捷键作用域（拖动期间“按下”发生在 Dock → s_gridHot 仍指向旧窗口/已清空；
    // 且 rebuildGrid 的选中接力只找回“旧选中”，新收进来的图标不在集合里）。
    // 若不处理，松手后直接按 F2/Delete 会因 “empty selection / not hot window” 失效。
    void selectIconByShellPathAndArm(const QString& shellPath);

    // —— 跨窗口拖拽：收纳盒 ⇄ Dock ——
    // ① Dock → 本窗口（落点查询）：屏幕逻辑坐标落在本窗口哪个分类上？
    // 全屏盒子视图下命中某个 FenceBox 则返回该盒分类；网格视图下落在内容区即返回当前分类；
    // 未命中、或命中不可作为目标的分类（见 isHiddenCategory）返回空字符串。
    QString dropCategoryAtGlobal(const QPoint& globalPos) const;
    // ③ 落点是否在一个**真实可放置目标**上（供「盒 → Dock」判定“松手是不是拖出了盒子”）。
    // 与 ① 的区别很关键：① 会在网格视图下回退到“当前分类”，本函数则严格区分
    // “命中真实盒子”与“落在壁纸/透明区域上”。
    // 为什么不能直接用窗口矩形：全屏接管时本窗口铺满整个桌面、背景完全透明，
    // 用矩形判定会把“丢到壁纸上的图标”也当成丢进了盒子，导致盒 → Dock 永远不生效。
    // · 全屏盒子视图 / 盒子窗口：只有命中某个 FenceBox 才算（盒子之间的空白＝壁纸）；
    // · 窗口化单分类网格视图：整窗就是一个分类容器，落在窗口内即算。
    bool hitRealDropTarget(const QPoint& globalPos) const;
    // ② 本窗口 → Dock（松手判定）：解析器由 MainWindow 注册，返回 true = 该路径已由 Dock 接管
    // （本窗口不应再做任何本地处理）。之所以用带返回值的回调而不是信号：调用点在
    // DesktopIconButton 拖拽结束的同一栈上，必须**同步**得知结果才能决定后续动作。
    using DockDropResolver = std::function<bool(const QString& sourcePath,
                                               const QString& displayName,
                                               const QPoint& globalPos)>;
    // 设置Dock投放resolver
    void setDockDropResolver(DockDropResolver fn) { m_dockDropResolver = std::move(fn); }
    // 外部拖动悬停在本窗口上时的提示：绘制一圈“松手即收进此处”的高亮边框（不改变窗口布局）。
    void setExternalDropHighlight(bool on);

    // —— 外部拖拽（Dock 图标 / 资源管理器文件）的**插入位置指示** ——
    // 与上面 setExternalDropHighlight 的分工：那个是整窗边框（“会收进这里”），
    // 这个是网格内的竖直指示线（“会插到这个位置”），与盒内拖拽看到的是同一条线。
    // 返回 true = 落点在本窗口的图标网格上（指示线已显示）；*outIndex = 插入下标；
    // -1 表示落点在本窗口内但不在图标网格上（标题栏 / 分类标签 / 盒间壁纸）→ 调用方按“追加末尾”处理。
    bool updateExternalDropIndicatorAt(const QPoint& globalPos, int* outIndex);
    // 只查询插入下标（松手时用；-1 = 不在图标网格上），不改变指示线显示
    int externalDropIndexAt(const QPoint& globalPos) const;
    // 收起插入位置指示线（悬停离开 / 拖拽结束）
    void clearExternalDropIndicator();
    // 本窗口在 globalPos 处**真正遮挡住下层（Dock）的不透明表面**矩形（屏幕坐标）。
    // 用途：Dock 的图标浮层只补画被遮住的那一块（见 DesktopMirrorWindow::setDragGhostClip）。
    // 判据与“可投放”无关（被遮挡 ≠ 能放进去）：全屏/盒子视图下窗口本身整屏透明，遮挡物只有
    // 各个 FenceBox 卡片；窗口化视图下整扇窗口（卡片面板）都是不透明的。空矩形＝此处不遮挡。
    QRect externalDropSurfaceRectGlobal(const QPoint& globalPos) const;
    // 外部投放（Dock → 收纳盒）插到指定位置后调用：把该分类最新的条目顺序写进本窗口模型**并落盘**。
    // 必须在 setItems() **之前**调用 —— setItems() → applyItemOrder() 会按磁盘里的顺序重排，
    // 若磁盘里的顺序还是旧的（新项不在其中），applyItemOrder 会把新项甩到末尾，
    // 用户“插到光标位置”的落点就白做了。
    void setCategoryOrderedItems(const QString& category, const QVector<DesktopItem>& items);

    // 本窗口的图标正在被拖拽（由 DesktopIconButton 在 QDrag::exec() 前后置位）。
    // 期间必须冻结一切视图重建：FenceBox 及其中的按钮就是拖拽源，一旦被 hide / 重父化 /
    // deleteLater，Windows 会立刻中止这次拖放（DoDragDrop 随源窗口消失而结束），
    // 于是 exec() 提前返回、QCursor::pos() 只取到“光标离开盒子的那一瞬”，
    // 表现为“鼠标一离开收纳盒，图标就落在盒边”而不是落在松手处。
    void setIconDragInProgress(bool on);

signals:
    // 分类变化信号
    void categoryChanged(const QString& category);
    void fencesModeChanged(bool on);   // 全屏收纳模式开关变化
    // 菜单请求信号
    void menuRequested();
    // request删除分类
    void requestDeleteCategory(const QString& category);
    // request重命名分类
    void requestRenameCategory(const QString& category);
    // request新建分类
    void requestNewCategory();
    // request新建盒子
    void requestNewBox();
    // 关闭请求信号
    void closeRequested();
    // 窗口几何（位置/大小）发生用户操作后改变时发出，供 MainWindow 持久化
    void windowGeometryChanged();
    // 图标被收纳/取消收纳（源文件已离开原路径）后请求 MainWindow 重新扫描桌面与隐藏存储
    void requestRefresh();
    // 网格内图标被删除（真实文件已进回收站）：MainWindow 据此清理
    // m_allItems / 各收纳盒数据 / CategoryStore 等本地持久化数据并刷新所有窗口。
    void itemsDeleted(const QStringList& paths);
    // 网格内图标重命名落盘成功：MainWindow 据此迁移本地持久化数据（分类记录、顺序键）
    // 并刷新所有窗口。
    void itemRenamed(const QString& oldPath, const QString& newPath);

protected:
    // 鼠标按下事件
    void mousePressEvent(QMouseEvent* event) override;
    // 鼠标移动事件
    void mouseMoveEvent(QMouseEvent* event) override;
    // 鼠标松开事件
    void mouseReleaseEvent(QMouseEvent* event) override;
    // 进入事件
    void enterEvent(QEvent* event) override;
    // 离开事件
    void leaveEvent(QEvent* event) override;
    // 缩放事件
    void resizeEvent(QResizeEvent* event) override;
    // 显示事件
    void showEvent(QShowEvent* event) override;
    // 事件过滤
    bool eventFilter(QObject* watched, QEvent* event) override;
    // 关闭事件
    void closeEvent(QCloseEvent* event) override;
    // 常态拦截提层（DesktopMirrorWindow::clampBandZOrder）：杜绝点击/Qt show 把
    // 收纳盒顶到普通程序之上再被压回的「一闪而过」。show desktop 期放行。
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;

private:
    // 初始化ui
    void setupUi();
    // 同步分类顺序
    void syncCategoryOrder();
    // 构建标签
    void buildTabs();
    // 重建网格
    void rebuildGrid();
    // 应用网格条目
    void applyGridItems(const QVector<DesktopItem>& items);
    // 几何变化的唯一出口（resizeEvent / showEvent 共用）：可用宽度变了（列数变化，列表档为行宽变化）
    // 只需把**现有按钮重新摆位**，不得走 rebuildGrid。
    void relayoutGridForGeometry();

    // —— 「顺序变了、集合没变」的廉价出口（2026-09-24）——
    // 场景：收纳盒里把图标 A 拖到 B 的位置（交换位置）、同一分类内挪动次序。
    // 此时数据只是换了个排列，按钮对象 / 图标位图完全可以原样复用 —— 不该走 rebuildGrid()。
    // 返回 true = 已原地重排完毕（未新建/未销毁任何控件、未重取任何图标）；调用方无需再 rebuildGrid()。
    // 返回 false = 不适用（集合增减 / 搜索态 / 找不到对应盒子 / 网格页不是该分类）→ 调用方必须 rebuildGrid()。
    bool tryReorderInPlace(const QString& category);

    // 「只有少数分类的条目发生了增删」时的廉价出口（盒视图专用）。
    // 典型触发：从 Dock 拖一个图标进收纳盒 —— 原先走 rebuildGrid() → buildFences()，
    // 把**全部盒子**的**全部按钮**销毁重建（每个按钮还要重取一次图标位图）。
    // 返回 true = 已原地处理完毕（调用方**不要**再 rebuildGrid()）；false = 形态不适合，调用方照旧全量重建。
    bool applyItemsDeltaInPlace(const QMap<QString, QVector<DesktopItem>>& next);
    // 窗口化网格视图下的原地重排实现（box 视图由 FenceBox::reorderItemsInPlace 承担）
    bool reorderGridButtonsInPlace(const QVector<DesktopItem>& items);
    // 解冻（菜单收起 / 编辑结束 / 拖拽结束）后统一补做被推迟的网格工作：
    // **先试原地重排**（只动位置、不重建），不适用才退回全量重建。
    void flushPendingGridWork();

    // 分类页缓存（2026-09-22 新增。用户口径：分类切换只在**第一次**加载，之后切回不再刷新）
    // 为什么需要：网格只有一个容器 + 一个布局，原先每次切分类都走 applyGridItems() 全量
    // **销毁重建**按钮（几十~几百个 DesktopIconButton 的构造 + 取图标），切换时肉眼可见一次刷新。
    // 现在切换分类改走 switchCategoryPage()：把上一个分类的按钮**收进**隐藏宿主保活（不销毁、
    // 不丢信号连接），切回时直接装回布局 —— 已加载过的分类零重建。
    // 不变量：缓存表里只存**非当前分类**的页（其按钮 parent 恒为 m_pageCacheHost，不在布局中）；
    // 当前分类的按钮始终在 m_gridLayout 里。切走 = park 入表，切回 = adopt 出表。
    // 失效/命中判据 = **逐页内容签名 + 视图档位**（**不再**用"全局数据代次"）：
    // 页签名 = 该页显示条目的 sourcePath 序列（顺序敏感）。装回时与该分类当前的条目签名比对：
    // · 完全相同 → 复用（哪怕别处刚增删过分类/条目 —— 本页内容没变就不该重建）；
    // · 不同     → 丢弃重建（内容或顺序确实变了，绝不显示过期内容）。
    // 好处：新增/删除分类、往别的分类增删文件，都**只影响自己那一页**；原先的全局 invalidate 会让
    // "改一次分类 → 之后每个分类切回去都重刷一遍"。分类被删/改名后的残留页由 pruneCategoryPages() 收走。
    // （列表档行宽**不作判据**：滚动条出现/消失就会抖，据此丢弃等于切回来仍重建；
    // 改为装回后按当前度量重设按钮尺寸。）
    // 几何变化（窗口缩放 → 列数变、滚动条出现/消失 → 视口宽变）**不得**触发 rebuildGrid，
    // 统一走 relayoutGridForGeometry()：只重排现有按钮，连当前页都不重建，更不毁缓存。
    struct GridMetrics {
        int iconW = 72; int iconH = 84; int iconPx = 48;
        int spacing = 18; int cols = 1; int rows = 0; bool listMode = false;
    };
    struct CachedPage {
        int viewMode = 0;              // 构建时的视图档位（大/中/小/列表）
        int rowWidth = 0;              // 列表档的行宽（仅记录，便于诊断；**不**作为命中判据，见下）
        int scrollY = 0;               // 切走那一刻的竖直滚动位置（装回时还原，避免"回顶部"被当成重新加载）
        QStringList signature;         // 本页显示条目的内容签名（sourcePath 序列，**顺序敏感**）
        QVector<DesktopIconButton*> buttons;
    };
    GridMetrics prepareGridLayout(const QVector<DesktopItem>& items);  // 配置布局参数并算出度量
    void parkCurrentGridPage(const QString& category);                 // 当前分类按钮收进缓存宿主
    bool adoptCachedGridPage(const QString& category, const GridMetrics& m, int* scrollYOut = nullptr);  // 装回缓存页（命中即复用）
    void discardCurrentGridPage();          // 直接销毁当前页的按钮（搜索结果等不可缓存的场合）
    void invalidateCategoryPages();         // 显式全清（视图档位切换等"所有页都过期"的场合）
    void pruneCategoryPages();              // 只销毁"分类已不存在"的孤儿页（增删分类时用，不连累其它分类）
    void trimPageCache();                   // 超出上限时丢弃最久未用的页（防内存无限增长）
    // 分类切换的唯一实现（不走 rebuildGrid）：命中缓存则复用，否则首次构建。
    // parkOld=false 表示当前显示的不是某个分类的页（搜索结果），旧按钮直接销毁。
    void switchCategoryPage(const QString& from, const QString& to, bool parkOld);
    // 重建后仍“活着且已被布局”的按钮集合（用于选中接力）。与 allIconButtons() 的区别：
    // allIconButtons 用 findChildren 扫描容器，会把上一代刚 takeAt/deleteLater 的旧按钮也捞出来
    // （它们仍挂在父控件下，要等事件循环才销毁），选中一旦挂到旧按钮就是野指针。
    // 这里网格视图只扫 m_gridLayout（旧按钮已被 takeAt 移出布局），盒子视图只扫 m_fenceBoxes
    // （旧盒子在 buildFences 里已 setParent(nullptr) 并从 m_fenceBoxes 移除）。
    QVector<DesktopIconButton*> freshGridButtons() const;
    // 网格视图当前应使用的列数：列表模式（m_gridColumns==1）强制单列；
    // 其余按滚动区可用宽度自适应，图标固定 72×84，间隔为图标宽度的 1/4。
    int computeGridColumns() const;
    // 列表档的行宽：滚动区可视宽度 - 布局边距 - 预留滚动条宽度 - 4px 余量（下限 160）。
    // 预留滚动条宽度是必须的：行宽按“当前”视口宽度算，一旦内容超高出现竖向滚动条，
    // 视口变窄，铺满的行就会超出可视区而多出一条横向滚动条（列表视图里最刺眼的瑕疵）。
    int listRowWidth() const;
    // 全屏收纳模式下，把所有分类渲染为平铺在壁纸上的半透明盒子
    void buildFences();
    // 把滚动区内容从单分类网格切到盒子容器（幂等），由 buildFences 在尺寸就绪后调用，
    // 避免在构造期提前 setWidget() 重定父引发窗口树未稳定导致的启动崩溃
    void ensureFencesScrollWidget();
    void relayoutFences();   // 根据当前窗口宽度重排盒子的行列布局
    // “系统”分类（我的电脑/回收站/网络）不创建盒子，而是以无标题 Dock 直接浮在壁纸上
    void buildSystemDock();
    // 位置系统Dock
    void positionSystemDock();
    // 响应收纳盒条目投放
    void onFenceItemDropped(const QUuid& id, const QString& sourceCategory,
                            const QString& targetCategory, int targetIndex);
    // 响应收纳盒files投放
    void onFenceFilesDropped(const QStringList& paths, const QString& targetCategory);
    // 盒子几何（自由拖动/缩放结果）按分类持久化；收纳盒窗口不落盘
    static QString fenceGeomKey(const QString& category);
    // 加载收纳盒几何
    QRect loadFenceGeometry(const QString& category) const;
    // 保存收纳盒几何
    void saveFenceGeometry(const QString& category, const QRect& geom);
    // 移除收纳盒几何
    void removeFenceGeometry(const QString& category);
    // 双击盒子标题就地重命名提交后的处理（含几何键迁移）
    void onFenceRenameCommitted(const QString& oldName, const QString& newName);
    // 当前条目
    QVector<DesktopItem> currentItems() const;
    // sorted分类
    QStringList sortedCategories() const;
    // make视图按钮
    QToolButton* makeViewButton(const QString& text, const QString& tip);
    // 创建添加菜单
    QMenu* createAddMenu();
    // 响应分类标签clicked
    void onCategoryTabClicked();
    // 分类标签「切换方式」（设置中心 → 外观设置 → 分区标签切换）
    // 键 `Appearance/tagSwitchMode` 曾是**空设置**：设置中心一直在读写，产品里没有任何消费者，
    // 于是「悬停切换」选了完全没反应；「点击切换」则因为历史实现本就是左键点击，看着像生效。
    // 这里补齐消费端，并保证两种模式都按其字面含义工作：
    // 0 = 点击切换（默认）：**只有左键点击标签才切换**，鼠标划过绝不切换（行为与历史实现一致）
    // 1 = 悬停切换：鼠标移到标签上即切换（140ms 防抖，避免扫过整条标签栏连切多次）
    void loadTagSwitchMode();                    // 从本地持久化读取（构造期 + 每次显示窗口 + 收到变更广播时）
    void reloadTagSwitchMode();                  // 设置中心改完立即生效：重读并（必要时）就地下发，无需重开窗口
    void applyTagSwitchModeToTabs();             // 把当前模式应用到已建好的标签按钮
    void activateCategoryTab(QToolButton* btn);  // 统一的切换动作（点击 / 悬停共用，避免两处逻辑漂移）
    void syncCategoryTabChecked(QToolButton* target);  // 标签「选中态」同步：悬停切换没有点击，必须显式置位
    void startHoverSwitch(QToolButton* btn);     // 悬停切换防抖：鼠标停稳后才真正切换
    // 头部菜单按钮「始终显示 ↔ 悬停显示」（设置中心 → 外观设置 → 分区菜单标签显示）
    // 键 `Appearance/menuLabelShow` 原先同样是**空设置**（只有设置中心在读写在自娱自乐）。
    // 用户口径（2026-09-22）：它控制**收纳盒窗口头部那排菜单按钮**（锁/视图/添加/菜单/展开）
    // 以及桌面助手面板标题栏的同款按钮 —— 0=始终显示（常驻）；1=悬停显示（鼠标不在本窗内就隐藏）。
    void loadMenuRevealMode();                 // 从本地持久化读取（构造期 + 每次显示 + 收到变更广播）
    void reloadMenuRevealMode();               // 设置中心改完立即生效（窗口正开着也不必重开）
    void applyMenuRevealVisibility();          // **唯一**显隐出口：所有"该不该显示"的判断都走这里

    // 「在快捷方式图标上显示箭头」（Appearance/shortcutArrow，2026-09-22）：
    // 开关变化时就地重取本窗口全部图标（清 DesktopScanner 图标缓存 + 清各按钮 icon 后重新入队）。
    void reloadShortcutArrowMode();
    // 更新徽章标签
    void updateBadgeLabel();
    // 切换视图模式
    void toggleViewMode();
    // 更新视图切换按钮
    void updateViewToggleButton();
    // 切换锁定
    void toggleLock();
    // 更新锁定按钮
    void updateLockButton();
    // 切换网格
    void toggleGrid();

    // 图标网格内拖拽排序与跨分类移动
    bool isGridChild(QObject* obj) const;
    // 目标索引从投放位置
    int targetIndexFromDropPos(const QPoint& gridContainerPos) const;
    // 移动条目到分类
    void moveItemToCategory(const QUuid& id, const QString& sourceCategory,
                            const QString& targetCategory, int targetIndex = -1);
    // reorder条目in分类
    void reorderItemInCategory(const QString& category, const QUuid& id, int targetIndex);

    // 图标被拖出/拖入后，延后请求 MainWindow 重新扫描刷新
    void onItemFileMovedOut();
    // 图标被拖到所有收纳盒窗口之外（DesktopIconButton::dragDroppedOutside 的落点）。
    // 先交给 MainWindow 注册的解析器判定“是否真的离开了所有收纳盒窗口”：
    // 是 → 发 itemDraggedToDock（MainWindow 取消其分类归属，图标回到 Dock）；
    // 否 → 什么都不做（落在别的盒子/窗口上，交由既有的跨盒拖放链路处理）。
    void onIconDragDroppedOutside(const QString& sourcePath, const QString& displayName,
                                  const QPoint& globalPos);

    // —— 图标选中集合（Ctrl 加选 / Shift 范围选），与 Windows 桌面一致 ——
    void selectExclusive(DesktopIconButton* b);
    // 切换选中
    void toggleSelection(DesktopIconButton* b);
    // 范围选择
    void rangeSelect(DesktopIconButton* b);
    DesktopIconButton* currentSelectionTarget() const;   // F2 的目标：锚点，否则最后一个
    // —— 图标删除 / 重命名：落盘 + 本地持久化数据同步 ——
    void onIconSelectionRequested(DesktopIconButton* b, bool additive, bool range);
    // 响应图标选中for菜单
    void onIconSelectionForMenu(DesktopIconButton* b);
    // 响应图标删除请求信号
    void onIconDeleteRequested(DesktopIconButton* b);
    // 响应图标renamed
    void onIconRenamed(const QString& oldPath, const QString& newPath);
    // 删除图标
    void deleteIcons(const QList<DesktopIconButton*>& targets);
    void removePathsFromMemory(const QStringList& paths);          // m_allItems 去项
    void onContextMenuVisibleChanged(bool visible);                // 菜单期冻结重排/重建
    void onIconEditingChanged(bool editing);                       // 改名编辑期同样冻结重排/重建
    // migrate条目顺序路径
    static void migrateItemOrderPath(const QString& category,
                                     const QString& oldPath, const QString& newPath);
    // 在指定分类中按 id 找到图标的当前源路径（用于改分类记录）
    QString sourcePathOf(const QUuid& id, const QString& category) const;
    // 根据源/目标分类，把该文件在 CategoryStore 中的分类记录改为 targetCategory（A 方案，不搬文件）。
    // 仅主窗口（非收纳盒）使用；收纳盒仍走纯内存 moveItemToCategory。
    void handleStorageDrop(const QUuid& id, const QString& sourceCategory, const QString& targetCategory);
    // 保存条目顺序
    void saveItemOrder(const QString& category);
    // 应用条目顺序
    void applyItemOrder(QVector<DesktopItem>& items, const QString& category);
    // 移除条目顺序
    void removeItemOrder(const QString& category);
    // 条目顺序按键
    static QString itemOrderKey(const QString& category);

public:
    // 分类标签拖拽排序
    void startTabDrag(QToolButton* btn);
    // handle标签投放
    void handleTabDrop(const QString& category, const QPoint& dropPos);
    void updateInsertMarker(const QPoint& pos);  // 根据落点更新插入位置指示线
    // 隐藏插入指示线
    void hideInsertMarker();
    void updateGridInsertMarker(const QPoint& pos);  // 网格内拖拽时显示“将插入到此处”的指示线
    // 隐藏网格插入指示线
    void hideGridInsertMarker();
    // 加载分类顺序
    void loadCategoryOrder();
    // 保存分类顺序
    void saveCategoryOrder();
    void persistCategoryOrder() { saveCategoryOrder(); }  // 供外部（新建/重命名分类后）主动持久化
    void removeCategoryFromPersistence(const QString& name); // 解散分类时从顺序/重命名映射中移除并保存
    void saveCurrentCategory();   // 把当前选中分类写入本地持久化数据（仅主窗口；防抖，见 .cpp）
    // 立即把「当前选中分类」落盘（取消防抖定时器并同步写一次）。见 saveCurrentCategory 的说明：
    // 析构必须调用 —— 防抖把写入推迟了几百毫秒，若窗口在此期间析构（定时器随父对象一起销毁），
    // 这次切换就永远没落盘，用户表现为"分类切了，重启后又回到旧的"。
    void flushCurrentCategory();
    // 把持久化的重命名映射与自定义分类名应用回 items（扫描得到的原始分类集）。
    // 必须在 refreshDesktop 把扫描结果写入 MainWindow::m_allItems 之后、且调用 setItems 之前执行，
    // 否则会作用在网格窗口尚未填充的空 m_allItems 上，导致重命名/自定义分类在重启后失效。
    void applyPersistedCategories(QMap<QString, QVector<DesktopItem>>& items, const QSet<QString>& disbanded);

    // 非锁定状态下的边缘拖拽缩放
    Qt::Edges detectResizeEdges(const QPoint& pos) const;
    // 缩放光标shape
    Qt::CursorShape resizeCursorShape(Qt::Edges edges) const;
    // 开始缩放
    void startResize(const QPoint& globalPos, Qt::Edges edges);
    // do缩放
    void doResize(const QPoint& globalPos);

    // 悬停光标：在子控件上也能感知鼠标移动并切换边缘光标
    void updateHoverCursor(const QPoint& pos);
    // 启用child鼠标tracking
    void enableChildMouseTracking(QWidget* w);

    QMap<QString, QVector<DesktopItem>> m_allItems;
    QStringList m_categoryOrder;   // 分类显示顺序（重命名时原地替换，保持位置不变）
    QMap<QString, QString> m_categoryRenames;  // 扫描原始分类名 -> 用户自定义名（刷新桌面后用于还原）
    QString m_currentCategory;
    // —— 「当前选中分类」落盘防抖（2026-09-24）——
    // 为什么：saveCurrentCategory() 原先每次分类切换都 saveValue + sync()（同步刷 INI）。
    // 机械盘 + 安全软件抢盘下单次可达数十~数百 ms，且全程在 GUI 线程 —— 正是"来回切换分类变得发涩"
    // 的一条直接来源。它要存的只是"下次打开默认选中哪个分类"，晚几百毫秒写、甚至偶尔丢一次都无感。
    // 规则：值未变不写；连续切换合并为"停止切换 400ms 后写一次"；析构由 flushCurrentCategory() 兜底。
    QTimer* m_saveCategoryTimer = nullptr;   // 惰性创建，parent = this（随窗口销毁）
    QString m_savedCategory;                 // 上次已写入 / 已排队的值（去重依据）
    bool m_categorySavedOnce = false;         // 是否至少写过一次（setItems 要求该键始终存在）
    QString m_searchText;
    int m_gridColumns = 8;       // 视图模式：1=列表（单列），非1=网格（列数按宽度自适应）
    int m_actualGridCols = 8;    // 当前实际列数，resize 时用于判断是否需要重排
    int m_listRowWidth = 0;      // 列表档当前行宽，resize 时用于判断是否需要重排（列数恒为 1，靠列数判定不会触发）
    int m_viewMode = ViewMedium;      // “查看”档位：大/中等/小图标 + 列表（持久化 IconGridWindow/viewMode）
    int m_lastGridMode = ViewMedium;  // 列表模式下切回网格时恢复的图标尺寸档（顶部“视”按钮用）

    QWidget* m_headerBar = nullptr;
    QLabel* m_badgeLabel = nullptr;
    QWidget* m_categoryBar = nullptr;
    QWidget* m_tabBar = nullptr;
    QHBoxLayout* m_tabLayout = nullptr;
    QFrame* m_insertMarker = nullptr;   // 拖拽时的插入位置指示线（覆盖在 tabBar 之上）
    QFrame* m_gridInsertMarker = nullptr;  // 网格内拖拽时“将插入到此处”的竖直指示线（覆盖在 grid 之上）
    QScrollArea* m_gridScroll = nullptr;
    QWidget* m_gridContainer = nullptr;
    QGridLayout* m_gridLayout = nullptr;

    // 分类页缓存（与 GridMetrics/CachedPage 的说明一并看）：
    // m_pageCacheHost 是挂在**本窗口**下的隐藏宿主 —— 刻意不放在 m_gridContainer 里，
    // 这样缓存中的按钮既不会被网格布局接管，也不会被 findChildren(m_gridContainer) 扫到
    // （否则 allIconButtons() 会把非当前分类的旧按钮一起当成"在显示的图标"，F2/Delete 判定会串）。
    QHash<QString, CachedPage> m_categoryPages;
    QStringList m_pageOrder;             // 页的最近使用顺序（队尾最新），用于限额淘汰
    QWidget* m_pageCacheHost = nullptr;
    // 没有"全局数据代次"成员：缓存是否可用由**逐页内容签名**决定（见 CachedPage::signature），
    // 这样增删分类 / 在别的分类里增删文件都不会把无关分类的页一起作废。

    // 图标选中集合（替换“点哪个算哪个”的隐式状态）。锚点用于 Shift 范围选。
    QList<DesktopIconButton*> m_selection;
    DesktopIconButton* m_anchor = nullptr;
    // 窗口级“当前正在内联重命名”的影子指针（由 editingChanged 维护，QPointer 自动失效）。
    // 为什么需要它：原来的判定是扫描 m_gridContainer 的子控件，有两个致命盲区 ——
    // ① 全屏盒子视图下图标挂在 FenceBox 里（不是 m_gridContainer 的子控件）→ 永远扫不到
    // → 编辑期不被识别 → F2/Delete 的判断与重建冻结全部失灵；
    // ② 重建时旧按钮是 deleteLater 的“待删子控件”，仍会被扫到，容易误判。
    QPointer<DesktopIconButton> m_editingBtn;
    bool m_menuOpen = false;          // 右键菜单弹出期：期间不做任何重排/重建
    // 独立的「点空白取消选中」全局过滤器（挂在 qApp 上）。为什么不用本窗口 this：
    // 本窗口的 eventFilter 里塞了大量窗口内逻辑（MouseMove 的 mapTo / 标签拖拽 / 图标拖放），
    // 这些逻辑只对本窗口子件有意义；若让本窗口直接挂 qApp，它们会对全进程所有窗口的事件执行，
    // 在对象析构/生命周期不受控时解引用悬垂指针 → 启动阶段即崩溃（2026-09-24 定位）。
    // 独立过滤器只做「MouseButtonPress → 是否点空白 → clearSelection」这一件事，天然安全。
    PressOutsideCancelFilter* m_pressCancelFilter = nullptr;
    // 菜单期被拦下的重建请求。为什么必须拦：菜单是原生模态循环，期间的 deleteLater
    // 会被它自己的事件循环执行 —— 按钮会在 exec 还没返回时就被销毁，之后访问即崩溃。
    bool m_pendingRebuild = false;
    // 冻结期被拦下的「原地重排」请求（分类名；空 = 无请求）。与 m_pendingRebuild 并存：
    // 解冻后先试原地重排（只动位置、不重建），不适用才退回全量重建 —— 见 flushPendingGridWork()。
    QString m_pendingReorderCategory;
    bool m_iconDragInProgress = false;   // 图标拖拽进行中：冻结重建（理由见 setIconDragInProgress）

    // 全屏收纳（Fences）模式相关
    bool m_fencesMode = false;     // 全屏接管：透明窗铺满工作区 + 显示系统 Dock
    // A3：relayoutFences() 在容器几何为 0 时会用 QTimer::singleShot(0) 自重排，
    // 此前**没有任何次数上限** —— 若容器长期为 0（未 show / 尺寸未定 / 已被隐藏），
    // 就退化成"每轮事件循环再投一个 0ms 定时器"的活锁：GUI 线程持续空转、CPU 拉满，
    // 用户观感就是"程序卡"。此计数只限制【同一轮】几何未就绪时的重试次数；
    // 任何真实几何变化（resize/showEvent/模式切换）都会重新调用 relayoutFences()。
    int m_fencesRelayoutRetry = 0;
    bool m_boxView = false;        // 窗口化默认使用单分类网格视图；进入全屏收纳时自动切为 true
    QWidget* m_fencesContainer = nullptr;     // 承载所有盒子的容器（自由布局，不使用布局管理器）
    QVector<FenceBox*> m_fenceBoxes;         // 当前显示的盒子（buildFences 维护）
    QRect m_preFencesGeom;                   // 进入全屏前的浮动窗口几何
    QFrame* m_card = nullptr;                // 玻璃卡片容器（setupUi 中赋值）
    QGraphicsDropShadowEffect* m_cardShadow = nullptr;  // 卡片阴影（全屏时禁用）
    QToolButton* m_fencesMenuBtn = nullptr;  // 全屏模式下右上角菜单按钮
    QLineEdit* m_fencesSearchEdit = nullptr; // 全屏模式下右上角浮动搜索框（进入时自动聚焦）
    void positionFencesOverlay();            // 定位全屏模式下的浮动搜索框 + 菜单按钮
    QWidget* m_systemDock = nullptr;         // 全屏模式下“系统”图标的无标题 Dock（浮于壁纸右下角）

    QToolButton* m_viewToggleBtn = nullptr;
    QToolButton* m_lockBtn = nullptr;
    QToolButton* m_moreBtn = nullptr;
    QToolButton* m_closeBtn = nullptr;
    QToolButton* m_addBtn = nullptr;     // 添加按钮（跟随主题色）
    QToolButton* m_menuBtn = nullptr;    // 菜单按钮（跟随主题色）
    QWidget* m_titleBadge = nullptr;     // 当前模块徽章（硬编码青色描边，跟随主题色）

    bool m_forceShortcutCategory = true;
    bool m_isBoxWindow = false;
    QTimer* m_layerKeeper = nullptr;   // 独立收纳盒窗口：500ms 看护，仅被可见 band 窗压住才救

    // 跨窗口拖拽（收纳盒 ⇄ Dock）
    DockDropResolver m_dockDropResolver;          // 由 MainWindow 注册：判定“拖出到 Dock”是否成立
    bool m_externalDropHighlight = false;         // 外部拖动悬停在本窗口上：绘制“松手即收进此处”边框
    QFrame* m_dropHint = nullptr;                 // 上述高亮边框的实体现（覆盖全窗口的透明描边层，惰性创建）

    QPoint m_dragPos;
    bool m_dragging = false;
    bool m_gridCollapsed = false;
    bool m_locked = false;

    // 分类标签拖拽排序状态
    QToolButton* m_dragSourceTab = nullptr;
    QPoint m_dragStartPos;
    bool m_tabDragging = false;

    // 分类标签切换方式（设置中心「分区标签切换」）：0 = 点击切换（默认） 1 = 悬停切换
    int m_tagSwitchMode = 0;
    QTimer* m_hoverSwitchTimer = nullptr;      // 悬停切换防抖（仅 mode==1 时启动，惰性创建）
    QPointer<QToolButton> m_hoverPendingTab;   // 防抖期间待切换的标签（QPointer：标签会随重建销毁）

    // 头部菜单按钮显示方式（设置中心「分区菜单标签显示」）：0 = 始终显示（默认） 1 = 悬停显示
    int m_menuRevealMode = 0;
    bool m_pointerInside = false;              // 鼠标是否在本窗口内（Enter/Leave 维护，悬停显示模式的唯一依据）

    int m_expandedWidth = 960;
    int m_expandedHeight = 720;

    bool m_resizing = false;
    Qt::Edges m_resizeEdges;
    QRect m_resizeStartGeom;
    QPoint m_resizeStartPos;
    // C2：悬停光标的“上次目标”，用于变化检测。
    // 本窗口给**所有子控件**都装了 eventFilter（含每个图标与标签），MouseMove 是逐像素频率，
    // 而 QWidget::setCursor() 在同值时不会短路。取值：-1 = 尚未设置；-2 = 已 unset；
    // 其余为 Qt::CursorShape 的枚举值。
    int m_lastHoverCursorShape = -1;
};

#endif // ICONGRIDWINDOW_H
