/*
 * @file main.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

// scprobe —— 「设置中心」真实控件试验台
// 目的：判定 设置中心 → 外观设置 → 分区标签切换（点击切换 / 悬停切换）的
// **本地持久化**是否真的失效（用户反馈：「开启/关闭没有入本地持久化数据」）。
// 做法（本项目既有方法论：不猜，直接问产品自己）：
// 把产品【已编译的 .obj】全链进来（去掉 main.obj），构造**真实** SettingCenterDialog，
// 用真实 QMouseEvent 点击单选框，然后**直接读磁盘原始字节**核对 INI 里的值。
// —— 读磁盘而非再开一个 QSettings：同进程内 QSettings 共享 QConfFile 缓存，
// 用它验证等于自己问自己，得不到真相。
// 会写用户 INI（saveSettings 落盘），由 run_scprobe.py 备份/还原。

#include <QApplication>
#include <QAbstractButton>
#include <QRadioButton>
#include <QCheckBox>
#include <QFile>
#include <QTextStream>
#include <QEventLoop>
#include <QTimer>
#include <QMouseEvent>
#include <QSettings>
#include <QDir>
#include <QStringList>

#include "theme.h"
#include "thememanager.h"
#include "settingsmanager.h"
#include "settingcenterdialog.h"

static QTextStream out(stdout);
static int g_fail = 0;

static void wait(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

static QString iniPath() {
    QSettings s(QSettings::IniFormat, QSettings::UserScope,
                QStringLiteral("DestopTools"), QStringLiteral("DestopTools"));
    return s.fileName();
}

// 读磁盘原始文本（绕开一切内存缓存），取 [Appearance] 段某键的值
static QString rawValue(const QString& path, const QString& key) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QStringLiteral("<打不开>");
    const QString all = QString::fromUtf8(f.readAll());
    f.close();
    bool inApp = false;
    for (const QString& line : all.split(QLatin1Char('\n'))) {
        const QString t = line.trimmed();
        if (t.startsWith(QLatin1Char('['))) {
            inApp = t.startsWith(QStringLiteral("[Appearance"));
            continue;
        }
        if (inApp && t.startsWith(key)) {
            const int eq = t.indexOf(QLatin1Char('='));
            if (eq >= 0) return t.mid(eq + 1).trimmed();
        }
    }
    return QStringLiteral("<无此键>");
}

// 真实鼠标点击（按下 + 抬起，走 Qt 正常分发）
static void realClick(QWidget* w) {
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

// 查找radio
// 作者：谭征
static QRadioButton* findRadio(QWidget* root, const QString& text) {
    for (QRadioButton* r : root->findChildren<QRadioButton*>())
        if (r->text() == text) return r;
    return nullptr;
}

// 查找检查
// 作者：谭征
static QCheckBox* findCheck(QWidget* root, const QString& text) {
    for (QCheckBox* c : root->findChildren<QCheckBox*>())
        if (c->text() == text) return c;
    return nullptr;
}

// 把对话框里【全部】单选框/复选框列出来 —— 用于识别「同一个设置有两套控件」这类问题
static void dumpButtons(QWidget* root, const QString& title) {
    const QList<QRadioButton*> rs = root->findChildren<QRadioButton*>();
    out << QStringLiteral("  [dump] %1: QRadioButton 共 %2 个\n").arg(title).arg(rs.size());
    int i = 0;
    for (QRadioButton* r : rs) {
        QWidget* p = r->parentWidget();
        out << QStringLiteral("      #%1 \"%2\" checked=%3 visible=%4 parent=%5 topLevelWindow=%6\n")
                   .arg(i++)
                   .arg(r->text())
                   .arg(r->isChecked() ? 1 : 0)
                   .arg(r->isVisibleTo(p) ? 1 : 0)
                   .arg(p ? p->metaObject()->className() : "null")
                   .arg(r->window() == root ? QStringLiteral("是") : QStringLiteral("否"));
    }
    const QList<QCheckBox*> cs = root->findChildren<QCheckBox*>();
    out << QStringLiteral("  [dump] %1: QCheckBox 共 %2 个\n").arg(title).arg(cs.size());
    i = 0;
    for (QCheckBox* c : cs) {
        QWidget* p = c->parentWidget();
        out << QStringLiteral("      #%1 \"%2\" checked=%3 visible=%4 parent=%5\n")
                   .arg(i++)
                   .arg(c->text())
                   .arg(c->isChecked() ? 1 : 0)
                   .arg(c->isVisibleTo(p) ? 1 : 0)
                   .arg(p ? p->metaObject()->className() : "null");
    }
    out.flush();
}

static QString flagsOf(QRadioButton* r) {
    if (!r) return QStringLiteral("?");
    return QStringLiteral("checked=%1 enabled=%2 visible=%3")
        .arg(r->isChecked() ? 1 : 0)
        .arg(r->isEnabled() ? 1 : 0)
        .arg(r->isVisibleTo(r->parentWidget()) ? 1 : 0);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    const QString ini = iniPath();
    const QString K = QStringLiteral("tagSwitchMode");

    out << QStringLiteral("═══ 设置中心「分区标签切换」持久化实测 ═══\n");
    out << QStringLiteral("INI = %1\n\n").arg(ini);

    out << QStringLiteral("--- 阶段 A：还原测试（磁盘写 1 → 对话框应显示「悬停切换」）---\n");
    {
        SettingsManager sm;
        sm.saveValue(QStringLiteral("Appearance/tagSwitchMode"), 1);
        sm.sync();
    }
    wait(80);
    out << QStringLiteral("  A1 磁盘 raw=%1  (期望 1)\n").arg(rawValue(ini, K));

    auto* dlg = new SettingCenterDialog(SettingCenterDialog::TabAppearance);
    dlg->show();
    wait(500);

    QRadioButton* clickR = findRadio(dlg, QStringLiteral("点击切换"));
    QRadioButton* hoverR = findRadio(dlg, QStringLiteral("悬停切换"));
    dumpButtons(dlg, QStringLiteral("新构造的对话框"));
    out << QStringLiteral("  A2 找到单选钮: 点击切换=%1  悬停切换=%2\n")
               .arg(clickR ? QStringLiteral("有") : QStringLiteral("无"),
                    hoverR ? QStringLiteral("有") : QStringLiteral("无"));
    if (!clickR || !hoverR) {
        out << QStringLiteral("  [!] 找不到单选钮，无法继续\n");
        ++g_fail;
        out.flush();
        return 1;
    }
    out << QStringLiteral("  A3 构造后状态: 点击[%1]   悬停[%2]\n").arg(flagsOf(clickR), flagsOf(hoverR));
    const bool restoreOk = (hoverR->isChecked() && !clickR->isChecked());
    if (!restoreOk) ++g_fail;
    out << QStringLiteral("     → %1\n")
               .arg(restoreOk ? QStringLiteral("正确恢复为「悬停切换」 OK")
                              : QStringLiteral("未恢复（磁盘=1 却显示「点击切换」）FAIL"));

    out << QStringLiteral("\n--- 阶段 B：真实点击「悬停切换」→ 应立即落盘为 1 ---\n");
    realClick(hoverR);
    out << QStringLiteral("  B1 点击后状态: 点击[%1]   悬停[%2]\n").arg(flagsOf(clickR), flagsOf(hoverR));
    QString v = rawValue(ini, K);
    out << QStringLiteral("  B2 磁盘 raw=%1  (期望 1)\n").arg(v);
    if (v != QStringLiteral("1")) ++g_fail;
    out << QStringLiteral("     → %1\n")
               .arg(v == QStringLiteral("1") ? QStringLiteral("已落盘 OK")
                                             : QStringLiteral("没落盘 FAIL"));

    out << QStringLiteral("\n--- 阶段 C：真实点击「点击切换」→ 应落盘为 0 ---\n");
    realClick(clickR);
    out << QStringLiteral("  C1 点击后状态: 点击[%1]   悬停[%2]\n").arg(flagsOf(clickR), flagsOf(hoverR));
    v = rawValue(ini, K);
    out << QStringLiteral("  C2 磁盘 raw=%1  (期望 0)\n").arg(v);
    if (v != QStringLiteral("0")) ++g_fail;
    out << QStringLiteral("     → %1\n")
               .arg(v == QStringLiteral("0") ? QStringLiteral("已落盘 OK")
                                             : QStringLiteral("没落盘 FAIL"));

    out << QStringLiteral("\n--- 阶段 D：对照组（复选框「盒子显示边框」，用户口径里这个是好的）---\n");
    QCheckBox* border = findCheck(dlg, QStringLiteral("盒子显示边框"));
    const QString borderBefore = rawValue(ini, QStringLiteral("boxBorder"));
    if (border) {
        border->setChecked(!border->isChecked());
        wait(150);
        const QString borderAfter = rawValue(ini, QStringLiteral("boxBorder"));
        out << QStringLiteral("  翻转后 磁盘 boxBorder: %1 → %2  %3\n")
                   .arg(borderBefore, borderAfter,
                        borderBefore != borderAfter ? QStringLiteral("[复选框能落盘]")
                                                    : QStringLiteral("[复选框也落不了盘!]"));
        border->setChecked(borderBefore == QStringLiteral("true"));   // 还原
        wait(120);
    } else {
        out << QStringLiteral("  [!] 未找到「盒子显示边框」复选框\n");
    }

    out << QStringLiteral("\n--- 阶段 E：关闭对话框（accept → finished 兜底落盘）后，再新建一个看显示什么 ---\n");
    realClick(hoverR);
    v = rawValue(ini, K);
    out << QStringLiteral("  E1 关闭前 磁盘 raw=%1\n").arg(v);
    dlg->accept();
    wait(250);
    v = rawValue(ini, K);
    out << QStringLiteral("  E2 关闭后 磁盘 raw=%1  (期望 1)\n").arg(v);
    if (v != QStringLiteral("1")) ++g_fail;

    auto* dlg2 = new SettingCenterDialog(SettingCenterDialog::TabAppearance);
    dlg2->show();
    wait(500);
    QRadioButton* c2 = findRadio(dlg2, QStringLiteral("点击切换"));
    QRadioButton* h2 = findRadio(dlg2, QStringLiteral("悬停切换"));
    out << QStringLiteral("  E3 新对话框: 点击[%1]   悬停[%2]\n").arg(flagsOf(c2), flagsOf(h2));
    const bool againOk = (h2 && h2->isChecked());
    if (!againOk) ++g_fail;
    out << QStringLiteral("     → %1\n")
               .arg(againOk ? QStringLiteral("重启后仍显示「悬停切换」 OK")
                            : QStringLiteral("重启后回到「点击切换」FAIL（= 用户看到的现象）"));

    out << QStringLiteral("\n--- 阶段 F：同页两对单选钮是否互相干扰（直接复现）---\n");
    {
        QRadioButton* alwaysR = findRadio(dlg, QStringLiteral("始终显示"));
        QRadioButton* mhoverR = findRadio(dlg, QStringLiteral("鼠标悬停显示"));
        out << QStringLiteral("  F0 同页 4 个单选钮的父窗口指针: 点击=%1 悬停=%2 始终=%3 鼠标悬停=%4\n")
                   .arg(reinterpret_cast<quintptr>(clickR->parentWidget()), 0, 16)
                   .arg(reinterpret_cast<quintptr>(hoverR->parentWidget()), 0, 16)
                   .arg(reinterpret_cast<quintptr>(alwaysR ? alwaysR->parentWidget() : nullptr), 0, 16)
                   .arg(reinterpret_cast<quintptr>(mhoverR ? mhoverR->parentWidget() : nullptr), 0, 16);
        out << QStringLiteral("     → 父窗口指针%1\n")
                   .arg((clickR->parentWidget() == (alwaysR ? alwaysR->parentWidget() : nullptr))
                            ? QStringLiteral("相同 ⇒ 会被 Qt 自动合并成一个互斥组（这就是病根）")
                            : QStringLiteral("不同 ⇒ 理论上互不影响"));
        if (alwaysR) {
            alwaysR->setChecked(false);
            hoverR->setChecked(true);
            wait(120);
            out << QStringLiteral("  F1 先选「悬停切换」: 点击[%1] 悬停[%2] 始终[%3]\n")
                       .arg(clickR->isChecked() ? 1 : 0).arg(hoverR->isChecked() ? 1 : 0)
                       .arg(alwaysR->isChecked() ? 1 : 0);
            alwaysR->setChecked(true);      // 等价于 loadSettings 里给「始终显示」赋值
            wait(120);
            const bool clobbered = !hoverR->isChecked();
            out << QStringLiteral("  F2 再给「始终显示」赋值后: 点击[%1] 悬停[%2] 始终[%3]\n")
                       .arg(clickR->isChecked() ? 1 : 0).arg(hoverR->isChecked() ? 1 : 0)
                       .arg(alwaysR->isChecked() ? 1 : 0);
            out << QStringLiteral("     → %1\n")
                       .arg(clobbered ? QStringLiteral("「悬停切换」被踩掉了 ⇒ 两对单选钮共用一组 FAIL")
                                      : QStringLiteral("互不影响 OK"));
            if (clobbered) ++g_fail;
            alwaysR->setChecked(true);
        }
    }

    out << QStringLiteral("\n═══ 失败项数 = %1  （0 = 持久化正常）═══\n").arg(g_fail);
    out.flush();

    dlg2->hide();
    dlg->hide();
    return 0;
}
