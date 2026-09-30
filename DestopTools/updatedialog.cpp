/*
 * @file updatedialog.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "updatedialog.h"
#include "theme.h"
#include "thememanager.h"
#include "desktopmirrorwindow.h"   // raiseAboveBandWindows / clampBandZOrder：弹框 z 序（同重命名弹框）
#include "editorwatch.h"           // clearOwner：弹框是独立顶层窗口，不能被 owner 的压底拖着沉下去
#include <QApplication>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QMouseEvent>
#include <QPainter>
#include <QGraphicsDropShadowEffect>
#include <QDesktopServices>
#include <QUrl>

UpdateDialog::UpdateDialog(bool hasUpdate, const QString& newVersion, QWidget* parent)
    // 必须是 Qt::Window（独立顶层窗口），不能是 Qt::Dialog（2026-09-16 定论，与 GlassMessageBox 同源）。
    // Qt::Dialog + parent 会让 Qt 把 parent 的 HWND 指定成本窗口的 owner，而本项目所有
    // parent 候选（设置中心 / 侧边面板 / Dock 图标区）都是 band 窗口：Dock 每秒被
    // SetWindowPos(HWND_BOTTOM) 压到壁纸层，被 owner 化的升级弹框会**跟着一起沉下去**，
    // 落到桌面图标层之下 —— 用户看到的就是「弹框被压在别的窗口下面，点不到、看不见」。
    : QDialog(parent, Qt::FramelessWindowHint | Qt::Window | Qt::NoDropShadowWindowHint)
    , m_hasUpdate(hasUpdate)
    , m_newVersion(newVersion) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(420, 200);
    setupUi();

    // z 序：弹框 > 收纳盒 / 桌面助手 / Dock 图标区（全部 band 窗口），但 < 其它任意程序
    // 与 GlassInputDialog（重命名弹框）/ GlassMessageBox 同方案。错峰三轮（0/60/200ms）而不是只做一次：
    // · show() 之后 Qt 仍会因 parent 链做一次内部 z 序调整，单次插入可能被它覆盖；
    // · Dock 的 parkAllTargetsAtBottom（1s 周期）与 band 守卫会持续改写 z 序，
    // 弹框必须在这些改写的空隙里把位置重新钉住。
    // assertLayerAboveBandWindows() 幂等，重复调用无副作用。
#ifdef Q_OS_WIN
    for (int delayMs : {0, 60, 200}) {
        QTimer::singleShot(delayMs, this, [this]() { assertLayerAboveBandWindows(); });
    }
#endif

    // 全局主题色 / 背景透明度：本窗是按需创建的模态对话框，全部样式在 setupUi() 里用
    // Theme::applyTokens(...) 就地生成 —— 每次弹出取的都是当时的主题色与背景透明度，天然最新。
    // 这里【不再】setWindowOpacity(整窗不透明度)：透明度已并入**背景色** alpha（theme.h 的 bgAlphaF），
    // 整窗不透明度会把圆角线框与文字一起淡化，与「圆角线框透明度不调整」的需求冲突。
}

// 把弹框钉在 band 窗口之上（幂等）。三步各自独立：任一失效都单独补齐，不做「一次性做完就算」。
// 作者：谭征
void UpdateDialog::assertLayerAboveBandWindows() {
#ifdef Q_OS_WIN
    if (!isVisible()) return;
    const HWND h = reinterpret_cast<HWND>(winId());
    if (!h || !IsWindow(h)) return;
    // ① 先清 owner：owner 一旦落在 Dock/收纳盒上，它被压底时会把我们一并拖下去。
    EditorWatch::clearOwner(h);
    // ② 再插到全部 band 窗口之上（不使用 HWND_TOP/TOPMOST，故仍低于其它正常程序）。
    DesktopMirrorWindow::raiseAboveBandWindows(h);
    // ③ 弹框必须可激活才收得到 Enter / Esc（「知道了」按钮的 default 也依赖键盘焦点）。
    // 本进程通常不是前台，activateWindow() 可能被前台锁忽略，这里只作补充、不阻塞。
    if (GetForegroundWindow() != h) activateWindow();
#endif
}

// 初始化ui
// 作者：谭征
void UpdateDialog::setupUi() {
    // 对话框本身完全透明，视觉内容由内部卡片提供（与 GlassMessageBox 保持一致）
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12); // 为阴影留空间
    root->setSpacing(0);

    // 玻璃卡片
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

    auto* caption = new QLabel(QStringLiteral("桌面助手"), m_titleBar);
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

    // 内容区
    auto* body = new QHBoxLayout();
    body->setContentsMargins(24, 20, 24, 16);
    body->setSpacing(16);

    // 青蓝渐变信息图标
    m_iconLabel = new QLabel(card);
    m_iconLabel->setFixedSize(48, 48);
    paintInfoIcon();

    auto* textLayout = new QVBoxLayout();
    textLayout->setSpacing(8);
    textLayout->setContentsMargins(0, 0, 0, 0);

    m_titleLabel = new QLabel(QStringLiteral("桌面助手升级"), card);
    QFont tf = m_titleLabel->font();
    tf.setPointSize(14);
    tf.setBold(true);
    m_titleLabel->setFont(tf);
    m_titleLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent; border: none;"));

    if (m_hasUpdate && !m_newVersion.isEmpty()) {
        m_msgLabel = new QLabel(QStringLiteral("发现新版本 %1，是否立即更新？").arg(m_newVersion), card);
    } else if (m_hasUpdate) {
        m_msgLabel = new QLabel(QStringLiteral("发现新版本，是否立即更新？"), card);
    } else {
        m_msgLabel = new QLabel(QStringLiteral("亲，桌面助手已经是最新版了，快去使用吧~"), card);
    }
    m_msgLabel->setWordWrap(true);
    m_msgLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 13px; background: transparent; border: none;"));

    textLayout->addWidget(m_titleLabel);
    textLayout->addWidget(m_msgLabel);
    textLayout->addStretch();

    body->addWidget(m_iconLabel, 0, Qt::AlignTop);
    body->addLayout(textLayout, 1);

    cardLayout->addLayout(body, 1);

    // 底部按钮
    auto* btnLayout = new QHBoxLayout();
    btnLayout->setContentsMargins(0, 0, 16, 12);
    btnLayout->addStretch();

    m_actionBtn = new QPushButton(m_hasUpdate ? QStringLiteral("更新") : QStringLiteral("知道了"), card);
    m_actionBtn->setCursor(Qt::PointingHandCursor);
    m_actionBtn->setFixedSize(84, 34);
    m_actionBtn->setStyleSheet(Theme::primaryButtonStyle());
    m_actionBtn->setDefault(true);      // 与重命名弹框一致：回车即可确认
    connect(m_actionBtn, &QPushButton::clicked, this, &UpdateDialog::onUpdateClicked);
    btnLayout->addWidget(m_actionBtn);

    cardLayout->addLayout(btnLayout);

    root->addWidget(card);
}

// paintinfo图标
// 作者：谭征
void UpdateDialog::paintInfoIcon() {
    if (!m_iconLabel) return;
    QPixmap pixmap(48, 48);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // 主题色渐变圆形背景（跟随当前主题色，与全局主题联动一致）
    QRadialGradient grad(24, 24, 24);
    const QColor accent = ThemeManager::instance()->accentColor();
    grad.setColorAt(0, accent);
    grad.setColorAt(1, accent.darker(115));
    painter.setPen(Qt::NoPen);
    painter.setBrush(grad);
    painter.drawEllipse(0, 0, 48, 48);

    // 白色 "i"，与 GlassMessageBox 信息图标保持一致
    painter.setPen(QPen(Qt::white, 3));
    painter.setBrush(Qt::white);
    painter.drawLine(24, 16, 24, 18);
    painter.drawLine(24, 22, 24, 34);

    m_iconLabel->setPixmap(pixmap);
}

// 响应更新clicked
// 作者：谭征
void UpdateDialog::onUpdateClicked() {
    if (m_hasUpdate) {
        // 占位：实际可替换为下载更新包或打开官网
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.example.com/download")));
    }
    accept();
}

// 标题栏本身不承载任何编辑交互，不会与输入/选择冲突。
// 作者：谭征
void UpdateDialog::mousePressEvent(QMouseEvent* event) {
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
void UpdateDialog::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPos() - m_dragPos);
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

// 鼠标松开事件
// 作者：谭征
void UpdateDialog::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        event->accept();
    }
    QDialog::mouseReleaseEvent(event);
}

// z 序守卫（与 GlassInputDialog「重命名弹框」/ GlassMessageBox 同源）。
// WM_WINDOWPOSCHANGING 是**同步发送**消息、不走消息队列，全局 QAbstractNativeEventFilter 看不到，
// 必须在窗口自己的 nativeEvent 里拦截，否则「升级弹框被收纳盒/助手盖住」的问题修不掉。
// 作者：谭征
bool UpdateDialog::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    MSG* msg = reinterpret_cast<MSG*>(message);
    if (msg && msg->message == WM_WINDOWPOSCHANGING) {
        WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(msg->lParam);
        // 冻结任何「顶层提层」（HWND_TOP/TOPMOST/NOTOPMOST/外部程序参照），使弹框保持当前
        // band 内位置 —— 即落在全部 band 窗口之上、所有正常程序之下，不被收纳盒/助手遮挡。
        DesktopMirrorWindow::clampBandZOrder(msg->hwnd, wp);
        // 必须清掉 clampBandZOrder 加上的 SWP_NOACTIVATE：升级弹框要能激活才能收键盘
        // （Enter / Esc 关闭、「知道了」按钮的 default）。z 序冻结（SWP_NOZORDER）仍保留。
        wp->flags &= ~SWP_NOACTIVATE;
        return false;                  // 走默认处理链，改写已生效
    }
#endif
    return QDialog::nativeEvent(eventType, message, result);
}
