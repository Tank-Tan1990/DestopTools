/*
 * @file diagtrace.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef DIAGTRACE_H
#define DIAGTRACE_H

// —— 临时诊断日志（定位期用，定位完成后可整体删除） ——
// 背景：F2 / 右键「重命名」两条入口在收纳盒与 Dock 上都出现“点了完全没反应”，
// 而这些都是**静默 return**：路径上任何一道闸门不成立都表现为“无反应”，
// 从现象无法区分是哪一道。本模块把每个判定点的结论写成一行日志到 exe 同目录
// 的 DestopTools_trace.log，一次复现即可定位。
// 设计约束（ 2026-09-15 修正，修复“F2/Delete 用一次就失效”的根因）：
// · header-only，inline + 函数内静态，不需要改 .pro；
// · 【非阻塞】只做 mutex + 内存追加（微秒级），落盘交给 500ms 定时器批量做（B6）——
// 本函数会被低级钩子回调（WH_KEYBOARD_LL / WH_MOUSE_LL）调用，那里【绝不能】同步
// open+write+flush：一旦那次 I/O 拖慢回调、超过 LowLevelHooksTimeout（约 300ms），
// Windows 会**静默摘除**钩子（句柄仍非空、回调却不再被调用）→ 表现即“F2/Delete
// 用过一次/跨窗口拖过一次之后就彻底失效、日志里再也看不到 keydown”。
// 旧实现每次调用都 open+write+flush，正是这个隐患的根源。
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QString>
#include <QTimer>

namespace DiagTrace {

// 统一诊断开关（trace.log 与 crash.log 共用，默认【关闭】）。
// 关闭时 DiagTrace::log() 立即返回、CrashTrace::install() 直接跳过 —— 两种日志都不写盘。
// 唯一开关：INI（%APPDATA%\DestopTools\DestopTools.ini）的 [Diagnostics] traceEnabled：
// · true  ⇒ trace.log 与 crash.log 都写盘；
// · false / 缺省 ⇒ 两种日志都不写盘。
// （原设计另有 DESTOPTOOLS_DIAG 环境变量覆盖，已按需求移除，仅以 INI 为准。）
// 为什么默认关闭：钩子回调与 gate 判定路径上有大量 DiagTrace::log 调用，
// 常开会让 GUI 线程周期性做文件 I/O，且 DestopTools_trace.log 只增不减。
inline bool enabled()
{
    static const bool on = []() -> bool {
        QSettings sm(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("DestopTools"), QStringLiteral("DestopTools"));
        return sm.value(QStringLiteral("Diagnostics/traceEnabled"), false).toBool();
    }();
    return on;
}

// 文件路径
inline QString filePath()
{
    static const QString p =
        QCoreApplication::applicationDirPath() + QStringLiteral("/DestopTools_trace.log");
    return p;
}

// 非阻塞写：加锁后仅追加进内存缓冲，由一个 500ms 定时器在事件循环里批量落盘。
// 回调里只做 mutex + QByteArray 追加，绝无文件 I/O，杜绝钩子被系统静默摘除的诱因。
// B6：落盘间隔 80ms → 500ms，并加“诊断开关”。
inline void log(const QString& line)
{
    if (!enabled()) return;   // B6：开关关闭 → 立即返回（不追加缓冲，也不会构造 Sink → 落盘定时器根本不启动）
    struct Sink {
        QMutex mtx;
        QByteArray buf;
        QTimer timer;
        Sink() {
            timer.setInterval(500);
            QObject::connect(&timer, &QTimer::timeout, [this]() { flush(); });
            timer.start();
        }
        ~Sink() {
            timer.stop();
            flush();               // 进程退出前把最后一批也落盘（filePath 已缓存，无需 QCoreApplication）
        }
        void flush() {
            QByteArray local;
            {
                QMutexLocker l(&mtx);
                if (buf.isEmpty()) return;
                local.swap(buf);
            }
            QFile f(filePath());
            if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return;
            f.write(local);
        }
    };
    static Sink sink;
    const QByteArray full = QDateTime::currentDateTime()
                                .toString(QStringLiteral("HH:mm:ss.zzz")).toUtf8()
                            + QByteArrayLiteral(" ") + line.toUtf8()
                            + QByteArrayLiteral("\r\n");
    QMutexLocker l(&sink.mtx);
    sink.buf += full;
}

// 拼接常用字段的短格式助手
inline QString boolStr(bool b) { return b ? QStringLiteral("Y") : QStringLiteral("N"); }

}   // namespace DiagTrace

#endif // DIAGTRACE_H
