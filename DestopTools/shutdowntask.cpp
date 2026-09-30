/*
 * @file shutdowntask.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "shutdowntask.h"
#include "settingsmanager.h"
#include "diagtrace.h"

#include <QProcess>
#include <QTime>
#include <QUuid>
#include <QTimer>

namespace {
constexpr const char* kSettingsKey = "Shutdown/Tasks";

// 该日期是否属于这个周期模式
bool dayMatches(int mode, const QDate& d) {
    switch (mode) {
    case ShutdownTask::Daily:   return true;
    case ShutdownTask::Workday: return d.dayOfWeek() >= 1 && d.dayOfWeek() <= 5;
    case ShutdownTask::Weekend: return d.dayOfWeek() == 6 || d.dayOfWeek() == 7;
    default:                    return false;
    }
}
}   // namespace

ShutdownTaskManager::ShutdownTaskManager(QObject* parent)
    : QObject(parent) {
    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &ShutdownTaskManager::onTick);
    load();
    m_timer->start();
}

// type名称
// 作者：谭征
QString ShutdownTaskManager::typeName(int type) {
    switch (type) {
    case ShutdownTask::Shutdown:      return QStringLiteral("关机");
    case ShutdownTask::ForceShutdown: return QStringLiteral("强制关机");
    case ShutdownTask::Reboot:        return QStringLiteral("重启");
    case ShutdownTask::Sleep:         return QStringLiteral("睡眠");
    case ShutdownTask::Logoff:        return QStringLiteral("注销");
    case ShutdownTask::Lock:          return QStringLiteral("锁定");
    default:                          return QStringLiteral("关机");
    }
}

// 模式名称
// 作者：谭征
QString ShutdownTaskManager::modeName(int mode) {
    switch (mode) {
    case ShutdownTask::Daily:   return QStringLiteral("每天");
    case ShutdownTask::Workday: return QStringLiteral("工作日");
    case ShutdownTask::Weekend: return QStringLiteral("周末");
    case ShutdownTask::Once:    return QStringLiteral("指定时间");
    default:                    return QStringLiteral("每天");
    }
}

// 下一次执行时刻：
// · 一次性任务 = 指定时刻（落在过去表示「已到点」，由 onTick 立即执行后移除）
// · 周期任务 = 今天起往后 8 天内第一个「日期符合周期 且 时刻尚未到」的时点，
// 并且跳过被「取消」掉的那一次（skipUntil）
// 作者：谭征
QDateTime ShutdownTaskManager::nextDueOf(const ShutdownTask& t) const {
    if (!t.enabled) return QDateTime();
    const QDateTime now = QDateTime::currentDateTime();
    if (t.mode == ShutdownTask::Once) {
        return t.onceAt.isValid() ? t.onceAt : QDateTime();
    }
    QDate d = now.date();
    for (int i = 0; i <= 8; ++i, d = d.addDays(1)) {
        if (!dayMatches(t.mode, d)) continue;
        const QDateTime cand(d, QTime(t.hour, t.minute, 0));
        if (cand < now) continue;                       // 今天这个时刻已过（或正在过的这一秒之后）
        if (t.skipUntil.isValid() && cand == t.skipUntil) continue;
        return cand;
    }
    return QDateTime();
}

// 下一个due
// 作者：谭征
QDateTime ShutdownTaskManager::nextDue() const {
    QDateTime best;
    for (const ShutdownTask& t : m_tasks) {
        const QDateTime d = nextDueOf(t);
        if (!d.isValid()) continue;
        if (!best.isValid() || d < best) best = d;
    }
    return best;
}

const ShutdownTask* ShutdownTaskManager::nearestTask() const {
    const QDateTime best = nextDue();
    if (!best.isValid()) return nullptr;
    for (const ShutdownTask& t : m_tasks) {
        const QDateTime d = nextDueOf(t);
        if (d.isValid() && d == best) return &t;
    }
    return nullptr;
}

// 添加task
// 作者：谭征
void ShutdownTaskManager::addTask(const ShutdownTask& t) {
    ShutdownTask task = t;
    if (task.id.isEmpty()) task.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_tasks.append(task);
    save();
    emit changed();
}

// 移除task
// 作者：谭征
void ShutdownTaskManager::removeTask(const QString& id) {
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks.at(i).id != id) continue;
        m_tasks.removeAt(i);
        save();
        emit changed();
        return;
    }
}

// 设置enabled
// 作者：谭征
void ShutdownTaskManager::setEnabled(const QString& id, bool on) {
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks.at(i).id != id) continue;
        if (m_tasks.at(i).enabled == on) return;
        m_tasks[i].enabled = on;
        if (!on) m_tasks[i].skipUntil = QDateTime();   // 停用后不再保留「跳过」状态
        save();
        emit changed();
        return;
    }
}

// 取消下一个
// 作者：谭征
void ShutdownTaskManager::cancelNext() {
    const ShutdownTask* n = nearestTask();
    if (!n) return;
    const QString id = n->id;
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (m_tasks.at(i).id != id) continue;
        if (m_tasks.at(i).mode == ShutdownTask::Once) {
            m_tasks.removeAt(i);                       // 一次性任务取消即结束
        } else {
            m_tasks[i].skipUntil = nextDueOf(m_tasks[i]);   // 周期任务只跳过最近这一次
        }
        save();
        emit changed();
        return;
    }
}

// 每秒心跳：到点即执行。周期任务执行后把该时刻写进 skipUntil 消费掉，
// 既防止同一秒内重复触发，也让下次排到下一周期。
// 作者：谭征
void ShutdownTaskManager::onTick() {
    const QDateTime now = QDateTime::currentDateTime();
    bool fired = false;
    for (int i = 0; i < m_tasks.size(); ++i) {
        if (!m_tasks.at(i).enabled) continue;
        const QDateTime due = nextDueOf(m_tasks.at(i));
        if (!due.isValid() || now < due) continue;

        const ShutdownTask t = m_tasks.at(i);
        if (t.mode == ShutdownTask::Once) {
            m_tasks.removeAt(i);
            --i;
        } else {
            m_tasks[i].skipUntil = due;
        }
        fired = true;
        fire(t);
    }
    if (fired) {
        save();
        emit changed();
    }
    emit ticked();
}

// 真正落到系统上的动作。全部走 detached 进程，不阻塞界面。
// 作者：谭征
void ShutdownTaskManager::fire(const ShutdownTask& t) {
    const QString when = QDateTime::currentDateTime().toString(QStringLiteral("MM-dd HH:mm:ss"));
    DiagTrace::log(QStringLiteral("[shutdown] fire type=%1(%2) mode=%3 at %4")
                       .arg(t.type).arg(typeName(t.type)).arg(t.mode).arg(when));
    switch (t.type) {
    case ShutdownTask::Shutdown:
        // /s 关机（不带 /f：给前台程序正常的关闭机会）
        QProcess::startDetached(QStringLiteral("shutdown.exe"),
                                QStringList{QStringLiteral("/s"), QStringLiteral("/t"), QStringLiteral("0")});
        break;
    case ShutdownTask::ForceShutdown:
        QProcess::startDetached(QStringLiteral("shutdown.exe"),
                                QStringList{QStringLiteral("/s"), QStringLiteral("/f"), QStringLiteral("/t"), QStringLiteral("0")});
        break;
    case ShutdownTask::Reboot:
        QProcess::startDetached(QStringLiteral("shutdown.exe"),
                                QStringList{QStringLiteral("/r"), QStringLiteral("/t"), QStringLiteral("0")});
        break;
    case ShutdownTask::Sleep:
        // 0,1,0 = Hibernate=FALSE, ForceCritical=TRUE, DisableWakeEvent=FALSE
        QProcess::startDetached(QStringLiteral("rundll32.exe"),
                                QStringList{QStringLiteral("powrprof.dll,SetSuspendState"),
                                            QStringLiteral("0,1,0")});
        break;
    case ShutdownTask::Logoff:
        QProcess::startDetached(QStringLiteral("shutdown.exe"), QStringList{QStringLiteral("/l")});
        break;
    case ShutdownTask::Lock:
        QProcess::startDetached(QStringLiteral("rundll32.exe"),
                                QStringList{QStringLiteral("user32.dll,LockWorkStation")});
        break;
    default:
        break;
    }
}

// / 其次尝试可执行文件所在目录、当前工作目录。
// 作者：谭征
void ShutdownTaskManager::load() {
    SettingsManager sm;
    const QStringList raw = sm.loadValue(QString::fromLatin1(kSettingsKey)).toStringList();
    const QDateTime now = QDateTime::currentDateTime();
    m_tasks.clear();
    for (const QString& line : raw) {
        const QStringList f = line.split(QLatin1Char('|'));
        if (f.size() < 6) continue;
        ShutdownTask t;
        t.id       = f.at(0);
        if (t.id.isEmpty()) t.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        t.enabled  = (f.at(1).toInt() != 0);
        t.type     = f.at(2).toInt();
        t.mode     = f.at(3).toInt();
        t.hour     = qBound(0, f.at(4).toInt(), 23);
        t.minute   = qBound(0, f.at(5).toInt(), 59);
        if (f.size() > 6 && !f.at(6).isEmpty()) t.onceAt = QDateTime::fromString(f.at(6), Qt::ISODate);
        if (f.size() > 7 && !f.at(7).isEmpty()) t.skipUntil = QDateTime::fromString(f.at(7), Qt::ISODate);
        // 一次性任务若在程序未运行期间已经过期，直接丢弃：开机瞬间执行关机是灾难性的。
        if (t.mode == ShutdownTask::Once && (!t.onceAt.isValid() || t.onceAt <= now)) continue;
        m_tasks.append(t);
    }
}

// 保存
// 作者：谭征
void ShutdownTaskManager::save() const {
    QStringList out;
    for (const ShutdownTask& t : m_tasks) {
        out << QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8")
                   .arg(t.id)
                   .arg(t.enabled ? 1 : 0)
                   .arg(t.type)
                   .arg(t.mode)
                   .arg(t.hour)
                   .arg(t.minute)
                   .arg(t.onceAt.isValid() ? t.onceAt.toString(Qt::ISODate) : QString())
                   .arg(t.skipUntil.isValid() ? t.skipUntil.toString(Qt::ISODate) : QString());
    }
    SettingsManager sm;
    sm.saveValue(QString::fromLatin1(kSettingsKey), out);
    sm.sync();   // QSettings 默认异步落盘：任务列表必须立刻写盘，否则崩溃/强杀会丢任务
}
