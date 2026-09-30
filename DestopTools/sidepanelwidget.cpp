/*
 * @file sidepanelwidget.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "sidepanelwidget.h"
#include "quicktoolswidget.h"
#include "theme.h"
#include "thememanager.h"
#include "settingcenterdialog.h"
#include "updatedialog.h"
#include "feedbackdialog.h"
#include "settingsmanager.h"
#include "glassmessagebox.h"
#include "glassinputdialog.h"
#include "todostore.h"
#include "todolistwindow.h"
#include "desktopmirrorwindow.h"
#include "regiondata.h"
#include "windowsnap.h"            // 自动对齐提示文案（标题栏 tooltip）
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QScrollArea>
#include <QTimer>
#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QCursor>
#include <QEvent>
#include <QMouseEvent>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QStyle>
#include <QMenu>
#include <QAction>
#include <QApplication>
#include <QPainter>
#include <QPixmap>
#include <QPen>
#include <QColor>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslError>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QUrlQuery>
#include <QVariantMap>
#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif

// 提醒声音播放（MCI / winmm）
// 播放设置中心「提醒声音」选中的声音文件。用 mciSendString 而非 QSoundEffect：
// 不引入 QtMultimedia 模块，wav/mp3/wma 等通吃。
// 行为：startReminderSound 后持续循环整曲，直到 stopReminderSound 被调用（提醒弹框关闭时）
// 才停止；未选声音 / 文件不存在 / 格式打不开 → 静默不播放、绝不崩溃。
static bool s_remindLoopActive = false;
static int  s_remindSoundSeq   = 0;

static void stopReminderSound() {
#ifdef Q_OS_WIN
    s_remindLoopActive = false;
    ++s_remindSoundSeq;   // 使仍在途的循环定时器失效
    mciSendStringW(L"close RemindSound", nullptr, 0, nullptr);
#endif
}

// 播放一段（seek 到开头 + play），并按曲长排定下一次循环；loop 标志与序号守卫保证只在
// 仍需要循环且是最新一次播放时才继续，stop 之后旧定时器不会误触新一轮播放。
static void s_playRemindLoopChunk(int seq);

static void startReminderSound(const QString& path) {
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(path.trimmed());
    if (native.isEmpty() || !QFileInfo::exists(native)) return;   // 没选声音 / 文件丢失 → 不播放
    mciSendStringW(L"close RemindSound", nullptr, 0, nullptr);    // 先清掉上次残留
    const QString openCmd = QStringLiteral("open \"%1\" alias RemindSound").arg(native);
    if (mciSendStringW(reinterpret_cast<const WCHAR*>(openCmd.utf16()), nullptr, 0, nullptr) != 0)
        return;   // 打开失败（格式不支持等）→ 静默，不影响弹窗
    // 统一时间格式为毫秒，保证后续 status length 取到的是毫秒数
    mciSendStringW(L"set RemindSound time format milliseconds", nullptr, 0, nullptr);
    s_remindLoopActive = true;
    const int seq = ++s_remindSoundSeq;
    s_playRemindLoopChunk(seq);
#else
    Q_UNUSED(path);
#endif
}

static void s_playRemindLoopChunk(int seq) {
#ifdef Q_OS_WIN
    if (!s_remindLoopActive || seq != s_remindSoundSeq) return;
    mciSendStringW(L"seek RemindSound to start", nullptr, 0, nullptr);
    mciSendStringW(L"play RemindSound", nullptr, 0, nullptr);
    WCHAR buf[32] = {0};
    if (mciSendStringW(L"status RemindSound length", buf, 32, nullptr) == 0) {
        bool ok = false;
        const int ms = QString::fromWCharArray(buf).toInt(&ok);
        if (ok && ms > 0) {
            QTimer::singleShot(ms + 200, [seq]() { s_playRemindLoopChunk(seq); });
        }
    }
#else
    Q_UNUSED(seq);
#endif
}

SidePanelWidget::SidePanelWidget(QWidget* parent)
    : QWidget(parent) {
    setMinimumWidth(290);
    setMaximumWidth(330);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    // 外框经 Theme::boxChrome：受「盒子显示边框」（Appearance/boxBorder）控制 —— 取消勾选时
    // 收纳盒与桌面助手界面都不画那圈线框。背景、圆角不受影响。
    setStyleSheet(Theme::boxChrome(Theme::windowStyle()));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* card = new QFrame(this);
    m_card = card;
    card->setStyleSheet(Theme::boxChrome(Theme::panelStyle()));

    auto* shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(24);
    shadow->setColor(Theme::shadow());
    shadow->setOffset(0, 4);
    card->setGraphicsEffect(shadow);

    auto* mainLayout = new QVBoxLayout(card);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    setupTitleBar(card);
    m_clockFrame = setupClock(card);

    m_toolsWidget = new QuickToolsWidget(card);
    connect(m_toolsWidget, &QuickToolsWidget::toolTriggered, this, &SidePanelWidget::toolTriggered);

    m_content = new QWidget(card);
    m_content->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* contentLayout = new QVBoxLayout(m_content);
    contentLayout->setContentsMargins(12, 10, 12, 10);
    contentLayout->setSpacing(10);
    contentLayout->addWidget(m_clockFrame);
    contentLayout->addWidget(m_toolsWidget);

    // 面板内待办条目列表（未完成；圆点点击完成，右键菜单管理；无条目时隐藏）
    m_todoScroll = new QScrollArea(m_content);
    m_todoScroll->setWidgetResizable(true);
    m_todoScroll->setFrameShape(QFrame::NoFrame);
    m_todoScroll->setStyleSheet(QStringLiteral(
        "QScrollArea { background: transparent; border: none; outline: none; }"
        "QScrollArea::viewport { background: transparent; border: none; }")
        + Theme::scrollBarStyle());
    m_todoHost = new QWidget();
    m_todoHost->setStyleSheet(QStringLiteral("background: transparent;"));
    m_todoListLayout = new QVBoxLayout(m_todoHost);
    m_todoListLayout->setContentsMargins(0, 0, 0, 0);
    m_todoListLayout->setSpacing(2);
    m_todoListLayout->addStretch();
    m_todoScroll->setWidget(m_todoHost);
    m_todoScroll->setVisible(false);
    // 待办框贴住快捷工具底部（5px），并向下充满至底部添加栏（超出滚动）
    contentLayout->addSpacing(5);
    contentLayout->addWidget(m_todoScroll, 1);

    // 底部待办栏（硬编码青色边框，跟随主题色联动）
    m_todoBar = new QFrame(m_content);
    m_todoBar->setStyleSheet(Theme::applyTokens(QStringLiteral("background: rgba(17,26,46,1.0); border: 1px solid rgba(34,211,238,0.10); border-radius: 8px;")));
    auto* todoLayout = new QHBoxLayout(m_todoBar);
    todoLayout->setContentsMargins(8, 6, 8, 6);
    m_todoIcon = new QLabel(QStringLiteral("+"), m_todoBar);
    m_todoIcon->setStyleSheet(QStringLiteral("color: %1; font-size: 14px; background: transparent;")
        .arg(ThemeManager::instance()->accentColor().name()));
    auto* todoLabel = new QLabel(QStringLiteral("添加待办事项"), m_todoBar);
    todoLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent;"));
    todoLayout->addWidget(m_todoIcon);
    todoLayout->addWidget(todoLabel);
    todoLayout->addStretch();
    // 「清单」入口：打开事项清单独立窗（未完成/已完成视图）
    m_todoListBtn = new QToolButton(m_todoBar);
    m_todoListBtn->setText(QStringLiteral("清单"));
    m_todoListBtn->setToolTip(QStringLiteral("打开事项清单"));
    m_todoListBtn->setStyleSheet(Theme::toolButtonStyle());
    m_todoListBtn->setCursor(Qt::PointingHandCursor);
    connect(m_todoListBtn, &QToolButton::clicked, this, &SidePanelWidget::openTodoListWindow);
    todoLayout->addWidget(m_todoListBtn);
    // 点击待办栏空白/文字 → 添加待办事项（清单按钮自己吞掉按下事件，不受影响）
    m_todoBar->setCursor(Qt::PointingHandCursor);
    m_todoBar->installEventFilter(this);
    contentLayout->addWidget(m_todoBar);

    mainLayout->addWidget(m_content, 1);

    root->addWidget(card);

    // 天气网络管理器与定时刷新（每 3 分钟）
    m_netManager = new QNetworkAccessManager(this);
    m_weatherTimer = new QTimer(this);
    m_weatherTimer->setInterval(3 * 60 * 1000); // 3 分钟
    connect(m_weatherTimer, &QTimer::timeout, this, &SidePanelWidget::fetchWeather);
    m_weatherTimer->start();

    // 加载常规设置并应用到时钟显示（内部会触发首次天气获取）
    loadGeneralSettings();
    applyGeneralSettings();

    // 恢复桌面助手面板上次的折叠/展开状态
    {
        SettingsManager sm;
        bool collapsed = sm.loadValue(QStringLiteral("SidePanelWidget/collapsed"), false).toBool();
        if (collapsed && !m_collapsed) {
            toggleCollapse();
        }
    }

    // 待办事项：数据变化 → 重建面板列表；提醒到期 → 弹窗提示
    connect(&TodoStore::instance(), &TodoStore::itemsChanged,
            this, &SidePanelWidget::rebuildTodoList);
    connect(&TodoStore::instance(), &TodoStore::reminderDue,
            this, [this](const QString& id, const QString& text) {
                // 提醒弹框出现时播放设置中心选择的整个声音文件（未选择则静默）
                SettingsManager sm;
                const QString remindSound = sm.loadValue(QStringLiteral("Appearance/remindSound")).toString();
                // 「闹铃」开关（设置中心 → 外观设置 → 其他）：取消勾选＝静音，但提醒弹框照常弹出。
                // 每次到期都重新读盘 —— 天然拿的是最新值，改完无需重启、也不依赖广播。
                const bool soundEnabled = sm.loadValue(QStringLiteral("Appearance/remindSoundEnabled"), true).toBool();
                if (soundEnabled) startReminderSound(remindSound);   // 弹框显示期间循环播放
                const int r = GlassMessageBox::warning(this, QStringLiteral("待办提醒"), text);
                stopReminderSound();               // 弹框关闭（OK/关闭按钮）→ 停止播放
                // 只有点了「OK」才算确认，此时才清除提醒（条目行首那个闪烁的喇叭随之消失）。
                // [X]/Esc 关闭 → 不清除：提醒与标记继续留着（下次重启还会再提醒一次），
                // 这样"没看清就被关掉"不会把提醒弄丢。
                if (r == QDialog::Accepted) TodoStore::instance().acknowledgeReminder(id);
            });
    rebuildTodoList();

    // 主题色/透明度联动：连接到 ThemeManager，主题变化时重绘硬编码青色样式；
    // 窗口整体不透明度由父窗口 AssistantWindow 统一控制。
    // 主题色/透明度联动：登记到 ThemeManager，主题色变化时重绘硬编码青色样式；
    // 窗口整体不透明度由父窗口 AssistantWindow 统一控制，故本部件不登记不透明度通道。
    // E：拖透明度时本部件样式完全不需要重刷（样式串已不含透明度）。
    ThemeManager::instance()->registerThemeTarget(this, [this]() { applyTheme(); });
    applyTheme(); // 初始按当前主题色应用一次

    // 标题栏菜单按钮显示方式（设置中心「分区菜单标签显示」，与收纳盒窗口同一个键）。
    // 与「分区标签切换」同款：构造读一次 + 每次显示重读 + 订阅广播就地热更新，改完立即生效。
    connect(ThemeManager::instance(), &ThemeManager::settingChanged, this,
            [this](const QString& key, const QVariant&) {
                if (key == QLatin1String("Appearance/menuLabelShow")) reloadMenuRevealMode();
            });
    loadMenuRevealMode();
    // 初始显隐按"光标此刻是否已在面板内"判定（开机自启时鼠标可能正停在面板上）
    m_pointerInside = rect().contains(mapFromGlobal(QCursor::pos()));
    applyMenuRevealVisibility();
}

// 标题栏菜单按钮：始终显示 ↔ 悬停显示（设置中心 → 外观设置 → 分区菜单标签显示）
// 作者：谭征
void SidePanelWidget::loadMenuRevealMode() {
    SettingsManager sm;
    const int mode = sm.loadValue(QStringLiteral("Appearance/menuLabelShow"), 0).toInt();
    m_menuRevealMode = (mode == 1) ? 1 : 0;   // 脏数据一律兜底为「始终显示」
}

// reload菜单显示模式
// 作者：谭征
void SidePanelWidget::reloadMenuRevealMode() {
    const int before = m_menuRevealMode;
    loadMenuRevealMode();
    if (m_menuRevealMode != before) applyMenuRevealVisibility();
}

// **唯一**显隐出口。只动容器里的按钮，容器（含定宽占位）始终可见 → 搜索框宽度恒定。
// 作者：谭征
void SidePanelWidget::applyMenuRevealVisibility() {
    if (!m_menuBtnBox) return;
    const bool show = (m_menuRevealMode == 0) || m_pointerInside;
    for (QToolButton* b : {m_searchBtn, m_layersBtn, m_menuBtn, m_moreBtn}) {
        if (b) b->setVisible(show);
    }
}

// 鼠标移入面板 → 亮出这排按钮。面板铺满整个助手窗口（AssistantWindow 布局边距为 0），
// 因此"移入面板"与"移入桌面助手界面"是同一件事；父部件在鼠标进入自己的子部件时不会收到 Leave，
// 所以从标题栏按钮划到时钟区不会闪隐。
// 作者：谭征
void SidePanelWidget::enterEvent(QEvent* event) {
    m_pointerInside = true;
    applyMenuRevealVisibility();
    QWidget::enterEvent(event);
}

// 离开事件
// 作者：谭征
void SidePanelWidget::leaveEvent(QEvent* event) {
    m_pointerInside = false;
    applyMenuRevealVisibility();
    QWidget::leaveEvent(event);
}

// 主题色/透明度联动：重绘对话框自身硬编码青色样式，并同步窗口不透明度
// 作者：谭征
void SidePanelWidget::applyTheme() {
    // 主题色/透明度联动：重绘所有硬编码青色（rgba(34,211,238,…)）的样式，
    // 并把固定 alpha 按当前全局透明度缩放，使面板/边框随透明度设置一起变化。
    // 本函数同时是「盒子显示边框」开关的重刷出口（ThemeManager::reloadAppearanceFlags()
    // 在 boxBorder 变化时追加一次 themeChanged → 登记表 → 本函数）。因此**窗口层也要刷** ——
    // 先前只刷 m_card：面板自身那层 windowStyle 的线框与卡片层完全重合，只刷卡片的话，
    // 关闭开关后仍会残留窗口层那一圈边框。
    setStyleSheet(Theme::boxChrome(Theme::windowStyle()));
    if (m_card) m_card->setStyleSheet(Theme::boxChrome(Theme::panelStyle()));
    if (m_todoBar) {
        m_todoBar->setStyleSheet(Theme::applyTokens(QStringLiteral(
            "background: %1; border: 1px solid rgba(34,211,238,%2); border-radius: 8px;")
            .arg(Theme::windowBgString(1.0))
            .arg(Theme::alphaF(0.10), 0, 'f', 2)));
    }
    // 底部待办栏「+」号跟随主题色实时刷新
    if (m_todoIcon) {
        m_todoIcon->setStyleSheet(QStringLiteral("color: %1; font-size: 14px; background: transparent;")
            .arg(ThemeManager::instance()->accentColor().name()));
    }
    if (m_titleBar) {
        // 必须带 `QFrame#sidePanelTitleBar` 选择器：无选择器的样式表会**传播到全部子控件**
        // （Qt 的样式表继承语义），裸 QWidget 子控件会白捡一份渐变背景 + 边框 + 圆角
        // → 定宽容器 m_menuBtnBox 就会画出一个多余的「圆角矩形框」（用户 2026-09-22 反馈）。
        m_titleBar->setStyleSheet(Theme::applyTokens(QStringLiteral(
            "QFrame#sidePanelTitleBar {"
            "background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 %1,stop:1 %1);"
            "border-top-left-radius: 16px; border-top-right-radius:  16px;"
            "border-bottom: 1px solid rgba(34,211,238,%2); }")
            .arg(Theme::windowBgString(1.0))
            .arg(Theme::alphaF(0.12), 0, 'f', 2)));
    }
    if (m_layersBtn) {
        m_layersBtn->setStyleSheet(Theme::applyTokens(QStringLiteral(
            "QToolButton { color: #FFFFFF; border: 1px solid rgba(34,211,238,%1); border-radius: 6px; background: transparent; }"
            "QToolButton:hover { background: rgba(34,211,238,%2); border-color: #22D3EE; }"
            "QToolButton:pressed { background: rgba(34,211,238,%3); border-color: #22D3EE; }")
            .arg(Theme::alphaF(0.55), 0, 'f', 2)
            .arg(Theme::alphaF(0.16), 0, 'f', 2)
            .arg(Theme::alphaF(0.26), 0, 'f', 2)));
    }
    if (m_clockFrame) {
        m_clockFrame->setStyleSheet(Theme::applyTokens(QStringLiteral(
            "background: %1; border: 1px solid rgba(34,211,238,%2); border-radius: 10px;")
            .arg(Theme::windowBgString(1.0))
            .arg(Theme::alphaF(0.10), 0, 'f', 2)));
    }
    // 标题栏输入/按钮（搜索框聚焦描边、搜索/菜单/展开按钮）跟随主题色
    if (m_searchEdit) Theme::applyLineEditStyle(m_searchEdit);   // 样式表 + 固定禁用灰占位色（占位色不随主题色变）
    if (m_searchBtn) m_searchBtn->setStyleSheet(Theme::toolButtonStyle());
    if (m_menuBtn) m_menuBtn->setStyleSheet(Theme::toolButtonStyle());
    if (m_moreBtn) m_moreBtn->setStyleSheet(Theme::toolButtonStyle());
    if (m_todoListBtn) m_todoListBtn->setStyleSheet(Theme::toolButtonStyle());
    if (m_todoScroll) {
        m_todoScroll->setStyleSheet(QStringLiteral(
            "QScrollArea { background: transparent; border: none; outline: none; }"
            "QScrollArea::viewport { background: transparent; border: none; }")
            + Theme::scrollBarStyle());
    }
}

// 初始化titlebar
// 作者：谭征
void SidePanelWidget::setupTitleBar(QWidget* parent) {
    auto* titleBar = new QFrame(parent);
    titleBar->setFixedHeight(Theme::titleBarHeight());
    // 标题栏样式必须用 objectName 限定选择器（见 applyTheme 同名说明）：
    // 无选择器的规则会传播给所有子控件，裸 QWidget 容器会继承出多余的背景/圆角框。
    titleBar->setObjectName(QStringLiteral("sidePanelTitleBar"));
    titleBar->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QFrame#sidePanelTitleBar {"
        "background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 rgba(17,26,46,1.0),stop:1 rgba(17,26,46,1.0));"
        "border-top-left-radius: 16px; border-top-right-radius: 16px;"
        "border-bottom: 1px solid rgba(34,211,238,0.12); }"
    )));
    m_titleBar = titleBar;
    auto* layout = new QHBoxLayout(titleBar);
    layout->setContentsMargins(10, 4, 10, 4);
    layout->setSpacing(6);

    auto* title = new QLabel(QStringLiteral("桌面助手"), titleBar);
    // 自动对齐（2026-09-24）：标题悬停提示说明吸附规则 + Alt 逃生口。
    title->setToolTip(WindowSnap::hintText());
    QFont f = title->font();
    f.setBold(true);
    f.setPointSize(11);
    title->setFont(f);
    title->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent; border: none;"));

    m_searchEdit = new QLineEdit(titleBar);
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索"));
    m_searchEdit->setMinimumWidth(80);
    Theme::applyLineEditStyle(m_searchEdit);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &SidePanelWidget::searchTextChanged);

    auto* searchBtn = new QToolButton(titleBar);
    m_searchBtn = searchBtn;
    searchBtn->setIcon(Theme::icon("search"));
    searchBtn->setIconSize(QSize(16, 16));
    searchBtn->setText(QString());
    searchBtn->setFixedSize(24, 24);
    searchBtn->setStyleSheet(Theme::toolButtonStyle());
    connect(searchBtn, &QToolButton::clicked, this, [this]() {
        emit searchTextChanged(m_searchEdit->text());
        emit requestFileSearch(m_searchEdit->text());   // 放大镜 → 唤起「快速搜索」文件搜索窗口
    });
    // 搜索框回车同样唤起文件搜索窗口（与放大镜同义）
    connect(m_searchEdit, &QLineEdit::returnPressed, this, [this]() {
        emit requestFileSearch(m_searchEdit->text());
    });

    auto* layersBtn = new QToolButton(titleBar);
    layersBtn->setFixedSize(24, 24);
    layersBtn->setIcon(Theme::whiteIcon("market"));
    layersBtn->setIconSize(QSize(18, 18));
    layersBtn->setToolTip(QStringLiteral("组件市场"));
    // 跟随主题色的渐变入口描边
    m_layersBtn = layersBtn;
    layersBtn->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QToolButton { color: #FFFFFF; border: 1px solid rgba(34,211,238,0.55); border-radius: 6px; background: transparent; }"
        "QToolButton:hover { background: rgba(34,211,238,0.16); border-color: #22D3EE; }"
        "QToolButton:pressed { background: rgba(34,211,238,0.26); border-color: #22D3EE; }"
    )));
    connect(layersBtn, &QToolButton::clicked, this, [this]() {
        GlassMessageBox::information(this, QStringLiteral("组件市场"),
                                     QStringLiteral("组件市场正在建设中，敬请期待。"));
    });

    m_menuBtn = new QToolButton(titleBar);
    m_menuBtn->setIcon(Theme::icon("menu"));
    m_menuBtn->setIconSize(QSize(16, 16));
    m_menuBtn->setText(QString());
    m_menuBtn->setFixedSize(24, 24);
    m_menuBtn->setStyleSheet(Theme::toolButtonStyle());
    connect(m_menuBtn, &QToolButton::clicked, this, &SidePanelWidget::showMenu);

    m_moreBtn = new QToolButton(titleBar);
    m_moreBtn->setIcon(QIcon(Theme::collapsePixmap(m_collapsed)));
    m_moreBtn->setIconSize(QSize(14, 14));
    m_moreBtn->setText(QString());
    m_moreBtn->setFixedSize(Theme::headerButtonSize());
    m_moreBtn->setStyleSheet(Theme::toolButtonStyle());
    connect(m_moreBtn, &QToolButton::clicked, this, &SidePanelWidget::toggleCollapse);
    // 「收起后，鼠标移动到标题自动展开」（Appearance/autoExpandOnHover）：收起态下鼠标移入
    // 折叠按钮即展开。与收纳盒窗口同款，判定见 eventFilter。
    m_moreBtn->installEventFilter(this);

    // 红框内的这排按钮（搜索 / 组件市场 / 菜单 / 展开）装进一个**定宽容器**：
    // 「悬停显示」模式下只隐藏容器里的按钮本身、容器照旧占住原来的宽度 ——
    // 否则隐藏后布局会把空间让给左侧搜索框，鼠标一进一出搜索框就反复伸缩（很跳）。
    m_menuBtnBox = new QWidget(titleBar);
    // 定宽容器只负责"占位"，**自己不能有任何背景/边框**：它是裸 QWidget，
    // 若父标题栏的样式表未加选择器，它会继承出一圈渐变底 + 圆角描边（多余的圆角矩形框）。
    // 父样式已用 objectName 限定，这里再显式声明一次兜底 + 表达意图。
    m_menuBtnBox->setObjectName(QStringLiteral("sidePanelMenuBtnBox"));
    m_menuBtnBox->setStyleSheet(QStringLiteral(
        "QWidget#sidePanelMenuBtnBox { background: transparent; border: none; }"));
    auto* menuBtnLayout = new QHBoxLayout(m_menuBtnBox);
    menuBtnLayout->setContentsMargins(0, 0, 0, 0);
    menuBtnLayout->setSpacing(layout->spacing());   // 与标题栏同间距，视觉上与原布局完全一致
    menuBtnLayout->addWidget(searchBtn);
    menuBtnLayout->addWidget(layersBtn);
    menuBtnLayout->addWidget(m_menuBtn);
    menuBtnLayout->addWidget(m_moreBtn);
    {
        int w = 0;
        // 4 个按钮都是 setFixedSize：sizeHint 可能比固定尺寸小（QToolButton 只按内容算），
        // 故取 sizeHint 与 minimumWidth 的较大者，保证容器宽度 ≥ 按钮实际占位，不会把按钮压变形。
        for (QToolButton* b : {searchBtn, layersBtn, m_menuBtn, m_moreBtn})
            w += qMax(b->sizeHint().width(), b->minimumWidth());
        m_menuBtnBox->setFixedWidth(w + layout->spacing() * 3);
    }

    layout->addWidget(title);
    layout->addWidget(m_searchEdit, 1);
    layout->addWidget(m_menuBtnBox);

    auto* mainLayout = qobject_cast<QVBoxLayout*>(parent->layout());
    if (mainLayout) mainLayout->addWidget(titleBar);
}

// 创建market图标
// 作者：谭征
QPixmap SidePanelWidget::createMarketIcon() const {
    // 使用主题切图市场图标；保持函数签名以兼容旧代码，统一渲染为白色
    return Theme::recolorPixmap(Theme::icon("market").pixmap(24, 24), Qt::white);
}

// 初始化clock
// 作者：谭征
QFrame* SidePanelWidget::setupClock(QWidget* parent) {
    auto* clockFrame = new QFrame(parent);
    clockFrame->setFrameShape(QFrame::NoFrame);
    clockFrame->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "background: rgba(17,26,46,1.0); border: 1px solid rgba(34,211,238,0.10); border-radius: 10px;"
    )));
    auto* clockLayout = new QVBoxLayout(clockFrame);
    clockLayout->setContentsMargins(10, 8, 10, 8);
    clockLayout->setSpacing(8);

    // 第一行：大号时间 + 右侧天气图标
    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(4);
    m_timeLabel = new QLabel(clockFrame);
    QFont tf = m_timeLabel->font();
    // 默认开启显示秒：30pt 的 hh:mm:ss + 温度 + 图标超出 320px 面板宽度，
    // 布局压穿 minimum 后 QLabel 文本溢出绘制会与温度重叠 → 显示秒时大字体档位用 26pt。
    tf.setPointSize(26);
    tf.setBold(true);
    m_timeLabel->setFont(tf);
    m_timeLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent;"));
    updateTimeLabelMinWidth();   // 预留「88:88:88」宽度，避免显示秒时末位被右侧天气区挤掉

    m_weatherIcon = new QLabel(clockFrame);
    m_weatherIcon->setFixedSize(36, 36);
    m_weatherIcon->setAlignment(Qt::AlignCenter);
    // 默认显示云图标；拿到真实天气代码后由 weatherPixmap 切换为 晴/雨/雪
    m_weatherIcon->setPixmap(weatherPixmap(3, 28));
    m_weatherIcon->setStyleSheet(QStringLiteral("background: transparent; border: none;"));

    // 温度文字（图标左侧），初始隐藏，拿到真实天气后显示
    m_weatherTempLabel = new QLabel(clockFrame);
    m_weatherTempLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QFont wtf = m_weatherTempLabel->font();
    wtf.setPointSize(14);
    wtf.setBold(true);
    m_weatherTempLabel->setFont(wtf);
    m_weatherTempLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent;"));
    m_weatherTempLabel->hide();

    topRow->addWidget(m_timeLabel);
    topRow->addStretch();
    topRow->addWidget(m_weatherTempLabel);
    topRow->addWidget(m_weatherIcon);

    // 第二行：日期/星期/农历拆为上下两行，避免单行太宽导致中文被截断
    auto* infoRow = new QFrame(clockFrame);
    infoRow->setFrameShape(QFrame::NoFrame);
    infoRow->setStyleSheet(QStringLiteral(
        "background: transparent; border: none; border-radius: 0px;"
    ));
    auto* infoLayout = new QVBoxLayout(infoRow);
    // 左外边距留出少量偏移，使日期/农历左边缘与大号时间第一个数字的左边缘对齐
    // （时间文本内部有轻微左留白，故这里不取 0，而是右移几个像素）
    infoLayout->setContentsMargins(12, 4, 8, 4);
    infoLayout->setSpacing(2);

    // 第一行：日期 · 星期（左对齐，与时间左边缘对齐）
    auto* firstRow = new QHBoxLayout;
    firstRow->setSpacing(0);
    firstRow->setContentsMargins(0, 0, 0, 0);

    m_dateLabel = new QLabel(clockFrame);
    m_dateLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    QFont df = m_dateLabel->font();
    df.setPointSize(10);
    m_dateLabel->setFont(df);
    m_dateLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent;"));

    auto* sep1 = new QLabel(QStringLiteral("·"), clockFrame);
    sep1->setStyleSheet(QStringLiteral("color: rgba(255,255,255,0.5); background: transparent; padding: 0px 4px;"));

    m_weekdayLabel = new QLabel(clockFrame);
    m_weekdayLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    QFont wdf = m_weekdayLabel->font();
    wdf.setPointSize(10);
    m_weekdayLabel->setFont(wdf);
    m_weekdayLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent;"));

    firstRow->addWidget(m_dateLabel);
    firstRow->addWidget(sep1);
    firstRow->addWidget(m_weekdayLabel);
    firstRow->addStretch();

    // 第二行：农历（左对齐，与时间左边缘对齐）
    auto* secondRow = new QHBoxLayout;
    secondRow->setSpacing(0);
    secondRow->setContentsMargins(0, 0, 0, 0);

    m_lunarLabel = new QLabel(clockFrame);
    m_lunarLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_lunarLabel->setStyleSheet(QStringLiteral("color: rgba(255,255,255,0.5); background: transparent;"));

    secondRow->addWidget(m_lunarLabel);
    secondRow->addStretch();

    infoLayout->addLayout(firstRow);
    infoLayout->addLayout(secondRow);

    clockLayout->addLayout(topRow);
    clockLayout->addWidget(infoRow);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &SidePanelWidget::updateTime);
    m_timer->start(1000);
    updateTime();

    return clockFrame;
}

// 更新time
// 作者：谭征
void SidePanelWidget::updateTime() {
    QDateTime now = QDateTime::currentDateTime();
    m_timeLabel->setText(now.toString(m_showSeconds ? QStringLiteral("hh:mm:ss") : QStringLiteral("hh:mm")));
    m_dateLabel->setText(now.toString(QStringLiteral("MM月dd日")));
    m_weekdayLabel->setText(now.toString(QStringLiteral("dddd")));
    // 360 截图显示：丙午·马·七月初七
    m_lunarLabel->setText(QStringLiteral("丙午·马·七月初七"));
}

// 更新time标签min宽度
// 作者：谭征
void SidePanelWidget::updateTimeLabelMinWidth() {
    if (!m_timeLabel) return;
    // 用最宽样本「88:88:88」按当前字体度量预留宽度：布局按标签 sizeHint 分配空间，
    // 「显示秒」从 hh:mm 切到 hh:mm:ss 时若不预留，大号字体的末位会被右侧天气温度/图标挤掉。
    const int w = QFontMetrics(m_timeLabel->font())
                      .horizontalAdvance(QStringLiteral("88:88:88"));
    m_timeLabel->setMinimumWidth(w + 4);
}

// 加载general设置
// 作者：谭征
void SidePanelWidget::loadGeneralSettings() {
    // 构造期调用：从设置载入“是否显示秒 / 是否大字体”成员。仅此一次在干净状态下读盘，可靠。
    SettingsManager sm;
    m_showSeconds = sm.loadValue(QStringLiteral("General/showSeconds"), true).toBool();
    m_largeTimeFont = sm.loadValue(QStringLiteral("General/largeTimeFont"), true).toBool();
    m_showCompletedItems = sm.loadValue(QStringLiteral("General/showCompleted"), true).toBool();
}

// 应用general设置
// 作者：谭征
void SidePanelWidget::applyGeneralSettings() {
    // 关键修复（2026-09-19）：此处【不再】从设置回读 m_showSeconds / m_largeTimeFont。
    // 旧实现会在套用前用 SettingsManager 重读，而设置中心保存与面板读取之间存在 QSettings
    // 跨实例缓存时序差，会读到旧值、再把信号 lambda 刚下发的正确值覆盖回去
    // （现象即“勾选大字体/显示秒无反应”）。成员值由 loadGeneralSettings() 初始化、
    // 由信号 lambda 直接赋值，这里只负责把当前成员状态套到 UI。
    SettingsManager sm;
    bool showClockWeather = sm.loadValue(QStringLiteral("General/showClockWeather"), true).toBool();

    if (m_timeLabel) {
        QFont f = m_timeLabel->font();
        // 字号与「时间大字体显示」复选框绑定：勾选=大字体 26pt，不勾选=小字体 14pt（与温度文字同字号）。
        f.setPointSize(m_largeTimeFont ? 26 : 14);
        f.setBold(true);
        m_timeLabel->setFont(f);
        updateTimeLabelMinWidth();   // 字体大小变化后重算预留宽度
    }
    if (m_clockFrame) {
        m_clockFrame->setVisible(showClockWeather);
    }
    updateTime();

    // 时钟/天气模块可见时，先显示缓存
    if (showClockWeather) {
        applyWeatherCache();
        // 若用户从未手动设置过天气区域，首次启动时根据 IP 自动定位；否则按保存的城市拉取天气
        bool regionSet = sm.loadValue(QStringLiteral("General/weatherRegionSet"), false).toBool();
        if (regionSet) {
            fetchWeather();
        } else {
            fetchIpLocation();
        }
    }
}

// 清除搜索
// 作者：谭征
void SidePanelWidget::clearSearch() {
    if (m_searchEdit) m_searchEdit->clear();
}

// 当前搜索文本
// 作者：谭征
QString SidePanelWidget::currentSearchText() const {
    return m_searchEdit ? m_searchEdit->text() : QString();
}

// 应用weather缓存
// 作者：谭征
void SidePanelWidget::applyWeatherCache() {
    SettingsManager sm;
    QString cachedCity = sm.loadValue(QStringLiteral("Weather/city"), QString()).toString();
    QVariant tempV = sm.loadValue(QStringLiteral("Weather/temperature"), QVariant());
    QVariant codeV = sm.loadValue(QStringLiteral("Weather/code"), QVariant());
    m_weatherLat = sm.loadValue(QStringLiteral("Weather/lat"), 0.0).toDouble();
    m_weatherLon = sm.loadValue(QStringLiteral("Weather/lon"), 0.0).toDouble();
    m_weatherCity = cachedCity;

    if (tempV.isValid() && !tempV.isNull() && m_weatherTempLabel) {
        m_weatherTempLabel->setText(QString::number(tempV.toDouble(), 'f', 0) + QStringLiteral("°"));
        m_weatherTempLabel->setToolTip(weatherDescription(codeV.toInt()) + QStringLiteral(" · ") + cachedCity);
        m_weatherTempLabel->show();
    }
    // 缓存命中天气代码时同步图标（晴/雨/雪）
    if (codeV.isValid() && !codeV.isNull() && m_weatherIcon) {
        m_weatherIcon->setPixmap(weatherPixmap(codeV.toInt(), 28));
    }
}

// 天气（Open-Meteo，免费、无需 key）
// 作者：谭征
void SidePanelWidget::fetchWeather() {
    if (!m_netManager) return;

    SettingsManager sm;
    QString province = sm.loadValue(QStringLiteral("General/weatherProvinceName"), QStringLiteral("陕西")).toString();
    QString city = sm.loadValue(QStringLiteral("General/weatherCityName"), QString()).toString();
    QString district = sm.loadValue(QStringLiteral("General/weatherDistrictName"), QString()).toString();

    // 优先用城市名，缺失时退回省份名
    QString query = city.isEmpty() ? province : city;

    // 城市未变化且已解析过经纬度：直接拉取天气，避免重复地理编码
    if (!m_weatherCity.isEmpty() && query == m_weatherCity
        && m_weatherLat != 0.0 && m_weatherLon != 0.0 && !m_weatherPending) {
        fetchWeatherData();
        return;
    }

    m_weatherCity = query;

    // 地理编码：城市名 → 经纬度（Open-Meteo，免费无 key，支持中文）
    if (m_geocodePending) return;
    m_geocodePending = true;

    QUrl url(QStringLiteral("https://geocoding-api.open-meteo.com/v1/search"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("name"), query);
    q.addQueryItem(QStringLiteral("count"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("language"), QStringLiteral("zh"));
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    url.setQuery(q);
    QNetworkReply* reply = m_netManager->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        onGeocodeFinished(reply);
    });
}

// 响应geocodefinished
// 作者：谭征
void SidePanelWidget::onGeocodeFinished(QNetworkReply* reply) {
    m_geocodePending = false;
    if (!reply || reply->error() != QNetworkReply::NoError) {
        // 网络失败：保持默认图标/缓存温度即可
        return;
    }
    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) return;
    QJsonArray results = doc.object().value(QStringLiteral("results")).toArray();
    if (results.isEmpty()) return;
    QJsonObject first = results.first().toObject();
    m_weatherLat = first.value(QStringLiteral("latitude")).toDouble();
    m_weatherLon = first.value(QStringLiteral("longitude")).toDouble();

    // 缓存经纬度，避免重复地理编码
    SettingsManager sm;
    sm.saveValue(QStringLiteral("Weather/lat"), m_weatherLat);
    sm.saveValue(QStringLiteral("Weather/lon"), m_weatherLon);
    sm.saveValue(QStringLiteral("Weather/city"), m_weatherCity);
    sm.sync();

    fetchWeatherData();
}

// fetchweather数据
// 作者：谭征
void SidePanelWidget::fetchWeatherData() {
    if (!m_netManager || m_weatherLat == 0.0 || m_weatherLon == 0.0) return;
    if (m_weatherPending) return;
    m_weatherPending = true;

    QUrl url(QStringLiteral("https://api.open-meteo.com/v1/forecast"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("latitude"), QString::number(m_weatherLat, 'f', 4));
    q.addQueryItem(QStringLiteral("longitude"), QString::number(m_weatherLon, 'f', 4));
    q.addQueryItem(QStringLiteral("current_weather"), QStringLiteral("true"));
    url.setQuery(q);
    QNetworkReply* reply = m_netManager->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        onWeatherFinished(reply);
    });
}

// 响应weatherfinished
// 作者：谭征
void SidePanelWidget::onWeatherFinished(QNetworkReply* reply) {
    m_weatherPending = false;
    if (!reply || reply->error() != QNetworkReply::NoError) return;

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) return;
    QJsonObject cw = doc.object().value(QStringLiteral("current_weather")).toObject();
    if (cw.isEmpty()) return;

    double temp = cw.value(QStringLiteral("temperature")).toDouble();
    int code = cw.value(QStringLiteral("weathercode")).toInt();

    if (m_weatherTempLabel) {
        m_weatherTempLabel->setText(QString::number(temp, 'f', 0) + QStringLiteral("°"));
        m_weatherTempLabel->setToolTip(weatherDescription(code) + QStringLiteral(" · ") + m_weatherCity);
        m_weatherTempLabel->show();
    }
    // 按天气代码切换图标：晴 / 雨 / 雪 / 多云 / 阴 等
    if (m_weatherIcon) {
        m_weatherIcon->setPixmap(weatherPixmap(code, 28));
    }

    // 缓存温度与天气代码
    SettingsManager sm;
    sm.saveValue(QStringLiteral("Weather/temperature"), temp);
    sm.saveValue(QStringLiteral("Weather/code"), code);
    sm.saveValue(QStringLiteral("Weather/city"), m_weatherCity);
    sm.sync();
}

// weatherdescription
// 作者：谭征
QString SidePanelWidget::weatherDescription(int wmoCode) {
    // WMO Weather interpretation codes
    switch (wmoCode) {
        case 0:  return QStringLiteral("晴");
        case 1:  return QStringLiteral("多云转晴");
        case 2:  return QStringLiteral("局部多云");
        case 3:  return QStringLiteral("阴");
        case 45: case 48: return QStringLiteral("雾");
        case 51: case 53: case 55: return QStringLiteral("毛毛雨");
        case 56: case 57: return QStringLiteral("冻毛毛雨");
        case 61: case 63: case 65: return QStringLiteral("小雨");
        case 66: case 67: return QStringLiteral("冻雨");
        case 71: case 73: case 75: return QStringLiteral("雪");
        case 77: return QStringLiteral("雪粒");
        case 80: case 81: case 82: return QStringLiteral("阵雨");
        case 85: case 86: return QStringLiteral("阵雪");
        case 95: return QStringLiteral("雷阵雨");
        case 96: case 99: return QStringLiteral("雷阵雨伴冰雹");
        default: return QStringLiteral("未知");
    }
}

// weatherpixmap
// 作者：谭征
QPixmap SidePanelWidget::weatherPixmap(int wmoCode, int size) {
    // 依据 WMO 天气代码绘制不同图标：晴 / 多云 / 阴 / 雨 / 雪
    // 全部用 QPainter 矢量绘制，无需额外图片资源，且贴合科技青白主题
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::HighQualityAntialiasing, true);

    const QColor sun(255, 211, 86);    // 暖阳黄
    const QColor cloud(225, 235, 245); // 云白
    const QColor rain(90, 200, 250);   // 雨青蓝
    const QColor snow(255, 255, 255);  // 雪白
    const qreal S = size / 28.0;       // 以 28px 为基准做等比缩放

    // 将天气代码归类到展示类别
    enum Cat { Clear, Partly, Cloud, Rain, Snow } cat = Cloud;
    if (wmoCode == 0) cat = Clear;                                   // 晴
    else if (wmoCode == 1 || wmoCode == 2) cat = Partly;             // 多云转晴 / 局部多云
    else if (wmoCode == 3 || wmoCode == 45 || wmoCode == 48) cat = Cloud; // 阴 / 雾
    else if ((wmoCode >= 51 && wmoCode <= 67)
             || (wmoCode >= 80 && wmoCode <= 82)
             || wmoCode == 95 || wmoCode == 96 || wmoCode == 99) cat = Rain;  // 各类雨 / 雷阵雨
    else if ((wmoCode >= 71 && wmoCode <= 77)
             || wmoCode == 85 || wmoCode == 86) cat = Snow;          // 各类雪 / 阵雪
    else cat = Cloud;

    auto drawSun = [&](qreal cx, qreal cy, qreal r) {
        p.save();
        p.translate(cx, cy);
        QPen sunPen(sun, 2.0 * S);
        sunPen.setCapStyle(Qt::RoundCap);
        p.setPen(sunPen);
        for (int i = 0; i < 8; ++i) {
            p.rotate(45.0);
            p.drawLine(QPointF(0, -r - 1.5 * S), QPointF(0, -r - 5.0 * S));
        }
        p.restore();
        p.setBrush(sun);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(cx, cy), r, r);
    };

    auto drawCloud = [&](qreal cx, qreal cy, qreal s, const QColor& c) {
        p.setBrush(c);
        p.setPen(Qt::NoPen);
        // 由若干圆形 + 底部圆角矩形拼出云朵轮廓
        p.drawEllipse(QPointF(cx - s * 0.38, cy + s * 0.05), s * 0.30, s * 0.30);
        p.drawEllipse(QPointF(cx + s * 0.38, cy + s * 0.05), s * 0.30, s * 0.30);
        p.drawEllipse(QPointF(cx, cy - s * 0.22), s * 0.36, s * 0.36);
        p.drawEllipse(QPointF(cx, cy), s * 0.55, s * 0.42);
        p.drawRoundedRect(QRectF(cx - s * 0.55, cy - s * 0.02, s * 1.10, s * 0.52), s * 0.22, s * 0.22);
    };

    if (cat == Clear) {
        drawSun(14 * S, 14 * S, 6 * S);
    } else if (cat == Partly) {
        drawSun(9 * S, 9 * S, 4.2 * S);
        drawCloud(16 * S, 17 * S, 16 * S, cloud);
    } else if (cat == Cloud) {
        drawCloud(14 * S, 14 * S, 20 * S, cloud);
    } else if (cat == Rain) {
        drawCloud(14 * S, 11 * S, 16 * S, cloud);
        p.setBrush(rain);
        p.setPen(Qt::NoPen);
        auto drop = [&](qreal x, qreal y) { p.drawEllipse(QPointF(x, y), 1.6 * S, 2.6 * S); };
        drop(9 * S, 22 * S); drop(14 * S, 23 * S); drop(19 * S, 22 * S);
    } else if (cat == Snow) {
        drawCloud(14 * S, 11 * S, 16 * S, cloud);
        p.setBrush(snow);
        p.setPen(Qt::NoPen);
        auto flake = [&](qreal x, qreal y) { p.drawEllipse(QPointF(x, y), 2.0 * S, 2.0 * S); };
        flake(9 * S, 22 * S); flake(14 * S, 23 * S); flake(19 * S, 22 * S);
    }

    p.end();
    return pm;
}

// 刷新weatherifcity变化信号
// 作者：谭征
void SidePanelWidget::refreshWeatherIfCityChanged() {
    // 设置中心切换城市后，强制重新地理编码并拉取天气
    m_weatherLat = 0.0;
    m_weatherLon = 0.0;
    m_weatherCity.clear();
    fetchWeather();
}

// 开始ipgeorequest
// 作者：谭征
void SidePanelWidget::startIpGeoRequest(bool useHttps) {
    if (!m_netManager) return;

    QUrl url(useHttps ? QStringLiteral("https://ip9.com.cn/get")
                      : QStringLiteral("http://ip9.com.cn/get"));
    QNetworkReply* reply = m_netManager->get(QNetworkRequest(url));

    connect(reply, &QNetworkReply::sslErrors, this, [reply](const QList<QSslError>&) {
        reply->ignoreSslErrors();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        onIpGeoFinished(reply);
    });
}

// fetchiplocation
// 作者：谭征
void SidePanelWidget::fetchIpLocation() {
    if (!m_netManager || m_ipGeoPending) return;
    m_ipGeoPending = true;
    m_ipGeoTryHttpFallback = false;

    startIpGeoRequest(true); // 先 HTTPS；失败后再自动 fallback 到 HTTP
}

// 响应ipgeofinished
// 作者：谭征
void SidePanelWidget::onIpGeoFinished(QNetworkReply* reply) {
    // HTTPS 失败且无 HTTP fallback 过，自动降级到 HTTP 再试一次
    if (reply && reply->error() != QNetworkReply::NoError &&
        reply->url().scheme() == QStringLiteral("https") && !m_ipGeoTryHttpFallback) {
        m_ipGeoTryHttpFallback = true;
        startIpGeoRequest(false); // HTTP fallback
        return;
    }

    m_ipGeoPending = false;
    m_ipGeoTryHttpFallback = false;
    if (!reply || reply->error() != QNetworkReply::NoError) {
        // IP 定位失败：继续用默认城市拉天气，不阻塞启动
        fetchWeather();
        return;
    }

    auto info = SettingCenterDialog::parseIp9Location(reply->readAll());
    if (!info.valid) {
        fetchWeather();
        return;
    }

    // 若接口未返回区县，回退到该市第一个区县
    QString district = info.district;
    if (district.isEmpty()) {
        QStringList ds = RegionData::districts(info.province, info.city);
        if (!ds.isEmpty()) district = ds.first();
    }

    // 持久化到设置，并标记为已设置
    SettingsManager sm;
    sm.saveValue(QStringLiteral("General/weatherProvinceName"), info.province);
    sm.saveValue(QStringLiteral("General/weatherCityName"), info.city);
    sm.saveValue(QStringLiteral("General/weatherDistrictName"), district);
    sm.saveValue(QStringLiteral("General/weatherRegionSet"), true);
    sm.saveValue(QStringLiteral("Weather/lat"), info.lat);
    sm.saveValue(QStringLiteral("Weather/lon"), info.lon);
    sm.saveValue(QStringLiteral("Weather/city"), info.city);
    sm.sync();

    // 直接用 IP 返回的经纬度拉天气，跳过 Open-Meteo 地理编码
    m_weatherLat = info.lat;
    m_weatherLon = info.lon;
    m_weatherCity = info.city;
    fetchWeatherData();
}

// 当前是否处于折叠状态
// 作者：谭征
bool SidePanelWidget::isCollapsed() const {
    return m_collapsed;
}

// 切换折叠
// 作者：谭征
void SidePanelWidget::toggleCollapse() {
    if (!m_content) return;
    m_collapsed = !m_collapsed;
    m_content->setVisible(!m_collapsed);
    if (m_moreBtn) {
        m_moreBtn->setIcon(QIcon(Theme::collapsePixmap(m_collapsed)));
    }

    // 折叠时不再纵向扩张，避免父窗口仍被撑大留下大片空白
    if (m_collapsed) {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    } else {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    }
    updateGeometry();

    emit collapseToggled(m_collapsed);

    // 持久化桌面助手面板折叠状态
    SettingsManager sm;
    sm.saveValue(QStringLiteral("SidePanelWidget/collapsed"), m_collapsed);
}

// 显示菜单
// 作者：谭征
void SidePanelWidget::showMenu() {
    QMenu menu(this);
    Theme::applyMenuStyle(&menu);

    QAction* appearanceAction = menu.addAction(Theme::icon("appearance"), QStringLiteral("改变外观"));
    QAction* settingsAction = menu.addAction(Theme::icon("settings"), QStringLiteral("设置中心"));
    QAction* dockResetAction = menu.addAction(Theme::icon("refresh"), QStringLiteral("dock复位"));
    QAction* newBoxAction = menu.addAction(Theme::icon("new_box"), QStringLiteral("新建收纳盒"));
    QAction* toggleDesktopAction = menu.addAction(Theme::icon("view_grid"), QStringLiteral("显示/隐藏系统桌面"));
    menu.addSeparator();
    QAction* updateAction = menu.addAction(Theme::icon("refresh"), QStringLiteral("检查更新"));
    QAction* feedbackAction = menu.addAction(Theme::icon("rename"), QStringLiteral("我要反馈"));
    menu.addSeparator();
    QAction* exitAction = menu.addAction(Theme::icon("close"), QStringLiteral("退出桌面助手"));

    QAction* chosen = menu.exec(m_menuBtn ? m_menuBtn->mapToGlobal(QPoint(0, m_menuBtn->height())) : QCursor::pos());
    if (chosen == exitAction) {
        qApp->quit();
    } else if (chosen == updateAction) {
        // 当前固定演示为「无更新」状态；将 false 改为 true 即可测试「有更新」分支
        UpdateDialog dlg(false, QString(), this);
        dlg.exec();
    } else if (chosen == feedbackAction) {
        FeedbackDialog dlg(this);
        dlg.exec();
    } else if (chosen == appearanceAction) {
        SettingCenterDialog dlg(SettingCenterDialog::TabAppearance, this);
        connect(&dlg, &SettingCenterDialog::timeShowSecondsChanged,
                this, [this](bool show) { m_showSeconds = show; updateTime(); saveGeneralClockSettings(); });
        connect(&dlg, &SettingCenterDialog::timeLargeFontChanged,
                this, [this](bool large) { m_largeTimeFont = large; applyGeneralSettings(); });
        connect(&dlg, &SettingCenterDialog::showClockWeatherChanged,
                this, [this](bool show) { if (m_clockFrame) m_clockFrame->setVisible(show); saveGeneralClockSettings(); });
        connect(&dlg, &SettingCenterDialog::showCompletedChanged,
                this, [this](bool show) { m_showCompletedItems = show; rebuildTodoList(); });
        connect(&dlg, &SettingCenterDialog::showMainWindowChanged,
                this, &SidePanelWidget::mainWindowVisibilityChanged);
        connect(&dlg, &SettingCenterDialog::quickToolsOrderChanged,
                this, &SidePanelWidget::updateToolOrder);
        connect(&dlg, &SettingCenterDialog::requestCreateNewBox,
                this, &SidePanelWidget::requestCreateNewBox);
        connect(&dlg, &SettingCenterDialog::weatherRegionChanged,
                this, &SidePanelWidget::refreshWeatherIfCityChanged);
        dlg.exec();
        // 关闭后按最新配置强制同步主界面快捷工具
        syncToolsFromSettings();
    } else if (chosen == settingsAction) {
        SettingCenterDialog dlg(SettingCenterDialog::TabGeneral, this);
        connect(&dlg, &SettingCenterDialog::timeShowSecondsChanged,
                this, [this](bool show) { m_showSeconds = show; updateTime(); saveGeneralClockSettings(); });
        connect(&dlg, &SettingCenterDialog::timeLargeFontChanged,
                this, [this](bool large) { m_largeTimeFont = large; applyGeneralSettings(); });
        connect(&dlg, &SettingCenterDialog::showClockWeatherChanged,
                this, [this](bool show) { if (m_clockFrame) m_clockFrame->setVisible(show); saveGeneralClockSettings(); });
        connect(&dlg, &SettingCenterDialog::showCompletedChanged,
                this, [this](bool show) { m_showCompletedItems = show; rebuildTodoList(); });
        connect(&dlg, &SettingCenterDialog::showMainWindowChanged,
                this, &SidePanelWidget::mainWindowVisibilityChanged);
        connect(&dlg, &SettingCenterDialog::quickToolsOrderChanged,
                this, &SidePanelWidget::updateToolOrder);
        connect(&dlg, &SettingCenterDialog::requestCreateNewBox,
                this, &SidePanelWidget::requestCreateNewBox);
        connect(&dlg, &SettingCenterDialog::weatherRegionChanged,
                this, &SidePanelWidget::refreshWeatherIfCityChanged);
        dlg.exec();
        // 关闭后按最新配置强制同步主界面快捷工具
        syncToolsFromSettings();
    } else if (chosen == newBoxAction) {
        // 直接触发新建收纳盒，复用设置中心同款信号链路
        emit requestCreateNewBox();
    } else if (chosen == dockResetAction) {
        // dock 复位：清空 dock 内图标 → 临时还原原生桌面重新扫描图标位置 → 按 Windows 桌面最新位置重排。
        emit requestDockReset();
    } else if (chosen == toggleDesktopAction) {
        // 显示/隐藏系统桌面图标层（安全兜底）
        emit requestToggleNativeDesktop();
    }
}

// 保存generalclock设置
// 作者：谭征
void SidePanelWidget::saveGeneralClockSettings() {
    SettingsManager sm;
    sm.saveValue(QStringLiteral("General/showSeconds"), m_showSeconds);
    sm.saveValue(QStringLiteral("General/largeTimeFont"), m_largeTimeFont);
    sm.saveValue(QStringLiteral("General/showClockWeather"), m_clockFrame ? m_clockFrame->isVisible() : true);
    sm.sync();
}

// 同步tools从设置
// 作者：谭征
void SidePanelWidget::syncToolsFromSettings() {
    if (!m_toolsWidget) return;
    SettingsManager sm;
    QStringList order = sm.loadValue(QStringLiteral("QuickTools/order")).toStringList();
    if (order.isEmpty()) {
        order = QuickToolsWidget::defaultToolNames();
    }
    m_toolsWidget->setToolOrder(order);
}

// 打开设置中心时也要 connect 这条信号；此前仅 sidepanelwidget 内部打开时可达）
// 作者：谭征
void SidePanelWidget::updateToolOrder(const QStringList& order) {
    if (m_toolsWidget) {
        m_toolsWidget->setToolOrder(order);
    }
}

// 待办事项（面板侧）

// 作者：谭征
void SidePanelWidget::rebuildTodoList() {
    if (!m_todoListLayout || !m_todoScroll) return;
    // 清空旧行（保留末尾 stretch）
    while (m_todoListLayout->count() > 1) {
        QLayoutItem* item = m_todoListLayout->takeAt(0);
        if (item && item->widget()) {
            item->widget()->hide();
            item->widget()->setParent(nullptr);
            item->widget()->deleteLater();
        }
        delete item;
    }
    // 「显示已完成事项」未勾选 → 只列未完成；勾选 → 全量按存储顺序显示（已完成条目留在原位，不挪末尾）
    const QVector<TodoItem> list = m_showCompletedItems
        ? TodoStore::instance().items()
        : TodoStore::instance().undoneItems();
    for (const TodoItem& it : list) {
        QWidget* row = buildTodoRow(it.id);
        m_todoListLayout->insertWidget(m_todoListLayout->count() - 1, row);
    }
    m_todoScroll->setVisible(!list.isEmpty());
}

// 构建todo行
// 作者：谭征
QWidget* SidePanelWidget::buildTodoRow(const QString& id) {
    const TodoItem it = TodoStore::instance().item(id);
    auto* row = new QFrame(m_todoHost);
    row->setStyleSheet(QStringLiteral(
        "QFrame { background: transparent; border: none; border-radius: 8px; }"));
    row->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(row, &QWidget::customContextMenuRequested, row, [this, row, id](const QPoint&) {
        const TodoUi::MenuResult r = TodoUi::execItemMenu(row, TodoStore::instance().item(id));
        TodoUi::applyItemAction(this, id, r);
    });

    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(6, 3, 6, 3);
    lay->setSpacing(8);

    // 圆点：点击切换 完成/恢复（优先级着色：高红/中黄/低绿；已完成=灰实心）
    auto* circle = new QToolButton(row);
    circle->setFixedSize(12, 12);
    circle->setCursor(Qt::PointingHandCursor);
    QString bg = QStringLiteral("transparent");
    if (it.done) {
        bg = QStringLiteral("rgba(255,255,255,0.30)");
    } else {
        switch (it.priority) {
            case 3: bg = QStringLiteral("#F87171"); break;
            case 2: bg = QStringLiteral("#FBBF24"); break;
            case 1: bg = QStringLiteral("#34D399"); break;
            default: break;
        }
    }
    circle->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QToolButton { background: %1; %2 border-radius: 6px; padding: 0; }"
        "QToolButton:hover { border: 1px solid %3; }")
        .arg(bg, (!it.done && it.priority == 0) ? QStringLiteral("border: 1px solid rgba(255,255,255,0.55);")
                                  : QStringLiteral("border: none;"),
             ThemeManager::instance()->accentColor().name())));
    connect(circle, &QToolButton::clicked, circle, [id, done = it.done]() {
        TodoStore::instance().setDone(id, !done);
    });
    // 提醒标记（喇叭 + 一闪一闪）：设了提醒的条目，在**行的最前方**显示一个喇叭图标。
    // · 图标：单色 PNG 资源 `speaker`（tools/make_speaker_icon.py 生成）染成提醒红 #FA5151。
    // 与右侧 12px **方形**优先级圆点区分：这里 14px 线性图标、且位置最靠左。
    // · 闪烁：QGraphicsOpacityEffect + 无限循环 QPropertyAnimation（1.0→0.15→1.0，900ms）。
    // effect 与 animation 都以本行为父对象 —— 行重建/销毁时一并回收，不留野动画。
    // · 消失时机：用户点过提醒弹框的 OK → TodoStore::acknowledgeReminder → itemsChanged → 这里重建时不再画。
    if (it.reminder.isValid()) {
        auto* bell = new QLabel(row);
        bell->setFixedSize(14, 14);
        // Qt 5.14 的 QIcon **没有**带 devicePixelRatio 的 pixmap() 重载（Qt 6 才有），
        // 只能自己按 DPR 算出需要的**设备像素**再让 QIcon 挑最合适的切图（24/48 两档）。
        const qreal dpr = devicePixelRatioF();
        const int px = qMax(14, qRound(14 * dpr));
        QPixmap pm = Theme::icon(QStringLiteral("speaker")).pixmap(QSize(px, px));
        pm = Theme::recolorPixmap(pm, QColor(QStringLiteral("#FA5151")));
        pm.setDevicePixelRatio(dpr);   // 按 DPR 出图 → 在屏上仍是 14×14 逻辑像素、缩放屏不发虚
        // —— 第二帧：预先按 0.25 不透明度合成好的同一张图 ——
        // B9：原实现用 QGraphicsOpacityEffect + 无限循环 QPropertyAnimation 做闪烁。
        // effect 是**离屏渲染**（每一帧都要把所在部件渲染进一张 pixmap 再整体合成），
        // 只要还有未确认的提醒就 60fps 常驻跑 —— 等于让 GUI 线程永远背着一份合成负担；
        // 而 GUI 线程同时要同步回调系统级低级鼠标钩子，这份负担会直接转成"全局鼠标发涩"。
        // 改为「两帧位图 + 450ms 定时器互换」：每 450ms 只有一次 setPixmap（14×14 的小图），
        // 没有 effect、没有动画帧、没有离屏合成；视觉上仍是原来的"一闪一闪"（一明一暗 = 900ms）。
        QPixmap pmDim(px, px);
        pmDim.fill(Qt::transparent);
        {
            // 这里刻意**不**先给 pmDim 设 DPR：让 QPainter 在设备像素坐标下工作，
            // 源/目标矩形都用设备像素 —— 与 DPR 取值完全无关，缩放屏也一致。
            // （若先设 DPR 再画，pm 的逻辑尺寸参与坐标变换，非 1.0 缩放下会只画到左上角 1/4。）
            QPainter pp(&pmDim);
            pp.setOpacity(0.25);
            pp.drawPixmap(QRect(0, 0, px, px), pm, QRect(0, 0, pm.width(), pm.height()));
            pp.end();
        }
        pmDim.setDevicePixelRatio(dpr);
        bell->setPixmap(pm);
        bell->setToolTip(QStringLiteral("提醒：%1").arg(it.reminder.toString(QStringLiteral("MM-dd hh:mm"))));
        auto* blink = new QTimer(bell);      // 以 bell 为父 → 行重建/销毁时随之回收，不留野定时器
        blink->setInterval(450);
        QObject::connect(blink, &QTimer::timeout, bell,
                         [bell, pm, pmDim, dim = false]() mutable {
                             if (!bell->isVisible()) return;   // 面板收起/隐藏时不必重绘
                             dim = !dim;
                             bell->setPixmap(dim ? pmDim : pm);
                         });
        blink->start();
        lay->addWidget(bell, 0, Qt::AlignVCenter);
    }
    lay->addWidget(circle);

    // 文本（双击编辑；有提醒时 tooltip 展示提醒时间；已完成=灰字划线）
    auto* text = new QLabel(it.text, row);
    const int fp = it.fontPoint > 0 ? it.fontPoint : 12;
    text->setStyleSheet(it.done
        ? QStringLiteral("color: rgba(255,255,255,0.45); background: transparent; border: none;"
                         " text-decoration: line-through; font-size: %1pt;").arg(fp)
        : QStringLiteral("color: #FFFFFF; background: transparent; border: none; font-size: %1pt;").arg(fp));
    text->setToolTip(it.reminder.isValid()
        ? QStringLiteral("提醒：%1").arg(it.reminder.toString(QStringLiteral("MM-dd hh:mm")))
        : QString());
    lay->addWidget(text, 1);

    return row;
}

// 添加todovia对话框
// 作者：谭征
void SidePanelWidget::addTodoViaDialog() {
    bool ok = false;
    const QString text = GlassInputDialog::getText(this, QStringLiteral("添加待办事项"),
                                                   QStringLiteral("内容"), QLineEdit::Normal,
                                                   QString(), &ok);
    if (ok && !text.trimmed().isEmpty()) TodoStore::instance().addItem(text);
}

// 打开todo列表窗口
// 作者：谭征
void SidePanelWidget::openTodoListWindow() {
    if (!m_todoListWindow) {
        m_todoListWindow = new TodoListWindow(this);
    }
    m_todoListWindow->showWindow();
}

// 应用级“点别处即提交”守卫（装在 qApp 上）：见 cpp 中的实现说明。
// 作者：谭征
bool SidePanelWidget::eventFilter(QObject* watched, QEvent* event) {
    // 点击底部待办栏（空白/「+」/文字）→ 添加待办事项；「清单」按钮自己吞掉按下事件不受影响
    if (watched == m_todoBar && event->type() == QEvent::MouseButtonPress) {
        addTodoViaDialog();
        return true;
    }
    // 「收起后，鼠标移动到标题自动展开」（Appearance/autoExpandOnHover，2026-09-22）
    // 口径：**收起状态下**鼠标移到标题栏的展开按钮上即自动展开（等于替用户点一次）；
    // 取消勾选则必须自己点击。
    // 只认折叠按钮、不做"整条标题栏 hover"：收起后面板就只剩这条标题栏，整条都算触发展开区
    // 的话，鼠标划过屏幕碰到面板就会弹开（收纳盒窗口同样口径）。
    // 关闭时（默认）第一句即短路，零额外开销。
    if (m_collapsed && watched == m_moreBtn && event->type() == QEvent::Enter) {
        if (ThemeManager::instance()->autoExpandOnHoverEnabled()) {
            toggleCollapse();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
