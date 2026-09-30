/*
 * @file shutdowntask.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef SHUTDOWNTASK_H
#define SHUTDOWNTASK_H

#include <QObject>
#include <QDateTime>
#include <QList>
#include <QString>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

// 定时关机：任务模型 + 调度器
// 设计要点：
// · 任务持久化在本地设置（SettingsManager → "Shutdown/Tasks"，每个任务一行竖线分隔编码），
// 程序重启后原样恢复；「暂时停用」的开关状态一并保存。
// · 调度用 1 秒心跳 QTimer，在主线程比较「下次执行时刻」，到期即调用系统命令执行动作。
// 比较采用「取下一个 >= now 的时刻」，同一秒内只会触发一次：周期任务触发后把该时刻
// 记进 skipUntil 消费掉，天然排到下一周期（每天 → 明天同一时刻）。
// ·  调度器活在本进程内：程序退出后任务不再执行（与 360 桌面助手「到点前助手一直开着」
// 的用法一致）。启动时会**丢弃已过期的一次性任务**，避免开机瞬间执行一次关机。
struct ShutdownTask {
    enum Type { Shutdown = 0, ForceShutdown, Reboot, Sleep, Logoff, Lock };
    enum Mode { Daily = 0, Workday, Weekend, Once };   // 每天 / 工作日 / 周末 / 指定时间点

    QString   id;
    int       type = Shutdown;
    int       mode = Daily;
    int       hour = 0;         // 0~23（周期任务使用）
    int       minute = 0;       // 0~59
    QDateTime onceAt;           // mode == Once 时使用（本地时间）
    bool      enabled = true;
    QDateTime skipUntil;        // 被「取消」掉的那一次执行时刻：只跳过该时刻，任务保留
};

class ShutdownTaskManager : public QObject {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit ShutdownTaskManager(QObject* parent = nullptr);

    const QList<ShutdownTask>& tasks() const { return m_tasks; }

    // 添加task
    void addTask(const ShutdownTask& t);
    // 移除task
    void removeTask(const QString& id);
    // 设置enabled
    void setEnabled(const QString& id, bool on);
    void cancelNext();                            // 取消最近一次即将执行：一次性任务直接移除

    // 下一个dueof
    QDateTime nextDueOf(const ShutdownTask& t) const;
    QDateTime nextDue() const;                    // 全部启用任务里最近的一次；invalid = 无
    // nearesttask
    const ShutdownTask* nearestTask() const;

    static QString typeName(int type);            // 关机 / 强制关机 / 重启 / 睡眠 / 注销 / 锁定
    static QString modeName(int mode);            // 每天 / 工作日 / 周末 / 指定时间

    // 加载
    void load();
    // 保存
    void save() const;

signals:
    void changed();   // 任务集合或开关发生变化：列表与提醒横幅需要整体重建
    void ticked();    // 每秒心跳：仅用于刷新倒计时文本（不重建控件）

private slots:
    // 响应tick
    void onTick();

private:
    // fire
    void fire(const ShutdownTask& t);
    QList<ShutdownTask> m_tasks;
    QTimer* m_timer = nullptr;
};

#endif // SHUTDOWNTASK_H
