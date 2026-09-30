/*
 * @file todostore.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef TODOSTORE_H
#define TODOSTORE_H

#include <QObject>
#include <QVector>
#include <QSet>
#include <QDateTime>
#include <QString>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

// 单条待办事项
struct TodoItem {
    QString id;             // QUuid（不带花括号）
    QString text;           // 条目内容
    bool done = false;      // 是否已完成
    QString doneTime;       // 完成时间（可读格式，用于清单展示/调试）
    int priority = 0;       // 优先级：0 无 / 1 低 / 2 中 / 3 高（圆点着色）
    int fontPoint = 0;      // 条目自定义字号；0 = 默认 13pt
    QDateTime reminder;     // 提醒时间；无效 = 未设置提醒
    int remKind = 0;        // 提醒来源选项：0 未设/清除 / 1 十分钟后 / 2 今天09:00（可调时间） / 3 明天09:00（可调时间） / 4 自定义完整时间
};

// 待办事项数据仓库（单例）：增删改查 + QSettings 持久化 + 提醒到期检查。
// 面板（SidePanelWidget）与「事项清单」窗（TodoListWindow）共用同一份数据，
// 任何修改都发 itemsChanged，两端各自刷新。
class TodoStore : public QObject {
    Q_OBJECT
public:
    // instance
    static TodoStore& instance();

    const QVector<TodoItem>& items() const { return m_items; }
    // undone条目
    QVector<TodoItem> undoneItems() const;
    // done条目
    QVector<TodoItem> doneItems() const;
    // undone数量
    int undoneCount() const;
    // done数量
    int doneCount() const;
    TodoItem item(const QString& id) const;   // 找不到返回 id 为空的默认项

    // afterId 非空 → 新条目插到该条目之后（「插入条目」）；否则追加到未完成区末尾
    QString addItem(const QString& text, const QString& afterId = QString());
    // 设置文本
    bool setText(const QString& id, const QString& text);
    // 设置done
    bool setDone(const QString& id, bool done);
    // 移除
    bool remove(const QString& id);
    // 移除done
    void removeDone();
    // 设置priority
    bool setPriority(const QString& id, int priority);
    // 设置字体点
    bool setFontPoint(const QString& id, int fontPoint);
    // 设置reminder
    bool setReminder(const QString& id, const QDateTime& dt, int remKind = 0);
    // 清除reminder
    bool clearReminder(const QString& id);

    // 用户确认提醒（点了提醒弹框的 OK）→ 清除该条提醒，条目行首的喇叭标记随之消失。
    // 提醒**不再在到期瞬间自动清除**（那会让「没看到通知就丢了提醒」）：到期只发一次
    // reminderDue，提醒时间与标记都保留着，直到本函数被调用；m_firedIds 防重复弹框。
    void acknowledgeReminder(const QString& id);

signals:
    void itemsChanged();                                  // 任何数据变化
    void reminderDue(const QString& id, const QString& text); // 提醒到期（触发后提醒自动清除）

private:
    // 构造函数：初始化对象
    explicit TodoStore(QObject* parent = nullptr);
    // 加载
    void load();
    // 保存
    void save();
    // 索引of
    int indexOf(const QString& id) const;
    void checkReminders();   // 定时扫描到期提醒

    QVector<TodoItem> m_items;
    QTimer* m_reminderTimer = nullptr;
    // 已弹过提醒弹框、但用户**还没点 OK 确认**的条目 id（内存态，不持久化）。
    // 作用：①防重复弹框（扫描是 15s 一次，不清除提醒的话会反复弹）；
    // ②重启后集合清空 → 未确认的提醒会重新弹一次（用户没处理完，理应继续提醒）。
    // 进出场：checkReminders 里 insert；acknowledgeReminder / setReminder / clearReminder / remove 里 remove。
    QSet<QString> m_firedIds;
};

// 条目右键菜单（面板与清单窗共用，对应 360 截图菜单）
namespace TodoUi {

enum class ItemAction { None, Edit, Insert, Remove, ToggleDone };

struct MenuResult {
    ItemAction action = ItemAction::None;
    int fontPoint = 0;          // >0 → 用户在「字体设置」子菜单选了字号
    int priority = -1;          // >=0 → 用户在「优先级」子菜单选了优先级
    bool reminderCleared = false;
    QDateTime reminder;         // 有效 → 设置提醒
    int remKind = 0;            // 与 reminder 配套的来源选项（写入 TodoItem::remKind，驱动菜单选中态）
};

// 弹出条目右键菜单：编辑条目 / 插入条目 / 删除条目 / 提醒管理 / 字体设置 / 优先级 / 标记(取消)已完成
MenuResult execItemMenu(QWidget* parent, const TodoItem& item);

// execItemMenu 返回后的通用分发：完成实际的数据修改与输入对话框（编辑/插入需要文本输入）
void applyItemAction(QWidget* parent, const QString& id, const MenuResult& r);

} // namespace TodoUi

#endif // TODOSTORE_H
