/*
 * @file shortcutedit.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef SHORTCUTEDIT_H
#define SHORTCUTEDIT_H

#include <QLineEdit>

class ShortcutEdit : public QLineEdit {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit ShortcutEdit(QWidget* parent = nullptr);

    // 快捷方式文本
    QString shortcutText() const;
    // 设置快捷方式文本
    void setShortcutText(const QString& text);

signals:
    // 用户完成一次快捷键输入后显式发出（无论是否通过 textChanged 触发保存，
    // 都可作为更可靠的保存时机）。
    void shortcutCommitted();

protected:
    // 按键按下事件
    void keyPressEvent(QKeyEvent* event) override;
    // focusin事件
    void focusInEvent(QFocusEvent* event) override;
    // 输入method事件
    void inputMethodEvent(QInputMethodEvent* event) override;
};

#endif // SHORTCUTEDIT_H
