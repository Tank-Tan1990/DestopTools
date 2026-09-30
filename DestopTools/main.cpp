/*
 * @file main.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "mainwindow.h"
#include "theme.h"
#include "thememanager.h"
#include "settingsmanager.h"
#include "glassmessagebox.h"
#include "desktopimmunityfilter.h"
#include "crashtrace.h"
#include "appexit.h"
#include <QApplication>
#include <QDebug>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPalette>
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

// 「应用备份」编排状态（见 appexit.h 注释）：设置中心写，事件循环退出后在此消费
QString g_pendingBackupDir;
bool g_restartAfterQuit = false;

int main(int argc, char *argv[])
{
    // 最早处安装崩溃捕获：把未处理异常/致命信号的【异常码 + 地址 + 归属模块 + 最后动作】
    // 写进 exe 同目录的 DestopTools_crash.log。此前 dock“改后缀重命名”等路径崩溃时
    // 没有任何线索可用（dumpDebug 已停用），必须先能定位才能根治。
    CrashTrace::install();

    QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication::setOrganizationName(QStringLiteral("DestopTools"));
    QApplication::setApplicationName(QStringLiteral("DestopTools"));
    QApplication::setApplicationDisplayName(QStringLiteral("桌面整理工具"));

    QApplication a(argc, argv);

    // A7：退出留痕 —— 补上 CrashTrace 覆盖不到的那一半。
    // CrashTrace 抓的是**崩溃**（未处理异常 / 致命信号）；而"程序自己关闭了"这类反馈里，
    // 真正需要区分的是「显式退出」与「被外部强杀」—— TerminateProcess 既不会触发
    // aboutToQuit，也不会给 CrashTrace 任何机会写盘。
    // 加这一行后即可二分定论：crash.log 出现 normal-quit ⇒ 走了正常 quit()；
    // 没有 ⇒ 进程是被外部终止的（再与事件查看器 / CrashDumps 交叉验证）。
    // 用 event()（直接落盘）而非 mark()（只写内存缓冲、仅崩溃时输出，正常退出永远看不到）。
    QObject::connect(&a, &QCoreApplication::aboutToQuit, []() {
        CrashTrace::event("normal-quit");
    });

    // 安装全局 Win32 消息过滤器：阻止“显示桌面”按钮（任务栏最右侧竖线 / Win+D）
    // 控制 DestopTools 任何窗口的显示/隐藏，确保本程序窗口始终可见。
    DesktopImmunityFilter immunityFilter;
    a.installNativeEventFilter(&immunityFilter);

    a.setFont(Theme::appFont());

    // 输入框占位文字：全局调色板**只钉 PlaceholderText 这一个角色** = 固定「禁用灰」，不随主题色/透明度变。
    // 占位色不由样式表决定（Qt5 QSS 没有 `::placeholder` 选择器，写了也是死规则），真正生效的是
    // QPalette::PlaceholderText，其默认值 = Text 角色取 50% alpha = **半透明黑** → 深色底上看着就是"变黑"。
    // 这里一次性钉死，覆盖所有只吃全局样式表、没单独 setStyleSheet 的输入框；
    // 有独立样式表的输入框（搜索框等）另由 Theme::applyLineEditStyle() 的守卫兜住。
    // 只改这一个角色，其它任何调色板角色/控件外观都不动。
    {
        QPalette pal = a.palette();
        pal.setColor(QPalette::PlaceholderText, Theme::placeholderTextColor());
        a.setPalette(pal);
    }

    // 启动前先载入已保存的主题色与透明度，使全局样式表与窗口不透明度从初始就生效
    {
        SettingsManager sm;
        ThemeManager* tm = ThemeManager::instance();
        const QString accent = sm.loadValue(QStringLiteral("Appearance/accent")).toString();
        if (!accent.isEmpty()) tm->setAccentColor(QColor(accent));
        // 默认 100 = 完全不透明（2026-09-21 由 80 改为 100）；透明度由用户在外观设置里自行调整。
        const int trans = sm.loadValue(QStringLiteral("Appearance/transparency"), 100).toInt();
        tm->setTransparency(trans);
    }

    // 全局样式表下发通道（唯一入口）。
    // 缓存一层"与上次结果完全相同就跳过"：a.setStyleSheet() 会让 Qt 遍历进程内【每一个 widget】
    // 重新解析样式并 repolish / 重算尺寸，代价随控件数线性增长（Dock + 网格的图标按钮 + 设置中心
    // 等合计数百个）。拖动透明度时 themeChanged 会被高频触发，重复下发同一份 QSS 纯属浪费，
    // 正是"拖透明度时鼠标卡死"的主要成因之一。
    static QString s_appliedQss;
    auto applyGlobalStyleSheet = [&a]() {
        const QString qss = Theme::globalStyleSheet();
        if (qss == s_appliedQss) return;   // 内容未变：不下发，避免全应用重排
        s_appliedQss = qss;
        a.setStyleSheet(qss);
    };

    applyGlobalStyleSheet();

    // 主题色/透明度联动：主题变化时重新生成并应用全局样式表，
    // 使全部 UI 界面（按钮、输入、列表、复选框、菜单等）的颜色/边框跟随主题色。
    QObject::connect(ThemeManager::instance(), &ThemeManager::themeChanged, &a, applyGlobalStyleSheet);

    // —— 单例模式：同一时间只允许运行一个程序实例 ——
    // 使用本地命名服务（QLocalServer/QLocalSocket）检测是否已有实例在运行。
    const QString singleInstanceName = QStringLiteral("DestopTools-SingleInstance");
    QLocalSocket socket;
    socket.connectToServer(singleInstanceName);
    if (socket.waitForConnected(500)) {
        // 已存在运行中的实例，弹框提醒后退出，不重复启动
        GlassMessageBox::information(nullptr,
            QStringLiteral("提示"),
            QStringLiteral("程序已经在运行，请勿重复打开。"));
        return 0;
    }
    // 清理上一次异常退出可能残留的失效套接字，再创建本地服务供后续实例检测
    QLocalServer::removeServer(singleInstanceName);
    QLocalServer* localServer = new QLocalServer(&a);
    if (!localServer->listen(singleInstanceName)) {
        // 极端情况下监听失败：为不影响用户使用仍允许启动，仅失去单例保护
        qWarning() << "单例本地服务创建失败：" << localServer->errorString();
    }

    int exitCode = 0;
    {
        MainWindow w;
        w.hide(); // 协调窗口不显示，只显示左右两个无边框子窗口
        exitCode = a.exec();
    }   // MainWindow（连同全部 band 窗）已析构，运行期向 INI 的落盘动作全部结束

    // 「应用备份」收尾（见 appexit.h）
    // 此时旧实例已完全停止写盘：用备份里的整份配置覆盖现行配置，再拉起新实例。
    if (g_restartAfterQuit && !g_pendingBackupDir.isEmpty()) {
        QSettings cur(QSettings::IniFormat, QSettings::UserScope,
                      QStringLiteral("DestopTools"), QStringLiteral("DestopTools"));
        cur.sync();   // 把退出前尚未刷盘的写操作落定（马上会被覆盖，但保证 remove/copy 无并发）
        // 两份配置都要还原：DestopTools.ini（主设置）+ categories.ini（分类映射库）。
        // 只还 DestopTools.ini 会漏掉主窗口分类的文件→分类从属，「解散分类」无法还原。
        const QString cfgDir = QFileInfo(cur.fileName()).absolutePath();
        const QStringList cfgFiles{ QStringLiteral("DestopTools.ini"), QStringLiteral("categories.ini") };
        for (const QString& fn : cfgFiles) {
            const QString src = g_pendingBackupDir + QStringLiteral("/") + fn;
            const QString dst = cfgDir + QStringLiteral("/") + fn;
            if (!QFile::exists(src)) continue;   // 旧备份缺该文件（如老备份无 categories.ini）：跳过，别删现行
            if (QFile::exists(dst)) QFile::remove(dst);
            if (!QFile::copy(src, dst)) {
                qWarning() << "应用备份失败：复制配置文件出错" << src << "->" << dst;
            }
        }
        // 必须先关掉旧实例的本地单例服务，否则 startDetached 拉起的【新实例】会
        // connectToServer 成功 → 误判“程序已在运行”→ 直接退出，重启从未发生，备份等于没应用。
        // localServer 是 a 的子对象，此刻仍在监听；先 removeServer + close 释放命名管道，
        // 新实例才能顺利 listen 并启动。
        QLocalServer::removeServer(singleInstanceName);
        if (localServer) localServer->close();
        QProcess::startDetached(QApplication::applicationFilePath(), QStringList());
    }
    return exitCode;
}
