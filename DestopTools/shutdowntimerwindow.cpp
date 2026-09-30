/*
 * @file shutdowntimerwindow.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "shutdowntimerwindow.h"
#include "shutdowntask.h"
#include "theme.h"
#include "thememanager.h"
#include "desktopmirrorwindow.h"   // raiseAboveBandWindows / clampBandZOrder / ensureLayerAboveBandWindows
#include "editorwatch.h"           // clearOwner：独立顶层窗口不能被 owner 的压底拖着沉下去
#include "glassmessagebox.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCalendarWidget>
#include <QComboBox>
#include <QCursor>
#include <QDateEdit>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPair>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

namespace {

// 任务开关：自绘「胶囊」开关
// 只重绘 + 复用 QAbstractButton 自带的 toggled 信号，因此**不需要 Q_OBJECT**。
// 这是有意的：带 Q_OBJECT 的类不能定义在与其同类名的 .cpp 里（moc 输出会与头文件的 moc
// 目标重名而冲突，导致元对象缺失、链接失败，见 FileSearchWorker 的踩坑记录）。
class SwitchButton : public QAbstractButton {
public:
    explicit SwitchButton(QWidget* parent = nullptr) : QAbstractButton(parent) {
        setCheckable(true);
        setChecked(true);
        setCursor(Qt::PointingHandCursor);
        setFixedSize(44, 22);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const qreal radius = r.height() / 2.0;
        const QColor accent = ThemeManager::instance()->accentColor();
        if (isChecked()) {
            QColor fill = accent;
            fill.setAlpha(190);
            p.setPen(QPen(accent, 1));
            p.setBrush(fill);
        } else {
            p.setPen(QPen(QColor(255, 255, 255, 60), 1));
            p.setBrush(QColor(255, 255, 255, 26));
        }
        p.drawRoundedRect(r, radius, radius);
        const qreal d = r.height() - 6;
        const qreal x = isChecked() ? r.right() - 3 - d : r.left() + 3;
        p.setPen(Qt::NoPen);
        p.setBrush(isChecked() ? QColor(255, 255, 255) : QColor(255, 255, 255, 170));
        p.drawEllipse(QRectF(x, r.top() + 3, d, d));
    }
};

// 助手形象「小安」：QPainter 现画的矢量机器人（不新增图片资源，配色跟随主题色）
QPixmap robotPixmap(int size) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal s = size;
    const QColor accent = ThemeManager::instance()->accentColor();
    QColor accentSoft = accent;
    accentSoft.setAlpha(80);

    // 地面阴影
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 55));
    p.drawEllipse(QRectF(s * 0.22, s * 0.84, s * 0.56, s * 0.09));

    // 天线
    p.setPen(QPen(accent, qMax(1.0, s * 0.035)));
    p.drawLine(QPointF(s * 0.5, s * 0.26), QPointF(s * 0.5, s * 0.14));
    p.setPen(Qt::NoPen);
    p.setBrush(accent);
    p.drawEllipse(QPointF(s * 0.5, s * 0.12), s * 0.055, s * 0.055);

    // 头（圆角方）
    const QRectF head(s * 0.16, s * 0.26, s * 0.68, s * 0.52);
    p.setBrush(QColor(255, 255, 255, 235));
    p.setPen(QPen(accentSoft, qMax(1.0, s * 0.025)));
    p.drawRoundedRect(head, s * 0.17, s * 0.17);

    // 显示屏
    const QRectF screen = head.adjusted(s * 0.10, s * 0.12, -s * 0.10, -s * 0.14);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(16, 24, 44, 235));
    p.drawRoundedRect(screen, s * 0.09, s * 0.09);

    // 眼睛 + 微笑
    p.setBrush(accent);
    const qreal eye = s * 0.052;
    p.drawEllipse(QPointF(screen.center().x() - s * 0.10, screen.center().y() - s * 0.035), eye, eye * 1.15);
    p.drawEllipse(QPointF(screen.center().x() + s * 0.10, screen.center().y() - s * 0.035), eye, eye * 1.15);
    p.setPen(QPen(accent, qMax(1.0, s * 0.028)));
    p.setBrush(Qt::NoBrush);
    const QRectF mouth(screen.center().x() - s * 0.085, screen.center().y() + s * 0.01, s * 0.17, s * 0.10);
    p.drawArc(mouth, 200 * 16, 140 * 16);

    // 两侧耳朵
    p.setPen(Qt::NoPen);
    p.setBrush(accentSoft);
    p.drawRoundedRect(QRectF(s * 0.10, s * 0.45, s * 0.07, s * 0.16), s * 0.03, s * 0.03);
    p.drawRoundedRect(QRectF(s * 0.83, s * 0.45, s * 0.07, s * 0.16), s * 0.03, s * 0.03);
    p.end();
    return pm;
}

QString comboStyle() {
    return Theme::applyTokens(QStringLiteral(
        "QComboBox { color: #FFFFFF; background: rgba(255,255,255,0.06); border: 1px solid rgba(34,211,238,0.22);"
        "            border-radius: 6px; padding: 3px 8px; font-size: 12px; }"
        "QComboBox:hover { border-color: rgba(34,211,238,0.42); }"
        "QComboBox:disabled { color: rgba(255,255,255,0.35); border-color: rgba(255,255,255,0.12); }"
        "QComboBox::drop-down { border: none; width: 18px; }"
        "QComboBox QAbstractItemView { background: rgba(17,26,46,1.0); color: #FFFFFF; outline: none;"
        "            border: 1px solid rgba(34,211,238,0.25); selection-background-color: rgba(34,211,238,0.22); }"));
}

QString dateStyle() {
    return Theme::applyTokens(QStringLiteral(
        "QDateEdit { color: #FFFFFF; background: rgba(255,255,255,0.06); border: 1px solid rgba(34,211,238,0.22);"
        "            border-radius: 6px; padding: 3px 8px; font-size: 12px; }"
        "QDateEdit:hover { border-color: rgba(34,211,238,0.42); }"
        "QDateEdit:disabled { color: rgba(255,255,255,0.35); border-color: rgba(255,255,255,0.12); }"
        "QDateEdit::drop-down { border: none; width: 18px; }"));
}

QString radioStyle() {
    return Theme::applyTokens(QStringLiteral(
        "QRadioButton { color: #FFFFFF; font-size: 12px; spacing: 6px; background: transparent; border: none; }"
        "QRadioButton:disabled { color: rgba(255,255,255,0.35); }"
        "QRadioButton::indicator { width: 14px; height: 14px; border: 1px solid rgba(34,211,238,0.45);"
        "            border-radius: 7px; background: rgba(17,26,46,1.0); }"
        "QRadioButton::indicator:hover { border-color: #22D3EE; }"
        "QRadioButton::indicator:checked { background: rgba(34,211,238,0.38); border: 2px solid #22D3EE; }"
        "QRadioButton::indicator:disabled { border-color: rgba(255,255,255,0.20); }"));
}

// 日期下拉里弹出的日历（QCalendarWidget 是独立弹框，样式不随父窗自动继承）。
// 必须走 Theme::applyTokens：否则 rgba(17,26,46,1.0) 不会被替换成 windowBgString，
// 日历弹框的底既不跟随主题色、也不跟随背景透明度（表现为"改完透明度，日历还是旧底"）。
QString calendarStyle() {
    return Theme::applyTokens(QStringLiteral(
        "QCalendarWidget QWidget { background: rgba(17,26,46,1.0); color: #FFFFFF; }"
        "QCalendarWidget QAbstractItemView { background: rgba(17,26,46,1.0); color: #FFFFFF;"
        "            selection-background-color: rgba(34,211,238,0.30); selection-color: #FFFFFF; }"
        "QCalendarWidget QToolButton { color: #FFFFFF; background: transparent; }"));
}

// make标签
// 作者：谭征
QLabel* makeLabel(const QString& text, QWidget* parent, const QString& extra = QString()) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 12px; background: transparent; border: none;")
                     + extra);
    return l;
}

}   // namespace

ShutdownTimerWindow::ShutdownTimerWindow(QWidget* parent)
    // 必须是 Qt::Window（独立顶层窗口），不能是 Qt::Dialog —— 与「快速搜索」窗 / UpdateDialog /
    // GlassMessageBox 同源：Qt::Dialog + parent 会让 Qt 把 parent（band 窗口）设成本窗口的 owner，
    // 弹框会随其周期性压底一起沉到桌面图标层之下（表现为「被压在别的窗口下面、点不到」）。
    : QDialog(parent, Qt::FramelessWindowHint | Qt::Window | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(478, 330);

    m_mgr = new ShutdownTaskManager(this);
    setupUi();

    // 层级看护：可见期间每 1s 只在「被可见的 band 窗口压住」时才救回（幂等，不打架）
    // B5：500ms → 1000ms（被压住是低频事件）。
    m_layerKeeper = new QTimer(this);
    m_layerKeeper->setInterval(1000);
    connect(m_layerKeeper, &QTimer::timeout, this, [this]() {
#ifdef Q_OS_WIN
        if (!isVisible()) return;
        const HWND h = reinterpret_cast<HWND>(winId());
        if (h && IsWindow(h)) DesktopMirrorWindow::ensureLayerAboveBandWindows(h);
#endif
    });

    // 任务集合变化 → 重建列表；每秒心跳 → 只刷倒计时文本
    connect(m_mgr, &ShutdownTaskManager::changed, this, &ShutdownTimerWindow::rebuildTasks);
    connect(m_mgr, &ShutdownTaskManager::ticked, this, &ShutdownTimerWindow::updateCountdown);
    rebuildTasks();

    // 主题色 / 背景透明度联动。
    // 本窗是「构造一次、反复 show」的长期窗，而 setupUi() 里绝大多数样式只在构造时写过一次 ——
    // 那些串里的背景 alpha 取的是"构造当时"的透明度。若这里只刷卡片/标题栏，用户改完透明度
    // 再打开本窗，下拉框底、单选指示器底、日历弹框底都会停在旧值（与其它界面不一致）。
    // 因此把 setupUi() 里**所有含背景色**的样式在这里一并重刷。
    // 无需 setWindowOpacity：透明度已并入背景色 alpha（theme.h 的 bgAlphaF），
    // 整窗不透明度会把圆角线框与文字一起淡化，与「圆角线框透明度不调整」冲突。
    ThemeManager::instance()->registerThemeTarget(this, [this]() {
        if (m_card) m_card->setStyleSheet(Theme::panelStyle());
        if (m_titleBar) m_titleBar->setStyleSheet(Theme::titleBarStyle());
        for (QRadioButton* rb : {m_modeRepeat, m_modeOnce}) if (rb) rb->setStyleSheet(radioStyle());
        if (m_typeGroup) {
            for (QAbstractButton* b : m_typeGroup->buttons()) b->setStyleSheet(radioStyle());
        }
        for (QComboBox* cb : {m_repeatCombo, m_repeatHour, m_repeatMinute, m_onceHour, m_onceMinute}) {
            if (cb) cb->setStyleSheet(comboStyle());
        }
        if (m_onceDate) {
            m_onceDate->setStyleSheet(dateStyle());
            if (QCalendarWidget* cal = m_onceDate->calendarWidget()) cal->setStyleSheet(calendarStyle());
        }
    });
}

ShutdownTimerWindow::~ShutdownTimerWindow() = default;

// 打开窗口（每次唤起都停在任务列表页，并刷新任务与倒计时）。
// 作者：谭征
void ShutdownTimerWindow::showWindow() {
    showListPage();
    rebuildTasks();
    if (!m_positioned) {
        m_positioned = true;
        if (QScreen* scr = QApplication::screenAt(QCursor::pos())) {
            const QRect a = scr->availableGeometry();
            move(a.center().x() - width() / 2, a.center().y() - height() / 2);
        }
    }
    show();
    raise();
    scheduleLayerAsserts();
    if (m_layerKeeper && !m_layerKeeper->isActive()) m_layerKeeper->start();
}

// UI

// 作者：谭征
void ShutdownTimerWindow::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);   // 为阴影留空间
    root->setSpacing(0);

    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("card"));
    m_card->setStyleSheet(Theme::panelStyle());
    auto* shadow = new QGraphicsDropShadowEffect(m_card);
    shadow->setBlurRadius(24);
    shadow->setColor(Theme::shadow());
    shadow->setOffset(0, 4);
    m_card->setGraphicsEffect(shadow);

    auto* cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    // 标题栏
    m_titleBar = new QFrame(m_card);
    m_titleBar->setObjectName(QStringLiteral("titleBar"));
    m_titleBar->setFixedHeight(Theme::titleBarHeight());
    m_titleBar->setStyleSheet(Theme::titleBarStyle());
    auto* titleLayout = new QHBoxLayout(m_titleBar);
    titleLayout->setContentsMargins(12, 0, 8, 0);
    titleLayout->setSpacing(6);
    auto* caption = makeLabel(QStringLiteral("定时关机"), m_titleBar, QStringLiteral("font-size: 13px;"));
    auto* closeBtn = new QToolButton(m_titleBar);
    closeBtn->setIcon(Theme::icon("close"));
    closeBtn->setIconSize(QSize(16, 16));
    closeBtn->setText(QString());
    closeBtn->setFixedSize(Theme::headerButtonSize());
    closeBtn->setStyleSheet(Theme::toolButtonStyle());
    connect(closeBtn, &QToolButton::clicked, this, &QDialog::reject);
    titleLayout->addWidget(caption);
    titleLayout->addStretch();
    titleLayout->addWidget(closeBtn);
    cardLayout->addWidget(m_titleBar);

    // 页面堆叠
    m_pages = new QStackedWidget(m_card);
    m_pages->setStyleSheet(QStringLiteral("QStackedWidget { background: transparent; border: none; }"));
    m_pages->addWidget(buildListPage());
    m_pages->addWidget(buildEditorPage());
    cardLayout->addWidget(m_pages, 1);

    root->addWidget(m_card);
}

// 构建列表页
// 作者：谭征
QWidget* ShutdownTimerWindow::buildListPage() {
    m_listPage = new QWidget(m_card);
    auto* v = new QVBoxLayout(m_listPage);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    // ① 提醒横幅：有启用任务时才显示，文案为「最近一次关机将在 xx 小时 xx 分钟 xx 秒后执行 [取消]」
    m_remindCard = new QFrame(m_listPage);
    m_remindCard->setObjectName(QStringLiteral("remindCard"));
    m_remindCard->setStyleSheet(QStringLiteral(
        "QFrame#remindCard { background: rgba(255,255,255,0.05); border: 1px solid rgba(255,255,255,0.10);"
        "                     border-radius: 10px; }"));
    auto* rl = new QHBoxLayout(m_remindCard);
    rl->setContentsMargins(10, 8, 12, 8);
    rl->setSpacing(10);

    auto* avatar = new QLabel(m_remindCard);
    avatar->setFixedSize(48, 48);
    avatar->setPixmap(robotPixmap(48));
    avatar->setStyleSheet(QStringLiteral("background: transparent; border: none;"));

    auto* rvl = new QVBoxLayout();
    rvl->setContentsMargins(0, 0, 0, 0);
    rvl->setSpacing(3);
    auto* who = makeLabel(QStringLiteral("小安提醒您"), m_remindCard,
                          QStringLiteral("font-weight: bold; font-size: 12px;"));
    m_remindText = makeLabel(QString(), m_remindCard, QStringLiteral("font-size: 12px;"));
    m_remindText->setTextFormat(Qt::RichText);
    rvl->addWidget(who);
    rvl->addWidget(m_remindText);

    m_remindCancel = new QToolButton(m_remindCard);
    m_remindCancel->setText(QStringLiteral("取消"));
    m_remindCancel->setCursor(Qt::PointingHandCursor);
    m_remindCancel->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QToolButton { color: #22D3EE; background: transparent; border: none; font-size: 12px; padding: 2px 4px; }"
        "QToolButton:hover { color: #67E8F9; }")));
    connect(m_remindCancel, &QToolButton::clicked, this, [this]() { m_mgr->cancelNext(); });

    rl->addWidget(avatar, 0, Qt::AlignVCenter);
    rl->addLayout(rvl, 1);
    rl->addWidget(m_remindCancel, 0, Qt::AlignVCenter);
    v->addWidget(m_remindCard);

    // ② 空态：暂无任务
    m_emptyState = new QWidget(m_listPage);
    auto* eh = new QHBoxLayout(m_emptyState);
    eh->setContentsMargins(22, 20, 22, 20);
    eh->setSpacing(14);
    auto* bigAvatar = new QLabel(m_emptyState);
    bigAvatar->setFixedSize(74, 74);
    bigAvatar->setPixmap(robotPixmap(74));
    bigAvatar->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    auto* evl = new QVBoxLayout();
    evl->setContentsMargins(0, 0, 0, 0);
    evl->setSpacing(6);
    auto* emptyTitle = makeLabel(QStringLiteral("暂无任务"), m_emptyState,
                                 QStringLiteral("font-size: 15px; font-weight: bold;"));
    auto* emptyHint = makeLabel(QStringLiteral("点击添加任务设置定时关机，多种模式供你选择！"), m_emptyState,
                                QStringLiteral("color: rgba(255,255,255,0.60); font-size: 12px;"));
    evl->addStretch();
    evl->addWidget(emptyTitle);
    evl->addWidget(emptyHint);
    evl->addStretch();
    eh->addWidget(bigAvatar, 0, Qt::AlignVCenter);
    eh->addLayout(evl, 1);
    eh->addStretch();
    v->addWidget(m_emptyState);

    // ③ 区头：我的任务 + 添加任务
    auto* header = new QFrame(m_listPage);
    header->setObjectName(QStringLiteral("sectionHeader"));
    header->setFixedHeight(44);
    header->setStyleSheet(QStringLiteral(
        "QFrame#sectionHeader { background: rgba(255,255,255,0.03);"
        "                       border-top: 1px solid rgba(255,255,255,0.10);"
        "                       border-bottom: 1px solid rgba(255,255,255,0.10); }"));
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(14, 0, 10, 0);
    hl->setSpacing(6);
    hl->addWidget(makeLabel(QStringLiteral("我的任务"), header, QStringLiteral("font-size: 13px;")));
    hl->addStretch();

    m_addTaskBtn = new QToolButton(header);
    m_addTaskBtn->setText(QStringLiteral("添加任务"));
    m_addTaskBtn->setIcon(Theme::icon("add"));
    m_addTaskBtn->setIconSize(QSize(14, 14));
    m_addTaskBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_addTaskBtn->setCursor(Qt::PointingHandCursor);
    m_addTaskBtn->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QToolButton { color: #22D3EE; background: transparent; border: none; font-size: 13px; padding: 2px 4px; }"
        "QToolButton:hover { color: #67E8F9; }")));
    connect(m_addTaskBtn, &QToolButton::clicked, this, [this]() { showEditorPage(); });
    hl->addWidget(m_addTaskBtn);
    v->addWidget(header);

    // ④ 任务列表
    m_taskScroll = new QScrollArea(m_listPage);
    m_taskScroll->setWidgetResizable(true);
    m_taskScroll->setFrameShape(QFrame::NoFrame);
    m_taskScroll->setStyleSheet(QStringLiteral(
        "QScrollArea { background: transparent; border: none; outline: none; }"
        "QScrollArea::viewport { background: transparent; border: none; }")
        + Theme::scrollBarStyle());
    m_taskHost = new QWidget();
    m_taskHost->setStyleSheet(QStringLiteral("background: transparent;"));
    m_taskColumn = new QVBoxLayout(m_taskHost);
    m_taskColumn->setContentsMargins(10, 6, 10, 8);
    m_taskColumn->setSpacing(0);
    m_taskScroll->setWidget(m_taskHost);
    v->addWidget(m_taskScroll, 1);

    return m_listPage;
}

// 构建editor页
// 作者：谭征
QWidget* ShutdownTimerWindow::buildEditorPage() {
    m_editorPage = new QWidget(m_card);
    auto* v = new QVBoxLayout(m_editorPage);
    v->setContentsMargins(16, 8, 16, 12);
    v->setSpacing(10);

    // 返回
    auto* backRow = new QHBoxLayout();
    auto* back = new QToolButton(m_editorPage);
    back->setText(QStringLiteral("← 返回"));
    back->setCursor(Qt::PointingHandCursor);
    back->setFixedHeight(26);
    back->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QToolButton { color: #FFFFFF; background: transparent; border: 1px solid rgba(34,211,238,0.30);"
        "              border-radius: 6px; padding: 2px 10px; font-size: 12px; }"
        "QToolButton:hover { background: rgba(34,211,238,0.12); border-color: rgba(34,211,238,0.55); }")));
    connect(back, &QToolButton::clicked, this, &ShutdownTimerWindow::onEditorCancel);
    backRow->addWidget(back);
    backRow->addStretch();
    v->addLayout(backRow);

    // 设置任务执行时间
    v->addWidget(makeLabel(QStringLiteral("设置任务执行时间"), m_editorPage,
                           QStringLiteral("font-size: 13px; font-weight: bold;")));

    // 周期模式：每天 / 工作日 / 周末 + 时·分
    auto* row1 = new QHBoxLayout();
    row1->setSpacing(6);
    m_modeRepeat = new QRadioButton(QStringLiteral("每天"), m_editorPage);
    m_modeRepeat->setChecked(true);
    m_modeRepeat->setStyleSheet(radioStyle());
    m_repeatCombo = new QComboBox(m_editorPage);
    m_repeatCombo->setStyleSheet(comboStyle());
    m_repeatCombo->addItem(QStringLiteral("每天"), ShutdownTask::Daily);
    m_repeatCombo->addItem(QStringLiteral("工作日"), ShutdownTask::Workday);
    m_repeatCombo->addItem(QStringLiteral("周末"), ShutdownTask::Weekend);
    m_repeatCombo->setFixedWidth(84);
    m_actionWord = makeLabel(QStringLiteral("关机将在"), m_editorPage);
    m_repeatHour = new QComboBox(m_editorPage);
    m_repeatHour->setStyleSheet(comboStyle());
    m_repeatMinute = new QComboBox(m_editorPage);
    m_repeatMinute->setStyleSheet(comboStyle());
    for (int i = 0; i < 24; ++i) m_repeatHour->addItem(QString::number(i), i);
    for (int i = 0; i < 60; ++i) m_repeatMinute->addItem(QString::number(i), i);
    m_repeatHour->setFixedWidth(66);
    m_repeatMinute->setFixedWidth(66);
    row1->addWidget(m_modeRepeat);
    row1->addWidget(m_repeatCombo);
    row1->addSpacing(6);
    row1->addWidget(m_actionWord);
    row1->addWidget(m_repeatHour);
    row1->addWidget(makeLabel(QStringLiteral("时"), m_editorPage));
    row1->addWidget(m_repeatMinute);
    row1->addWidget(makeLabel(QStringLiteral("分时执行"), m_editorPage));
    row1->addStretch();
    v->addLayout(row1);

    // 指定时间点模式：日期 + 时·分
    auto* row2 = new QHBoxLayout();
    row2->setSpacing(6);
    m_modeOnce = new QRadioButton(QStringLiteral("选择指定时间点关机:"), m_editorPage);
    m_modeOnce->setStyleSheet(radioStyle());
    m_onceDate = new QDateEdit(QDate::currentDate(), m_editorPage);
    m_onceDate->setDisplayFormat(QStringLiteral("yyyy年MM月dd日"));
    m_onceDate->setCalendarPopup(true);
    m_onceDate->setStyleSheet(dateStyle());
    m_onceDate->setFixedWidth(136);
    if (QCalendarWidget* cal = m_onceDate->calendarWidget()) {
        cal->setStyleSheet(calendarStyle());
    }
    m_onceHour = new QComboBox(m_editorPage);
    m_onceHour->setStyleSheet(comboStyle());
    m_onceMinute = new QComboBox(m_editorPage);
    m_onceMinute->setStyleSheet(comboStyle());
    for (int i = 0; i < 24; ++i) m_onceHour->addItem(QString::number(i), i);
    for (int i = 0; i < 60; ++i) m_onceMinute->addItem(QString::number(i), i);
    m_onceHour->setFixedWidth(66);
    m_onceMinute->setFixedWidth(66);
    row2->addWidget(m_modeOnce);
    row2->addWidget(m_onceDate);
    row2->addWidget(m_onceHour);
    row2->addWidget(makeLabel(QStringLiteral("时"), m_editorPage));
    row2->addWidget(m_onceMinute);
    row2->addWidget(makeLabel(QStringLiteral("分"), m_editorPage));
    row2->addStretch();
    v->addLayout(row2);

    // 设置任务类型
    v->addWidget(makeLabel(QStringLiteral("设置任务类型"), m_editorPage,
                           QStringLiteral("font-size: 13px; font-weight: bold;")));
    auto* typeRow = new QHBoxLayout();
    typeRow->setSpacing(10);
    m_typeGroup = new QButtonGroup(m_editorPage);
    const QVector<QPair<int, QString>> types = {
        {ShutdownTask::Shutdown,      QStringLiteral("关机")},
        {ShutdownTask::ForceShutdown, QStringLiteral("强制关机")},
        {ShutdownTask::Reboot,        QStringLiteral("重启")},
        {ShutdownTask::Sleep,         QStringLiteral("睡眠")},
        {ShutdownTask::Logoff,        QStringLiteral("注销")},
        {ShutdownTask::Lock,          QStringLiteral("锁定")}
    };
    for (const auto& kv : types) {
        auto* rb = new QRadioButton(kv.second, m_editorPage);
        rb->setStyleSheet(radioStyle());
        m_typeGroup->addButton(rb, kv.first);
        typeRow->addWidget(rb);
        if (kv.first == ShutdownTask::Shutdown) rb->setChecked(true);
    }
    typeRow->addStretch();
    v->addLayout(typeRow);

    v->addStretch();

    // 底部按钮
    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(10);
    btnRow->addStretch();
    auto* cancelBtn = new QPushButton(QStringLiteral("取消"), m_editorPage);
    cancelBtn->setCursor(Qt::PointingHandCursor);
    cancelBtn->setFixedSize(80, 32);
    cancelBtn->setStyleSheet(Theme::pushButtonStyle());
    connect(cancelBtn, &QPushButton::clicked, this, &ShutdownTimerWindow::onEditorCancel);
    auto* okBtn = new QPushButton(QStringLiteral("确定"), m_editorPage);
    okBtn->setCursor(Qt::PointingHandCursor);
    okBtn->setDefault(true);
    okBtn->setFixedSize(80, 32);
    okBtn->setStyleSheet(Theme::primaryButtonStyle());
    connect(okBtn, &QPushButton::clicked, this, &ShutdownTimerWindow::onEditorConfirm);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(okBtn);
    v->addLayout(btnRow);

    // 模式/类型联动
    m_modeGroup = new QButtonGroup(m_editorPage);
    m_modeGroup->addButton(m_modeRepeat, 0);
    m_modeGroup->addButton(m_modeOnce, 1);
    connect(m_modeGroup, QOverload<int>::of(&QButtonGroup::buttonClicked),
            this, &ShutdownTimerWindow::onModeChanged);
    connect(m_typeGroup, QOverload<int>::of(&QButtonGroup::buttonClicked),
            this, [this](int) { onTypeChanged(); });
    onModeChanged(0);
    onTypeChanged();

    return m_editorPage;
}

// 行为

// 作者：谭征
QString ShutdownTimerWindow::repeatText(const ShutdownTask& t) {
    return ShutdownTaskManager::modeName(t.mode) + ShutdownTaskManager::typeName(t.type);
}

// 当前typeid
// 作者：谭征
int ShutdownTimerWindow::currentTypeId() const {
    const int id = m_typeGroup ? m_typeGroup->checkedId() : -1;
    return id < 0 ? int(ShutdownTask::Shutdown) : id;
}

// 同步actionword
// 作者：谭征
void ShutdownTimerWindow::syncActionWord() {
    if (m_actionWord)
        m_actionWord->setText(ShutdownTaskManager::typeName(currentTypeId()) + QStringLiteral("将在"));
}

// 响应type变化信号
// 作者：谭征
void ShutdownTimerWindow::onTypeChanged() {
    syncActionWord();
}

// 响应模式变化信号
// 作者：谭征
void ShutdownTimerWindow::onModeChanged(int id) {
    const bool repeat = (id == 0);
    m_repeatCombo->setEnabled(repeat);
    m_repeatHour->setEnabled(repeat);
    m_repeatMinute->setEnabled(repeat);
    m_onceDate->setEnabled(!repeat);
    m_onceHour->setEnabled(!repeat);
    m_onceMinute->setEnabled(!repeat);
}

// 构建task行
// 作者：谭征
QWidget* ShutdownTimerWindow::buildTaskRow(const ShutdownTask& t) {
    auto* row = new QWidget(m_taskHost);
    row->setFixedHeight(46);
    row->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* hl = new QHBoxLayout(row);
    hl->setContentsMargins(4, 0, 4, 0);
    hl->setSpacing(0);

    auto* name = makeLabel(ShutdownTaskManager::typeName(t.type), row, QStringLiteral("font-size: 13px;"));
    name->setFixedWidth(76);
    const QDateTime due = m_mgr->nextDueOf(t);
    auto* time = makeLabel(due.isValid() ? due.toString(QStringLiteral("yyyy年MM月dd日 HH:mm"))
                                         : QStringLiteral("已停用"),
                           row, QStringLiteral("color: rgba(255,255,255,0.72);"));
    time->setFixedWidth(168);
    auto* mode = makeLabel(repeatText(t), row, QStringLiteral("color: rgba(255,255,255,0.55);"));
    mode->setFixedWidth(96);

    auto* del = new QToolButton(row);
    del->setText(QStringLiteral("删除"));
    del->setCursor(Qt::PointingHandCursor);
    del->setStyleSheet(QStringLiteral(
        "QToolButton { color: rgba(251,146,60,0.95); background: transparent; border: none;"
        "              font-size: 12px; padding: 2px 6px; }"
        "QToolButton:hover { color: #FDBA74; text-decoration: underline; }"));
    const QString id = t.id;
    connect(del, &QToolButton::clicked, this, [this, id]() { m_mgr->removeTask(id); });

    auto* sw = new SwitchButton(row);
    sw->setChecked(t.enabled);
    connect(sw, &QAbstractButton::toggled, this, [this, id](bool on) { m_mgr->setEnabled(id, on); });

    hl->addWidget(name);
    hl->addWidget(time);
    hl->addWidget(mode);
    hl->addWidget(del);
    hl->addStretch();
    hl->addWidget(sw);
    return row;
}

// 重建tasks
// 作者：谭征
void ShutdownTimerWindow::rebuildTasks() {
    if (!m_taskColumn) return;
    while (m_taskColumn->count() > 0) {
        QLayoutItem* item = m_taskColumn->takeAt(0);
        if (item && item->widget()) {
            // 该函数可能由任务行内的按钮信号（删除/开关）触发：绝不能立即 delete 调用栈上的
            // sender，故先 hide 再 deleteLater（重建期间不可见的旧行不会残留视觉）。
            item->widget()->hide();
            item->widget()->setParent(nullptr);
            item->widget()->deleteLater();
        }
        delete item;
    }

    const bool hasTasks = !m_mgr->tasks().isEmpty();
    if (m_emptyState) m_emptyState->setVisible(!hasTasks);

    for (const ShutdownTask& t : m_mgr->tasks()) {
        m_taskColumn->addWidget(buildTaskRow(t));
        auto* line = new QFrame(m_taskHost);
        line->setFixedHeight(1);
        line->setStyleSheet(QStringLiteral("background: rgba(255,255,255,0.07); border: none;"));
        m_taskColumn->addWidget(line);
    }
    m_taskColumn->addStretch();
    updateCountdown();
}

// 更新countdown
// 作者：谭征
void ShutdownTimerWindow::updateCountdown() {
    if (!m_remindCard || !m_remindText) return;
    const ShutdownTask* n = m_mgr->nearestTask();
    const QDateTime due = n ? m_mgr->nextDueOf(*n) : QDateTime();
    if (!n || !due.isValid()) {
        m_remindCard->setVisible(false);
        return;
    }
    m_remindCard->setVisible(true);
    qint64 secs = QDateTime::currentDateTime().secsTo(due);
    if (secs < 0) secs = 0;
    const QString accent = ThemeManager::instance()->accentColor().name();
    auto num = [&](qint64 v) {
        return QStringLiteral("<b style='color:%1'>%2</b>").arg(accent).arg(v, 2, 10, QLatin1Char('0'));
    };
    m_remindText->setText(QStringLiteral("最近一次%1将在 %2 小时 %3 分钟 %4 秒后执行")
                              .arg(ShutdownTaskManager::typeName(n->type),
                                   num(secs / 3600), num((secs % 3600) / 60), num(secs % 60)));
}

// 显示列表页
// 作者：谭征
void ShutdownTimerWindow::showListPage() {
    if (m_pages) m_pages->setCurrentIndex(0);
}

// 显示editor页
// 作者：谭征
void ShutdownTimerWindow::showEditorPage() {
    if (!m_pages) return;
    // 每次进入设置页都回到默认：每模式 + 今天 + 关机
    m_modeRepeat->setChecked(true);
    m_repeatCombo->setCurrentIndex(0);
    m_repeatHour->setCurrentIndex(0);
    m_repeatMinute->setCurrentIndex(0);
    const QDateTime now = QDateTime::currentDateTime();
    m_onceDate->setDate(now.date());
    m_onceHour->setCurrentIndex(now.time().hour());
    m_onceMinute->setCurrentIndex(now.time().minute());
    if (QAbstractButton* b = m_typeGroup->button(int(ShutdownTask::Shutdown))) b->setChecked(true);
    onModeChanged(0);
    onTypeChanged();
    m_pages->setCurrentIndex(1);
}

// 响应editor取消
// 作者：谭征
void ShutdownTimerWindow::onEditorCancel() {
    showListPage();
}

// 响应editorconfirm
// 作者：谭征
void ShutdownTimerWindow::onEditorConfirm() {
    ShutdownTask t;
    t.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    t.type = currentTypeId();
    t.enabled = true;
    if (m_modeOnce->isChecked()) {
        t.mode = ShutdownTask::Once;
        const QDateTime dt(m_onceDate->date(),
                           QTime(m_onceHour->currentIndex(), m_onceMinute->currentIndex(), 0));
        if (dt <= QDateTime::currentDateTime()) {
            GlassMessageBox::warning(this, QStringLiteral("定时关机"),
                                     QStringLiteral("指定的时间点已过，请重新选择日期与时间。"));
            return;
        }
        t.onceAt = dt;
        t.hour = dt.time().hour();
        t.minute = dt.time().minute();
    } else {
        t.mode = m_repeatCombo->currentData().toInt();   // 与 ShutdownTask::Mode 一一对应
        t.hour = m_repeatHour->currentIndex();
        t.minute = m_repeatMinute->currentIndex();
    }
    m_mgr->addTask(t);   // addTask → changed() → rebuildTasks()
    showListPage();
}

// 窗口基础设施

// 无边框窗（Qt::FramelessWindowHint）没有系统标题栏，Windows 不会替我们移动它：
// 只留「按住标题栏拖动」一条通道（与 快速搜索窗 / 重命名弹框 / 备份弹框 同款）。
// 作者：谭征
bool ShutdownTimerWindow::isDragAreaAt(const QPoint& pos) const {
    QWidget* hit = childAt(pos);
    if (!hit) return false;   // 卡片外 12px 透明阴影留白：不响应，避免误触
    for (QWidget* p = hit; p && p != this; p = p->parentWidget()) {
        if (p == m_titleBar) return true;   // 关闭按钮是 QToolButton、自己吞掉按下事件，走不到这里
    }
    return hit == m_card || hit == m_listPage || hit == m_editorPage;
}

// 标题栏本身不承载任何编辑交互，不会与输入/选择冲突。
// 作者：谭征
void ShutdownTimerWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && isDragAreaAt(event->pos())) {
        m_dragging = true;
        m_dragPos = event->globalPos() - frameGeometry().topLeft();
        event->accept();
        return;
    }
    QDialog::mousePressEvent(event);
}

// 鼠标移动事件
// 作者：谭征
void ShutdownTimerWindow::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPos() - m_dragPos);
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

// 鼠标松开事件
// 作者：谭征
void ShutdownTimerWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        event->accept();
        return;
    }
    QDialog::mouseReleaseEvent(event);
}

// 与 GlassInputDialog（重命名弹框）/ UpdateDialog 同源，幂等，可反复调用。
// 作者：谭征
void ShutdownTimerWindow::assertLayerAboveBandWindows() {
#ifdef Q_OS_WIN
    if (!isVisible()) return;
    const HWND h = reinterpret_cast<HWND>(winId());
    if (!h || !IsWindow(h)) return;
    EditorWatch::clearOwner(h);                              // ① 清 owner，避免随其压底而沉下去
    DesktopMirrorWindow::raiseAboveBandWindows(h);           // ② 插到全部 band 窗口之上
    if (GetForegroundWindow() != h) activateWindow();        // ③ 可激活才能收键盘/日历弹层
#endif
}

// schedulelayerasserts
// 作者：谭征
void ShutdownTimerWindow::scheduleLayerAsserts() {
#ifdef Q_OS_WIN
    // show() 之后 Qt 仍会沿 parent 链补一次内部 z 序调整，单次断言会被它覆盖，
    // 故错峰多轮再钉（与 UpdateDialog / 快速搜索窗同款结论，幂等）。
    for (int delayMs : {0, 60, 200, 400, 800}) {
        QTimer::singleShot(delayMs, this, [this]() { assertLayerAboveBandWindows(); });
    }
#endif
}

// 显示事件
// 作者：谭征
void ShutdownTimerWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    scheduleLayerAsserts();
    if (m_layerKeeper) m_layerKeeper->start();
    rebuildTasks();
}

// 隐藏事件
// 作者：谭征
void ShutdownTimerWindow::hideEvent(QHideEvent* event) {
    if (m_layerKeeper) m_layerKeeper->stop();
    m_dragging = false;
    QDialog::hideEvent(event);
}

// 助手面板顶到普通程序之上再被压回的「一闪而过」。show desktop 期放行。
// 作者：谭征
bool ShutdownTimerWindow::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    MSG* msg = reinterpret_cast<MSG*>(message);
    if (msg && msg->message == WM_WINDOWPOSCHANGING) {
        WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(msg->lParam);
        DesktopMirrorWindow::clampBandZOrder(msg->hwnd, wp);
        // 必须清掉 SWP_NOACTIVATE：本窗要用键盘/日历弹层，须能激活；z 序冻结（SWP_NOZORDER）保留
        wp->flags &= ~SWP_NOACTIVATE;
        return false;
    }
#endif
    return QDialog::nativeEvent(eventType, message, result);
}
