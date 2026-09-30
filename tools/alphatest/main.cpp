// 最小验证程序：确认「透明度滑块只改背景 alpha，不改线框 alpha」这条核心逻辑。
// 为什么需要它：本改动把透明度从 setWindowOpacity（整窗）搬到样式串里的**背景色**上，
// 而"背景色"与"线框色"共用同一批 QSS 字符串。任何一处误用都会让圆角线框跟着变淡 ——
// 这种事靠读代码看不出来（几十个 rgba 字面量），必须跑一遍把数字打出来。
// 不链接真实 thememanager.cpp：它 include 了 desktopmirrorwindow.h / icongridwindow.h
// 的钩子静态函数，会把整个应用拖进来。这里给一个最小替身，只实现 theme.h 真正用到的三件事
// （instance / 透明度 / effectiveOpacity），因此验证的是**产品头文件里的真实逻辑**。
#include <QCoreApplication>
#include <QTextStream>
#include "theme.h"
#include "thememanager.h"

// ThemeManager 最小替身（见文件头说明）
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

void check(bool ok, const QString& what) {
    if (!ok) ++g_fail;
    QTextStream(stdout) << (ok ? "  [OK]   " : "  [FAIL] ") << what << "\n";
}

// 取字符串中【第一段】"rgba(r,g,b,a)" 的 a。
// 不能简单地取"整串最后一个逗号"——像 windowStyle()/menuStyle() 这种串里
// 背景在前、描边在后，两者都是 rgba，取最后一个逗号会读到**描边**的 alpha，
// 于是"背景跟随"这条断言会假失败（本程序第一版就踩了这个坑）。
QString firstAlpha(const QString& s) {
    const int i = s.indexOf(QStringLiteral("rgba("));
    if (i < 0) return QStringLiteral("?");
    const int j = s.indexOf(QLatin1Char(')'), i);
    if (j < 0) return QStringLiteral("?");
    const QString inner = s.mid(i + 5, j - i - 5);
    const int k = inner.lastIndexOf(QLatin1Char(','));
    return k < 0 ? QStringLiteral("?") : inner.mid(k + 1).trimmed();
}

// 取描边（主题色 34,211,238）的 alpha —— 用来证明线框不随滑块变化
QString accentBorderAlpha(const QString& s) {
    const int bi = s.indexOf(QStringLiteral("34,211,238,"));
    if (bi < 0) return QStringLiteral("?");
    const int j = s.indexOf(QLatin1Char(')'), bi);
    // "34,211,238," 恰好 11 个字符 → alpha 从 bi+11 起
    return (j < 0) ? QStringLiteral("?") : s.mid(bi + 11, j - bi - 11).trimmed();
}

}   // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    ThemeManager* tm = ThemeManager::instance();
    tm->setAccentColor(QColor(QStringLiteral("#22D3EE")));

    // 需求：滑块往下拖 → 背景越来越透；圆角线框**始终**保持设计值。
    // 逐档打印，肉眼与断言双通道。
    struct Row { int t; QString bgWindow, bgPanel, bgDeep, glassAlpha, border, winBg, winBorder; };
    QVector<Row> rows;

    for (int t : {100, 80, 60, 40, 20, 0}) {
        tm->setTransparency(t);
        Row r;
        r.t          = t;
        r.bgWindow   = firstAlpha(Theme::windowBgString(1.0));
        r.bgPanel    = firstAlpha(Theme::panelBgString(1.0));
        r.bgDeep     = firstAlpha(Theme::deepBgString(1.0));
        r.glassAlpha = QString::number(Theme::windowGlass().alpha());
        r.border     = QString::number(Theme::alphaF(0.22), 'f', 2);
        // windowStyle() = 窗口底（背景）+ 1px 描边（线框），一次打印两者，最直观
        const QString ws = Theme::windowStyle();
        r.winBg     = firstAlpha(ws);
        r.winBorder = accentBorderAlpha(ws);
        rows.append(r);
    }

    out << "transparency | windowBg | panelBg | deepBg | glassColor.a | alphaF(0.22) border | windowStyle bg | windowStyle border\n";
    for (const Row& r : rows) {
        out << QStringLiteral("%1").arg(r.t, 12)
            << " | " << r.bgWindow.leftJustified(8)
            << " | " << r.bgPanel.leftJustified(7)
            << " | " << r.bgDeep.leftJustified(6)
            << " | " << r.glassAlpha.leftJustified(12)
            << " | " << r.border.leftJustified(24)
            << " | " << r.winBg.leftJustified(14)
            << " | " << r.winBorder << "\n";
    }
    out << "\n";

    // 断言 1：背景 alpha 必须随透明度单调递减（100 档 = 1.00）
    out << "[1] background alpha follows the slider\n";
    check(rows.first().bgWindow == QStringLiteral("1.00"), "transparency=100 -> windowBg alpha == 1.00");
    for (int i = 1; i < rows.size(); ++i) {
        check(rows[i].bgWindow.toDouble() < rows[i - 1].bgWindow.toDouble(),
              QStringLiteral("t=%1 windowBg alpha < t=%2").arg(rows[i].t).arg(rows[i - 1].t));
    }
    check(rows.last().bgWindow.toDouble() >= 0.15, "t=0 windowBg alpha >= 0.15 (floor kept)");

    // 断言 2：线框 alpha 任何档位都必须恒定（且等于设计值）
    out << "[2] border alpha NEVER changes\n";
    for (const Row& r : rows) {
        check(r.border == QStringLiteral("0.22"), QStringLiteral("t=%1 alphaF(0.22) == 0.22").arg(r.t));
        check(r.winBorder == QStringLiteral("0.20"),
              QStringLiteral("t=%1 windowStyle border == 0.20").arg(r.t));
    }

    // 断言 3：panel / deep / glass 走同一条通道，不能有漏网
    out << "[3] every background helper is on the same channel\n";
    check(rows.first().bgPanel == QStringLiteral("1.00") && rows.last().bgPanel.toDouble() < 1.0,
          "panelBgString follows the slider");
    check(rows.first().bgDeep == QStringLiteral("1.00") && rows.last().bgDeep.toDouble() < 1.0,
          "deepBgString follows the slider");
    check(rows.first().glassAlpha == QStringLiteral("255") && rows.last().glassAlpha.toInt() < 255,
          "windowGlass() (QColor path) follows the slider");

    // 断言 4：applyTokens 把裸字面量换成同一串 → 全应用背景一次性收敛
    out << "[4] applyTokens funnel maps every raw literal to the same string\n";
    tm->setTransparency(50);
    check(Theme::applyTokens(QStringLiteral("rgba(17,26,46,1.0)")) == Theme::windowBgString(1.0),
          "applyTokens(rgba(17,26,46,1.0)) == windowBgString(1.0)");
    check(Theme::applyTokens(QStringLiteral("rgba(11,16,33,1.0)")) == Theme::panelBgString(1.0),
          "applyTokens(rgba(11,16,33,1.0)) == panelBgString(1.0)");
    check(firstAlpha(Theme::applyTokens(QStringLiteral("rgba(17,26,46,1.0)"))) == QStringLiteral("0.50"),
          "t=50 -> background literal becomes alpha 0.50");

    // 断言 5：菜单 / 弹框样式走同一函数 → 自动跟随（需求里的"菜单弹框"）
    out << "[5] popup (QMenu / combo view) backgrounds follow too\n";
    const QString menu = Theme::menuStyle();
    check(firstAlpha(menu) == QStringLiteral("0.50"), "menuStyle() background alpha == 0.50 at t=50");
    check(accentBorderAlpha(menu) == QStringLiteral("0.28"), "menuStyle() border stays 0.28");

    out << "\n================ RESULT: " << (g_fail == 0 ? "ALL PASS" : QStringLiteral("%1 FAIL").arg(g_fail))
        << " ================\n";
    return g_fail == 0 ? 0 : 1;
}
