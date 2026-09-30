/*
 * @file fencebox.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef FENCEBOX_H
#define FENCEBOX_H

#include <QWidget>
#include <QVector>
#include <QRect>
#include "desktopitem.h"

class DesktopIconButton;   // 盒内图标按钮（前向声明足够：信号只用到指针）

QT_BEGIN_NAMESPACE
class QGridLayout;
class QLabel;
class QLineEdit;
class QFrame;
class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;
class QMouseEvent;
class QResizeEvent;
QT_END_NAMESPACE

// 全屏收纳（Fences）模式下的单个分类盒子：标题栏 + 内部图标网格。
// 自身处理拖入事件，跨盒拖拽即“改分类”（仅改记录，不搬文件）。
// 盒子可由用户拖标题栏移动、拖边框缩放（真·Fences 体验），几何由窗口按分类持久化。
class FenceBox : public QWidget {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit FenceBox(const QString& category, QWidget* parent = nullptr);

    // 设置条目
    void setItems(const QVector<DesktopItem>& items);

    // 顺序变了、但**条目集合完全不变**时的廉价重排：按新顺序把**现有按钮**放回布局。
    // 按钮对象、图标位图、信号连接一律原样复用（不 deleteLater / 不 new / 不重取图标）——
    // 「盒内交换两个图标的位置」应有的代价就只是几何变化，不该整盒重刷一遍。
    // 返回 false = 集合对不上（增 / 删 / 改名 / 跨盒移动）→ 调用方必须退回 setItems() 全量重建。
    bool reorderItemsInPlace(const QVector<DesktopItem>& items);

    // 增量应用：**复用**集合里已存在的按钮，只为新增项新建按钮、把移除项的按钮销毁，再按新顺序重排。
    // 这是「从 Dock 拖一个图标进收纳盒」该有的代价：目标盒只多一个按钮，其余按钮（含图标位图）
    // 原地保留，别的盒子更是完全不动。返回 false = 差异过大或数据异常 → 调用方退回 setItems() 全量重建。
    bool applyItemsDelta(const QVector<DesktopItem>& items);

    QString category() const { return m_category; }

    // 按当前图标数量推荐的“自然尺寸”，供窗口首次自动排布使用
    QSize naturalSize() const;
    // 盒子允许的最小尺寸（一列图标 + 标题栏）
    static QSize minimumBoxSize();
    static int titleBarHeight() { return 28; }
    // 进入就地重命名编辑态（双击标题触发，也可由外部调用）
    void beginInlineRename();

    // —— 外部拖拽悬停（Dock 图标 / 资源管理器文件 → 盒内）——
    // 为什么需要单独一套：外部拖拽的鼠标事件被**发起方窗口**（Dock）持有，本盒收不到
    // dragEnter/dragMove/drop，只能由 IconGridWindow 按全局坐标把“光标在哪”转发进来。
    // 命中盒内图标网格时显示“将插入到此处”的竖直指示线并返回 true；
    // *outIndex = 插入下标（-1 = 落点不在网格上，调用方按“追加末尾”处理）。
    bool showExternalDropMarker(const QPoint& globalPos, int* outIndex);
    // 只查询插入下标（松手时用），-1 = 落点不在本盒的图标网格上；不改变指示线显示
    int externalDropIndexAt(const QPoint& globalPos) const;
    // 收起外部拖拽指示线（悬停离开 / 拖拽结束）
    void hideExternalDropMarker();

signals:
    // 跨盒/盒内拖拽重排：id + 源分类 + 目标分类（即本盒分类）+ 目标插入索引
    void itemDropped(const QUuid& id, const QString& sourceCategory,
                     const QString& targetCategory, int targetIndex);
    // 从资源管理器/桌面拖入的文件，收纳进本盒分类
    void filesDropped(const QStringList& paths, const QString& targetCategory);
    // 图标源文件已离开原路径（被外部搬走/删除），请求刷新
    void fileMovedOut();
    // 用户拖动/缩放盒子结束（鼠标释放）后发出，供窗口持久化盒子几何
    void geometryEdited(const QString& category, const QRect& geom);
    // 双击标题就地重命名并提交
    void renameCommitted(const QString& oldName, const QString& newName);

    // —— 盒内图标的操作语义（原样上抛给 IconGridWindow）——
    // FenceBox 只是布局容器；选中集合 / 快捷键上下文 / 编辑态 / 改名同步必须由窗口统一维护，
    // 否则全屏盒子视图下 F2·Delete 因“没有选中项”永远不触发，且改名不会同步到本地持久化数据。
    void iconSelectionRequested(DesktopIconButton* btn, bool additive, bool range);
    // 图标选中for菜单
    void iconSelectionForMenu(DesktopIconButton* btn);
    // 图标删除请求信号
    void iconDeleteRequested(DesktopIconButton* btn);
    // 图标上下文菜单可见变化信号
    void iconContextMenuVisibleChanged(bool visible);
    // 图标editing变化信号
    void iconEditingChanged(DesktopIconButton* btn, bool editing);
    // 图标重命名committed
    void iconRenameCommitted(const QString& oldPath, const QString& newPath);
    // 盒内图标被拖到所有收纳盒窗口之外（未被子窗口/其它盒/外部程序接住）：上抛窗口做“放回 Dock”。
    void iconDragDroppedOutside(const QString& sourcePath, const QString& displayName, const QPoint& globalPos);

protected:
    // 拖拽进入事件
    void dragEnterEvent(QDragEnterEvent* event) override;
    // 拖拽移动事件
    void dragMoveEvent(QDragMoveEvent* event) override;
    // 拖拽离开事件
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    // 投放事件
    void dropEvent(QDropEvent* event) override;
    // 鼠标按下事件
    void mousePressEvent(QMouseEvent* event) override;
    // 鼠标移动事件
    void mouseMoveEvent(QMouseEvent* event) override;
    // 鼠标松开事件
    void mouseReleaseEvent(QMouseEvent* event) override;
    // 鼠标双击click事件
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    // 离开事件
    void leaveEvent(QEvent* event) override;
    // 缩放事件
    void resizeEvent(QResizeEvent* event) override;
    // 事件过滤
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // 重建
    void rebuild();
    // 构造一个盒内图标按钮并接好全部信号 —— rebuild() 与 applyItemsDelta() **共用**这一段，
    // 避免两条路径各写一份接线而漏掉其中一个（漏接 = F2/Delete/改名在该盒内静默失效）。
    DesktopIconButton* createItemButton(const DesktopItem& item);
    void relayoutIcons(bool force);          // 按当前盒宽重算列数并重排已有图标（不重建控件）
    // 列for宽度
    int columnsForWidth() const;
    // 更新title文本
    void updateTitleText();
    void layoutOverlays();                   // 标题右侧计数标签 / 空盒提示 / 重命名输入框定位
    void applyInnerCompactPolicy(int rows, int cols); // 固定已用单元格大小，剩余空间推到右下
    // 提交inline重命名
    void commitInlineRename();
    // 取消inline重命名
    void cancelInlineRename();
    // 目标索引从位置
    int targetIndexFromPos(const QPoint& containerPos) const;
    void updateInsertMarker(int targetIndex);   // 拖拽悬停时显示“将插入到此处”的竖直指示线
    // 隐藏插入指示线
    void hideInsertMarker();
    // edges在
    Qt::Edges edgesAt(const QPoint& pos) const;
    // 光标foredges
    static Qt::CursorShape cursorForEdges(Qt::Edges edges);

    QString m_category;
    QVector<DesktopItem> m_items;
    QLabel* m_title = nullptr;
    QLabel* m_countLabel = nullptr;      // 标题右侧的图标数量
    QLineEdit* m_titleEdit = nullptr;    // 双击标题进入的就地重命名输入框
    QLabel* m_emptyHint = nullptr;       // 空盒子占位提示
    QWidget* m_inner = nullptr;
    QGridLayout* m_innerLayout = nullptr;
    QFrame* m_insertMarker = nullptr;   // 盒内拖拽插入位置指示线（覆盖在图标之上，鼠标穿透）
    int m_innerCols = 5;                // 当前每行图标数（随盒宽自适应）

    // 拖动 / 缩放状态
    bool m_moving = false;
    bool m_boxResizing = false;
    bool m_renaming = false;
    Qt::Edges m_resizeEdges;
    QRect m_startGeom;
    QPoint m_startGlobal;
    // C3：悬停光标的“上次目标”编码，用于变化检测（mouseMoveEvent 是逐像素到达的，
    // 而 QWidget::setCursor() 在同值时不会短路）。-1 = 尚未设置过（保证首帧一定写一次）。
    // 取值约定：0 = 默认光标；1+N = 边缘缩放光标（N = Qt::Edges 的位值）；1000 = 标题栏移动光标。
    int m_lastHoverCursorKey = -1;
};

#endif // FENCEBOX_H
