/*
 * @file settingsmanager.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include "desktopitem.h"
#include <QSettings>
#include <QMap>
#include <QVector>
#include <QRect>
#include <QStringList>
#include <QColor>
#include <QVariant>

struct BoxState {
    QString title;
    QColor color;
    QStringList itemIds; // 用 QUuid::toString()
};

// 用户通过“新建收纳盒”创建的独立收纳盒的持久化结构。
// categories：分类名称 → 该分类下全部图标的 shellPath（小写键用于重载时匹配），
// 是桌面真实文件绝对路径；收纳盒与“主窗口”互斥持有同一文件，重载时据此把文件从主窗口剔除。
struct UserBoxPersist {
    QString id;                                  // 稳定唯一 id（UUID），重命名/重启都用它关联
    QString title;                               // 收纳盒标题（= 窗口标题 / 首个分类名）
    QRect geometry;                              // 窗口几何（展开态）
    bool collapsed = false;                      // 折叠态
    QString currentCategory;                      // 重启后恢复时选中的分类（与默认盒 currentCategory 同口径）
    QMap<QString, QStringList> categories;        // 分类名 → shellPath 列表
    QStringList categoryOrder;                    // 分类显示顺序（与 IconGridWindow::m_categoryOrder 同口径；QMap 迭代会按 key 字母序，必须独立保存顺序）
};

class SettingsManager {
public:
    // 构造函数：初始化对象
    explicit SettingsManager();

    // 保存盒子
    void saveBoxes(const QMap<QString, BoxState>& boxes);
    // 加载盒子
    QMap<QString, BoxState> loadBoxes() const;

    // 用户创建的独立收纳盒：持久化到本地（UserBoxes 组），重启后由 MainWindow 重建。
    void saveUserBoxes(const QVector<UserBoxPersist>& boxes);
    // 加载user盒子
    QVector<UserBoxPersist> loadUserBoxes() const;

    // 保存窗口几何
    void saveWindowGeometry(const QByteArray& geom);
    // 加载窗口几何
    QByteArray loadWindowGeometry() const;

    // 保存value
    void saveValue(const QString& key, const QVariant& value);
    // 加载value
    QVariant loadValue(const QString& key, const QVariant& defaultValue = QVariant()) const;

    // 强制立即把内存中的修改刷写到磁盘（QSettings 默认异步写入，
    // 若程序/对话框快速关闭可能丢失最近一次修改）。
    void sync();

private:
    mutable QSettings m_settings;
};

#endif // SETTINGSMANAGER_H