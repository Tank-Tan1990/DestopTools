/*
 * @file rasterize_icons.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include <QApplication>
#include <QSvgRenderer>
#include <QPainter>
#include <QPixmap>
#include <QStringList>
#include <QDir>
#include <QByteArray>

int main(int argc, char** argv) {
    QApplication a(argc, argv);
    QString src = QDir::toNativeSeparators(QDir::currentPath() + "/images/icons/src/");
    QString out = QDir::toNativeSeparators(QDir::currentPath() + "/images/icons/");
    QStringList names = {
        QStringLiteral("logo"),
        QStringLiteral("about_logo"),
        QStringLiteral("menu"),
        QStringLiteral("search"),
        QStringLiteral("view_grid"),
        QStringLiteral("settings"),
        QStringLiteral("appearance"),
        QStringLiteral("refresh"),
        QStringLiteral("more"),
        QStringLiteral("close"),
        QStringLiteral("close_hover"),
        QStringLiteral("collapse"),
        QStringLiteral("add"),
        QStringLiteral("unlock"),
        QStringLiteral("rename"),
        QStringLiteral("delete"),
        QStringLiteral("new_category"),
        QStringLiteral("new_box"),
        QStringLiteral("home"),
        QStringLiteral("market"),
        QStringLiteral("check"),
        QStringLiteral("feedback"),
        QStringLiteral("tool_organize"),
        QStringLiteral("tool_search"),
        QStringLiteral("tool_files"),
        QStringLiteral("tool_shutdown"),
        QStringLiteral("tool_screenshot"),
        QStringLiteral("tool_notes"),
        QStringLiteral("tool_wallpaper"),
        QStringLiteral("tool_lock"),
        QStringLiteral("tool_browser"),
        QStringLiteral("tool_calc"),
        QStringLiteral("tool_registry"),
        QStringLiteral("tool_cmd"),
        QStringLiteral("tool_clean"),
        QStringLiteral("tool_manage"),
        QStringLiteral("weather")
    };
    QList<int> scales = {1, 2};
    int count = 0;
    for (const QString& n : names) {
        QSvgRenderer r(src + n + ".svg");
        if (!r.isValid()) { qWarning("invalid svg: %s", qPrintable(n)); continue; }
        for (int s : scales) {
            int size = 24 * s;
            QPixmap pm(size, size);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            r.render(&p, QRect(0, 0, size, size));
            QString path = out + n + QString("_%1x.png").arg(s);
            if (pm.save(path))
                ++count;
        }
    }
    printf("rasterized %d png files\n", count);
    return 0;
}
