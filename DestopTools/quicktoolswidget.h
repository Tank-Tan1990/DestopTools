/*
 * @file quicktoolswidget.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef QUICKTOOLSWIDGET_H
#define QUICKTOOLSWIDGET_H

#include <QWidget>

class QGridLayout;

// 桌面助手右侧快速工具网格
class QuickToolsWidget : public QWidget {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit QuickToolsWidget(QWidget* parent = nullptr);

    // 默认工具名称
    static QStringList defaultToolNames();
    // 工具图标
    static QIcon toolIcon(const QString& name, QWidget* w);

    // 工具顺序
    QStringList toolOrder() const;
    // 设置工具顺序
    void setToolOrder(const QStringList& order);

signals:
    // 工具触发信号
    void toolTriggered(const QString& name);

private slots:
    void applyTheme();   // 主题色联动：重绘快捷工具按钮样式与图标

private:
    // 初始化ui
    void setupUi();
    // 构建网格
    void buildGrid(const QStringList& tools);

    QStringList m_toolOrder;
    QGridLayout* m_gridLayout = nullptr;
};

#endif // QUICKTOOLSWIDGET_H
