/*
 * @file dockiconbutton.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef DOCKICONBUTTON_H
#define DOCKICONBUTTON_H

#include "desktopitem.h"
#include <QWidget>
#include <QIcon>
#include <QLineEdit>
#include <QElapsedTimer>
#include <QTimer>
#include <QPixmap>

// 全屏 Dock 镜像层里的一个桌面风格图标按钮。
// 自绘图标+标签（白字阴影、按下蓝选高亮），与 Windows 桌面观感一致；
// 双击用 Shell 启动；右键弹出原生上下文菜单（IContextMenu，含删除/重命名/属性等）；
// 拖动时把新位置回传给 DesktopMirrorWindow，由其重定位真实桌面图标。
class DockIconButton : public QWidget {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit DockIconButton(const DesktopItem& item, QWidget* parent = nullptr);
    ~DockIconButton() override;   // 清理顶层内联编辑框，避免按钮被重建时编辑框变孤儿窗

    const DesktopItem& item() const { return m_item; }
    // 设置图标
    void setIcon(const QIcon& icon) { m_icon = icon; update(); }
    void setCellSize(int cellW);          // 设置单元格（按钮）逻辑尺寸
    void setIconSize(int px);             // 设置图标绘制像素尺寸（与桌面一致）
    int iconSize() const { return m_iconSize; }
    // 设置selected
    void setSelected(bool sel);
    bool isSelected() const { return m_selected; }

    // 由 DesktopMirrorWindow 在需要弹出该图标原生右键菜单时调用
    void showContextMenu(const QPoint& globalPos) { showNativeContextMenu(globalPos); }

    // 在图标标签位置启动内联重命名（QLineEdit 叠加），输入完成后由 SetNameOf 落盘。
    // 仅对真实文件系统图标生效；特殊命名空间项（我的电脑/网络/回收站等）不启用。
    void startInlineRename();

    // 内联重命名状态/控制：供 Dock 在“点空白处 / 点其它图标 / 新建下一个”时提交当前编辑。
    bool isEditing() const { return m_editing; }
    // 提交重命名
    void commitRename() { commitInlineRename(); }
    // 取消重命名
    void cancelRename() { cancelInlineRename(); }
    // 编辑框在屏幕上的矩形（Qt 逻辑坐标）：判据用“屏幕坐标是否落在框内”，而不是事件接收对象的
    // 父子链。鼠标事件在 Qt 中会先派发给顶层 QWidgetWindow（其 parent 为空、不在编辑框的 parent
    // 链上），用对象判据会把“点进编辑框”误判成“点别处”→ 一按下就提交、框再也用不成。
    bool editorQtRectContains(const QPoint& globalPt) const {
        return m_editor && m_editor->isVisible() && m_editor->frameGeometry().contains(globalPt);
    }
    // 编辑框在屏幕上的矩形（Win32 物理像素）：系统消息钩子（WH_MOUSE_LL）用物理坐标，故单列一份。
    QRect editorNativeScreenRect() const;
    // 编辑框是否可见（钩子回调用；只读、无副作用）
    bool editorVisible() const { return m_editor && m_editor->isVisible(); }
    // 编辑框是否“刚弹出”（未超过提交宽限期）。鼠标侧的一切“点框外即提交”都要先问它：
    // 右键菜单选“重命名”时那一次点击的按下消息可能晚于编辑框显示才到达，若不设宽限，
    // 会把刚弹出的框立刻关掉 —— 表现即“右键重命名失效 / 框一闪而过”。
    bool editorJustOpened() const;
    // 输入法是否正在合成（拼音未上屏）。合成中按回车/Esc 属于输入法操作（上屏候选 / 取消合成），
    // 系统键盘钩子必须放行，不能当成“提交/取消重命名”。
    bool imeComposing() const { return m_imeComposing; }
    // 该项是否可重命名（真实文件系统项）。Dock 用它决定是否值得把新建项放进自动重命名队列。
    bool isRenameable() const {
        return !m_item.isSpecial && !m_item.shellPath.startsWith(QLatin1String("::{"));
    }

    // —— “拖到回收站即删除”相关 ——
    // 本项是否是回收站图标（拖放删除的落点）。
    bool isRecycleBin() const;
    // 本项是否允许被“拖到回收站”删除。必须比 isRenameable() 更严：重命名只改名字，
    // 删除会真的把文件送去回收站，任何一个误判都是不可逆的数据损失。
    // 拦截：系统命名空间项（此电脑/网络/回收站/控制面板，含个人文件夹的 CLSID 形式）、
    // 任何 "::{...}" 形式、目标不存在、盘根/无文件名、
    // 用户主目录及其所有上级目录（个人文件夹若被解析成真实路径就落在这里）。
    // 说明：Dock 的“拖到回收站”只对真实文件系统项生效，与 F2/Delete 快捷键保持同一套判据。
    bool isDeletableOnDrop() const;

    // 拖动悬停在回收站上时的高亮（提示“松手即删除”）。
    void setDropTarget(bool on);
    bool isDropTarget() const { return m_dropTarget; }

    // —— 可视命中区（局部坐标，与 paintEvent 绘制几何严格一致） ——
    // 单元格固定 72x84，而图标只有 48x48、标签更窄，二者之外还留着大片空白。
    // 若不区分“单元格矩形”与“图标/标签可视区”，用户在图标旁的空白处按下时事件会落在本按钮上
    // （Qt 命中测试只看控件矩形），被当成“点了这个图标”而选中/保持选中 ——
    // 观感就是“点 Dock 空白处清不掉选中状态”。
    // 因此：只有落在可视区上才算点图标；落在单元格内边距上的按下必须放行给父窗口（Dock）处理。
    QRect iconVisualRect() const;
    QRect labelVisualRect() const;
    bool hitsVisualArea(const QPoint& localPos) const;
    // 图标 + 标签的并集可视矩形（局部坐标）：画框多选（橡皮筋）的相交判定用它。
    // 必须与 hitsVisualArea() 同源，不能用单元格矩形：单元格 72x84 里图标只有 48x48，
    // 用单元格判定会出现“框还没碰到图标就把旁边那项选中了”的观感偏差。
    QRect hitsVisualRect() const;

protected:
    void enterEvent(QEvent* event) override;
    void leaveEvent(QEvent* event) override;

signals:
    // 拖动结束时请求重定位真实桌面图标：globalTopLeft 为图标在屏幕上的新左上角（逻辑坐标）。
    void requestReposition(const QString& shellPath, const QPoint& globalTopLeft);
    // 双击/打开后请求 Dock 取消所有选中
    void activated();
    // 内联重命名落盘成功后上报（oldName 用于 Dock 在 m_entries 中定位并更新该项）
    void renamed(const QString& oldName, const QString& newName);
    // 内联重命名开始/结束（带 self 便于 Dock 定位具体按钮）：
    // Dock 据此在编辑期间延后桌面刷新，避免重建按钮销毁正在编辑的项。
    void inlineEditStarted(DockIconButton* self);
    void inlineEditFinished(DockIconButton* self);
    // 鼠标按下时上报给 Dock 窗口，由窗口统一决策选中策略（多选/框选/右键菜单）。
    // buttons/mods 透传自 QMouseEvent，便于窗口区分左键、右键、Ctrl/Shift。
    void iconPressed(DockIconButton* self, const QPoint& globalPos,
                     Qt::MouseButtons buttons, Qt::KeyboardModifiers mods);
    // 拖动过程中（自身已跟手移动）上报，窗口据此同步同组其它选中图标位移。
    void dragMove(DockIconButton* self);
    // 拖动结束，上报自身最终全局左上角，窗口据此把整组（若多选）位置回写真实桌面。
    void dragEnd(DockIconButton* self, const QPoint& finalGlobalTopLeft);
    // 左键手势结束（含未位移的普通单击与双击），与 dragEnd 互补：dragEnd 只在真拖动时才发，
    // 此信号保证「按下即进入拖动态」之后，无论是否发生位移都能在松手时对称复位窗口的拖动态，
    // 避免拖动态标志/快照（裸指针）泄漏。
    void iconReleased();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void launch();
    void showNativeContextMenu(const QPoint& globalPos);
    // 内联重命名：提交（Enter/失焦）/取消（Esc）。cleanupEditor 负责销毁编辑框并恢复标签绘制。
    void commitInlineRename();
    void cancelInlineRename();
    void cleanupInlineEditor();
    // 落盘改名（COM + Shell SetNameOf）。与“收框”分离：收框先做（轻量、可在系统钩子回调上下文执行），
    // 落盘后可再通知 Dock；失败提示走队列消息框，绝不在钩子回调里弹模态窗口。
    void performRename(const QString& oldName, const QString& newName);
    // 编辑期看护：每 150ms 复查编辑框是否真的“在屏幕上最上层 + 真的是前台窗口”，
    // 任一条不成立立即再断言。Qt 的 isVisible()/hasFocus() 对这两件事都会说谎
    // （见 editorwatch.h）。这是“框出来了却看不见 / 打不进字”的自愈兜底。
    void editorWatchTick();

    DesktopItem m_item;
    QIcon m_icon;
    int m_cellW = 72;          // 单元格逻辑宽度：与收纳盒/网格视图统一为 72
    int m_iconSize = 48;       // 图标绘制像素尺寸：与收纳盒/网格视图统一为 48
    bool m_selected = false;
    bool m_hovered = false;      // 鼠标悬停状态（Windows 桌面风格淡蓝高亮）
    bool m_dropTarget = false;   // 拖动悬停在“回收站”上（松手即删除的提示态）
    // C1：回收站判定结果，构造期算一次。isRecycleBin() 会被 recycleBinButton()
    // 对【每个】Dock 按钮调用，而它内部是 2 次 QString 分配（trimmed().toLower()）；
    // 拖动期间该函数每像素被调用一次，属于纯重复劳动。条目在按钮生命周期内不会变。
    bool m_isRecycleBinItem = false;

    // D2：静态层（图标 + 标签）缓存。
    // paintEvent 里“向 QIcon 取位图 + 标签字体度量与二分截断”在 hover 等高频重绘中纯属重复劳动，
    // 而这些内容的输入（尺寸 / 文本 / 选中态 / 编辑态 / 图标 / 字体）是稳定的 —— 故预渲染进
    // 一张 QPixmap；hover 底、选中底、回收站提示环、拖动半透明等高频变化的状态仍每次叠加。
    QPixmap m_staticLayer;
    int     m_staticKeyW = 0;          // 设备像素宽
    int     m_staticKeyH = 0;          // 设备像素高
    qint64  m_staticKeyIcon = 0;       // QIcon::cacheKey()
    QString m_staticKeyText;           // 显示名
    QString m_staticKeyFont;           // QFont::key()
    bool    m_staticKeySelected = false;
    bool    m_staticKeyEditing = false;
    bool    m_staticKeyValid = false;

    QPoint m_pressGlobal;      // 按下时的全局坐标
    QPoint m_grabOffset;       // 光标相对按钮左上角的偏移（用于拖动跟手）
    bool m_dragging = false;   // 是否已越过拖动阈值
    bool m_pressedLeft = false;

    // 内联重命名状态
    QLineEdit* m_editor = nullptr;   // 标签位置叠加的编辑框（nullptr 表示非编辑态）
    bool m_editing = false;          // 是否处于内联重命名中（paintEvent 据此跳过标签绘制）
    bool m_renameCancelled = false;  // Esc 取消标记，editingFinished 据此走取消分支
    // 编辑框 show() 的时刻：新建项弹框时进程并非前台，Windows 会先派发一轮激活/失焦抖动，
    // 若把这轮抖动当成“用户点别处”就会立刻提交 → 编辑框一闪而过。用开始时间做短时宽限。
    QElapsedTimer m_editorShownTick;
    bool m_editorFocused = false;    // 编辑框是否已真正拿到键盘焦点（拿到后停止补抢，避免与用户输入抢焦点）
    bool m_imeComposing = false;     // 输入法是否正在合成（据此对回车/Esc 放行，见 imeComposing()）
    // 编辑期间挂靠的前台线程 id（0 = 未挂靠）。Dock 是 WS_EX_NOACTIVATE，不挂靠则 SetFocus 无效、
    // 打字进不了编辑框；挂靠保持整个编辑期，收框时对称解除。存为整数以免头文件依赖 windows.h。
    quintptr m_attachedTid = 0;
    // 编辑期看护定时器（懒创建，parent=this）
    QTimer* m_editWatchTimer = nullptr;
    int m_editWatchCount = 0;
};

#endif // DOCKICONBUTTON_H
