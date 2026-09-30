/*
 * @file categorystore.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "categorystore.h"
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

static QString storeKey() { return QStringLiteral("Categories/mapping"); }

// 独立的分类数据库 INI（与 DestopTools 主设置分开，便于整体清空）
static QSettings makeSettings() {
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("DestopTools"), QStringLiteral("categories"));
}

// 路径分隔统一为“/”：避免 Windows 下 Qt/Shell/旧版本存储的“\\”与当前扫描返回的“\\”
// 不一致导致 CategoryStore 查找失败，表现为“持久化数据加载失败/分类清空”。
static QString normalizedStorePath(const QString& path) {
    return QDir::fromNativeSeparators(path);
}

// 真实 Windows 桌面目录（用于判断拖入文件是否来自桌面）
// 作者：谭征
QString CategoryStore::desktopPath() {
    return QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
}

const QString& CategoryStore::desktopCategory() {
    static const QString s = QStringLiteral("其它");
    return s;
}

// 在首次刷新前调用，避免旧版数据里残留的“未分类”分类仍占用文件。
// 作者：谭征
void CategoryStore::migrateLegacyUncategorized() {
    // 旧版本曾使用“未分类”作为未归类分类名，早期版本还用旧 classify() 产生过“其他”分类；
    // 现统一并入规则里的“其它”，避免残留隐藏分类或幻影“其他”盒子。
    renameCategory(QStringLiteral("未分类"), desktopCategory());
    renameCategory(QStringLiteral("其他"), desktopCategory());
}

// 整张映射表（每行格式 "sourcePath<tab>category"）
// 作者：谭征
QStringList CategoryStore::readAll() {
    QSettings s = makeSettings();
    QStringList rows = s.value(storeKey()).toStringList();
    // 落盘时统一为正斜杠；读盘时再做一次归一化，兼容旧版反斜杠数据。
    for (QString& row : rows) {
        const int t = row.indexOf(QLatin1Char('\t'));
        if (t > 0) {
            row = normalizedStorePath(row.left(t)) + QLatin1Char('\t') + row.mid(t + 1);
        }
    }
    return rows;
}

// writeall
// 作者：谭征
void CategoryStore::writeAll(const QStringList& rows) {
    QSettings s = makeSettings();
    s.setValue(storeKey(), rows);
    s.sync();
}

// 读取/写入某个桌面文件的分类；返回空字符串表示尚未归类（即位于真实桌面）
// 作者：谭征
QString CategoryStore::categoryOf(const QString& sourcePath) {
    if (sourcePath.isEmpty()) return QString();
    const QString key = normalizedStorePath(sourcePath);
    const QStringList rows = readAll();
    for (const QString& row : rows) {
        const int t = row.indexOf(QLatin1Char('\t'));
        // 大小写不敏感比较：Windows 路径大小写不敏感（历史版本曾以小写入库，如粘贴副本）
        if (t > 0 && row.left(t).compare(key, Qt::CaseInsensitive) == 0) return row.mid(t + 1);
    }
    return QString();
}

// 设置分类
// 作者：谭征
void CategoryStore::setCategory(const QString& sourcePath, const QString& category) {
    if (sourcePath.isEmpty() || category.isEmpty()) return;
    removeFiledDockPath(sourcePath);   // 一旦被正式归入某收纳盒，便不再是“仅隐藏于 Dock”的已归档态
    const QString key = normalizedStorePath(sourcePath);
    QStringList rows = readAll();
    for (int i = 0; i < rows.size(); ++i) {
        const int t = rows[i].indexOf(QLatin1Char('\t'));
        if (t > 0 && rows[i].left(t).compare(key, Qt::CaseInsensitive) == 0) {
            rows[i] = key + QLatin1Char('\t') + category;
            writeAll(rows);
            return;
        }
    }
    rows.append(key + QLatin1Char('\t') + category);
    writeAll(rows);
}

// 清除分类
// 作者：谭征
void CategoryStore::clearCategory(const QString& sourcePath) {
    if (sourcePath.isEmpty()) return;
    removeFiledDockPath(sourcePath);
    const QString key = normalizedStorePath(sourcePath);
    QStringList rows = readAll();
    for (int i = rows.size() - 1; i >= 0; --i) {
        const int t = rows[i].indexOf(QLatin1Char('\t'));
        if (t > 0 && rows[i].left(t).compare(key, Qt::CaseInsensitive) == 0) rows.removeAt(i);
    }
    writeAll(rows);
}

// 判断classified
// 作者：谭征
bool CategoryStore::isClassified(const QString& sourcePath) {
    return !categoryOf(sourcePath).isEmpty();
}

// 重命名某分类：把所有映射中等于 oldName 的分类改为 newName
// 作者：谭征
void CategoryStore::renameCategory(const QString& oldName, const QString& newName) {
    if (oldName == newName) return;
    QStringList rows = readAll();
    bool changed = false;
    for (int i = 0; i < rows.size(); ++i) {
        const int t = rows[i].indexOf(QLatin1Char('\t'));
        if (t > 0 && rows[i].mid(t + 1) == oldName) {
            rows[i] = rows[i].left(t) + QLatin1Char('\t') + newName;
            changed = true;
        }
    }
    if (changed) writeAll(rows);
}

// 删除某分类：清空所有属于该分类的文件记录（文件回到真实桌面，仍在原桌面位置，不丢）
// 作者：谭征
void CategoryStore::removeCategory(const QString& category) {
    QStringList rows = readAll();
    bool changed = false;
    for (int i = rows.size() - 1; i >= 0; --i) {
        const int t = rows[i].indexOf(QLatin1Char('\t'));
        if (t > 0 && rows[i].mid(t + 1) == category) { rows.removeAt(i); changed = true; }
    }
    if (changed) writeAll(rows);
}

// (path -> cat) 一次性落盘（只一次 sync）。两者配合，N 个文件的整理从 O(N) 次文件 IO 降到 2 次。
// 作者：谭征
QHash<QString, QString> CategoryStore::readMap() {
    QHash<QString, QString> map;
    const QStringList rows = readAll();
    for (const QString& row : rows) {
        const int t = row.indexOf(QLatin1Char('\t'));
        if (t > 0) map.insert(row.left(t), row.mid(t + 1));
    }
    return map;
}

// 应用additions
// 作者：谭征
void CategoryStore::applyAdditions(const QHash<QString, QString>& additions) {
    if (additions.isEmpty()) return;
    QStringList rows = readAll();
    bool changed = false;
    for (auto it = additions.begin(); it != additions.end(); ++it) {
        const QString path = normalizedStorePath(it.key());
        const QString& cat = it.value();
        bool found = false;
        for (int i = 0; i < rows.size(); ++i) {
            const int t = rows[i].indexOf(QLatin1Char('\t'));
            if (t > 0 && rows[i].left(t).compare(path, Qt::CaseInsensitive) == 0) { found = true; break; }
        }
        if (!found) {
            rows.append(path + QLatin1Char('\t') + cat);
            changed = true;
        }
    }
    if (changed) writeAll(rows);   // 所有新增只落盘一次
}

// 批量覆盖更新：对已有记录更新分类，对无记录则新增。用于“桌面整理”按钮按当前设置重新整理所有文件。
// 作者：谭征
void CategoryStore::applyUpdates(const QHash<QString, QString>& updates) {
    if (updates.isEmpty()) return;
    QStringList rows = readAll();
    bool changed = false;
    for (auto it = updates.begin(); it != updates.end(); ++it) {
        const QString path = normalizedStorePath(it.key());
        const QString& cat = it.value();
        bool found = false;
        for (int i = 0; i < rows.size(); ++i) {
            const int t = rows[i].indexOf(QLatin1Char('\t'));
            if (t > 0 && rows[i].left(t).compare(path, Qt::CaseInsensitive) == 0) {
                if (rows[i].mid(t + 1) != cat) {
                    rows[i] = path + QLatin1Char('\t') + cat;
                    changed = true;
                }
                found = true;
                break;
            }
        }
        if (!found) {
            rows.append(path + QLatin1Char('\t') + cat);
            changed = true;
        }
    }
    if (changed) writeAll(rows);   // 所有更新只落盘一次
}

// 该分类是否存在（即有文件被归入）
// 作者：谭征
bool CategoryStore::categoryExists(const QString& category) {
    const QStringList rows = readAll();
    for (const QString& row : rows) {
        const int t = row.indexOf(QLatin1Char('\t'));
        if (t > 0 && row.mid(t + 1) == category) return true;
    }
    return false;
}

// 当前所有存在的分类名
// 作者：谭征
QStringList CategoryStore::allCategories() {
    QStringList result;
    const QStringList rows = readAll();
    for (const QString& row : rows) {
        const int t = row.indexOf(QLatin1Char('\t'));
        if (t > 0) {
            const QString cat = row.mid(t + 1);
            if (!result.contains(cat)) result.append(cat);
        }
    }
    return result;
}

// 清空全部分类记录
// 作者：谭征
void CategoryStore::clearAll() {
    QSettings s = makeSettings();
    s.remove(QStringLiteral("Categories"));
    s.sync();
}

// 「已归档（隐藏于 Dock）」集合
// 独立键存储，与分类映射互不干扰；路径统一为正斜杠，大小写不敏感匹配交给使用方。
static QString filedKey() { return QStringLiteral("Dock/filed"); }

// 与主分类映射相互独立：已归档文件不参与任何收纳盒渲染，仅从 Dock 隐藏。
// 作者：谭征
QStringList CategoryStore::filedDockPaths() {
    QSettings s = makeSettings();
    QStringList rows = s.value(filedKey()).toStringList();
    for (QString& r : rows) r = normalizedStorePath(r);
    return rows;
}

// 添加filedDock路径
// 作者：谭征
void CategoryStore::addFiledDockPath(const QString& sourcePath) {
    if (sourcePath.isEmpty()) return;
    const QString key = normalizedStorePath(sourcePath);
    QSettings s = makeSettings();
    QStringList rows = s.value(filedKey()).toStringList();
    for (const QString& r : rows) {
        if (normalizedStorePath(r) == key) return;   // 已记录，避免重复
    }
    rows.append(key);
    s.setValue(filedKey(), rows);
    s.sync();
}

// 移除filedDock路径
// 作者：谭征
void CategoryStore::removeFiledDockPath(const QString& sourcePath) {
    if (sourcePath.isEmpty()) return;
    const QString key = normalizedStorePath(sourcePath);
    QSettings s = makeSettings();
    QStringList rows = s.value(filedKey()).toStringList();
    bool changed = false;
    for (int i = rows.size() - 1; i >= 0; --i) {
        if (normalizedStorePath(rows[i]) == key) { rows.removeAt(i); changed = true; }
    }
    if (changed) { s.setValue(filedKey(), rows); s.sync(); }
}

// 剔除已归档集合中磁盘上已不存在的文件（源文件被删除后不再需要隐藏）
// 作者：谭征
void CategoryStore::pruneFiledDockPaths() {
    QSettings s = makeSettings();
    QStringList rows = s.value(filedKey()).toStringList();
    bool changed = false;
    for (int i = rows.size() - 1; i >= 0; --i) {
        if (!QFile::exists(normalizedStorePath(rows[i]))) { rows.removeAt(i); changed = true; }
    }
    if (changed) { s.setValue(filedKey(), rows); s.sync(); }
}
