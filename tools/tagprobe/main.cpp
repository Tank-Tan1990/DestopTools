/*
 * @file main.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

// 真实控件试验台：收纳盒/桌面整理窗的「分类标签切换」（设置中心 → 外观设置 → 分区标签切换）
// 做法：链接产品已编译的 .obj（去掉 main.obj），跑**真实** IconGridWindow + ThemeManager
// + SettingsManager，程序化发真实鼠标事件，逐条判定。
// 覆盖矩阵（每种模式 × 两种载体）：
// 模式 0 = 点击切换（默认）：左键点击应切换；鼠标悬停**不应**切换
// 模式 1 = 悬停切换          ：左键点击应切换；鼠标悬停**应**切换（140ms 防抖后）
// 载体 A = 主窗口（桌面整理窗） / 载体 B = 独立收纳盒窗口（setIsBoxWindow(true)，用户截图 2 场景）
// 本程序会写用户 INI（IconGridWindow/currentCategory、Appearance/tagSwitchMode 等），
// 由 run_tagprobe.py 运行前备份、运行后原样恢复。
#include <QApplication>
#include <QWidget>
#include <QToolButton>
#include <QRadioButton>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QTextStream>
#include <QMap>
#include <QVector>
#include <QTimer>
#include <QEventLoop>
#include <QDebug>

#include "icongridwindow.h"
#include "desktopitem.h"
#include "thememanager.h"
#include "theme.h"
#include "settingsmanager.h"
#include "settingcenterdialog.h"   // ④ 热更新实测：真实设置中心点单选框

// 构造函数：初始化对象
// 作者：谭征
static QTextStream& O() { static QTextStream s(stdout); return s; }
static int g_fail = 0;

static void wait(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

static void setMode(int m) {
    SettingsManager sm;
    sm.saveValue(QStringLiteral("Appearance/tagSwitchMode"), m);
    sm.sync();
}

// 真实左键点击：press → （可选 dx 像素微位移）→ release。
// dx 必须小于 QApplication::startDragDistance()，否则会进入标签拖拽排序（QDrag::exec 阻塞）。
static void realClick(QToolButton* b, int dx) {
    const QPoint c  = b->rect().center();
    const QPoint gp = b->mapToGlobal(c);
    QMouseEvent press(QEvent::MouseButtonPress, c, gp, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(b, &press);
    if (dx != 0) {
        const QPoint c2 = c + QPoint(dx, 0);
        QMouseEvent move(QEvent::MouseMove, c2, b->mapToGlobal(c2), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(b, &move);
    }
    const QPoint cr = c + QPoint(dx, 0);
    QMouseEvent rel(QEvent::MouseButtonRelease, cr, b->mapToGlobal(cr), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(b, &rel);
}

static QVector<QToolButton*> collectTabs(IconGridWindow* w) {
    QVector<QToolButton*> tabs;
    const auto all = w->findChildren<QToolButton*>();
    for (QToolButton* b : all) {
        if (b->property("isCategoryTab").toBool()) tabs << b;
    }
    return tabs;
}

// 一套完整实测：mode = 当前期望的模式（0 点击 / 1 悬停）
static void runCase(const QString& label, IconGridWindow* w, int mode) {
    w->show();
    wait(700);

    const QVector<QToolButton*> tabs = collectTabs(w);
    O() << QStringLiteral("   ── %1 ──\n").arg(label);
    O() << QStringLiteral("      标签数=%1  初始 currentCategory=\"%2\"   标签：")
               .arg(tabs.size()).arg(w->currentCategory());
    for (auto* t : tabs) O() << QStringLiteral("\"%1\" ").arg(t->text());
    O() << QStringLiteral("\n");

    // ① 左键点击第 2 个标签 → 两种模式下都应切换
    if (tabs.size() > 1) {
        QToolButton* t = tabs.at(1);
        const QString want = t->text();
        realClick(t, 0);
        wait(250);
        const bool ok = (w->currentCategory() == want);
        if (!ok) ++g_fail;
        O() << QStringLiteral("      ① 左键点击 \"%1\" → \"%2\"   %3\n")
                   .arg(want, w->currentCategory(),
                        ok ? QStringLiteral("[切换 OK]") : QStringLiteral("[未切换 FAIL]"));
    }

    // ② 带 2px 微位移的左键点击第 3 个标签（真实点击抖动）→ 应仍切换
    if (tabs.size() > 2) {
        QToolButton* t = tabs.at(2);
        const QString want = t->text();
        realClick(t, 2);
        wait(250);
        const bool ok = (w->currentCategory() == want);
        if (!ok) ++g_fail;
        O() << QStringLiteral("      ② 左键点击(2px 微动) \"%1\" → \"%2\"   %3\n")
                   .arg(want, w->currentCategory(),
                        ok ? QStringLiteral("[切换 OK]") : QStringLiteral("[未切换 FAIL]"));
    }

    // ③ 悬停到第 4 个标签 → 判定是否与所选模式一致
    if (tabs.size() > 3) {
        QToolButton* t = tabs.at(3);
        const QString before = w->currentCategory();
        const QPoint c = t->rect().center();
        QHoverEvent h(QEvent::HoverEnter, c, c, Qt::NoModifier);
        QApplication::sendEvent(t, &h);
        wait(380);   // > 140ms 防抖
        const bool switched = (w->currentCategory() != before);
        const bool expect   = (mode == 1);
        const bool ok       = (switched == expect);
        if (!ok) ++g_fail;
        O() << QStringLiteral("      ③ 悬停 \"%1\" → \"%2\"   期望%3   %4\n")
                   .arg(t->text(), w->currentCategory(),
                        expect ? QStringLiteral("[切换]") : QStringLiteral("[不切换]"),
                        ok ? QStringLiteral("[符合 OK]") : QStringLiteral("[不符 FAIL]"));
    }
    O() << QStringLiteral("\n");

    w->hide();
    wait(150);
}

// ④ 热更新实测：在设置中心改完「分区标签切换」，**已经开着**的窗口必须就地生效
// 这是用户口径的核心：「点击切换选中 → 左键点击切换；悬停切换选中 → 鼠标移入切换」，
// 且改完立刻按新语义工作（不必关窗口、不必重启）。
// 判定用**真实 SettingCenterDialog** 里点单选框 → 端到端覆盖 saveSettings 的广播链路。
// 作者：谭征
static QRadioButton* findRadio(QWidget* root, const QString& text) {
    for (QRadioButton* r : root->findChildren<QRadioButton*>())
        if (r->text() == text) return r;
    return nullptr;
}

// 对任意控件的真实左键点击（与上面 QToolButton 版区分：那版还要带拖拽微位移）
static void realClickWidget(QWidget* w) {
    if (!w) return;
    const QPoint c(w->width() / 2, w->height() / 2);
    const QPoint g = w->mapToGlobal(c);
    QMouseEvent press(QEvent::MouseButtonPress, c, g, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    wait(40);
    QMouseEvent rel(QEvent::MouseButtonRelease, c, g, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &rel);
    wait(150);
}

static void runLiveCase(const QString& label, IconGridWindow* w) {
    w->show();
    wait(700);
    O() << QStringLiteral("   ── %1 ──\n").arg(label);

    // 起点：磁盘写 0（点击切换）并广播一次，等价于「程序启动时就是点击切换」
    {
        SettingsManager sm;
        sm.saveValue(QStringLiteral("Appearance/tagSwitchMode"), 0);
        sm.sync();
    }
    ThemeManager::notifySettingChanged(QStringLiteral("Appearance/tagSwitchMode"), 0);
    wait(200);

    // 悬停第 idx 个（起）标签，判定是否切换（expectSwitch = 该模式下的期望）。
    // 若该标签恰是当前分类会「悬停也不会有变化」，故往后找一个非当前分类的标签，
    // 保证每一项都是真判定，不出现"跳过"。
    auto hoverTab = [&](int idx, const QString& stage, bool expectSwitch) {
        const QVector<QToolButton*> tabs = collectTabs(w);
        QToolButton* t = nullptr;
        for (int i = idx; i < tabs.size(); ++i) {
            if (tabs.at(i)->text() != w->currentCategory()) { t = tabs.at(i); break; }
        }
        if (!t) { O() << QStringLiteral("      %1：无非当前标签，跳过\n").arg(stage); ++g_fail; return; }
        const QString before = w->currentCategory();
        const QPoint c = t->rect().center();
        QHoverEvent h(QEvent::HoverEnter, c, c, Qt::NoModifier);
        QApplication::sendEvent(t, &h);
        wait(380);   // > 140ms 防抖
        const bool sw = (w->currentCategory() != before);
        const bool ok = (sw == expectSwitch);
        if (!ok) ++g_fail;
        O() << QStringLiteral("      %1：悬停 \"%2\" → \"%3\"   期望%4   %5\n")
                   .arg(stage, t->text(), w->currentCategory(),
                        expectSwitch ? QStringLiteral("[切换]") : QStringLiteral("[不切换]"),
                        ok ? QStringLiteral("[符合 OK]") : QStringLiteral("[不符 FAIL]"));
    };

    // ① 当前=点击切换 → 悬停不应切换
    hoverTab(3, QStringLiteral("① 点击切换态"), false);

    // ② 真实设置中心点「悬停切换」→ 已开窗口应就地变为悬停切换
    {
        SettingCenterDialog dlg(SettingCenterDialog::TabAppearance);
        if (QRadioButton* r = findRadio(&dlg, QStringLiteral("悬停切换"))) {
            realClickWidget(r);
            O() << QStringLiteral("      · 已在真实设置中心点击「悬停切换」(checked=%1)\n")
                       .arg(r->isChecked() ? 1 : 0);
            if (!r->isChecked()) ++g_fail;
        } else {
            O() << QStringLiteral("      · [!] 未找到「悬停切换」单选框\n");
            ++g_fail;
        }
    }
    wait(200);
    hoverTab(2, QStringLiteral("② 改为悬停切换后"), true);

    // ③ 再点回「点击切换」→ 应就地恢复为点击切换
    {
        SettingCenterDialog dlg(SettingCenterDialog::TabAppearance);
        if (QRadioButton* r = findRadio(&dlg, QStringLiteral("点击切换"))) {
            realClickWidget(r);
            O() << QStringLiteral("      · 已在真实设置中心点击「点击切换」(checked=%1)\n")
                       .arg(r->isChecked() ? 1 : 0);
            if (!r->isChecked()) ++g_fail;
        } else {
            O() << QStringLiteral("      · [!] 未找到「点击切换」单选框\n");
            ++g_fail;
        }
    }
    wait(200);
    hoverTab(1, QStringLiteral("③ 改回点击切换后"), false);

    // ④ 点击切换态下，左键点击仍必须切换
    {
        const QVector<QToolButton*> tabs = collectTabs(w);
        if (tabs.size() > 2) {
            QToolButton* t = tabs.at(2);
            const QString want = t->text();
            realClick(t, 0);
            wait(250);
            const bool ok = (w->currentCategory() == want);
            if (!ok) ++g_fail;
            O() << QStringLiteral("      ④ 左键点击 \"%1\" → \"%2\"   %3\n")
                       .arg(want, w->currentCategory(),
                            ok ? QStringLiteral("[切换 OK]") : QStringLiteral("[未切换 FAIL]"));
        }
    }
    O() << QStringLiteral("\n");
    w->hide();
    wait(150);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // 复刻产品 main.cpp 的启动顺序（主题色 / 透明度 / 全局样式表）
    {
        QPalette pal = app.palette();
        pal.setColor(QPalette::PlaceholderText, Theme::placeholderTextColor());
        app.setPalette(pal);
    }
    {
        SettingsManager sm;
        ThemeManager* tm = ThemeManager::instance();
        const QString accent = sm.loadValue(QStringLiteral("Appearance/accent")).toString();
        if (!accent.isEmpty()) tm->setAccentColor(QColor(accent));
        tm->setTransparency(sm.loadValue(QStringLiteral("Appearance/transparency"), 100).toInt());
    }
    app.setStyleSheet(Theme::globalStyleSheet());

    int origMode = 0;
    {
        SettingsManager sm;
        origMode = sm.loadValue(QStringLiteral("Appearance/tagSwitchMode"), 0).toInt();
    }
    O() << QStringLiteral("用户 INI 原值：Appearance/tagSwitchMode=%1  (0=点击切换 1=悬停切换)\n")
               .arg(origMode);
    O() << QStringLiteral("（运行结束时由 run_tagprobe.py 原样恢复整份 INI）\n\n");

    QMap<QString, QVector<DesktopItem>> items;
    const QStringList cats = {QStringLiteral("目录"), QStringLiteral("文档"),
                              QStringLiteral("压缩"), QStringLiteral("图片"),
                              QStringLiteral("快捷方式"), QStringLiteral("网址"),
                              QStringLiteral("其它")};
    for (const QString& c : cats) {
        QVector<DesktopItem> v;
        DesktopItem it;
        it.displayName = QStringLiteral("测试项-") + c;
        it.sourcePath  = QStringLiteral("C:/__probe__/") + c + QStringLiteral(".txt");
        it.targetPath  = it.sourcePath;
        it.category    = c;
        v << it;
        items.insert(c, v);
    }

    for (int mode = 0; mode <= 1; ++mode) {
        setMode(mode);
        O() << QStringLiteral("════════ 设置：分区标签切换 = %1 ════════\n")
                   .arg(mode == 0 ? QStringLiteral("点击切换（默认）") : QStringLiteral("悬停切换"));

        // 载体 A：主窗口（桌面整理窗）
        {
            auto* w = new IconGridWindow();
            w->setItems(items);
            runCase(QStringLiteral("载体 A：主窗口（桌面整理窗）"), w, mode);
            delete w;
            wait(200);
        }
        // 载体 B：独立收纳盒窗口（用户截图 2 的场景）
        {
            auto* w = new IconGridWindow();
            w->setIsBoxWindow(true);
            w->setForceShortcutCategory(false);
            w->setItems(items);
            runCase(QStringLiteral("载体 B：独立收纳盒窗口"), w, mode);
            delete w;
            wait(200);
        }
    }

    // ④ 热更新：设置中心改完立即生效（两种载体）
    O() << QStringLiteral("════════ 设置中心改完立即生效（端到端：真实设置中心点单选框）════════\n");
    {
        auto* w = new IconGridWindow();
        w->setItems(items);
        runLiveCase(QStringLiteral("载体 A：主窗口（桌面整理窗）"), w);
        delete w;
        wait(200);
    }
    {
        auto* w = new IconGridWindow();
        w->setIsBoxWindow(true);
        w->setForceShortcutCategory(false);
        w->setItems(items);
        runLiveCase(QStringLiteral("载体 B：独立收纳盒窗口"), w);
        delete w;
        wait(200);
    }

    O() << QStringLiteral("失败项数 = %1\n").arg(g_fail);
    O().flush();
    return g_fail;
}
