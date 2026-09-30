/*
 * @file filesearchworker.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef FILESEARCHWORKER_H
#define FILESEARCHWORKER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QSet>
#include <QDir>
#include <QFileInfo>
#include <QFileIconProvider>
#include <QStorageInfo>
#include <QStandardPaths>
#include <QFile>
#include <atomic>

// 后台文件枚举 worker（运行在独立 QThread，不阻塞 UI）。
// 手动栈式递归（而非 QDirIterator::Subdirectories），以便在遇到
// $Recycle.Bin / System Volume Information 等不可枚举目录时**剪枝**，
// 并用 canonicalPath 去重避免 NTFS 目录 junction 造成的无限循环。
// worker 只回传路径与是否目录（QStringList / QList<bool> 均为 Qt 内建元类型，
// 可直接用于跨线程排队连接），图标由主线程构建，避免在非 GUI 线程触碰 QPixmap。
class FileSearchWorker : public QObject {
    Q_OBJECT
public:
    std::atomic<qint64> m_activeToken{0};

signals:
    // results就绪
    void resultsReady(qint64 token, const QStringList& paths, const QList<bool>& isDirs, int runningTotal);
    // 搜索finished
    void searchFinished(qint64 token, int total);

public slots:
    // do搜索
    void doSearch(qint64 token, QStringList roots, QString keyword, int maxResults) {
        m_activeToken.store(token);
        const QString kw = keyword.toLower();
        int total = 0;
        QStringList paths;
        QList<bool> isDirs;

        QStringList dirStack = roots;
        QSet<QString> visited;
        while (!dirStack.isEmpty()) {
            if (m_activeToken.load() != token) return;   // 已被新搜索取代
            const QString dir = dirStack.takeLast();
            const QString lower = dir.toLower();
            // 剪枝：回收站 / 系统卷信息目录无法枚举，跳过以免卡死
            if (lower.endsWith(QStringLiteral("$recycle.bin")) ||
                lower.endsWith(QStringLiteral("system volume information"))) {
                continue;
            }
            QDir d(dir);
            if (!d.isReadable()) continue;                // 无权限目录直接跳过
            const QFileInfoList entries = d.entryInfoList(
                QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
            for (const QFileInfo& fi : entries) {
                if (m_activeToken.load() != token) return;
                if (fi.fileName().toLower().contains(kw)) {
                    paths.append(fi.filePath());
                    isDirs.append(fi.isDir());
                    ++total;
                    if (paths.size() >= 50) {
                        emit resultsReady(token, paths, isDirs, total);
                        paths.clear();
                        isDirs.clear();
                    }
                    if (total >= maxResults) {
                        emit resultsReady(token, paths, isDirs, total);
                        emit searchFinished(token, total);
                        return;
                    }
                }
                if (fi.isDir()) {
                    const QString cn = fi.canonicalFilePath();
                    if (!cn.isEmpty() && !visited.contains(cn)) {
                        visited.insert(cn);
                        dirStack.append(fi.filePath());
                    }
                }
            }
        }
        emit resultsReady(token, paths, isDirs, total);
        emit searchFinished(token, total);
    }
};

#endif // FILESEARCHWORKER_H
