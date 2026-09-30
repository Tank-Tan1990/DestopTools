/*
 * @file main.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

// 输入框占位文字（placeholder）颜色的**真实像素**验证台。
// 现象（用户截图解码实测，PNG 320x720）
// 桌面助手标题栏搜索框：底色 rgb(16,23,22)，「搜索」两字笔画 rgb(8,11,11)
// → 笔画**比底色更黑**，且恰为底色 × 50%（半透明黑叠加）。用户语："变成黑色"。
// 关键前提：必须还原【应用级样式表】这一层
// 产品 main.cpp 会 `a.setStyleSheet(Theme::globalStyleSheet())`（应用级），
// 控件自己还有 `setStyleSheet(Theme::lineEditStyle())`（控件级）。
// 只还原控件级时，占位色是"50% 白"（亮灰）；加上应用级之后才复现出"50% 黑"。
// 故本试验台**必须**两层级同时下发，否则测的不是产品。
// 三个候选修法，全部实测
// ① 现状：不动 → 期望复现"更暗"
// ② 控件级 palette：setStyleSheet 之后设 QPalette::PlaceholderText
// ③ 应用级 palette：a.setPalette() 设 QPalette::PlaceholderText（一次性、零副作用）
// 判定标准：占位色明显亮于底色；且**主题色变化 + 全局 QSS 重下发**后逐像素不变。
#include <QApplication>
#include <QFrame>
#include <QLineEdit>
#include <QPalette>
#include <QImage>
#include <QTextStream>
#include <QVector>
#include <QHash>
#include <QPair>
#include <QtGlobal>

#include <algorithm>

#include "theme.h"
#include "thememanager.h"

// ThemeManager 最小替身（同 tools/alphatest / menualpha）
// 作者：谭征
ThemeManager* ThemeManager::instance() {
    static ThemeManager s_instance;
    return &s_instance;
}
ThemeManager::ThemeManager(QObject* parent) : QObject(parent) {}
// 设置主题色
// 作者：谭征
void ThemeManager::setAccentColor(const QColor& c) { m_accent = c; }
// 设置透明度
// 作者：谭征
void ThemeManager::setTransparency(int t) { m_transparency = qBound(0, t, 100); }
// 整窗不透明度会把圆角线框与文字一起淡化，与「线框透明度不调整」的需求冲突。
// 作者：谭征
double ThemeManager::effectiveOpacity() const { return qBound(0.15, m_transparency / 100.0, 1.0); }

namespace {

int g_fail = 0;

QString rgb(const QColor& c) {
    return c.isValid()
        ? QStringLiteral("rgb(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue())
        : QStringLiteral("?");
}
int lum(const QColor& c) { return (c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000; }
int dist(const QColor& a, const QColor& b) {
    return qMax(qAbs(a.red() - b.red()), qMax(qAbs(a.green() - b.green()), qAbs(a.blue() - b.blue())));
}

struct Sample {
    QColor bg, glyph;
    int    glyphPixels = 0;
    int    deltaLum    = 0;      // glyph - bg
    bool   ok          = false;
};

// 离屏渲染 host：出现最多的颜色 = 底色；离底色 > 24 的像素里**出现最多的**颜色 = 笔画色
// （取众数而不是均值：小字号有抗锯齿，均值会被稀释，众数正好落在笔画实心处）
Sample sampleGlyphs(QFrame* host, QLineEdit* edit) {
    Sample s;
    const QImage im = host->grab().toImage();
    if (im.isNull()) return s;

    const QRect box = edit->geometry().adjusted(4, 4, -4, -4).intersected(im.rect());
    if (box.isEmpty()) return s;

    QHash<QRgb, int> hist, glyphHist;
    for (int y = box.top(); y <= box.bottom(); ++y) {
        for (int x = box.left(); x <= box.right(); ++x)
            ++hist[im.pixel(x, y)];
    }
    int best = -1;
    for (auto it = hist.constBegin(); it != hist.constEnd(); ++it) {
        if (it.value() > best) { best = it.value(); s.bg = QColor::fromRgb(it.key()); }
    }
    int bestG = -1;
    for (int y = box.top(); y <= box.bottom(); ++y) {
        for (int x = box.left(); x <= box.right(); ++x) {
            const QRgb k = im.pixel(x, y);
            const QColor c = QColor::fromRgb(k);
            if (dist(c, s.bg) <= 24) continue;
            ++s.glyphPixels;
            const int n = ++glyphHist[k];
            if (n > bestG) { bestG = n; s.glyph = c; }
        }
    }
    if (s.glyphPixels == 0) return s;
    s.deltaLum = lum(s.glyph) - lum(s.bg);
    s.ok       = true;
    return s;
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

// 逐组打印 Text / WindowText / PlaceholderText —— 判断 QSS 的 color 到底进了哪几组
void dumpPalette(QTextStream& out, const QString& tag, const QPalette& p) {
    out << QStringLiteral("    [调色板] %1  currentGroup=%2\n")
               .arg(tag).arg(QLatin1String(groupName(p.currentColorGroup())));
    const QPalette::ColorGroup groups[3] = {QPalette::Active, QPalette::Inactive, QPalette::Disabled};
    for (int i = 0; i < 3; ++i) {
        out << QStringLiteral("        %1: Text=%2  WindowText=%3  PlaceholderText=%4\n")
                   .arg(QLatin1String(groupName(groups[i])), -8)
                   .arg(rgb(p.color(groups[i], QPalette::Text)), -18)
                   .arg(rgb(p.color(groups[i], QPalette::WindowText)), -18)
                   .arg(rgb(p.color(groups[i], QPalette::PlaceholderText)));
    }
}

// 创建/构建st
// 作者：谭征
QFrame* makeHost(QLineEdit** outEdit) {
    auto* host = new QFrame;
    host->setFixedSize(260, 40);
    host->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QFrame { background: rgba(17,26,46,1.0); border: none; }")));

    auto* edit = new QLineEdit(host);
    edit->setPlaceholderText(QStringLiteral("搜索"));
    edit->setGeometry(10, 7, 180, 26);
    edit->setStyleSheet(Theme::lineEditStyle());     // 控件级（产品 sidepanelwidget 同款）
    *outEdit = edit;
    return host;
}

// 重新下发应用级样式表 = 模拟 themeChanged（改主题色/透明度后 main.cpp 的行为）
void republishAppQss(QApplication& app) { app.setStyleSheet(Theme::globalStyleSheet()); }

void report(QTextStream& out, const QString& tag, const Sample& s, int expectMinDelta) {
    out << QStringLiteral("%1\n").arg(tag);
    if (!s.ok) {
        out << QStringLiteral("    [!] 采样失败\n");
        ++g_fail;
        return;
    }
    out << QStringLiteral("    底色=%1(lum=%2)  笔画=%3(lum=%4)  笔画像素=%5  与底色差=%6\n")
               .arg(rgb(s.bg)).arg(lum(s.bg)).arg(rgb(s.glyph)).arg(lum(s.glyph))
               .arg(s.glyphPixels)
               .arg(s.deltaLum >= 0 ? QStringLiteral("+%1").arg(s.deltaLum)
                                    : QStringLiteral("%1").arg(s.deltaLum));
    const bool pass = s.deltaLum >= expectMinDelta;
    out << QStringLiteral("    → %1\n")
               .arg(pass ? QStringLiteral("[亮于底色，占位色正确  OK]")
                         : QStringLiteral("[比底色暗 —— 半透明黑叠加  FAIL]"));
    if (!pass) ++g_fail;
}

}   // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    ThemeManager::instance()->setTransparency(100);      // 与用户截图同档
    ThemeManager::instance()->setAccentColor(QColor(0x22, 0xD3, 0xEE));

    QTextStream out(stdout);
    out << QStringLiteral("═══ 输入框占位文字颜色实测（还原产品：应用级 + 控件级双层样式表）═══\n");
    out << QStringLiteral("禁用灰基准 = QMenu::item:disabled 同款 rgba(255,255,255,0.40)\n\n");

    // 产品 main.cpp 的应用级下发，必须在建控件之前（同产品顺序）
    republishAppQss(app);
    dumpPalette(out, QStringLiteral("QApplication::palette()（项目从不设深色调色板，这里就是系统默认）"),
                app.palette());
    out << QStringLiteral("\n");

    QColor glyphRef;

    // ① 现状复现（应用级 + 控件级，无任何 palette 干预）
    {
        QLineEdit* edit = nullptr;
        QFrame*    host = makeHost(&edit);
        const Sample s = sampleGlyphs(host, edit);
        out << QStringLiteral("① 现状（产品原实现）\n");
        if (s.ok) {
            out << QStringLiteral("    底色=%1(lum=%2)  笔画=%3(lum=%4)  笔画像素=%5  与底色差=%6\n")
                       .arg(rgb(s.bg)).arg(lum(s.bg)).arg(rgb(s.glyph)).arg(lum(s.glyph))
                       .arg(s.glyphPixels)
                       .arg(s.deltaLum >= 0 ? QStringLiteral("+%1").arg(s.deltaLum)
                                            : QStringLiteral("%1").arg(s.deltaLum));
            out << QStringLiteral("    → %1\n")
                       .arg(s.deltaLum < 0
                                ? QStringLiteral("复现成功：笔画是半透明黑（= 底色 × 50%），与用户截图一致 ")
                                : QStringLiteral("未复现（试验台前提不成立，需重查）"));
            if (s.deltaLum >= 0) ++g_fail;
        } else { out << QStringLiteral("    [!] 采样失败\n"); ++g_fail; }
        dumpPalette(out, QStringLiteral("① 现状（应用级+控件级 QSS，无 palette 干预）"), edit->palette());
        delete host;
    }
    out << QStringLiteral("\n");

    // ①b 强制 Inactive 组：桌面助手是 WS_EX_NOACTIVATE 常驻窗，控件多半跑在 Inactive 组
    {
        QLineEdit* edit = nullptr;
        QFrame*    host = makeHost(&edit);
        QPalette   p = edit->palette();
        p.setCurrentColorGroup(QPalette::Inactive);
        edit->setPalette(p);
        const Sample s = sampleGlyphs(host, edit);
        out << QStringLiteral("①b 现状 + 强制 Inactive 颜色组\n");
        if (s.ok) {
            out << QStringLiteral("    底色=%1  笔画=%2  与底色差=%3\n")
                       .arg(rgb(s.bg), rgb(s.glyph)).arg(s.deltaLum);
            out << QStringLiteral("    → %1\n")
                       .arg(s.deltaLum < 0
                                ? QStringLiteral("复现成功：Inactive 组下占位色退化成半透明黑 （= 产品真因）")
                                : QStringLiteral("仍为亮灰：Inactive 组不是真因"));
        } else { out << QStringLiteral("    [!] 采样失败\n"); }
        delete host;
    }
    out << QStringLiteral("\n");

    // ①c 强制 Inactive 组 + 控件级修复
    {
        QLineEdit* edit = nullptr;
        QFrame*    host = makeHost(&edit);
        Theme::applyLineEditStyle(edit);
        QPalette   p = edit->palette();
        p.setCurrentColorGroup(QPalette::Inactive);
        edit->setPalette(p);
        const Sample s = sampleGlyphs(host, edit);
        out << QStringLiteral("①c 强制 Inactive + Theme::applyLineEditStyle()\n");
        if (s.ok) {
            out << QStringLiteral("    底色=%1  笔画=%2  与底色差=%3  %4\n")
                       .arg(rgb(s.bg), rgb(s.glyph)).arg(s.deltaLum)
                       .arg(s.deltaLum >= 25 ? QStringLiteral("[亮灰  OK]") : QStringLiteral("[仍暗  FAIL]"));
            if (s.deltaLum < 25) ++g_fail;
        } else { out << QStringLiteral("    [!] 采样失败\n"); ++g_fail; }
        delete host;
    }
    out << QStringLiteral("\n");

    // ①d 【关键】控件级样式表之后，应用级样式表再下发一次（= 用户改主题色/透明度的真实时序）
    {
        QLineEdit* edit = nullptr;
        QFrame*    host = makeHost(&edit);            // 只有控件级样式表，无 palette 干预
        ThemeManager::instance()->setAccentColor(QColor(0xFF, 0x8A, 0x3D));
        republishAppQss(app);                        // main.cpp: themeChanged → a.setStyleSheet()
        const Sample s = sampleGlyphs(host, edit);
        out << QStringLiteral("①d 【关键】控件级样式表之后，应用级样式表再下发一次\n");
        if (s.ok) {
            out << QStringLiteral("    底色=%1  笔画=%2  与底色差=%3\n")
                       .arg(rgb(s.bg), rgb(s.glyph)).arg(s.deltaLum);
            out << QStringLiteral("    → %1\n")
                       .arg(s.deltaLum < 0
                                ? QStringLiteral("**复现成功**：应用级重刷把占位色打回半透明黑 （= 产品真因）")
                                : QStringLiteral("仍是亮灰：该时序不是真因"));
        } else { out << QStringLiteral("    [!] 采样失败\n"); }
        dumpPalette(out, QStringLiteral("①d 该状态下调色板"), edit->palette());
        delete host;
        ThemeManager::instance()->setAccentColor(QColor(0x22, 0xD3, 0xEE));
        republishAppQss(app);
    }
    out << QStringLiteral("\n");

    // ①e 同一时序 + 修复（applyLineEditStyle）
    {
        QLineEdit* edit = nullptr;
        QFrame*    host = makeHost(&edit);
        Theme::applyLineEditStyle(edit);             // 修复
        ThemeManager::instance()->setAccentColor(QColor(0xFF, 0x8A, 0x3D));
        republishAppQss(app);                        // 再被打一次应用级重刷
        const Sample s = sampleGlyphs(host, edit);
        out << QStringLiteral("①e ①d 时序 + Theme::applyLineEditStyle()（修复是否扛住）\n");
        if (s.ok) {
            out << QStringLiteral("    底色=%1  笔画=%2  与底色差=%3  %4\n")
                       .arg(rgb(s.bg), rgb(s.glyph)).arg(s.deltaLum)
                       .arg(s.deltaLum >= 25 ? QStringLiteral("[亮灰，扛住了  OK]")
                                             : QStringLiteral("[被冲回暗字  FAIL]"));
            if (s.deltaLum < 25) ++g_fail;
        } else { out << QStringLiteral("    [!] 采样失败\n"); ++g_fail; }
        delete host;
        ThemeManager::instance()->setAccentColor(QColor(0x22, 0xD3, 0xEE));
        republishAppQss(app);
    }
    out << QStringLiteral("\n");

    // ② 控件级 palette（在 setStyleSheet 之后设）
    {
        QLineEdit* edit = nullptr;
        QFrame*    host = makeHost(&edit);
        Theme::applyLineEditStyle(edit);            // 产品修复路径：样式表 + palette 一起设
        const Sample s = sampleGlyphs(host, edit);
        glyphRef = s.glyph;
        report(out, QStringLiteral("② 控件级修复 Theme::applyLineEditStyle()"), s, 25);
        delete host;
    }
    out << QStringLiteral("\n");

    // ③ 控件级修复能否扛住「改主题色 + 全局 QSS 重下发」
    {
        QLineEdit* edit = nullptr;
        QFrame*    host = makeHost(&edit);
        Theme::applyLineEditStyle(edit);

        ThemeManager::instance()->setAccentColor(QColor(0xFF, 0x8A, 0x3D));   // 换橙色主题
        republishAppQss(app);                                                // 全应用 repolish

        const Sample s = sampleGlyphs(host, edit);
        report(out, QStringLiteral("③ ②之后「换主题色 + 全局 QSS 重下发」（模拟 themeChanged）"), s, 25);
        if (s.ok && dist(s.glyph, glyphRef) > 3) {
            out << QStringLiteral("    → 笔画色也变了（%1 → %2）：控件级 palette 被全局 repolish 冲掉\n")
                       .arg(rgb(glyphRef)).arg(rgb(s.glyph));
        }
        delete host;
    }
    out << QStringLiteral("\n");

    // ⑦ A/B 对照：占位色被"重抛光/调色板重置"冲掉后，守卫能不能救回来
    // ⑦a 无守卫（= 只在创建时设一次的老做法）→ 冲掉后应变黑
    // ⑦b 有守卫（applyLineEditStyle）→ 冲掉后应自动恢复灰
    {
        QLineEdit* edit = nullptr;
        QFrame*    host = makeHost(&edit);
        QPalette   p = edit->palette();
        p.setColor(QPalette::PlaceholderText, Theme::placeholderTextColor());
        edit->setPalette(p);                       // 只设一次，不装守卫（对照组）
        edit->setPalette(QPalette());              // 模拟重抛光把调色板打回默认
        const Sample s = sampleGlyphs(host, edit);
        out << QStringLiteral("⑦a 对照（无守卫）：设一次 palette 后被重置\n");
        if (s.ok) {
            out << QStringLiteral("    笔画=%1  与底色差=%2  → %3\n")
                       .arg(rgb(s.glyph)).arg(s.deltaLum)
                       .arg(s.deltaLum < 0 ? QStringLiteral("确实被打回半透明黑（证明重置有效）")
                                           : QStringLiteral("未被打回（重置无效，⑦b 结论不可信）"));
            if (s.deltaLum >= 0) ++g_fail;
        } else { out << QStringLiteral("    [!] 采样失败\n"); ++g_fail; }
        delete host;
    }
    {
        QLineEdit* edit = nullptr;
        QFrame*    host = makeHost(&edit);
        Theme::applyLineEditStyle(edit);           // 有守卫
        edit->setPalette(QPalette());              // 同样打回默认
        const Sample s = sampleGlyphs(host, edit);
        out << QStringLiteral("⑦b 有守卫：同样被重置，守卫是否自动恢复\n");
        if (s.ok) {
            out << QStringLiteral("    笔画=%1  与底色差=%2  → %3\n")
                       .arg(rgb(s.glyph)).arg(s.deltaLum)
                       .arg(s.deltaLum >= 25 ? QStringLiteral("守卫自动恢复成禁用灰  OK")
                                             : QStringLiteral("没恢复  FAIL"));
            if (s.deltaLum < 25) ++g_fail;
        } else { out << QStringLiteral("    [!] 采样失败\n"); ++g_fail; }
        delete host;
    }
    out << QStringLiteral("\n");

    // ④ 应用级 palette 方案（一次性设 PlaceholderText，零副作用）
    {
        QPalette pal = app.palette();
        pal.setColor(QPalette::PlaceholderText, Theme::placeholderTextColor());
        app.setPalette(pal);

        QLineEdit* edit = nullptr;
        QFrame*    host = makeHost(&edit);
        const Sample s = sampleGlyphs(host, edit);
        report(out, QStringLiteral("④ 应用级 palette（app.setPalette 只设 PlaceholderText 一个角色）"), s, 25);
        delete host;
    }
    out << QStringLiteral("\n");

    // ⑤ 应用级方案扛重刷 + 不跟随主题色
    {
        ThemeManager::instance()->setAccentColor(QColor(0x22, 0xD3, 0xEE));   // 切回青色
        republishAppQss(app);

        QLineEdit* edit = nullptr;
        QFrame*    host = makeHost(&edit);
        const Sample s = sampleGlyphs(host, edit);
        report(out, QStringLiteral("⑤ 应用级方案 + 再换主题色 + 全局 QSS 重下发"), s, 25);
        if (s.ok) {
            out << QStringLiteral("    → 与 ② 控件级方案笔画色差 = %1  %2\n")
                       .arg(dist(s.glyph, glyphRef))
                       .arg(dist(s.glyph, glyphRef) <= 6
                                ? QStringLiteral("[一致 → 不跟随主题色  OK]")
                                : QStringLiteral("[不一致 → 仍在跟随主题色  FAIL]"));
            if (dist(s.glyph, glyphRef) > 6) ++g_fail;
        }
        delete host;
    }
    out << QStringLiteral("\n");

    // ⑥ 正文不受影响（输入文字后仍为纯白）
    {
        QLineEdit* edit = nullptr;
        QFrame*    host = makeHost(&edit);
        edit->setText(QStringLiteral("AA"));
        const Sample s = sampleGlyphs(host, edit);
        report(out, QStringLiteral("⑥ 输入正文应仍为 #FFFFFF"), s, 0);
        if (s.ok) {
            const bool white = lum(s.glyph) >= 230;
            out << QStringLiteral("    → %1\n")
                       .arg(white ? QStringLiteral("正文仍为纯白，占位色改动无副作用  OK")
                                  : QStringLiteral("正文被改动  FAIL"));
            if (!white) ++g_fail;
        }
        delete host;
    }

    out << QStringLiteral("\n═══ 失败项数 = %1 （0 = 全部通过）═══\n").arg(g_fail);
    out.flush();
    return g_fail == 0 ? 0 : 1;
}
