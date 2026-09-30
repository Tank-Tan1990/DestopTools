/*
 * @file categorystore.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef CATEGORYSTORE_H
#define CATEGORYSTORE_H

#include "desktopitem.h"
#include <QString>
#include <QStringList>
#include <QHash>

// 分类数据库（A 方案 / 360 式）：桌面文件始终留在原位置（如 F:\Desktop），
// 本类只持久化「sourcePath -> 分类名」映射，绝不物理搬运文件。
// 文件的归类/取消归类只是改这里的记录；程序据此在窗口内渲染，并（可选）隐藏系统桌面图标。
// 关键安全约定：本类任何方法都不移动/删除真实文件，因此启动/刷新阶段不会因物理移动而崩溃。
class CategoryStore {
public:
    // 真实 Windows 桌面目录（用于判断拖入文件是否来自桌面）
    static QString desktopPath();

    // 代表“未归类文件的归属分类”，即规则里的“其它”分类。
    // 程序不再使用名为“未分类”的分类：未归类文件统一进入“其它”，
    // 既符合用户要求（不在规则内的文件进“其他”），也避免界面出现冗余的“未分类”盒子。
    static const QString& desktopCategory();

    // 把历史遗留的“未分类”记录一次性迁移到“其它”（desktopCategory）。
    // 在首次刷新前调用，避免旧版数据里残留的“未分类”分类仍占用文件。
    static void migrateLegacyUncategorized();

    // 读取/写入某个桌面文件的分类；返回空字符串表示尚未归类（即位于真实桌面）
    static QString categoryOf(const QString& sourcePath);
    // 设置分类
    static void setCategory(const QString& sourcePath, const QString& category);
    static void clearCategory(const QString& sourcePath);   // 取消归类 → 回到真实桌面（无分类记录）
    // 判断classified
    static bool isClassified(const QString& sourcePath);

    // 热点批量接口：避免“桌面整理”时对每个文件反复读/写整份 INI 造成的秒级卡顿。
    // readMap() 一次读入整张映射（sourcePath -> 分类）；applyAdditions() 仅把库中尚不存在的
    // (path -> cat) 一次性落盘（只一次 sync）。两者配合，N 个文件的整理从 O(N) 次文件 IO 降到 2 次。
    static QHash<QString, QString> readMap();
    // 应用additions
    static void applyAdditions(const QHash<QString, QString>& additions);
    // 批量覆盖更新：对已有记录更新分类，对无记录则新增。用于“桌面整理”按钮按当前设置重新整理所有文件。
    static void applyUpdates(const QHash<QString, QString>& updates);

    // 重命名某分类：把所有映射中等于 oldName 的分类改为 newName
    static void renameCategory(const QString& oldName, const QString& newName);
    // 删除某分类：清空所有属于该分类的文件记录（文件回到真实桌面，仍在原桌面位置，不丢）
    static void removeCategory(const QString& category);
    // 该分类是否存在（即有文件被归入）
    static bool categoryExists(const QString& category);
    // 当前所有存在的分类名
    static QStringList allCategories();
    // 清空全部分类记录
    static void clearAll();

    // 「已归档（隐藏于 Dock）」集合：从 Dock（桌面镜像）隐藏、但不属于任何收纳盒的文件。
    // 用于「在 Dock 复制图标 → 在收纳盒粘贴」场景：粘贴生成副本归入收纳盒，源文件标记为已归档，
    // 使其立即从 Dock 消失（文件仍物理留在桌面，只是不在 Dock 显示）。重启/刷新后持久生效。
    // 与主分类映射相互独立：已归档文件不参与任何收纳盒渲染，仅从 Dock 隐藏。
    static QStringList filedDockPaths();
    // 添加filedDock路径
    static void addFiledDockPath(const QString& sourcePath);
    // 移除filedDock路径
    static void removeFiledDockPath(const QString& sourcePath);
    // 剔除已归档集合中磁盘上已不存在的文件（源文件被删除后不再需要隐藏）
    static void pruneFiledDockPaths();

private:
    // 整张映射表（每行格式 "sourcePath<tab>category"）
    static QStringList readAll();
    // writeall
    static void writeAll(const QStringList& rows);
};

#endif // CATEGORYSTORE_H
