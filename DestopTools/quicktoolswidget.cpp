#include "quicktoolswidget.h"
#include "theme.h"
#include "thememanager.h"
#include "settingsmanager.h"
#include <QGridLayout>
#include <QToolButton>
#include <QVBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QPainter>
#include <QSet>

// 工具图标
// 作者：谭征
QIcon QuickToolsWidget::toolIcon(const QString& name, QWidget* /*w*/) {
    // 科技感统一图标资源
    if (name == QStringLiteral("桌面整理")) return Theme::icon("tool_organize");
    else if (name == QStringLiteral("快速搜索")) return Theme::icon("tool_search");
    else if (name == QStringLiteral("文件管理")) return Theme::icon("tool_files");
    else if (name == QStringLiteral("定时关机")) return Theme::icon("tool_shutdown");
    else if (name == QStringLiteral("截屏")) return Theme::icon("tool_screenshot");
    else if (name == QStringLiteral("记事本")) return Theme::icon("tool_notes");
    else if (name == QStringLiteral("壁纸")) return Theme::icon("tool_wallpaper");
    else if (name == QStringLiteral("锁屏")) return Theme::icon("tool_lock");
    else if (name == QStringLiteral("上网")) return Theme::icon("tool_browser");
    else if (name == QStringLiteral("计算器")) return Theme::icon("tool_calc");
    else if (name == QStringLiteral("注册表")) return Theme::icon("tool_registry");
    else if (name == QStringLiteral("命令行")) return Theme::icon("tool_cmd");
    else if (name == QStringLiteral("磁盘清理")) return Theme::icon("tool_clean");
    else if (name == QStringLiteral("工具管理")) return Theme::icon("tool_manage");

    // 兜底：首字青色图标
    QPixmap pix(24, 24);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    QColor glow = ThemeManager::instance()->accentColor();
    glow.setAlpha(90);
    p.setBrush(glow);
    p.drawEllipse(2, 2, 20, 20);
    p.setPen(ThemeManager::instance()->accentColor());
    p.setFont(QFont(QString(), 10, QFont::Bold));
    p.drawText(pix.rect(), Qt::AlignCenter, name.left(1));
    p.end();
    return QIcon(pix);
}

// 默认工具名称
// 作者：谭征
QStringList QuickToolsWidget::defaultToolNames() {
    return {
        QStringLiteral("桌面整理"), QStringLiteral("快速搜索"),
        QStringLiteral("文件管理"), QStringLiteral("定时关机"), QStringLiteral("截屏"),
        QStringLiteral("记事本"), QStringLiteral("壁纸"), QStringLiteral("锁屏"), QStringLiteral("上网"),
        QStringLiteral("计算器"), QStringLiteral("注册表"), QStringLiteral("命令行"),
        QStringLiteral("磁盘清理"), QStringLiteral("工具管理")
    };
}

QuickToolsWidget::QuickToolsWidget(QWidget* parent)
    : QWidget(parent) {
    setupUi();

    // 主题色联动：连到 ThemeManager，改主题色后重绘所有快捷工具按钮的样式与图标
    // 主题色联动：登记到 ThemeManager，改主题色后重绘所有快捷工具按钮的样式与图标。
    // E：仅登记「样式」通道 —— 本部件是子控件，整窗不透明度由父窗负责。
    ThemeManager::instance()->registerThemeTarget(this, [this]() { applyTheme(); });
    applyTheme(); // 初始按当前主题色应用一次
}

// 主题色/透明度联动：重绘对话框自身硬编码青色样式，并同步窗口不透明度
// 作者：谭征
void QuickToolsWidget::applyTheme() {
    // 图标颜色由 ThemeManager::accentColor() 烘焙进 QPixmap，必须重新生成；
    // 按钮样式由 Theme::quickToolButtonStyle()（走 applyTokens）返回，需重新 setStyleSheet。
    for (int i = 0; i < m_gridLayout->count(); ++i) {
        QLayoutItem* item = m_gridLayout->itemAt(i);
        QToolButton* btn = item ? qobject_cast<QToolButton*>(item->widget()) : nullptr;
        if (!btn) continue;
        btn->setIcon(toolIcon(btn->text(), this));
        btn->setStyleSheet(Theme::quickToolButtonStyle());
    }
}

// 工具顺序
// 作者：谭征
QStringList QuickToolsWidget::toolOrder() const {
    return m_toolOrder;
}

// 设置工具顺序
// 作者：谭征
void QuickToolsWidget::setToolOrder(const QStringList& order) {
    // 严格按传入顺序显示，仅保留默认工具集合中存在的项；
    // 不再自动补全缺失项，以便与设置中心「移除/添加小工具」保持同步。
    QSet<QString> valid = defaultToolNames().toSet();
    QStringList newOrder;
    for (const QString& name : order) {
        if (valid.contains(name)) {
            newOrder << name;
        }
    }

    m_toolOrder = newOrder;
    buildGrid(m_toolOrder);
    updateGeometry();
    update();

    SettingsManager sm;
    sm.saveValue(QStringLiteral("QuickTools/order"), m_toolOrder);
}

// 初始化ui
// 作者：谭征
void QuickToolsWidget::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(8);

    auto* title = new QLabel(QStringLiteral("快捷工具"), this);
    QFont f = title->font();
    f.setBold(true);
    f.setPointSize(10);
    title->setFont(f);
    title->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent; border: none;"));
    mainLayout->addWidget(title);

    m_gridLayout = new QGridLayout();
    m_gridLayout->setSpacing(6);
    m_gridLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addLayout(m_gridLayout);
    mainLayout->addStretch();

    SettingsManager sm;
    QStringList order = sm.loadValue(QStringLiteral("QuickTools/order")).toStringList();
    if (order.isEmpty()) {
        order = defaultToolNames();
    }
    setToolOrder(order);
}

// 构建网格
// 作者：谭征
void QuickToolsWidget::buildGrid(const QStringList& tools) {
    // 清除旧按钮：立即删除（不使用 deleteLater，确保模态对话框等嵌套
    // 事件循环场景下也能立即完成布局更新与重绘）
    while (m_gridLayout->count() > 0) {
        QLayoutItem* item = m_gridLayout->takeAt(0);
        if (item && item->widget()) {
            delete item->widget();
        }
        delete item;
    }

    const int cols = 4;
    for (int i = 0; i < tools.size(); ++i) {
        auto* btn = new QToolButton(this);
        btn->setIcon(toolIcon(tools[i], this));
        btn->setIconSize(QSize(22, 22));
        btn->setText(tools[i]);
        btn->setFixedSize(60, 54);
        btn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        btn->setStyleSheet(Theme::quickToolButtonStyle());

        connect(btn, &QToolButton::clicked, this, [this, name = tools[i]]() {
            emit toolTriggered(name);
        });
        m_gridLayout->addWidget(btn, i / cols, i % cols, Qt::AlignCenter);
    }
}
