/*
 * @file updatedialog.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef UPDATEDIALOG_H
#define UPDATEDIALOG_H

#include <QDialog>

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QFrame;
QT_END_NAMESPACE

/**
 * @brief 桌面助手升级提示对话框
 *
 * 模拟 360 桌面助手检查更新弹框：
 * - 无更新时右下角按钮显示「知道了」
 * - 有更新时右下角按钮显示「更新」
 */
class UpdateDialog : public QDialog {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit UpdateDialog(bool hasUpdate, const QString& newVersion = QString(), QWidget* parent = nullptr);

protected:
    // 原生事件
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;
    // 鼠标按下事件
    void mousePressEvent(QMouseEvent* event) override;
    // 鼠标移动事件
    void mouseMoveEvent(QMouseEvent* event) override;
    // 鼠标松开事件
    void mouseReleaseEvent(QMouseEvent* event) override;

private slots:
    // 响应更新clicked
    void onUpdateClicked();

private:
    // 初始化ui
    void setupUi();
    // paintinfo图标
    void paintInfoIcon();
    // 把弹框重新压到全部 band 窗口（收纳盒 / 桌面助手 / Dock 图标区）之上、正常程序之下，
    // 并清掉 Qt 可能替我们指定的 owner（否则会随 owner 的周期性压底一起沉到桌面图标层之下）。
    // 与 GlassInputDialog（重命名弹框）/ GlassMessageBox 同源，幂等，可反复调用。
    void assertLayerAboveBandWindows();

    bool m_hasUpdate = false;
    QString m_newVersion;

    QLabel* m_iconLabel = nullptr;
    QLabel* m_titleLabel = nullptr;
    QLabel* m_msgLabel = nullptr;
    QPushButton* m_actionBtn = nullptr;
    QFrame* m_titleBar = nullptr;

    bool m_dragging = false;
    QPoint m_dragPos;
};

#endif // UPDATEDIALOG_H
