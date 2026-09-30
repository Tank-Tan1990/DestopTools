/*
 * @file desktopiconbutton.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef DESKTOPICONBUTTON_H
#define DESKTOPICONBUTTON_H

#include "desktopitem.h"
#include <QToolButton>
#include <QVector>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>

QT_BEGIN_NAMESPACE
class QLineEdit;
QT_END_NAMESPACE

// 收纳盒 / 整理窗口网格里的单个图标按钮。
// 与 Dock 图标（DockIconButton）保持同一套操作语义：左键选中、F2 重命名、Delete/D 删除、
// 右键菜单（打开/重命名/删除）。区别只在“谁能拿到键盘”：
// Dock 与收纳盒窗口都带 WS_EX_NOACTIVATE，二者**都拿不到键盘焦点**，
// 所以快捷键同样下沉到系统级 WH_KEYBOARD_LL（由 IconGridWindow 统一持有），
// 而重命名输入框做成“无父顶层窗口”，靠线程输入挂靠抢前台 —— 与 Dock 完全同源。
class DesktopIconButton : public QToolButton {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit DesktopIconButton(const DesktopItem& item, QWidget* parent = nullptr);
    ~DesktopIconButton() override;

    const DesktopItem& item() const { return m_item; }
    DesktopItem& item() { return m_item; }

    // 快捷方式箭头缩放因子：收纳盒（默认 + 新建）需要把角标放大到 2 倍，其余（FenceBox 等）保持 1.0。
    // 默认 1.0，由 IconGridWindow 在创建按钮后显式设为 2.0。
    void setShortcutArrowScale(double scale) { m_arrowScale = scale; }
    double shortcutArrowScale() const { return m_arrowScale; }

    // 把当前按钮加入“异步图标加载”队列（构造时调用）。真实图标由定时器在主线程分批取出，
    // 避免一次性为大量图标同步调用 Shell（QFileIconProvider::icon）导致界面卡死。
    static void queueIconLoad(DesktopIconButton* btn);

    // —— 选中态（Windows 桌面风格淡蓝高亮；由 IconGridWindow 统一管理） ——
    bool isSelected() const { return m_selected; }
    // 设置selected
    void setSelected(bool sel);

    // —— 视图尺寸（右键“查看”→ 大图标 / 中等图标 / 小图标） ——
    // 由 IconGridWindow 按当前视图模式设置：单元格尺寸 + 图标绘制像素，并立即按新尺寸重取图标
    // （进程内图标缓存按 parsename|size 区分，所以换尺寸会拿到对应分辨率、不会放大发虚）。
    // FenceBox（全屏收纳盒子）不调用本函数，保持构造默认的 72×84 / 48。
    void setViewMetrics(int cellW, int cellH, int iconPx);

    // —— 列表档（右键“查看”→ 列表） ——
    // 行式布局：图标在左、文字在右（左对齐 + 中间省略），与 Windows 资源管理器“列表”视图同款观感。
    // 默认的 QToolButton 是“图标在上、文字在下且整体居中”，QSS 又无法改内容对齐，故列表档整块自绘
    // （见 paintListMode）。单元格尺寸仍由 setViewMetrics 给出：cellW=行宽、cellH=行高、iconPx=小图标尺寸。
    void setListMode(bool on);
    bool isListMode() const { return m_listMode; }

    // —— 内联重命名 ——
    bool isEditing() const { return m_editing; }
    bool canRename() const;          // 系统命名空间项不可重命名
    bool canDelete() const;          // 受保护项（系统项/个人文件夹/桌面目录本身…）不可删除
    void startInlineRename();        // F2 / 右键“重命名”入口
    // 提交inline重命名
    void commitInlineRename();
    // 取消inline重命名
    void cancelInlineRename();
    // 编辑框是否刚弹出（宽限期）：编辑期的系统级鼠标钩子据此忽略“打开菜单的那一次手势”，
    // 否则右键菜单选「重命名」时，弹出瞬间到来的那一击会立刻把编辑框提交掉。
    bool editorJustOpened() const;
    // 输入法合成中：合成期的 Enter/Esc 属于「确认/取消候选词」，系统钩子一律不得拦截。
    bool isImeComposing() const { return m_imeComposing; }
    // 编辑期系统级鼠标钩子的落点：点编辑框以外即提交（与 Windows 桌面语义一致）。
    void handleEditClickOutside(const QPoint& physScreenPt);

signals:
    // 条目moved
    void itemMoved(const QUuid& id, const QString& newCategory);
    // 拖拽结束后源文件已不在原路径（被移入隐藏存储或移回桌面/被 Explorer 拖走）时发出，
    // 供网格窗口触发一次完整刷新，使桌面视图与收纳视图重新与磁盘一致。
    void fileMovedOut();
    // 左键按下：请求窗口更新选中集合（Ctrl 切换 / Shift 范围 / 普通单选）。
    // additive=true 表示按住 Ctrl（切换），range=true 表示按住 Shift（范围选）。
    void selectionRequested(DesktopIconButton* self, bool additive, bool range);
    // 右键按下：请求窗口确保本项被选中（Windows 语义：右键未选中项会先选中它）。
    void selectionRequestedForMenu(DesktopIconButton* self);
    // 重命名落盘成功：旧路径 → 新路径。窗口据此迁移分类记录/顺序持久化并刷新。
    void renameCommitted(const QString& oldPath, const QString& newPath);
    // 右键菜单选了“删除”：窗口负责落盘 + 清理本地持久化数据。
    void deleteRequested(DesktopIconButton* self);
    // 右键菜单弹出(true)/收起(false)。菜单是**原生模态循环**，期间的 deleteLater 会被
    // 它自己的事件循环执行（按钮在 exec 尚未返回时就被销毁 → 之后访问即崩溃）；
    // 因此窗口必须在这段时间里冻结一切重建/重排（见 IconGridWindow::m_menuOpen）。
    void contextMenuVisibleChanged(bool visible);
    // 内联编辑开始(true)/结束(false)。窗口据此在“有图标正在改名”期间冻结重建 ——
    // 否则一次延后到达的重建会把正在编辑的按钮 deleteLater 掉，按钮析构时顺手销毁编辑框，
    // 用户看到的就是“F2 / 右键重命名后输入框一闪就没了”（被当成点了没反应）。
    void editingChanged(bool editing);
    // 图标被拖拽后**没有被任何拖放接收方接住**（drag->exec() 返回 IgnoreAction）且源文件仍在原处：
    // 典型就是“从收纳盒把图标拖到收纳盒窗口之外（Dock / 桌面区域）”。
    // 之所以要单独发这个信号：Qt 的 QDrag 只能告诉我们“没人接”，无法告诉我们“落在屏幕哪一点”
    // 属于哪个容器；窗口/主窗口据此用坐标判定是否真的拖出了所有收纳盒窗口，进而执行
    // “放回 Dock”（取消分类）。轻微抖动（仍在原窗口内）不会移出，避免误触。
    void dragDroppedOutside(const QString& sourcePath, const QString& displayName, const QPoint& globalPos);

protected:
    // paint事件
    void paintEvent(QPaintEvent* event) override;
    // 事件过滤
    bool eventFilter(QObject* watched, QEvent* event) override;
    // 鼠标按下事件
    void mousePressEvent(QMouseEvent* event) override;
    // 鼠标移动事件
    void mouseMoveEvent(QMouseEvent* event) override;
    // 鼠标双击click事件
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    // 鼠标松开事件
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    // 显示上下文菜单
    void showContextMenu(const QPoint& globalPos);
    // 打开条目
    void openItem();
    void paintListMode();                     // 列表档：左图标 + 右文字（整块自绘）
    void loadIconNow();                       // 实际取图标并 setIcon（仅主线程）
    void cleanupEditor();                     // 收掉编辑框（先 hide 再 deleteLater）
    void forceFocusStaggered();               // 错峰补抢编辑框键盘焦点（拿到即停）
    void editorWatchTick();                   // 编辑期看护：真可见性 + 真焦点，发现问题立即再断言
    void requestDelete();                     // 右侧菜单选“删除”后的 0ms 投递入口
    static void processIconQueue();           // 定时器回调：每拍处理一小批，保持 UI 响应

    DesktopItem m_item;
    QPoint m_dragStartPos;
    // 「单击选中」改为在**释放时**触发（见 mousePressEvent / mouseReleaseEvent）：
    // 把「点选」与「拖拽移动」分开 —— 否则任何一次拖拽（哪怕没交换位置）都会因「按下即选中」
    // 让图标进入选中态，拖拽结束后该选中态残留，表现为「拖动一下图标就高亮、多个图标都高亮、
    // 点空白还取消不掉」。按下时只记状态，确认没拖拽才在释放时上抛 selectionRequested。
    Qt::KeyboardModifiers m_pressMods = Qt::NoModifier;  // 按下时的修饰键（Ctrl/Shift，释放时沿用）
    bool m_pressValid = false;   // 左键按下待选中；一旦启动拖拽即置 false
    bool m_dragging = false;     // 已启动拖拽（释放时据此抑制「单击选中」）
    bool m_iconQueued = false;
    int m_iconPx = 48;            // 当前按多少像素取/绘图标（随“查看”视图模式变化）
    double m_arrowScale = 1.0;     // 快捷方式箭头缩放因子（收纳盒=2.0，其余=1.0）
    bool m_listMode = false;      // 列表档：图标在左、文字在右（paintListMode 自绘）

    bool m_selected = false;
    bool m_editing = false;
    bool m_renameCancelled = false;
    bool m_imeComposing = false;              // 输入法合成中：Esc 属于“取消候选”，不能当成取消重命名
    QLineEdit* m_editor = nullptr;            // 无父顶层窗口（收纳盒窗口拿不到焦点，子控件更不可能）
    QElapsedTimer m_editorShownTick;          // 弹出时刻：用于“失焦宽限期”，消掉首轮激活抖动
    quintptr m_attachedTid = 0;               // 编辑期挂靠的前台线程 id（0 = 未挂靠）
    bool m_editorFocused = false;             // 已真正拿到键盘焦点（错峰补抢据此停止，不与用户抢输入）
    // 编辑期看护定时器（懒创建，parent=this）：每 150ms 检查编辑框是否真的“在屏幕上最上层 + 真前台”。
    // 必要性见 editorwatch.h 文件头：Qt 的 isVisible()/hasFocus() 都无法反映「被压住」与「假焦点」。
    QTimer* m_editWatchTimer = nullptr;
    int m_editWatchCount = 0;

    // 跨实例共享的异步图标加载队列与定时器（懒创建，随 QApplication 生命周期存在）
    static QVector<QPointer<DesktopIconButton>> s_iconQueue;
    static QTimer* s_iconTimer;
};

#endif // DESKTOPICONBUTTON_H
