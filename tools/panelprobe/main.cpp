/*
 * @file main.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

// 产品真实控件试验台：直接构造产品里的 SidePanelWidget（链接产品 .obj，非替身），
// 读取助手面板标题栏搜索框的调色板，并离屏渲染量像素。
// 目的：复现用户报告的「改完主题色后，搜索框占位文字『搜索』变成黑色」。
// 隔离试验台（tools/placeholderprobe）测了 5 种时序都复现不出来 —— 说明真因在产品真实上下文里，
// 所以这里不再模拟，直接把真实控件跑起来，一步步逼问它。
#include <QApplication>
#include <QLineEdit>
#include <QPalette>
#include <QTimer>
#include <QEventLoop>
#include <QImage>
#include <QTextStream>
#include <QPoint>
#include <QtGlobal>

#include "theme.h"
#include "thememanager.h"
#include "settingsmanager.h"
#include "sidepanelwidget.h"

namespace {

int g_fail = 0;

void wait(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

QString rgb(const QColor& c) {
    return c.isValid()
        ? QStringLiteral("rgb(%1,%2,%3,a%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha())
        : QStringLiteral("?");
}
int lum(const QColor& c) { return (c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000; }
int dist(const QColor& a, const QColor& b) {
    return qMax(qAbs(a.red() - b.red()), qMax(qAbs(a.green() - b.green()), qAbs(a.blue() - b.blue())));
}

// 分组名称
// 作者：谭征
const char* groupName(QPalette::ColorGroup g) {
    switch (g) {
    case QPalette::Active:   return "Active";
    case QPalette::Inactive: return "Inactive";
    case QPalette::Disabled: return "Disabled";
    default:                 return "?";
    }
}

struct Sample {
    QColor bg, glyph;
    int    deltaLum = 0, glyphPixels = 0;
    bool   ok = false;
};

// 渲染整个面板，在搜索框范围内取"出现最多的颜色 = 底色"，再取离底色最远像素的众数 = 笔画色
Sample sampleEdit(QWidget* panel, QLineEdit* edit) {
    Sample s;
    const QImage im = panel->grab().toImage();
    if (im.isNull()) return s;
    const QPoint tl = edit->mapTo(panel, QPoint(0, 0));
    const QRect  box = QRect(tl, edit->size()).adjusted(4, 4, -4, -4).intersected(im.rect());
    if (box.isEmpty()) return s;

    QHash<QRgb, int> hist, glyphHist;
    for (int y = box.top(); y <= box.bottom(); ++y)
        for (int x = box.left(); x <= box.right(); ++x)
            ++hist[im.pixel(x, y)];
    int best = -1;
    for (auto it = hist.constBegin(); it != hist.constEnd(); ++it)
        if (it.value() > best) { best = it.value(); s.bg = QColor::fromRgb(it.key()); }

    int bestG = -1;
    for (int y = box.top(); y <= box.bottom(); ++y) {
        for (int x = box.left(); x <= box.right(); ++x) {
            const QRgb   k = im.pixel(x, y);
            const QColor c = QColor::fromRgb(k);
            if (dist(c, s.bg) <= 20) continue;
            ++s.glyphPixels;
            const int n = ++glyphHist[k];
            if (n > bestG) { bestG = n; s.glyph = c; }
        }
    }
    if (!s.glyphPixels) return s;
    s.deltaLum = lum(s.glyph) - lum(s.bg);
    s.ok = true;
    return s;
}

void dumpEdit(QTextStream& out, const QString& tag, QLineEdit* edit) {
    const QPalette p = edit->palette();
    out << QStringLiteral("    [调色板] %1  currentGroup=%2\n")
               .arg(tag).arg(QLatin1String(groupName(p.currentColorGroup())));
    const QPalette::ColorGroup groups[3] = {QPalette::Active, QPalette::Inactive, QPalette::Disabled};
    for (int i = 0; i < 3; ++i) {
        out << QStringLiteral("        %1 Text=%2 WindowText=%3 PlaceholderText=%4\n")
                   .arg(QLatin1String(groupName(groups[i])), -8)
                   .arg(rgb(p.color(groups[i], QPalette::Text)), -22)
                   .arg(rgb(p.color(groups[i], QPalette::WindowText)), -22)
                   .arg(rgb(p.color(groups[i], QPalette::PlaceholderText)));
    }
    out << QStringLiteral("        stylesheet%1\n")
               .arg(edit->styleSheet().isEmpty() ? QStringLiteral(" 为空（只吃全局样式表）")
                                                 : QStringLiteral(" 非空（%1 字符）").arg(edit->styleSheet().size()));
}

void report(QTextStream& out, const QString& tag, QWidget* panel, QLineEdit* edit, const QString& expect) {
    out << tag << QStringLiteral("\n");
    const Sample s = sampleEdit(panel, edit);
    if (!s.ok) { out << QStringLiteral("    [!] 采样失败\n"); ++g_fail; return; }
    out << QStringLiteral("    底色=%1(lum=%2)  笔画=%3(lum=%4)  笔画像素=%5  与底色差=%6\n")
               .arg(rgb(s.bg)).arg(lum(s.bg)).arg(rgb(s.glyph)).arg(lum(s.glyph)).arg(s.glyphPixels)
               .arg(s.deltaLum >= 0 ? QStringLiteral("+%1").arg(s.deltaLum) : QString::number(s.deltaLum));
    out << QStringLiteral("    %1\n").arg(expect);
    out << QStringLiteral("    → %1\n")
               .arg(s.deltaLum >= 25 ? QStringLiteral("占位字亮于底色（固定禁用灰）OK")
                                     : QStringLiteral("占位字暗于底色 → 半透明黑  FAIL"));
    if (s.deltaLum < 25) ++g_fail;
}

}   // namespace

int main(int argc, char** argv) {
    QApplication::setOrganizationName(QStringLiteral("DestopTools"));
    QApplication::setApplicationName(QStringLiteral("DestopToolsProbe"));
    QApplication app(argc, argv);

    QTextStream out(stdout);

    // 复刻产品 main.cpp 的启动次序
    app.setFont(Theme::appFont());
    {
        SettingsManager   sm;
        ThemeManager*     tm = ThemeManager::instance();
        const QString     accent = sm.loadValue(QStringLiteral("Appearance/accent")).toString();
        if (!accent.isEmpty()) tm->setAccentColor(QColor(accent));
        tm->setTransparency(100);
        out << QStringLiteral("载入的主题色 = %1\n").arg(tm->accentColor().name());
    }
    app.setStyleSheet(Theme::globalStyleSheet());
    QObject::connect(ThemeManager::instance(), &ThemeManager::themeChanged, &app,
                     [&app]() { app.setStyleSheet(Theme::globalStyleSheet()); });

    SidePanelWidget panel;
    panel.resize(357, 760);
    panel.show();
    wait(900);

    QLineEdit* edit = nullptr;
    const QList<QLineEdit*> edits = panel.findChildren<QLineEdit*>();
    for (QLineEdit* e : edits)
        if (e->placeholderText() == QStringLiteral("搜索")) { edit = e; break; }
    out << QStringLiteral("面板内 QLineEdit 数 = %1，找到「搜索」占位框 = %2\n\n")
               .arg(edits.size()).arg(edit ? QStringLiteral("是") : QStringLiteral("否"));
    if (!edit) { out.flush(); return 2; }

    out << QStringLiteral("═══ 阶段 A：启动后（主题色 = %1）═══\n")
               .arg(ThemeManager::instance()->accentColor().name());
    dumpEdit(out, QStringLiteral("A 启动后"), edit);
    report(out, QStringLiteral("A 启动后"), &panel, edit, QStringLiteral("期望：亮灰"));
    out << QStringLiteral("\n");

    out << QStringLiteral("═══ 阶段 B：改主题色 → #FF8A3D（橙），走真实 themeChanged 链路 ═══\n");
    ThemeManager::instance()->setAccentColor(QColor(0xFF, 0x8A, 0x3D));
    wait(700);
    dumpEdit(out, QStringLiteral("B 改色后"), edit);
    report(out, QStringLiteral("B 改色后"), &panel, edit, QStringLiteral("期望：仍是同一档亮灰（不随主题色）"));
    out << QStringLiteral("\n");

    out << QStringLiteral("═══ 阶段 C：再改回默认青 #22D3EE ═══\n");
    ThemeManager::instance()->setAccentColor(QColor(0x22, 0xD3, 0xEE));
    wait(700);
    dumpEdit(out, QStringLiteral("C 改回后"), edit);
    report(out, QStringLiteral("C 改回后"), &panel, edit, QStringLiteral("期望：仍是同一档亮灰"));
    out << QStringLiteral("\n");

    out << QStringLiteral("═══ 阶段 D：拖透明度到 0 ═══\n");
    ThemeManager::instance()->setTransparency(0);
    wait(700);
    dumpEdit(out, QStringLiteral("D 透明度0"), edit);
    report(out, QStringLiteral("D 透明度 0"), &panel, edit, QStringLiteral("期望：占位字仍亮于底色"));
    out << QStringLiteral("\n");

    // 阶段 E：**人为把占位色打成"变黑"状态**，验证守卫能不能自己救回来
    // 把 PlaceholderText 直接设成「黑 50% alpha」= 用户截图里那个 rgb(8,11,11) 的成因，
    // 然后不再做任何操作，看守卫是否在事件循环里自动把它改回禁用灰。
    out << QStringLiteral("═══ 阶段 E：人为打成黑（PlaceholderText = rgba(0,0,0,128)），验证守卫自救 ═══\n");
    {
        QPalette p = edit->palette();
        p.setColor(QPalette::PlaceholderText, QColor(0, 0, 0, 128));   // ← 这就是"变黑"的真身
        edit->setPalette(p);
    }
    wait(400);            // 只等事件循环，不做任何其它操作
    dumpEdit(out, QStringLiteral("E 人工变黑后"), edit);
    report(out, QStringLiteral("E 人工变黑后"), &panel, edit,
           QStringLiteral("期望：守卫自动改回禁用灰（若仍是暗字 → 守卫无效）"));
    out << QStringLiteral("\n");

    panel.hide();
    out << QStringLiteral("═══ 失败项数 = %1 ═══\n").arg(g_fail);
    out.flush();
    return 0;
}
