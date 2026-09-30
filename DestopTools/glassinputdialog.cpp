/*
 * @file glassinputdialog.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "glassinputdialog.h"
#include "theme.h"
#include "thememanager.h"
#include "desktopmirrorwindow.h"
#include <QApplication>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QMouseEvent>
#include <QGraphicsDropShadowEffect>

// 获取文本
// 作者：谭征
QString GlassInputDialog::getText(QWidget* parent,
                                  const QString& title,
                                  const QString& label,
                                  QLineEdit::EchoMode echo,
                                  const QString& text,
                                  bool* ok) {
    GlassInputDialog dlg(parent, title, label, echo, text);
    const int ret = dlg.exec();
    if (ok) *ok = (ret == QDialog::Accepted);
    if (ret == QDialog::Accepted) return dlg.m_lineEdit->text();
    return QString();
}

GlassInputDialog::GlassInputDialog(QWidget* parent,
                                   const QString& title,
                                   const QString& label,
                                   QLineEdit::EchoMode echo,
                                   const QString& text)
    : QDialog(parent, Qt::FramelessWindowHint | Qt::Window | Qt::NoDropShadowWindowHint)
    , m_titleText(title)
    , m_labelText(label)
    , m_echo(echo)
    , m_defaultText(text) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(360, 220);
    setupUi();

    // z 序：弹框 > 收纳盒/桌面助手（全部 band 窗口），但 < 其它任意程序
    // 与 SettingCenterDialog 同方案：Qt::Window 使其独立于 parent 链 z 序，
    // 延迟到事件循环末尾 raiseAboveBandWindows 插到 band 窗口之上。
    // 提层后显式激活窗口并把焦点交还输入框，确保可正常打字（nativeEvent 已清 SWP_NOACTIVATE）。
#ifdef Q_OS_WIN
    QTimer::singleShot(0, this, [this]() {
        DesktopMirrorWindow::raiseAboveBandWindows((HWND)winId());
        activateWindow();
        m_lineEdit->setFocus();
        m_lineEdit->activateWindow();
    });
#endif

    // 全局主题色 / 背景透明度：本窗是按需创建的模态对话框，全部样式在 setupUi() 里用
    // Theme::applyTokens(...) 就地生成 —— 每次弹出取的都是当时的主题色与背景透明度，天然最新。
    // 这里【不再】setWindowOpacity(整窗不透明度)：透明度已并入**背景色** alpha（theme.h 的 bgAlphaF），
    // 整窗不透明度会把圆角线框与文字一起淡化，与「圆角线框透明度不调整」的需求冲突。
}

// 初始化ui
// 作者：谭征
void GlassInputDialog::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(0);

    auto* card = new QFrame(this);
    card->setStyleSheet(Theme::panelStyle());

    auto* shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(24);
    shadow->setColor(Theme::shadow());
    shadow->setOffset(0, 4);
    card->setGraphicsEffect(shadow);

    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    // 自定义标题栏
    m_titleBar = new QFrame(card);
    m_titleBar->setObjectName(QStringLiteral("titleBar"));
    m_titleBar->setFixedHeight(Theme::titleBarHeight());
    m_titleBar->setStyleSheet(Theme::titleBarStyle());
    auto* titleLayout = new QHBoxLayout(m_titleBar);
    titleLayout->setContentsMargins(12, 0, 8, 0);
    titleLayout->setSpacing(6);

    auto* caption = new QLabel(m_titleText, m_titleBar);
    caption->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 13px; background: transparent; border: none;"));

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

    // 输入区
    auto* body = new QVBoxLayout();
    body->setContentsMargins(24, 22, 24, 8);
    body->setSpacing(12);

    auto* label = new QLabel(m_labelText, card);
    label->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 13px; background: transparent; border: none;"));

    m_lineEdit = new QLineEdit(card);
    m_lineEdit->setText(m_defaultText);
    m_lineEdit->setEchoMode(m_echo);
    Theme::applyLineEditStyle(m_lineEdit);
    m_lineEdit->setAutoFillBackground(true);
    m_lineEdit->setAttribute(Qt::WA_TranslucentBackground, false);
    m_lineEdit->setMinimumHeight(28);
    m_lineEdit->setFocus();
    m_lineEdit->selectAll();

    body->addWidget(label);
    body->addWidget(m_lineEdit);
    body->addStretch();

    cardLayout->addLayout(body, 1);

    // 按钮区
    auto* btnLayout = new QHBoxLayout();
    btnLayout->setContentsMargins(24, 12, 24, 20);
    btnLayout->addStretch();

    auto* cancelBtn = new QPushButton(QStringLiteral("取消"), card);
    cancelBtn->setCursor(Qt::PointingHandCursor);
    cancelBtn->setFixedSize(72, 32);
    cancelBtn->setStyleSheet(Theme::pushButtonStyle());
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

    auto* okBtn = new QPushButton(QStringLiteral("确定"), card);
    okBtn->setCursor(Qt::PointingHandCursor);
    okBtn->setFixedSize(72, 32);
    okBtn->setStyleSheet(Theme::primaryButtonStyle());
    okBtn->setDefault(true);
    connect(okBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_lineEdit, &QLineEdit::returnPressed, this, &QDialog::accept);

    btnLayout->addWidget(cancelBtn);
    btnLayout->addSpacing(12);
    btnLayout->addWidget(okBtn);

    cardLayout->addLayout(btnLayout);

    root->addWidget(card);
}

// 助手面板顶到普通程序之上再被压回的「一闪而过」。show desktop 期放行。
// 作者：谭征
bool GlassInputDialog::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    MSG* msg = reinterpret_cast<MSG*>(message);
    // 与 SettingCenterDialog 同方案：WM_WINDOWPOSCHANGING 是同步发送消息、不走消息队列，
    // 必须在本窗口 nativeEvent 拦截（全局 QAbstractNativeEventFilter 看不到）。
    // 冻结任何「顶层提层」（HWND_TOP/TOPMOST/NOTOPMOST/外部程序参照），使弹框保持当前
    // band 内位置——即落在全部 band 窗口（收纳盒/桌面助手）之上、所有正常程序之下，
    // 从而不被收纳盒/桌面助手界面遮挡。
    if (msg && msg->message == WM_WINDOWPOSCHANGING) {
        WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(msg->lParam);
        DesktopMirrorWindow::clampBandZOrder(msg->hwnd, wp);
        // 注意：输入弹框必须可激活才能接收键盘输入（QLineEdit 才能收到 WM_CHAR）。
        // clampBandZOrder 默认加 SWP_NOACTIVATE 以「不抢焦点」，但那会让弹框永远不成为
        // 前台窗口、输入框收不到按键。这里清掉该标志，允许弹框激活获取焦点；
        // z 序冻结（SWP_NOZORDER）仍保留——弹框稳定落在全部 band 窗口之上、所有正常程序之下。
        wp->flags &= ~SWP_NOACTIVATE;
        return false;                  // 走默认处理链，改写已生效
    }
#endif
    return QDialog::nativeEvent(eventType, message, result);
}

// 标题栏本身不承载任何编辑交互，不会与输入/选择冲突。
// 作者：谭征
void GlassInputDialog::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        QWidget* child = childAt(event->pos());
        bool onTitleBar = false;
        while (child && child != this) {
            if (child == m_titleBar) {
                onTitleBar = true;
                break;
            }
            child = child->parentWidget();
        }
        if (onTitleBar) {
            m_dragging = true;
            m_dragPos = event->globalPos() - frameGeometry().topLeft();
            event->accept();
            return;
        }
    }
    QDialog::mousePressEvent(event);
}

// 鼠标移动事件
// 作者：谭征
void GlassInputDialog::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPos() - m_dragPos);
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

// 鼠标松开事件
// 作者：谭征
void GlassInputDialog::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
    }
    QDialog::mouseReleaseEvent(event);
}
