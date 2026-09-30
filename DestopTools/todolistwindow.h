/*
 * @file todolistwindow.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef TODOLISTWINDOW_H
#define TODOLISTWINDOW_H

#include <QDialog>
#include <QPoint>
#include <QString>

QT_BEGIN_NAMESPACE
class QMouseEvent;
class QShowEvent;
class QHideEvent;
class QLabel;
class QFrame;
class QToolButton;
class QScrollArea;
class QWidget;
class QVBoxLayout;
class QTimer;
QT_END_NAMESPACE

// 「事项清单」独立窗（助手面板待办栏的展开入口）：
// · 标题栏：事项清单 - 未完成/已完成 (N) + 视图切换 + 关闭 —— 可拖动；
// · 中部：条目列表（圆点点击 标记完成/恢复；右键菜单同面板条目）；
// · 底部：「添加待办事项」；已完成视图右下角附「清空已完成」。
// 窗口基础设施与 WallpaperWindow 完全同源（三件套 + 500ms 看护 + 标题栏拖动）。
class TodoListWindow : public QDialog {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit TodoListWindow(QWidget* parent = nullptr);
    ~TodoListWindow() = default;

    void showWindow();          // 打开（默认停在未完成视图）

protected:
    // 显示事件
    void showEvent(QShowEvent* event) override;
    // 隐藏事件
    void hideEvent(QHideEvent* event) override;
    // 原生事件
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;
    // 鼠标按下事件
    void mousePressEvent(QMouseEvent* event) override;
    // 鼠标移动事件
    void mouseMoveEvent(QMouseEvent* event) override;
    // 鼠标松开事件
    void mouseReleaseEvent(QMouseEvent* event) override;
    // 事件过滤
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // 初始化ui
    void setupUi();
    void refresh();              // 按当前视图重建条目列表 + 标题计数
    QWidget* buildRow(const QString& id);   // 构建单条条目行
    // 设置视图
    void setView(bool showDone);
    void refreshTheme();         // 主题色/透明度联动
    void addViaDialog();         // 底部「添加待办事项」

    // 判断拖拽area在
    bool isDragAreaAt(const QPoint& pos) const;
    // assertlayerabove条带windows
    void assertLayerAboveBandWindows();
    // schedulelayerasserts
    void scheduleLayerAsserts();

    QFrame* m_card = nullptr;
    QFrame* m_titleBar = nullptr;
    QLabel* m_caption = nullptr;
    QToolButton* m_undoneBtn = nullptr;
    QToolButton* m_doneBtn = nullptr;
    QScrollArea* m_listScroll = nullptr;
    QWidget* m_listHost = nullptr;
    QVBoxLayout* m_listLayout = nullptr;
    QFrame* m_bottomBar = nullptr;
    QWidget* m_addLabel = nullptr;    // 底部「⊕ 添加待办事项」（QPushButton 扁平化）
    QToolButton* m_clearDoneBtn = nullptr;   // 已完成视图右下角「清空已完成」

    bool m_showDone = false;     // false=未完成视图 / true=已完成视图
    QTimer* m_layerKeeper = nullptr;
    bool m_dragging = false;
    QPoint m_dragPos;
};

#endif // TODOLISTWINDOW_H
