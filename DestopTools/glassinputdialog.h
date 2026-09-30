/*
 * @file glassinputdialog.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef GLASSINPUTDIALOG_H
#define GLASSINPUTDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QFrame>

/**
 * @brief 无边框玻璃风格的单行文本输入对话框（替代 QInputDialog::getText）。
 */
class GlassInputDialog : public QDialog {
    Q_OBJECT
public:
    // 获取文本
    static QString getText(QWidget* parent,
                           const QString& title,
                           const QString& label,
                           QLineEdit::EchoMode echo = QLineEdit::Normal,
                           const QString& text = QString(),
                           bool* ok = nullptr);

protected:
    // 构造函数：初始化对象
    explicit GlassInputDialog(QWidget* parent,
                              const QString& title,
                              const QString& label,
                              QLineEdit::EchoMode echo,
                              const QString& text);
    // 原生事件
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;
    // 鼠标按下事件
    void mousePressEvent(QMouseEvent* event) override;
    // 鼠标移动事件
    void mouseMoveEvent(QMouseEvent* event) override;
    // 鼠标松开事件
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    // 初始化ui
    void setupUi();

    QString m_titleText;
    QString m_labelText;
    QLineEdit::EchoMode m_echo;
    QString m_defaultText;

    QFrame* m_titleBar = nullptr;
    QLineEdit* m_lineEdit = nullptr;
    bool m_dragging = false;
    QPoint m_dragPos;
};

#endif // GLASSINPUTDIALOG_H
