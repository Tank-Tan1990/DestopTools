#include "settingsmanager.h"
#include "theme.h"
#include <QApplication>

SettingsManager::SettingsManager()
    : m_settings(QSettings::IniFormat, QSettings::UserScope,
                 QStringLiteral("DestopTools"), QStringLiteral("DestopTools")) {
}

// 保存盒子
// 作者：谭征
void SettingsManager::saveBoxes(const QMap<QString, BoxState>& boxes) {
    m_settings.beginGroup(QStringLiteral("Boxes"));
    m_settings.remove(QString());
    for (auto it = boxes.cbegin(); it != boxes.cend(); ++it) {
        m_settings.beginGroup(it.key());
        m_settings.setValue(QStringLiteral("title"), it.value().title);
        m_settings.setValue(QStringLiteral("color"), it.value().color.name(QColor::HexArgb));
        m_settings.setValue(QStringLiteral("items"), it.value().itemIds);
        m_settings.endGroup();
    }
    m_settings.endGroup();
}

// 加载盒子
// 作者：谭征
QMap<QString, BoxState> SettingsManager::loadBoxes() const {
    QMap<QString, BoxState> result;
    m_settings.beginGroup(QStringLiteral("Boxes"));
    for (const QString& key : m_settings.childGroups()) {
        m_settings.beginGroup(key);
        BoxState state;
        state.title = m_settings.value(QStringLiteral("title"), key).toString();
        // 同上：落盘默认值必须用**基色**（alpha 恒 255），不能用会随透明度变化的 Theme::panelBg()。
        QString defaultColor = Theme::panelBgColor().name(QColor::HexArgb);
        state.color = QColor(m_settings.value(QStringLiteral("color"), defaultColor).toString());
        state.itemIds = m_settings.value(QStringLiteral("items")).toStringList();
        result.insert(key, state);
        m_settings.endGroup();
    }
    m_settings.endGroup();
    return result;
}

// 用户创建的独立收纳盒：持久化到本地（UserBoxes 组），重启后由 MainWindow 重建。
// 作者：谭征
void SettingsManager::saveUserBoxes(const QVector<UserBoxPersist>& boxes) {
    m_settings.beginGroup(QStringLiteral("UserBoxes"));
    m_settings.remove(QString());
    for (const UserBoxPersist& b : boxes) {
        if (b.id.isEmpty()) continue;
        m_settings.beginGroup(b.id);
        m_settings.setValue(QStringLiteral("title"), b.title);
        m_settings.setValue(QStringLiteral("geomX"), b.geometry.x());
        m_settings.setValue(QStringLiteral("geomY"), b.geometry.y());
        m_settings.setValue(QStringLiteral("geomW"), b.geometry.width());
        m_settings.setValue(QStringLiteral("geomH"), b.geometry.height());
        m_settings.setValue(QStringLiteral("collapsed"), b.collapsed);
        m_settings.setValue(QStringLiteral("currentCategory"), b.currentCategory);
        // 分类显示顺序：QMap 迭代按 key 字母序，用户拖拽交换的顺序会被打乱，
        // 必须单独保存（与 IconGridWindow::m_categoryOrder 同口径）。
        m_settings.setValue(QStringLiteral("categoryOrder"), b.categoryOrder.join(QLatin1Char(',')));
        m_settings.setValue(QStringLiteral("catCount"), b.categories.size());
        int idx = 0;
        for (auto it = b.categories.constBegin(); it != b.categories.constEnd(); ++it, ++idx) {
            m_settings.setValue(QStringLiteral("cat_%1_name").arg(idx), it.key());
            m_settings.setValue(QStringLiteral("cat_%1_items").arg(idx), it.value());
        }
        m_settings.endGroup();
    }
    m_settings.endGroup();
}

// 加载user盒子
// 作者：谭征
QVector<UserBoxPersist> SettingsManager::loadUserBoxes() const {
    QVector<UserBoxPersist> result;
    m_settings.beginGroup(QStringLiteral("UserBoxes"));
    for (const QString& key : m_settings.childGroups()) {
        m_settings.beginGroup(key);
        UserBoxPersist b;
        b.id = key;
        b.title = m_settings.value(QStringLiteral("title"), key).toString();
        const int x = m_settings.value(QStringLiteral("geomX"), 0).toInt();
        const int y = m_settings.value(QStringLiteral("geomY"), 0).toInt();
        const int w = m_settings.value(QStringLiteral("geomW"), 0).toInt();
        const int h = m_settings.value(QStringLiteral("geomH"), 0).toInt();
        b.geometry = QRect(x, y, w, h);
        b.collapsed = m_settings.value(QStringLiteral("collapsed"), false).toBool();
        b.currentCategory = m_settings.value(QStringLiteral("currentCategory")).toString();
        const QString orderStr = m_settings.value(QStringLiteral("categoryOrder")).toString();
        if (!orderStr.isEmpty()) {
            b.categoryOrder = orderStr.split(QLatin1Char(','), Qt::SkipEmptyParts);
        }
        const int catCount = m_settings.value(QStringLiteral("catCount"), 0).toInt();
        for (int i = 0; i < catCount; ++i) {
            const QString name = m_settings.value(QStringLiteral("cat_%1_name").arg(i)).toString();
            const QStringList items = m_settings.value(QStringLiteral("cat_%1_items").arg(i)).toStringList();
            if (!name.isEmpty()) b.categories.insert(name, items);
        }
        m_settings.endGroup();
        result.append(b);
    }
    m_settings.endGroup();
    return result;
}

// 保存窗口几何
// 作者：谭征
void SettingsManager::saveWindowGeometry(const QByteArray& geom) {
    m_settings.setValue(QStringLiteral("MainWindow/geometry"), geom);
}

// 加载窗口几何
// 作者：谭征
QByteArray SettingsManager::loadWindowGeometry() const {
    return m_settings.value(QStringLiteral("MainWindow/geometry")).toByteArray();
}

// 保存value
// 作者：谭征
void SettingsManager::saveValue(const QString& key, const QVariant& value) {
    m_settings.setValue(key, value);
}

// 加载value
// 作者：谭征
QVariant SettingsManager::loadValue(const QString& key, const QVariant& defaultValue) const {
    return m_settings.value(key, defaultValue);
}

// 若程序/对话框快速关闭可能丢失最近一次修改）。
// 作者：谭征
void SettingsManager::sync() {
    m_settings.sync();
}
