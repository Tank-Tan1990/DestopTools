/*
 * @file main.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

// 弹层（QMenu / 子菜单 / QComboBox 下拉 / 表头）背景透明度「真实像素」探测。
// 为什么必须做：上一轮只做了**字符串级**验证（断言 menuStyle() 里背景 alpha 随滑块变），
// 结论是"已覆盖菜单弹框"。但用户实测发现这些弹层背景依然是实心深色 —— 字符串对 ≠ 画面对。
// 本程序的判据（不依赖任何猜测）：
// 把弹层显示在**纯白画布**上，分别在 透明度=100（背景 alpha=1.00）与 =0（alpha=0.15）两档
// 截屏读**真实像素**。若两档像素相同 → 该弹层的背景 alpha 根本没参与合成（= 没被控制）；
// 若明显变亮 → 生效。
// 同时打印 WA_TranslucentBackground / WS_EX_LAYERED，用于印证"窗口是否支持 alpha 合成"。
// 不链接真实 thememanager.cpp（它把 Dock 钩子静态函数一起拖进来），用最小替身，
// 因此验证的仍是**产品头文件 theme.h 里的真实样式生成逻辑**。
#include <QApplication>
#include <QWidget>
#include <QMenu>
#include <QTableWidget>
#include <QHeaderView>
#include <QScreen>
#include <QTimer>
#include <QEventLoop>
#include <QImage>
#include <QPixmap>
#include <QPair>
#include <QPoint>
#include <QVector>
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

int g_fail = 0;

void wait(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// 平均值采样：取 5x5 平均值，避开抗锯齿/文字边缘的单点噪声
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
    if (!n) return QColor();
    return QColor(int(r / n), int(g / n), int(b / n));
}

QString rgbStr(const QColor& c) {
    if (!c.isValid()) return QStringLiteral("(取色失败)");
    return QStringLiteral("rgb(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue());
}

QPair<QString, QString> winFlags(QWidget* w) {
    if (!w) return qMakePair(QStringLiteral("-"), QStringLiteral("-"));
    const HWND h = reinterpret_cast<HWND>(w->winId());
    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    return qMakePair(w->testAttribute(Qt::WA_TranslucentBackground) ? QStringLiteral("yes")
                                                                    : QStringLiteral("no"),
                     (ex & WS_EX_LAYERED) ? QStringLiteral("yes") : QStringLiteral("no"));
}

// 产品 settingcenterdialog.cpp 里规则表的样式模板（逐字复制，保证测的是同一条串）
QString ruleTableStyle() {
    QString s = Theme::applyTokens(QStringLiteral(
        "QTableWidget { background-color: TABLE_BG_COLOR; border: 1px solid rgba(34,211,238,0.18); gridline-color: transparent; color: #FFFFFF; }"
        "QHeaderView::section { background-color: TABLE_BG_COLOR; color: #FFFFFF; padding: 6px; border: none; border-bottom: 1px solid rgba(34,211,238,0.18); }"
        "QTableWidget::item { background: transparent; border-bottom: 1px solid rgba(255,255,255,0.06); padding: 4px; color: #FFFFFF; }"));
    s.replace(QStringLiteral("TABLE_BG_COLOR"), Theme::windowBgString(1.0));
    return s;
}

struct Rec {
    QString name, translucent, layered, px100, px015, extra;
    bool    changed = false;
};

QVector<Rec> g_recs;

}   // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTextStream out(stdout);

    ThemeManager* tm = ThemeManager::instance();
    tm->setAccentColor(QColor(QStringLiteral("#22D3EE")));

    // 纯白画布：alpha 若生效，深色弹层会被白底明显冲淡 → 与"不生效"差异巨大，判据清晰
    QWidget host;
    host.setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    host.setStyleSheet(QStringLiteral("background:#FFFFFF;"));
    host.setGeometry(80, 80, 1000, 760);
    host.show();
    wait(450);

    out << QStringLiteral("画布基线（应为 rgb(255,255,255)）: %1\n\n")
               .arg(rgbStr(screenPixel(600, 720)));
    out.flush();

    // 菜单两档探测：同一采样点、只变透明度，比较像素
    // setup    ：每档 popup 前调用（按档位重设样式/属性）
    // afterShow：popup 后调用（用于"窗口已创建后才能做"的 Win32 干预）
    auto probeWidget = [&](const QString& name, QWidget* popup, const QPoint& sampleOff,
                           const QString& extra,
                           std::function<void()> setup     = nullptr,
                           std::function<void()> afterShow = nullptr) {
        Rec r;
        r.name  = name;
        r.extra = extra;
        for (int pass = 0; pass < 2; ++pass) {
            tm->setTransparency(pass == 0 ? 100 : 0);
            if (setup) setup();
            if (QMenu* m = qobject_cast<QMenu*>(popup)) {
                m->setStyleSheet(Theme::menuStyle());
                m->popup(host.mapToGlobal(QPoint(60, 60)));
            } else {
                popup->show();
            }
            if (afterShow) { afterShow(); wait(200); }
            wait(320);
            const QPoint tl = popup->mapToGlobal(QPoint(0, 0));
            const QColor c  = screenPixel(tl.x() + sampleOff.x(), tl.y() + sampleOff.y());
            if (pass == 0) r.px100 = rgbStr(c);
            else           r.px015 = rgbStr(c);
            const QPair<QString, QString> f = winFlags(popup);
            r.translucent = f.first;
            r.layered     = f.second;
            popup->close();
            wait(220);
        }
        r.changed = (r.px100 != r.px015);
        g_recs.append(r);

        out << QStringLiteral("%1").arg(name, -40)
            << QStringLiteral(" translucent=") << r.translucent
            << QStringLiteral(" layered=")     << r.layered
            << QStringLiteral("  t=100→")      << r.px100
            << QStringLiteral("  t=0→")        << r.px015
            << (r.changed ? QStringLiteral("   [背景 alpha 生效]")
                          : QStringLiteral("   [背景 alpha 未生效 ←]"))
            << QStringLiteral("\n");
        if (!extra.isEmpty()) out << QStringLiteral("      · ") << extra << QStringLiteral("\n");
        out.flush();
    };

    // 采样点统一取菜单底部的**纯背景留白**（下面每个菜单都是「仅 1 个条目 + 固定 190x120」），
    // 避开 item 的选中/悬停高亮 —— 否则高亮色会掩盖 alpha 的真实变化（第一版就踩过：
    // 采样点落在高亮项上，② 读到偏青的 (20,60,66)，差点误判成"生效"）。
    const QPoint kBlank(95, 100);

    // ① 主菜单：现状（仅 menuStyle()）
    {
        auto* m = new QMenu(&host);
        m->setFixedSize(190, 120);
        m->addAction(QStringLiteral("菜单项一"));
        probeWidget(QStringLiteral("① QMenu 现状（仅 menuStyle()）"), m, kBlank,
                    QStringLiteral("产品中所有右键菜单的当前形态"));
        delete m;
    }

    // ② 主菜单：显式开启 WA_TranslucentBackground
    {
        auto* m = new QMenu(&host);
        m->setFixedSize(190, 120);
        m->setAttribute(Qt::WA_TranslucentBackground, true);
        m->addAction(QStringLiteral("菜单项一"));
        probeWidget(QStringLiteral("② QMenu + WA_TranslucentBackground"), m, kBlank,
                    QStringLiteral("候选修复方案：让弹层支持 alpha 合成"));
        delete m;
    }

    // ③ 子菜单（提醒管理 / 字体设置 / 优先级 那一类）
    {
        auto* m = new QMenu(&host);
        m->setFixedWidth(190);
        m->addAction(QStringLiteral("菜单项一"));
        QMenu* sub = m->addMenu(QStringLiteral("子菜单"));
        sub->setFixedSize(190, 120);
        sub->addAction(QStringLiteral("子项一"));
        probeWidget(QStringLiteral("③ 子菜单 现状"), sub, kBlank,
                    QStringLiteral("桌面截图中显示为纯黑/浅灰的那一类"));
        delete m;
    }

    // 以下候选同样用「仅 1 个条目 + 固定 190x120 高」的菜单（采样点见上面的 kBlank）。

    // ⑧ QMenu 不带 parent
    // QMenu(parent) 有 owner 关系；普通 QWidget 对照能拿到 layered，差别可能就在这里
    {
        auto* m = new QMenu(nullptr);
        m->setFixedSize(190, 120);
        m->setAttribute(Qt::WA_TranslucentBackground, true);
        m->addAction(QStringLiteral("菜单项一"));
        probeWidget(QStringLiteral("⑧ QMenu(无 parent) +Translucent"), m, kBlank,
                    QStringLiteral("去掉 owner 关系"));
        delete m;
    }

    // ⑨ QMenu：先强制建窗口 → 设属性 → 再重建平台窗口
    // Qt 文档要求 WA_TranslucentBackground 必须在窗口创建前设置；若 QMenu 已提前建窗，
    // 就必须靠 setWindowFlags 触发平台窗口重建，让该属性真正落到 CreateWindowEx 上。
    {
        auto* m = new QMenu(&host);
        m->setFixedSize(190, 120);
        m->winId();                                   // 确保此刻已存在平台窗口
        m->setAttribute(Qt::WA_TranslucentBackground, true);
        m->setWindowFlags(m->windowFlags());          // 触发平台窗口重建
        m->addAction(QStringLiteral("菜单项一"));
        probeWidget(QStringLiteral("⑨ QMenu +Translucent +重建窗口"), m, kBlank,
                    QStringLiteral("windowFlags 重建后再看 layered"));
        delete m;
    }

    // ⑫ QMenu：popup 之后绕过 Qt，直接给窗口加 WS_EX_LAYERED
    {
        auto* m = new QMenu(&host);
        m->setFixedSize(190, 120);
        m->addAction(QStringLiteral("菜单项一"));
        probeWidget(QStringLiteral("⑫ QMenu + popup 后手动 WS_EX_LAYERED"), m, kBlank,
                    QStringLiteral("窗口已创建后由 Win32 直接补 alpha 合成位"),
                    nullptr,
                    [m]() {
                        const HWND h = reinterpret_cast<HWND>(m->winId());
                        const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
                        SetWindowLongPtrW(h, GWL_EXSTYLE, ex | WS_EX_LAYERED);
                        SetWindowPos(h, nullptr, 0, 0, 0, 0,
                                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
                    });
        delete m;
    }

    // ⑪ 保底方案：整窗不透明度 setWindowOpacity
    // 它的判据不同：与"两档透明度像素不同"无关，而是看 0.35 相比 1.0 是否**变亮**（真的与白底混合）
    {
        Rec r;
        r.name = QStringLiteral("⑪ QMenu + setWindowOpacity(0.35)（保底方案）");
        auto* m = new QMenu(&host);
        m->setFixedSize(190, 120);
        m->setStyleSheet(Theme::menuStyle());
        m->addAction(QStringLiteral("菜单项一"));
        for (int pass = 0; pass < 2; ++pass) {
            m->setWindowOpacity(pass == 0 ? 1.0 : 0.35);
            m->popup(host.mapToGlobal(QPoint(60, 60)));
            wait(340);
            const QPoint tl = m->mapToGlobal(QPoint(0, 0));
            const QColor c  = screenPixel(tl.x() + kBlank.x(), tl.y() + kBlank.y());
            if (pass == 0) r.px100 = rgbStr(c);
            else           r.px015 = rgbStr(c);
            const QPair<QString, QString> f = winFlags(m);
            r.translucent = f.first;
            r.layered     = f.second;
            m->close();
            wait(220);
        }
        r.changed = (r.px100 != r.px015);
        g_recs.append(r);
        out << QStringLiteral("%1").arg(r.name, -40)
            << QStringLiteral(" translucent=") << r.translucent
            << QStringLiteral(" layered=")     << r.layered
            << QStringLiteral("  opacity=1.0→") << r.px100
            << QStringLiteral("  opacity=0.35→") << r.px015
            << (r.changed ? QStringLiteral("   [与底层混合 → 真透明]")
                          : QStringLiteral("   [无变化]"))
            << QStringLiteral("\n      · ") << QStringLiteral("若此项变亮，说明它就是唯一可用的真透明通道")
            << QStringLiteral("\n");
        out.flush();
        delete m;
    }

    // ④ 表头（设置中心 → 桌面整理 → 整理规则 表）
    {
        auto* table = new QTableWidget(3, 2, &host);
        table->setHorizontalHeaderLabels(QStringList() << QString() << QString());  // 空标签 → 表头无文字，采样更干净
        table->verticalHeader()->setVisible(false);
        table->setStyleSheet(ruleTableStyle());
        table->setGeometry(80, 380, 700, 300);
        table->show();
        wait(350);

        QHeaderView* hh = table->horizontalHeader();
        Rec r;
        r.name = QStringLiteral("④ QHeaderView::section（规则表表头）");
        for (int pass = 0; pass < 2; ++pass) {
            tm->setTransparency(pass == 0 ? 100 : 0);
            table->setStyleSheet(ruleTableStyle());
            wait(320);
            const QPoint htl = hh->mapToGlobal(QPoint(0, 0));
            const QColor ch  = screenPixel(htl.x() + hh->width() / 2, htl.y() + hh->height() / 2);
            if (pass == 0) r.px100 = rgbStr(ch);
            else           r.px015 = rgbStr(ch);
            // 顺带取一个数据行像素做对照（表头 vs 表体是否一致）
            const QColor cb = screenPixel(htl.x() + hh->width() / 2, htl.y() + hh->height() + 22);
            if (pass == 0) r.extra = QStringLiteral("表体首行同档像素=") + rgbStr(cb);
            const QPair<QString, QString> f = winFlags(table);
            r.translucent = f.first;
            r.layered     = f.second;
        }
        r.changed = (r.px100 != r.px015);
        g_recs.append(r);
        out << QStringLiteral("%1").arg(r.name, -40)
            << QStringLiteral(" translucent=") << r.translucent
            << QStringLiteral(" layered=")     << r.layered
            << QStringLiteral("  t=100→")      << r.px100
            << QStringLiteral("  t=0→")        << r.px015
            << (r.changed ? QStringLiteral("   [背景 alpha 生效]")
                          : QStringLiteral("   [背景 alpha 未生效 ←]"))
            << QStringLiteral("\n")
            << QStringLiteral("      · ") << r.extra << QStringLiteral("\n");
        out.flush();
        table->hide();
    }

    // 汇总
    out << QStringLiteral("\n================ 汇总 ================\n");
    int notChanged = 0;
    for (const Rec& r : g_recs) {
        if (!r.changed) ++notChanged;
        out << QStringLiteral("%1  %2\n")
                   .arg(r.changed ? QStringLiteral("[生效]  ") : QStringLiteral("[未生效]"),
                        r.name);
    }
    out << QStringLiteral("未生效项数 = %1\n").arg(notChanged);
    out.flush();

    g_fail = notChanged;
    return g_fail;
}
