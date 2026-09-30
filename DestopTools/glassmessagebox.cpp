/*
 * @file glassmessagebox.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "glassmessagebox.h"
#include "theme.h"
#include "thememanager.h"
#include "desktopmirrorwindow.h"   // raiseAboveBandWindows / clampBandZOrder：弹框 z 序（同重命名弹框）
#include "editorwatch.h"           // clearOwner：弹框是独立顶层窗口，不能被 owner 的压底拖着沉下去
#include <QApplication>
#include <QScreen>        // confirm(): 锚点定位（screenAt / availableGeometry）
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QMouseEvent>
#include <QPainter>
#include <QGraphicsDropShadowEffect>

// information
// 作者：谭征
int GlassMessageBox::information(QWidget* parent, const QString& title, const QString& text) {
    GlassMessageBox dlg(parent, title, text, InfoIcon);
    return dlg.exec();
}

// warning
// 作者：谭征
int GlassMessageBox::warning(QWidget* parent, const QString& title, const QString& text) {
    GlassMessageBox dlg(parent, title, text, WarningIcon);
    return dlg.exec();
}

// confirm
// 作者：谭征
int GlassMessageBox::confirm(QWidget* parent, const QString& title, const QString& text,
                             const QString& okText, const QString& cancelText,
                             const QString& okIconName, const QRect& frameRect) {
    GlassMessageBox dlg(parent, title, text, InfoIcon, okText, cancelText, okIconName);
    // 就地确认（拖框建盒用）：弹框的**位置与大小都取自用户刚画出的那个框** —— 框画在哪、多大，
    // 确认层就落在哪、多大（视觉上"那个框原地变成了确认层"），点确认才真正变成收纳盒。
    // 兜底：小于卡片最小需求时回到 360×180（否则 48px 图标 + 文字 + 按钮会被挤爆），
    // 并整体夹进该屏幕可用区，避免贴边画框时确认层有一半在屏幕外。
    if (!frameRect.isEmpty()) {
        QScreen* scr = QGuiApplication::screenAt(frameRect.center());
        if (!scr) scr = QGuiApplication::primaryScreen();
        const QRect avail = scr ? scr->availableGeometry() : QRect();
        int w = qMax(360, frameRect.width());
        int h = qMax(180, frameRect.height());
        if (avail.isValid()) {
            w = qMin(w, avail.width());
            h = qMin(h, avail.height());
        }
        dlg.setFixedSize(w, h);
        QPoint pos = frameRect.topLeft();
        if (avail.isValid()) {
            pos.setX(qBound(avail.x(), pos.x(), qMax(avail.x(), avail.right() - w + 1)));
            pos.setY(qBound(avail.y(), pos.y(), qMax(avail.y(), avail.bottom() - h + 1)));
        }
        dlg.move(pos);            // move() 会置 WA_Moved，Qt 不再按 parent 重算初始位置
    }
    return dlg.exec();
}

GlassMessageBox::GlassMessageBox(QWidget* parent, const QString& title, const QString& text, IconType iconType,
                                 const QString& okText, const QString& cancelText, const QString& okIconName)
    // 必须是 Qt::Window（独立顶层窗口），不能是 Qt::Dialog（2026-09-16 定论，用户报「提示弹框层级不对」）。
    // Qt::Dialog + parent 会让 Qt 把 parent 的 HWND 指定成本窗口的 owner，而本项目所有
    // parent 候选（Dock 图标 / 图标网格 / 桌面助手面板）都是 band 窗口：Dock 每秒被
    // SetWindowPos(HWND_BOTTOM) 压到壁纸层，被 owner 化的提示弹框会**跟着一起沉下去**，
    // 落到桌面图标层之下 —— 用户看到的就是「弹框被压在别的窗口下面，点不到、看不见」。
    // 与 GlassInputDialog（重命名 / 新建分类弹框）、SettingCenterDialog 保持同一标志集。
    : QDialog(parent, Qt::FramelessWindowHint | Qt::Window | Qt::NoDropShadowWindowHint)
    , m_iconType(iconType)
    , m_okText(okText)
    , m_cancelText(cancelText)
    , m_okIconName(okIconName) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(360, 180);
    setupUi(title, text);

    // 全局主题色 / 背景透明度：本窗是按需创建的模态提示框，全部样式在 setupUi() 里用
    // Theme::applyTokens(...) 就地生成 —— 每次弹出取的都是当时的主题色与背景透明度，天然最新。
    // 这里【不再】setWindowOpacity(整窗不透明度)：透明度已并入**背景色** alpha（theme.h 的 bgAlphaF），
    // 整窗不透明度会把圆角线框与文字一起淡化，与「圆角线框透明度不调整」的需求冲突。

#ifdef Q_OS_WIN
    // z 序：弹框 > 收纳盒 / 桌面助手 / Dock 图标区（全部 band 窗口），但 < 其它任意程序
    // 与 GlassInputDialog 同方案。错峰三轮（0/60/200ms）而不是只做一次，原因：
    // · show() 之后 Qt 仍会因 parent 链做一次内部 z 序调整，单次插入可能被它覆盖；
    // · Dock 的 parkAllTargetsAtBottom（1s 周期）与 band 守卫会持续改写 z 序，
    // 弹框必须在这些改写的空隙里把位置重新钉住。
    // assertLayerAboveBandWindows() 幂等，重复调用无副作用。
    for (int delayMs : {0, 60, 200}) {
        QTimer::singleShot(delayMs, this, [this]() { assertLayerAboveBandWindows(); });
    }
#endif
}

// 把弹框钉在 band 窗口之上（幂等）。三步各自独立：任一失效都单独补齐，不做「一次性做完就算」。
// 作者：谭征
void GlassMessageBox::assertLayerAboveBandWindows() {
#ifdef Q_OS_WIN
    if (!isVisible()) return;
    const HWND h = reinterpret_cast<HWND>(winId());
    if (!h || !IsWindow(h)) return;
    // ① 先清 owner：owner 一旦落在 Dock/收纳盒上，它被压底时会把我们一并拖下去。
    EditorWatch::clearOwner(h);
    // ② 再插到全部 band 窗口之上（不使用 HWND_TOP/TOPMOST，故仍低于其它正常程序）。
    DesktopMirrorWindow::raiseAboveBandWindows(h);
    // ③ 弹框必须可激活才收得到 Enter / Esc（OK 按钮的 default 也依赖键盘焦点）。
    // 本进程通常不是前台，activateWindow() 可能被前台锁忽略，这里只作补充、不阻塞。
    if (GetForegroundWindow() != h) activateWindow();
#endif
}

// 初始化ui
// 作者：谭征
void GlassMessageBox::setupUi(const QString& title, const QString& text) {
    // 对话框本身完全透明，所有视觉内容由 card 提供（与 SidePanelWidget 一致）
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12); // 留出阴影空间
    root->setSpacing(0);

    // 玻璃卡片：与桌面助手面板完全一致
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
    m_titleBar->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 rgba(17,26,46,1.0),stop:1 rgba(17,26,46,1.0));"
        "border-top-left-radius: 16px; border-top-right-radius: 16px;"
        "border-bottom: 1px solid rgba(34,211,238,0.12);"
    )));
    auto* titleLayout = new QHBoxLayout(m_titleBar);
    titleLayout->setContentsMargins(12, 0, 8, 0);
    titleLayout->setSpacing(6);

    auto* caption = new QLabel(title, m_titleBar);
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

    m_iconLabel = new QLabel(card);
    m_iconLabel->setFixedSize(48, 48);
    paintIcon(m_iconType);

    auto* msgLabel = new QLabel(text, card);
    msgLabel->setWordWrap(true);
    msgLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 13px; background: transparent; border: none;"));

    body->addWidget(m_iconLabel, 0, Qt::AlignVCenter);
    body->addWidget(msgLabel, 1, Qt::AlignVCenter);

    cardLayout->addLayout(body, 1);

    // 按钮区：默认单个「OK」（information / warning 原样保留）；confirm() 时按传入文字 + 图标渲染。
    // 文字是动作（如「创建收纳盒」）时按钮更宽，故用紧凑 padding 版样式 —— primaryButtonStyle()
    // 的 `padding: 8px 20px` 会在 28px 高的按钮上把内容区压到只剩 12px，16px 的图标会被裁掉。
    auto* btnLayout = new QHBoxLayout();
    btnLayout->setContentsMargins(0, 0, 16, 12);
    btnLayout->setSpacing(10);
    btnLayout->addStretch();

    const bool customBtn = !m_okText.isEmpty();
    if (customBtn && !m_cancelText.isEmpty()) {
        auto* cancelBtn = new QPushButton(m_cancelText, card);
        cancelBtn->setFixedHeight(28);
        cancelBtn->setMinimumWidth(64);
        cancelBtn->setStyleSheet(Theme::applyTokens(QStringLiteral(
            "QPushButton { color: #FFFFFF; background: rgba(255,255,255,0.04); border: 1px solid rgba(34,211,238,0.40);"
            " border-radius: 8px; padding: 2px 16px; font-size: 12px; }"
            "QPushButton:hover { background: rgba(34,211,238,0.12); border-color: #22D3EE; }"
            "QPushButton:pressed { background: rgba(34,211,238,0.22); border-color: #67E8F9; }"
        )));
        cancelBtn->setCursor(Qt::PointingHandCursor);
        connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
        btnLayout->addWidget(cancelBtn);
    }

    auto* okBtn = new QPushButton(customBtn ? m_okText : QStringLiteral("OK"), card);
    if (customBtn) {
        okBtn->setFixedHeight(28);
        okBtn->setMinimumWidth(64);
        okBtn->setStyleSheet(Theme::applyTokens(QStringLiteral(
            "QPushButton { color: #FFFFFF; background: rgba(34,211,238,0.10); border: 1px solid #22D3EE;"
            " border-radius: 8px; padding: 2px 16px; font-size: 12px; font-weight: bold; }"
            "QPushButton:hover { background: rgba(34,211,238,0.22); border-color: #67E8F9; }"
            "QPushButton:pressed { background: rgba(34,211,238,0.32); border-color: #67E8F9; }"
        )));
        if (!m_okIconName.isEmpty()) {
            okBtn->setIcon(Theme::icon(m_okIconName));
            okBtn->setIconSize(QSize(16, 16));
        }
    } else {
        okBtn->setFixedSize(64, 28);
        okBtn->setStyleSheet(Theme::primaryButtonStyle());
    }
    okBtn->setCursor(Qt::PointingHandCursor);
    okBtn->setDefault(true);            // 与重命名/新建分类弹框一致：回车即可确认
    connect(okBtn, &QPushButton::clicked, this, &QDialog::accept);

    btnLayout->addWidget(okBtn);
    cardLayout->addLayout(btnLayout);

    root->addWidget(card);
}

// paint图标
// 作者：谭征
void GlassMessageBox::paintIcon(IconType iconType) {
    if (!m_iconLabel) return;
    QPixmap pixmap(48, 48);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // 图标背景：信息用青蓝渐变，警告用琥珀渐变
    QRadialGradient grad(24, 24, 24);
    if (iconType == WarningIcon) {
        grad.setColorAt(0, QColor("#FBBF24"));
        grad.setColorAt(1, QColor("#F59E0B"));
    } else {
        // 信息图标跟随当前主题色（与全局主题联动一致）
        const QColor accent = ThemeManager::instance()->accentColor();
        grad.setColorAt(0, accent);
        grad.setColorAt(1, accent.darker(115));
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(grad);
    painter.drawEllipse(0, 0, 48, 48);

    // 白色 "i" / "!"，与截图信息图标保持一致
    painter.setPen(QPen(Qt::white, 3));
    painter.setBrush(Qt::white);
    if (iconType == WarningIcon) {
        painter.drawLine(24, 14, 24, 28);
        painter.drawEllipse(22, 32, 4, 4);
    } else {
        painter.drawLine(24, 16, 24, 18);
        painter.drawLine(24, 22, 24, 34);
    }

    m_iconLabel->setPixmap(pixmap);
}

// 标题栏本身不承载任何编辑交互，不会与输入/选择冲突。
// 作者：谭征
void GlassMessageBox::mousePressEvent(QMouseEvent* event) {
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
void GlassMessageBox::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPos() - m_dragPos);
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

// 鼠标松开事件
// 作者：谭征
void GlassMessageBox::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
    }
    QDialog::mouseReleaseEvent(event);
}

// z 序守卫（与 SettingCenterDialog / GlassInputDialog 同源）。
// WM_WINDOWPOSCHANGING 是**同步发送**消息、不走消息队列，全局 QAbstractNativeEventFilter 看不到，
// 必须在窗口自己的 nativeEvent 里拦截，否则「弹框被收纳盒/助手盖住」的问题修不掉。
// 作者：谭征
bool GlassMessageBox::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    MSG* msg = reinterpret_cast<MSG*>(message);
    if (msg && msg->message == WM_WINDOWPOSCHANGING) {
        WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(msg->lParam);
        // 冻结任何「顶层提层」（HWND_TOP/TOPMOST/NOTOPMOST/外部程序参照），使弹框保持当前
        // band 内位置 —— 即落在全部 band 窗口之上、所有正常程序之下，不被收纳盒/助手遮挡。
        DesktopMirrorWindow::clampBandZOrder(msg->hwnd, wp);
        // 必须清掉 clampBandZOrder 加上的 SWP_NOACTIVATE：提示弹框要能激活才能收键盘
        // （Enter / Esc 关闭、OK 按钮的 default）。z 序冻结（SWP_NOZORDER）仍保留。
        wp->flags &= ~SWP_NOACTIVATE;
        return false;                  // 走默认处理链，改写已生效
    }
#endif
    return QDialog::nativeEvent(eventType, message, result);
}
