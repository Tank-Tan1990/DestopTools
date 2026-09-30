/*
 * @file feedbackdialog.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef FEEDBACKDIALOG_H
#define FEEDBACKDIALOG_H

#include <QDialog>

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QFrame;
class QStackedWidget;
class QTextEdit;
class QComboBox;
class QLineEdit;
class QCheckBox;
QT_END_NAMESPACE

/**
 * @brief 帮助与反馈对话框
 *
 * 包含三个标签页：常见问题、我要反馈、反馈记录。
 * 当前实现「我要反馈」页的输入表单，其余为占位页。
 */
class FeedbackDialog : public QDialog {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit FeedbackDialog(QWidget* parent = nullptr);

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
    // 响应标签clicked
    void onTabClicked(int index);
    // 响应submit
    void onSubmit();

private:
    // 初始化ui
    void setupUi();
    // 创建feedback页
    QWidget* createFeedbackPage();
    // 创建placeholder页
    QWidget* createPlaceholderPage(const QString& title);
    // 设置标签style
    void setTabStyle(int activeIndex);
    // 把弹框重新压到全部 band 窗口（收纳盒 / 桌面助手 / Dock 图标区）之上、正常程序之下，
    // 并清掉 Qt 可能替我们指定的 owner（否则会随 owner 的周期性压底一起沉到桌面图标层之下）。
    // 与 GlassInputDialog（重命名弹框）/ UpdateDialog 同源，幂等，可反复调用。
    void assertLayerAboveBandWindows();

    QFrame* m_titleBar = nullptr;
    QStackedWidget* m_stack = nullptr;
    QVector<QPushButton*> m_tabBtns;

    QTextEdit* m_contentEdit = nullptr;
    QCheckBox* m_screenshotCheck = nullptr;
    QPushButton* m_attachBtn = nullptr;
    QComboBox* m_contactTypeCombo = nullptr;
    QLineEdit* m_contactEdit = nullptr;
    QLabel* m_attachLabel = nullptr;

    bool m_dragging = false;
    QPoint m_dragPos;
};

#endif // FEEDBACKDIALOG_H
