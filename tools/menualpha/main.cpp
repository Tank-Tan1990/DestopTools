/*
 * @file main.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

// 验证 QMenu 背景透明度的「真实画面」修复方案。
// 前置事实（tools/popupprobe + tools/wndprobe 实测）：
// · QMenu 的 WS_EX_LAYERED 恒为 no → 它的半透明背景只能在**黑色 backing store** 上合成，
// 所以透明度调低时菜单只是"更黑"，永远透不出后面的桌面（用户看到的就是"没被控制"）；
// · 同机同 Qt，普通 QWidget（Qt::Window / Qt::Popup）+ WA_TranslucentBackground 都能拿到
// LAYERED=yes → 机制本身可用；
// · 只有「先 setWindowFlags 触发平台窗口重建、再设 Translucent」能拿到 LAYERED=yes。
// 原因：QMenu 在构造/首次 winId 时**已建好平台窗口**，之后设 WA_TranslucentBackground
// 影响不到已经 CreateWindowEx 的窗口（Qt 只在创建时按该属性决定 alpha buffer 与 LAYERED）。
// 本程序做**真实像素**终验：把菜单弹在纯白画布上，比较 透明度=100（背景 alpha=1.00）与
// 0（alpha=0.15）两档像素。若低档**明显变亮**（与白底混合）→ 真的透出桌面 ；
// 若仍是黑底（变暗）→ 方案无效 。
#include <QApplication>
#include <QWidget>
#include <QMenu>
#include <QScreen>
#include <QTimer>
#include <QEventLoop>
#include <QImage>
#include <QPixmap>
#include <QTableWidget>
#include <QHeaderView>
#include <QComboBox>
#include <QToolButton>
#include <QTextStream>
#include <QtGlobal>

#include <functional>

#include <windows.h>

#include "theme.h"
#include "thememanager.h"

// ThemeManager 最小替身（同 tools/alphatest）
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

void wait(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

QColor screenPixel(int x, int y) {
    const QPixmap pm = QGuiApplication::primaryScreen()->grabWindow(0, x - 2, y - 2, 5, 5);
    const QImage im = pm.toImage();
    if (im.isNull() || im.width() < 1) return QColor();
    qint64 r = 0, g = 0, b = 0;
    int n = 0;
    for (int yy = 0; yy < im.height(); ++yy) {
        for (int xx = 0; xx < im.width(); ++xx) {
            const QColor c = im.pixelColor(xx, yy);
            r += c.red(); g += c.green(); b += c.blue(); ++n;
        }
    }
    return n ? QColor(int(r / n), int(g / n), int(b / n)) : QColor();
}

QString rgb(const QColor& c) {
    return c.isValid()
        ? QStringLiteral("rgb(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue())
        : QStringLiteral("?");
}

int lum(const QColor& c) {
    return (c.red() * 299 + c.green() * 587 + c.blue() * 114) / 1000;
}

bool layeredOf(QWidget* w) {
    const HWND h = reinterpret_cast<HWND>(w->winId());
    return (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
}

int g_fail = 0;

// 产品 settingcenterdialog.cpp 里规则表的样式模板（逐字复制，保证测的是同一条串）
QString ruleTableStyle() {
    QString s = Theme::applyTokens(QStringLiteral(
        "QTableWidget { background-color: TABLE_BG_COLOR; border: 1px solid rgba(34,211,238,0.18); gridline-color: transparent; color: #FFFFFF; }"
        "QHeaderView::section { background-color: TABLE_BG_COLOR; color: #FFFFFF; padding: 6px; border: none; border-bottom: 1px solid rgba(34,211,238,0.18); }"
        "QTableWidget::item { background: transparent; border-bottom: 1px solid rgba(255,255,255,0.06); padding: 4px; color: #FFFFFF; }"));
    s.replace(QStringLiteral("TABLE_BG_COLOR"), Theme::windowBgString(1.0));
    return s;
}

}   // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTextStream out(stdout);

    // 兜底：万一某个 popup 卡住，90 秒后强制退出（输出实时 flush，结论不会丢）
    QTimer::singleShot(90000, []() { ::ExitProcess(0); });

    ThemeManager* tm = ThemeManager::instance();
    tm->setAccentColor(QColor(QStringLiteral("#22D3EE")));

    QWidget host;
    host.setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    host.setStyleSheet(QStringLiteral("background:#FFFFFF;"));
    host.setGeometry(80, 80, 1000, 760);
    host.show();
    wait(450);

    out << QStringLiteral("白底基线: %1\n").arg(rgb(screenPixel(600, 720)));
    {
        QMenu probeFlags;
        out << QStringLiteral("QMenu 默认 windowFlags = 0x%1\n\n")
                   .arg(quint32(probeFlags.windowFlags()), 8, 16, QLatin1Char('0'));
    }
    out.flush();

    const QPoint kBlank(95, 100);   // 菜单底部纯背景留白（菜单 = 1 条目 + 固定 190x120）

    auto runCase = [&](const QString& name, const std::function<void(QMenu*)>& configure) {
        QMenu m(&host);
        m.setFixedSize(190, 120);
        m.addAction(QStringLiteral("菜单项一"));
        configure(&m);                       // 必须在首次 popup（建窗）之前完成设置

        QColor c100, c015;
        bool   layered = false;
        for (int pass = 0; pass < 2; ++pass) {
            tm->setTransparency(pass == 0 ? 100 : 0);
            m.setStyleSheet(Theme::menuStyle());
            m.popup(host.mapToGlobal(QPoint(60, 60)));
            wait(340);
            const QPoint tl = m.mapToGlobal(QPoint(0, 0));
            const QColor c  = screenPixel(tl.x() + kBlank.x(), tl.y() + kBlank.y());
            if (pass == 0) c100 = c;
            else           c015 = c;
            layered = layeredOf(&m);
            m.close();
            wait(200);
        }

        const bool brighter = lum(c015) > lum(c100) + 40;
        if (!brighter) ++g_fail;

        out << QStringLiteral("%1\n").arg(name)
            << QStringLiteral("    LAYERED=%1   t=100→%2   t=0→%3   %4\n")
                   .arg(layered ? QStringLiteral("Y") : QStringLiteral("N"),
                        rgb(c100), rgb(c015),
                        brighter ? QStringLiteral("[变亮 → 真的透出桌面  OK]")
                                 : QStringLiteral("[未变亮 → 仍是黑底合成  FAIL]"));
        out.flush();
    };

    runCase(QStringLiteral("① 现状：仅 menuStyle()"),
            [](QMenu*) {});

    runCase(QStringLiteral("② 方案D：显式 flags(Popup|Frameless|NoDropShadow) + Translucent"),
            [](QMenu* m) {
                m->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
                m->setAttribute(Qt::WA_TranslucentBackground, true);
            });

    runCase(QStringLiteral("③ 方案D'：显式 flags(Popup|Frameless) + Translucent"),
            [](QMenu* m) {
                m->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
                m->setAttribute(Qt::WA_TranslucentBackground, true);
            });

    runCase(QStringLiteral("④ 方案D''：原 flags 触发重建 + Translucent"),
            [](QMenu* m) {
                m->setWindowFlags(m->windowFlags());     // 触发平台窗口重建（不改语义）
                m->setAttribute(Qt::WA_TranslucentBackground, true);
            });

    // ⑧ 关键：直接跑**产品实际调用的那条路径**（Theme::applyMenuStyle），
    // 确保产品里那个 inline 函数的 flags 组合与实验结论一致。
    runCase(QStringLiteral("⑧ 产品路径：Theme::applyMenuStyle(&menu)  【最终验收项】"),
            [](QMenu* m) { Theme::applyMenuStyle(m); });

    // 表头（设置中心 → 桌面整理 → 整理规则 表的表头）
    // 假设：QHeaderView 继承 QAbstractItemView，其 viewport 默认会先铺一层**不透明**底，
    // 而 `QHeaderView::section` 是子控件规则，关不掉那层底 → section 的半透明背景叠在
    // 不透明底上，看起来就是不透明（用户截图里表头比表体明显更实）。
    // 对照：额外给 QHeaderView 本体一条 background: 透明 规则。
    auto runHeaderCase = [&](const QString& name, const QString& extraQss) {
        auto* table = new QTableWidget(3, 4, &host);
        table->verticalHeader()->setVisible(false);
        table->setHorizontalHeaderLabels(QStringList()
            << QStringLiteral("类型") << QStringLiteral("包含后缀")
            << QStringLiteral("选择桌面分区") << QStringLiteral("启用"));
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                table->setItem(r, c, new QTableWidgetItem(QStringLiteral("内容")));
        table->setGeometry(80, 380, 700, 320);
        table->show();

        QHeaderView* hh  = table->horizontalHeader();
        const int    hhH = hh->height();
        QColor h100, h015, b100, b015;
        for (int pass = 0; pass < 2; ++pass) {
            tm->setTransparency(pass == 0 ? 100 : 0);
            table->setStyleSheet(ruleTableStyle() + extraQss);
            wait(340);
            const QPoint htl = hh->mapToGlobal(QPoint(0, 0));
            // 采样点取第一列 section 的右侧空白（避开"类型"二字）
            const QColor ch = screenPixel(htl.x() + 200, htl.y() + hhH / 2);
            const QColor cb = screenPixel(htl.x() + 200, htl.y() + hhH + 22);
            if (pass == 0) { h100 = ch; b100 = cb; }
            else           { h015 = ch; b015 = cb; }
        }
        const bool hb = lum(h015) > lum(h100) + 40;
        const bool bb = lum(b015) > lum(b100) + 40;
        if (!hb) ++g_fail;

        out << QStringLiteral("%1\n").arg(name)
            << QStringLiteral("    表头(h=%1) t=100→%2  t=0→%3  %4\n")
                   .arg(hhH).arg(rgb(h100), rgb(h015),
                        hb ? QStringLiteral("[跟随透明  OK]") : QStringLiteral("[不跟随  FAIL]"))
            << QStringLiteral("    表体       t=100→%1  t=0→%2  %3\n")
                   .arg(rgb(b100), rgb(b015),
                        bb ? QStringLiteral("[跟随透明  OK]") : QStringLiteral("[不跟随  FAIL]"));
        out.flush();
        table->hide();
        delete table;
    };

    runHeaderCase(QStringLiteral("⑤ 表头 现状 QSS"), QString());
    runHeaderCase(QStringLiteral("⑥ 表头 + QHeaderView{background:transparent}"),
                  QStringLiteral("QHeaderView { background-color: transparent; border: none; }"));

    // ⑨ QComboBox 下拉弹层：与 QMenu 同属"弹层"这一类，先确认它是否也有同样的 alpha 合成问题
    // （产品里只设了 `QComboBox QAbstractItemView { background-color: TABLE_BG_COLOR }`）
    {
        auto* combo = new QComboBox(&host);
        combo->addItems(QStringList() << QStringLiteral("选项一")
                                      << QStringLiteral("选项二")
                                      << QStringLiteral("选项三"));
        combo->setGeometry(120, 690, 200, 30);
        combo->show();
        wait(220);

        QWidget* popup = combo->view()->window();   // 下拉弹层的载体（QComboBoxPrivateContainer）
        auto exOf = [](QWidget* w) -> qint64 {
            return qint64(GetWindowLongPtrW(reinterpret_cast<HWND>(w->winId()), GWL_EXSTYLE));
        };
        const bool   distinct = (popup != combo);
        const bool   layBefore = (exOf(popup) & WS_EX_LAYERED) != 0;

        // 施加与菜单完全相同的修复手法
        popup->setWindowFlags(popup->windowFlags()
                              | Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        popup->setAttribute(Qt::WA_TranslucentBackground, true);
        const bool   layAfter = (exOf(popup) & WS_EX_LAYERED) != 0;

        out << QStringLiteral("⑨ QComboBox 下拉弹层\n")
            << QStringLiteral("    载体 window() 独立于 combo = %1（应为 Y，否则取错了对象）\n")
                   .arg(distinct ? QStringLiteral("Y") : QStringLiteral("N"))
            << QStringLiteral("    修复前 LAYERED=%1   施加同款修复后 LAYERED=%2\n")
                   .arg(layBefore ? QStringLiteral("Y") : QStringLiteral("N"),
                        layAfter  ? QStringLiteral("Y") : QStringLiteral("N"));
        out.flush();
        combo->hidePopup();
        combo->hide();
        delete combo;
    }

    // ⑩ / ⑪ 产品「+ 按钮」菜单的真实路径：QToolButton::setMenu() + InstantPopup
    // 与其它菜单的唯一差别：菜单不是每次弹出时新建的局部 QMenu，而是**一次创建、长期持有**。
    // ⑩ 每次弹出前重刷样式表 → 预期跟随；
    // ⑪ 只在创建时设一次样式   → 预期停在创建那一刻（这就是用户看到的"没被控制"）。
    auto runToolButtonCase = [&](const QString& name, bool refreshStyleOnEachPopup) {
        auto* m = new QMenu(&host);
        m->setFixedSize(190, 120);
        m->addAction(QStringLiteral("菜单项一"));

        tm->setTransparency(100);                 // 创建时：不透明档
        Theme::applyMenuStyle(m);                 // createAddMenu() 就是这么调的

        auto* btn = new QToolButton(&host);
        btn->setMenu(m);
        btn->setPopupMode(QToolButton::InstantPopup);
        btn->setGeometry(500, 640, 24, 24);
        btn->show();
        wait(150);

        QColor          c100, c015;
        bool            layered = false;
        Qt::WindowFlags flagsAfterPopup;
        for (int pass = 0; pass < 2; ++pass) {
            tm->setTransparency(pass == 0 ? 100 : 0);
            if (refreshStyleOnEachPopup) m->setStyleSheet(Theme::menuStyle());

            m->popup(host.mapToGlobal(QPoint(60, 60)));
            wait(340);
            const QPoint tl = m->mapToGlobal(QPoint(0, 0));
            const QColor c  = screenPixel(tl.x() + kBlank.x(), tl.y() + kBlank.y());
            if (pass == 0) { c100 = c; flagsAfterPopup = m->windowFlags(); }
            else           { c015 = c; }
            layered = layeredOf(m);
            m->close();
            wait(220);
        }

        const bool brighter = lum(c015) > lum(c100) + 40;
        if (!brighter) ++g_fail;

        out << QStringLiteral("%1\n").arg(name)
            << QStringLiteral("    popup后 flags=0x%1   LAYERED=%2   t=100→%3   t=0→%4   %5\n")
                   .arg(quint32(flagsAfterPopup), 8, 16, QLatin1Char('0'))
                   .arg(layered ? QStringLiteral("Y") : QStringLiteral("N"),
                        rgb(c100), rgb(c015),
                        brighter ? QStringLiteral("[跟随透明度变化  OK]")
                                 : QStringLiteral("[停在旧值 → 没被控制  FAIL]"));
        out.flush();

        btn->setMenu(nullptr);
        btn->hide();
        delete btn;
        delete m;
    };

    runToolButtonCase(QStringLiteral("⑩ QToolButton 菜单 + 每次弹出手工重刷样式"), true);
    runToolButtonCase(QStringLiteral("⑪ QToolButton 菜单 + 只在创建时设一次样式（产品现状，靠 applyMenuStyle 内建的 aboutToShow 兜底）"), false);

    out << QStringLiteral("\n失败项数 = %1  （0 = 已找到可行方案）\n").arg(g_fail);
    out.flush();
    return g_fail;
}
