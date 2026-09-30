/*
 * @file todostore.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "todostore.h"
#include "settingsmanager.h"
#include "glassinputdialog.h"
#include "theme.h"

#include <QMenu>
#include <QCursor>
#include <QTimer>
#include <QUuid>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

// TodoStore

// 作者：谭征
TodoStore& TodoStore::instance() {
    static TodoStore s;
    return s;
}

TodoStore::TodoStore(QObject* parent)
    : QObject(parent) {
    load();

    // 提醒扫描：每 15 秒检查一次到期提醒
    m_reminderTimer = new QTimer(this);
    m_reminderTimer->setInterval(15 * 1000);
    connect(m_reminderTimer, &QTimer::timeout, this, &TodoStore::checkReminders);
    m_reminderTimer->start();
}

// / 其次尝试可执行文件所在目录、当前工作目录。
// 作者：谭征
void TodoStore::load() {
    m_items.clear();
    SettingsManager sm;
    const QJsonDocument doc = QJsonDocument::fromJson(
        sm.loadValue(QStringLiteral("Todo/items"), QString()).toString().toUtf8());
    if (!doc.isArray()) return;
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject o = v.toObject();
        TodoItem it;
        it.id = o.value(QStringLiteral("id")).toString();
        it.text = o.value(QStringLiteral("text")).toString();
        it.done = o.value(QStringLiteral("done")).toBool();
        it.doneTime = o.value(QStringLiteral("doneTime")).toString();
        it.priority = o.value(QStringLiteral("priority")).toInt(0);
        it.fontPoint = o.value(QStringLiteral("fontPoint")).toInt(0);
        const QString rem = o.value(QStringLiteral("reminder")).toString();
        if (!rem.isEmpty()) it.reminder = QDateTime::fromString(rem, Qt::ISODate);
        it.remKind = o.value(QStringLiteral("remKind")).toInt(0);
        if (!it.id.isEmpty()) m_items.append(it);
    }
}

// 保存
// 作者：谭征
void TodoStore::save() {
    QJsonArray arr;
    for (const TodoItem& it : m_items) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), it.id);
        o.insert(QStringLiteral("text"), it.text);
        o.insert(QStringLiteral("done"), it.done);
        o.insert(QStringLiteral("doneTime"), it.doneTime);
        o.insert(QStringLiteral("priority"), it.priority);
        o.insert(QStringLiteral("fontPoint"), it.fontPoint);
        if (it.reminder.isValid())
            o.insert(QStringLiteral("reminder"), it.reminder.toString(Qt::ISODate));
        o.insert(QStringLiteral("remKind"), it.remKind);
        arr.append(o);
    }
    SettingsManager sm;
    sm.saveValue(QStringLiteral("Todo/items"),
                 QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    sm.sync();
}

// 索引of
// 作者：谭征
int TodoStore::indexOf(const QString& id) const {
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items[i].id == id) return i;
    return -1;
}

// 条目
// 作者：谭征
TodoItem TodoStore::item(const QString& id) const {
    const int i = indexOf(id);
    return i >= 0 ? m_items[i] : TodoItem();
}

// undone条目
// 作者：谭征
QVector<TodoItem> TodoStore::undoneItems() const {
    QVector<TodoItem> out;
    for (const TodoItem& it : m_items)
        if (!it.done) out.append(it);
    return out;
}

// done条目
// 作者：谭征
QVector<TodoItem> TodoStore::doneItems() const {
    QVector<TodoItem> out;
    for (const TodoItem& it : m_items)
        if (it.done) out.append(it);
    return out;
}

// undone数量
// 作者：谭征
int TodoStore::undoneCount() const { return undoneItems().size(); }
// done数量
// 作者：谭征
int TodoStore::doneCount() const { return doneItems().size(); }

// afterId 非空 → 新条目插到该条目之后（「插入条目」）；否则追加到未完成区末尾
// 作者：谭征
QString TodoStore::addItem(const QString& text, const QString& afterId) {
    if (text.trimmed().isEmpty()) return QString();
    TodoItem it;
    it.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    it.text = text.trimmed();
    const int at = afterId.isEmpty() ? -1 : indexOf(afterId);
    if (at >= 0)
        m_items.insert(at + 1, it);
    else
        m_items.append(it);
    save();
    emit itemsChanged();
    return it.id;
}

// 设置文本
// 作者：谭征
bool TodoStore::setText(const QString& id, const QString& text) {
    const int i = indexOf(id);
    if (i < 0 || text.trimmed().isEmpty()) return false;
    m_items[i].text = text.trimmed();
    save();
    emit itemsChanged();
    return true;
}

// 设置done
// 作者：谭征
bool TodoStore::setDone(const QString& id, bool done) {
    const int i = indexOf(id);
    if (i < 0 || m_items[i].done == done) return false;
    m_items[i].done = done;
    m_items[i].doneTime = done
        ? QDateTime::currentDateTime().toString(QStringLiteral("MM-dd hh:mm"))
        : QString();
    save();
    emit itemsChanged();
    return true;
}

// 移除
// 作者：谭征
bool TodoStore::remove(const QString& id) {
    const int i = indexOf(id);
    if (i < 0) return false;
    m_items.removeAt(i);
    m_firedIds.remove(id);   // 条目都没了，别在集合里留残留 id
    save();
    emit itemsChanged();
    return true;
}

// 移除done
// 作者：谭征
void TodoStore::removeDone() {
    bool changed = false;
    for (int i = m_items.size() - 1; i >= 0; --i) {
        if (m_items[i].done) { m_firedIds.remove(m_items[i].id); m_items.removeAt(i); changed = true; }
    }
    if (!changed) return;
    save();
    emit itemsChanged();
}

// 设置priority
// 作者：谭征
bool TodoStore::setPriority(const QString& id, int priority) {
    const int i = indexOf(id);
    if (i < 0) return false;
    m_items[i].priority = qBound(0, priority, 3);
    save();
    emit itemsChanged();
    return true;
}

// 设置字体点
// 作者：谭征
bool TodoStore::setFontPoint(const QString& id, int fontPoint) {
    const int i = indexOf(id);
    if (i < 0) return false;
    m_items[i].fontPoint = fontPoint;
    save();
    emit itemsChanged();
    return true;
}

// 设置reminder
// 作者：谭征
bool TodoStore::setReminder(const QString& id, const QDateTime& dt, int remKind) {
    const int i = indexOf(id);
    if (i < 0 || !dt.isValid()) return false;
    m_items[i].reminder = dt;
    m_items[i].remKind = remKind;
    m_firedIds.remove(id);   // 重新设了提醒 → 允许再次触发
    save();
    emit itemsChanged();
    return true;
}

// 清除reminder
// 作者：谭征
bool TodoStore::clearReminder(const QString& id) {
    const int i = indexOf(id);
    if (i < 0) return false;
    m_items[i].reminder = QDateTime();
    m_items[i].remKind = 0;
    m_firedIds.remove(id);
    save();
    emit itemsChanged();
    return true;
}

// reminderDue，提醒时间与标记都保留着，直到本函数被调用；m_firedIds 防重复弹框。
// 作者：谭征
void TodoStore::acknowledgeReminder(const QString& id) {
    m_firedIds.remove(id);
    clearReminder(id);   // 内部会 save + itemsChanged → 面板行首的喇叭标记随之消失
}

// 检查reminders
// 作者：谭征
void TodoStore::checkReminders() {
    const QDateTime now = QDateTime::currentDateTime();
    QStringList dueIds, dueTexts;
    for (const TodoItem& it : m_items) {
        if (it.reminder.isValid() && it.reminder <= now && !it.text.isEmpty()
            && !m_firedIds.contains(it.id)) {
            dueIds << it.id;
            dueTexts << it.text;
        }
    }
    if (dueIds.isEmpty()) return;
    for (int i = 0; i < dueIds.size(); ++i) {
        // 这里**不再 clearReminder**：提醒要保留到用户点过弹框的 OK（见 acknowledgeReminder）——
        // 条目行首那个闪烁的喇叭标记就是「这条还没被确认」的可视凭据。
        // 改由 m_firedIds 防重复：否则 15 秒一次的扫描会把人烦死。
        m_firedIds.insert(dueIds[i]);
        emit reminderDue(dueIds[i], dueTexts[i]);
    }
}

// 条目右键菜单（面板与清单窗共用）

namespace TodoUi {

MenuResult execItemMenu(QWidget* parent, const TodoItem& item) {
    MenuResult r;
    QMenu menu(parent);
    Theme::applyMenuStyle(&menu);

    QAction* editAct = menu.addAction(Theme::icon("rename"), QStringLiteral("编辑条目"));
    QAction* insertAct = menu.addAction(Theme::icon("add"), QStringLiteral("插入条目"));
    QAction* removeAct = menu.addAction(Theme::icon("delete"), QStringLiteral("删除条目"));

    QMenu* reminderMenu = menu.addMenu(Theme::icon("settings"), QStringLiteral("提醒管理"));
    Theme::applyMenuStyle(reminderMenu);
    QAction* clearRemAct = reminderMenu->addAction(QStringLiteral("清除提醒"));
    clearRemAct->setEnabled(item.reminder.isValid());
    reminderMenu->addSeparator();
    // 已设置对应提醒时，菜单项文字显示实际时刻（如"今天 14:30"），让"时间变了 / 已选中"一目了然。
    const QString todayLabel = (item.remKind == 2 && item.reminder.isValid())
        ? QStringLiteral("今天 %1").arg(item.reminder.toString(QStringLiteral("HH:mm")))
        : QStringLiteral("今天 09:00");
    const QString tomorrowLabel = (item.remKind == 3 && item.reminder.isValid())
        ? QStringLiteral("明天 %1").arg(item.reminder.toString(QStringLiteral("HH:mm")))
        : QStringLiteral("明天 09:00");
    QAction* rem10m = reminderMenu->addAction(QStringLiteral("10 分钟后"));
    QAction* remToday = reminderMenu->addAction(todayLabel);
    QAction* remTomorrow = reminderMenu->addAction(tomorrowLabel);
    // 选中态与「优先级」一致：当前生效的提醒选项前打 。
    // 相对时间选项（10分钟）无法从提醒时刻反推来源，由 TodoItem::remKind 记录。
    for (QAction* a : {clearRemAct, rem10m, remToday, remTomorrow}) a->setCheckable(true);
    if (!item.reminder.isValid()) {
        clearRemAct->setChecked(true);
    } else if (item.remKind == 1) {
        rem10m->setChecked(true);
    } else if (item.remKind == 2) {
        remToday->setChecked(true);
    } else if (item.remKind == 3 || item.remKind == 4) {
        remTomorrow->setChecked(true);
    }

    QMenu* fontMenu = menu.addMenu(Theme::icon("appearance"), QStringLiteral("字体设置"));
    Theme::applyMenuStyle(fontMenu);
    QAction* fontSmall = fontMenu->addAction(QStringLiteral("小 (11pt)"));
    QAction* fontMid = fontMenu->addAction(QStringLiteral("中 (13pt)"));
    QAction* fontBig = fontMenu->addAction(QStringLiteral("大 (16pt)"));
    fontSmall->setCheckable(true);
    fontMid->setCheckable(true);
    fontBig->setCheckable(true);
    if (item.fontPoint <= 0) fontMid->setChecked(true);
    else if (item.fontPoint == 11) fontSmall->setChecked(true);
    else if (item.fontPoint == 16) fontBig->setChecked(true);
    else fontMid->setChecked(true);

    QMenu* priMenu = menu.addMenu(Theme::icon("more"), QStringLiteral("优先级"));
    Theme::applyMenuStyle(priMenu);
    QAction* priNone = priMenu->addAction(QStringLiteral("无"));
    QAction* priLow = priMenu->addAction(QStringLiteral("低"));
    QAction* priMid = priMenu->addAction(QStringLiteral("中"));
    QAction* priHigh = priMenu->addAction(QStringLiteral("高"));
    for (QAction* a : {priNone, priLow, priMid, priHigh}) a->setCheckable(true);
    (item.priority == 1 ? priLow : item.priority == 2 ? priMid : item.priority == 3 ? priHigh : priNone)
        ->setChecked(true);

    menu.addSeparator();
    QAction* doneAct = menu.addAction(Theme::icon("check"),
                                      item.done ? QStringLiteral("标记未完成")
                                                : QStringLiteral("标记已完成"));

    QAction* chosen = menu.exec(QCursor::pos());
    if (!chosen) return r;
    if (chosen == editAct) r.action = ItemAction::Edit;
    else if (chosen == insertAct) r.action = ItemAction::Insert;
    else if (chosen == removeAct) r.action = ItemAction::Remove;
    else if (chosen == doneAct) r.action = ItemAction::ToggleDone;
    else if (chosen == clearRemAct) r.reminderCleared = true;
    else if (chosen == rem10m) r.reminder = QDateTime::currentDateTime().addSecs(10 * 60), r.remKind = 1;
    else if (chosen == remToday) {
        // 「今天 09:00」时间可调：与明天提醒同款交互，默认 09:00（已有提醒则默认其当前时刻）。
        bool ok = false;
        const QString defText = item.reminder.isValid()
            ? item.reminder.toString(QStringLiteral("HH:mm"))
            : QDateTime::currentDateTime().addSecs(10 * 60).toString(QStringLiteral("HH:mm"));
        const QString t = GlassInputDialog::getText(parent, QStringLiteral("今天提醒"),
                                                    QStringLiteral("提醒时间（HH:mm 或 yyyy-MM-dd HH:mm）"),
                                                    QLineEdit::Normal, defText, &ok);
        if (!ok) return r;   // 取消调整：不设置提醒
        const QDate today = QDateTime::currentDateTime().date();
        const QTime tm = QTime::fromString(t.trimmed(), QStringLiteral("HH:mm"));
        if (tm.isValid()) {
            r.reminder = QDateTime(today, tm);
            r.remKind = 2;
        } else {
            const QDateTime dt = QDateTime::fromString(t.trimmed(), QStringLiteral("yyyy-MM-dd HH:mm"));
            if (dt.isValid()) {
                r.reminder = dt;
                r.remKind = 4;
            } else {
                return r;   // 无法解析：视为取消
            }
        }
    }
    else if (chosen == remTomorrow) {
        // 「明天 09:00」时间可调：弹出输入框，默认 09:00（已有提醒则默认其当前时刻）。
        // 支持 HH:mm（保持"明天"日期）或完整 yyyy-MM-dd HH:mm。
        bool ok = false;
        const QString defText = item.reminder.isValid()
            ? item.reminder.toString(QStringLiteral("HH:mm"))
            : QStringLiteral("09:00");
        const QString t = GlassInputDialog::getText(parent, QStringLiteral("明天提醒"),
                                                    QStringLiteral("提醒时间（HH:mm 或 yyyy-MM-dd HH:mm）"),
                                                    QLineEdit::Normal, defText, &ok);
        if (!ok) return r;   // 取消调整：不设置提醒
        const QDateTime base(QDateTime::currentDateTime().date().addDays(1), QTime(9, 0));
        const QTime tm = QTime::fromString(t.trimmed(), QStringLiteral("HH:mm"));
        if (tm.isValid()) {
            r.reminder = QDateTime(base.date(), tm);
            r.remKind = 3;
        } else {
            const QDateTime dt = QDateTime::fromString(t.trimmed(), QStringLiteral("yyyy-MM-dd HH:mm"));
            if (dt.isValid()) {
                r.reminder = dt;
                r.remKind = 4;
            } else {
                return r;   // 无法解析：视为取消
            }
        }
    }
    else if (chosen == fontSmall) r.fontPoint = 11;
    else if (chosen == fontMid) r.fontPoint = 13;
    else if (chosen == fontBig) r.fontPoint = 16;
    else if (chosen == priNone) r.priority = 0;
    else if (chosen == priLow) r.priority = 1;
    else if (chosen == priMid) r.priority = 2;
    else if (chosen == priHigh) r.priority = 3;
    return r;
}

void applyItemAction(QWidget* parent, const QString& id, const MenuResult& r) {
    TodoStore& store = TodoStore::instance();
    if (r.action == ItemAction::Edit) {
        bool ok = false;
        const QString text = GlassInputDialog::getText(parent, QStringLiteral("编辑条目"),
                                                       QStringLiteral("内容"),
                                                       QLineEdit::Normal,
                                                       store.item(id).text, &ok);
        if (ok && !text.trimmed().isEmpty()) store.setText(id, text);
    } else if (r.action == ItemAction::Insert) {
        bool ok = false;
        const QString text = GlassInputDialog::getText(parent, QStringLiteral("插入条目"),
                                                       QStringLiteral("内容（插入到该条目下方）"),
                                                       QLineEdit::Normal, QString(), &ok);
        if (ok && !text.trimmed().isEmpty()) store.addItem(text, id);
    } else if (r.action == ItemAction::Remove) {
        store.remove(id);
    } else if (r.action == ItemAction::ToggleDone) {
        store.setDone(id, !store.item(id).done);
    }
    if (r.reminderCleared) {
        store.clearReminder(id);
    } else if (r.reminder.isValid()) {
        store.setReminder(id, r.reminder, r.remKind);
    }
    if (r.fontPoint > 0) store.setFontPoint(id, r.fontPoint);
    if (r.priority >= 0) store.setPriority(id, r.priority);
}

} // namespace TodoUi
