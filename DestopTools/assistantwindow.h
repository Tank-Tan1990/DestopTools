/*
 * @file assistantwindow.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef ASSISTANTWINDOW_H
#define ASSISTANTWINDOW_H

#include <QWidget>

QT_BEGIN_NAMESPACE
class SidePanelWidget;
class QMouseEvent;
class QCloseEvent;
class QShowEvent;
QT_END_NAMESPACE

// 右侧无边框窗口：桌面助手面板
class AssistantWindow : public QWidget {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit AssistantWindow(QWidget* parent = nullptr);
    // 面板
    SidePanelWidget* panel() const;
    // 展开尺寸
    QSize expandedSize() const;
    // 设置展开尺寸
    void setExpandedSize(const QSize& size);
    // 设置折叠/展开状态。save 为 true 时表示由用户交互触发，会持久化并通知主窗口；
    // 为 false 时仅更新窗口尺寸，供启动恢复阶段使用，避免覆盖已持久化的展开尺寸。
    void setPanelCollapsed(bool collapsed, bool save = false);

signals:
    // 透传 SidePanelWidget 的信号
    void toolTriggered(const QString& name);
    // 搜索文本变化信号
    void searchTextChanged(const QString& text);
    // 菜单请求信号
    void menuRequested();
    // 窗口几何（位置/大小）发生用户操作后改变时发出，供 MainWindow 持久化
    void windowGeometryChanged();

protected:
    // 鼠标按下事件
    void mousePressEvent(QMouseEvent* event) override;
    // 鼠标移动事件
    void mouseMoveEvent(QMouseEvent* event) override;
    // 鼠标松开事件
    void mouseReleaseEvent(QMouseEvent* event) override;
    // 关闭事件
    void closeEvent(QCloseEvent* event) override;
    // 显示事件
    void showEvent(QShowEvent* event) override;
    // 常态拦截提层（DesktopMirrorWindow::clampBandZOrder）：杜绝点击/Qt show 把
    // 助手面板顶到普通程序之上再被压回的「一闪而过」。show desktop 期放行。
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;

private slots:
    // 响应面板折叠
    void onPanelCollapsed(bool collapsed);

private:
    SidePanelWidget* m_panel = nullptr;
    QPoint m_dragPos;
    bool m_dragging = false;
    bool m_collapsed = false;
    int m_expandedWidth = 320;
    int m_expandedHeight = 720;
};

#endif // ASSISTANTWINDOW_H
