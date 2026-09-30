/*
 * @file shortcutedit.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "shortcutedit.h"
#include <QKeyEvent>
#include <QKeySequence>
#include <QInputMethodEvent>

ShortcutEdit::ShortcutEdit(QWidget* parent)
    : QLineEdit(parent) {
    // 设为可编辑以显示闪烁光标（输入框获得焦点时光标可见），
    // 但实际的文本由按键捕获逻辑设置，禁止通过输入法/粘贴直接插入文本。
    setPlaceholderText(QStringLiteral("无"));
    setText(QStringLiteral("无"));
    setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // 禁用右键菜单与拖放，避免通过菜单粘贴或拖入文本
    setContextMenuPolicy(Qt::NoContextMenu);
    setAcceptDrops(false);
}

// 快捷方式文本
// 作者：谭征
QString ShortcutEdit::shortcutText() const {
    QString t = text();
    return t == QStringLiteral("无") ? QString() : t;
}

// 设置快捷方式文本
// 作者：谭征
void ShortcutEdit::setShortcutText(const QString& text) {
    setText(text.isEmpty() ? QStringLiteral("无") : text);
}

// focusin事件
// 作者：谭征
void ShortcutEdit::focusInEvent(QFocusEvent* event) {
    QLineEdit::focusInEvent(event);
    // 获得焦点时不清空（鼠标单击/双击/Tab 均保留当前显示），
    // 并将光标定位到文本末尾，使输入框显示闪烁光标，提示可输入。
    setCursorPosition(text().length());
}

// 按键按下事件
// 作者：谭征
void ShortcutEdit::keyPressEvent(QKeyEvent* event) {
    int key = event->key();

    // Tab / Shift+Tab 用于焦点切换，不捕获
    if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
        QLineEdit::keyPressEvent(event);
        return;
    }

    // Esc 表示取消/清空
    if (key == Qt::Key_Escape) {
        setText(QStringLiteral("无"));
        emit shortcutCommitted();
        clearFocus();
        return;
    }

    Qt::KeyboardModifiers mods = event->modifiers();
    bool hasCtrl  = mods & Qt::ControlModifier;
    bool hasAlt   = mods & Qt::AltModifier;
    bool hasShift = mods & Qt::ShiftModifier;
    bool hasMeta  = mods & Qt::MetaModifier;

    QStringList parts;
    if (hasCtrl)  parts << QStringLiteral("Ctrl");
    if (hasAlt)   parts << QStringLiteral("Alt");
    if (hasShift) parts << QStringLiteral("Shift");
    if (hasMeta)  parts << QStringLiteral("Meta");

    // 排除单独的修饰键
    if (key == Qt::Key_Control || key == Qt::Key_Alt ||
        key == Qt::Key_Shift || key == Qt::Key_Meta ||
        key == Qt::Key_AltGr) {
        setText(parts.isEmpty() ? QStringLiteral("无") : parts.join(QStringLiteral(" + ")));
        emit shortcutCommitted();
        return;
    }

    QString keyText = QKeySequence(key).toString(QKeySequence::NativeText);
    if (keyText.isEmpty()) {
        keyText = event->text().toUpper();
    }
    if (!keyText.isEmpty()) {
        // 如果普通键本身就是大写字母且没有 Shift，按 Qt 习惯显示为大写
        parts << keyText;
    }

    if (parts.isEmpty()) {
        setText(QStringLiteral("无"));
        emit shortcutCommitted();
        return;
    }

    setText(parts.join(QStringLiteral(" + ")));
    emit shortcutCommitted();
    clearFocus();
}

// 输入method事件
// 作者：谭征
void ShortcutEdit::inputMethodEvent(QInputMethodEvent* /*event*/) {
    // 忽略输入法组合事件，避免中文输入法在框内插入候选文本；
    // 快捷键内容仅由 keyPressEvent 捕获设置。
}
