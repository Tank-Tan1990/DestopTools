/*
 * @file backupdialog.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef BACKUPDIALOG_H
#define BACKUPDIALOG_H

#include <QDialog>
#include <QPoint>

QT_BEGIN_NAMESPACE
class QLineEdit;
class QCheckBox;
class QLabel;
class QFrame;
QT_END_NAMESPACE

class BackupDialog : public QDialog {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit BackupDialog(QWidget* parent = nullptr);

    // 备份名称
    QString backupName() const;
    // 同步设置
    bool syncSettings() const;

protected:
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
    // 把弹框重新压到全部 band 窗口（收纳盒 / 桌面助手 / Dock 图标区）之上、正常程序之下，
    // 并清掉 Qt 可能替我们指定的 owner（否则会随 owner 的周期性压底一起沉到桌面图标层之下）。
    // 与 GlassInputDialog（重命名弹框）/ UpdateDialog 同源，幂等，可反复调用。
    void assertLayerAboveBandWindows();

    QLineEdit* m_nameEdit = nullptr;
    QCheckBox* m_syncCheck = nullptr;
    QFrame* m_titleBar = nullptr;

    bool m_dragging = false;
    QPoint m_dragPos;
};

#endif // BACKUPDIALOG_H
