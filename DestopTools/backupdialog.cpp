/*
 * @file backupdialog.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "backupdialog.h"
#include "theme.h"
#include "thememanager.h"
#include "desktopmirrorwindow.h"   // raiseAboveBandWindows / clampBandZOrder：弹框 z 序（同重命名弹框）
#include "editorwatch.h"           // clearOwner：弹框是独立顶层窗口，不能被 owner 的压底拖着沉下去
#include <QApplication>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QToolButton>
#include <QFrame>
#include <QMouseEvent>
#include <QGraphicsDropShadowEffect>

BackupDialog::BackupDialog(QWidget* parent)
    // 必须是 Qt::Window（独立顶层窗口），不能是 Qt::Dialog（2026-09-16 定论，与 GlassMessageBox /
    // UpdateDialog 同源）。Qt::Dialog + parent 会让 Qt 把 parent（band 窗口，Dock 每秒被压到
    // HWND_BOTTOM）的 HWND 指定成本窗口的 owner，弹框会跟着一起沉到桌面图标层之下 —— 用户看到
    // 的就是「弹框被压在别的窗口下面，点不到、看不见」。
    : QDialog(parent, Qt::FramelessWindowHint | Qt::Window | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(380, 200);
    setupUi();

    // z 序：弹框 > 收纳盒 / 桌面助手 / Dock 图标区（全部 band 窗口），但 < 其它任意程序
    // 错峰三轮（0/60/200ms）：show() 之后 Qt 仍会因 parent 链做一次内部 z 序调整，且 Dock 的
    // parkAllTargetsAtBottom（1s 周期）会持续改写 z 序，弹框须在空隙里反复把位置钉住。幂等。
#ifdef Q_OS_WIN
    for (int delayMs : {0, 60, 200}) {
        QTimer::singleShot(delayMs, this, [this]() { assertLayerAboveBandWindows(); });
    }
#endif

    // 全局主题色 / 背景透明度：本窗是**按需创建**的模态对话框，全部样式在 setupUi() 里用
    // Theme::applyTokens(...) 就地生成 —— 每次弹出取的都是当时的主题色与背景透明度，天然最新，
    // 无需登记刷新（也就不需要 ThemeManager 记住一个每次都新建的对象）。
    // 这里【不再】setWindowOpacity(整窗不透明度)：透明度已并入**背景色** alpha
    // （theme.h 的 bgAlphaF），而整窗不透明度会把圆角线框与文字一起淡化，
    // 与「圆角线框透明度不调整」的需求冲突。
}

// 把弹框钉在 band 窗口之上（幂等）。三步各自独立：任一失效都单独补齐。
// 作者：谭征
void BackupDialog::assertLayerAboveBandWindows() {
#ifdef Q_OS_WIN
    if (!isVisible()) return;
    const HWND h = reinterpret_cast<HWND>(winId());
    if (!h || !IsWindow(h)) return;
    EditorWatch::clearOwner(h);                              // ① 清 owner，避免随其压底而沉下去
    DesktopMirrorWindow::raiseAboveBandWindows(h);           // ② 插到全部 band 窗口之上
    if (GetForegroundWindow() != h) activateWindow();        // ③ 可激活才能收键盘（名称输入框）
#endif
}

// 初始化ui
// 作者：谭征
void BackupDialog::setupUi() {
    // 对话框完全透明，视觉由内部玻璃卡片提供（与 UpdateDialog/GlassInputDialog 一致）
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
    auto* body = new QVBoxLayout();
    body->setContentsMargins(24, 20, 24, 16);
    body->setSpacing(16);

    // 备份名称
    auto* nameRow = new QHBoxLayout();
    nameRow->setSpacing(10);
    auto* nameLabel = new QLabel(QStringLiteral("备份名称"), card);
    nameLabel->setFixedWidth(70);
    nameLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent; border: none;"));

    m_nameEdit = new QLineEdit(card);
    m_nameEdit->setText(QStringLiteral("桌面备份"));
    // 与设置中心输入框一致的浅玻璃填充，确保在深色卡片上可辨识
    m_nameEdit->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QLineEdit { background-color: rgba(255,255,255,0.06); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.18); border-radius: 6px; padding: 5px 8px; }"
        "QLineEdit:focus { border-color: rgba(34,211,238,0.35); }"
    )));
    m_nameEdit->setAutoFillBackground(true);
    m_nameEdit->setAttribute(Qt::WA_TranslucentBackground, false);

    nameRow->addWidget(nameLabel);
    nameRow->addWidget(m_nameEdit, 1);
    body->addLayout(nameRow);

    // 同步备份设置中心设置
    m_syncCheck = new QCheckBox(QStringLiteral("同步备份设置中心设置"), card);
    m_syncCheck->setChecked(true);
    m_syncCheck->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QCheckBox { color: #FFFFFF; spacing: 8px; background: transparent; border: none; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid rgba(34,211,238,0.30); border-radius: 3px; background: transparent; }"
        "QCheckBox::indicator:hover { border-color: #22D3EE; }"
        "QCheckBox::indicator:checked { background: transparent; border: 1px solid rgba(34,211,238,0.30); image: url(:/icons/check_plain_1x.png); }"
        "QCheckBox::indicator:checked:hover { border-color: #22D3EE; }"
    )));
    body->addWidget(m_syncCheck);
    // 备份改为「整份配置快照」（DestopTools.ini 全量复制），设置中心设置必然包含在内，
    // 该复选框不再有独立语义 → 隐藏（保留名称输入与确认/取消流程不变）。
    m_syncCheck->setVisible(false);

    body->addStretch();

    // 底部按钮
    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(10);
    btnRow->addStretch();

    auto* cancelBtn = new QPushButton(QStringLiteral("取消"), card);
    cancelBtn->setCursor(Qt::PointingHandCursor);
    cancelBtn->setFixedSize(72, 32);
    cancelBtn->setStyleSheet(Theme::pushButtonStyle());
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

    auto* confirmBtn = new QPushButton(QStringLiteral("确认"), card);
    confirmBtn->setDefault(true);
    confirmBtn->setCursor(Qt::PointingHandCursor);
    confirmBtn->setFixedSize(72, 32);
    confirmBtn->setStyleSheet(Theme::primaryButtonStyle());
    connect(confirmBtn, &QPushButton::clicked, this, &QDialog::accept);

    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(confirmBtn);
    body->addLayout(btnRow);

    cardLayout->addLayout(body, 1);

    root->addWidget(card);
}

// 备份名称
// 作者：谭征
QString BackupDialog::backupName() const {
    return m_nameEdit ? m_nameEdit->text().trimmed() : QString();
}

// 同步设置
// 作者：谭征
bool BackupDialog::syncSettings() const {
    return m_syncCheck ? m_syncCheck->isChecked() : true;
}

// 标题栏本身不承载任何编辑交互，不会与输入/选择冲突。
// 作者：谭征
void BackupDialog::mousePressEvent(QMouseEvent* event) {
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
void BackupDialog::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPos() - m_dragPos);
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

// 鼠标松开事件
// 作者：谭征
void BackupDialog::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        event->accept();
    }
    QDialog::mouseReleaseEvent(event);
}

// z 序守卫（与 GlassInputDialog「重命名弹框」/ UpdateDialog 同源）。
// WM_WINDOWPOSCHANGING 是**同步发送**消息、不走消息队列，全局 QAbstractNativeEventFilter 看不到，
// 必须在窗口自己的 nativeEvent 里拦截，否则「弹框被收纳盒/助手盖住」的问题修不掉。
// 作者：谭征
bool BackupDialog::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    MSG* msg = reinterpret_cast<MSG*>(message);
    if (msg && msg->message == WM_WINDOWPOSCHANGING) {
        WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(msg->lParam);
        DesktopMirrorWindow::clampBandZOrder(msg->hwnd, wp);
        // 必须清掉 clampBandZOrder 加上的 SWP_NOACTIVATE：弹框要能激活才能收键盘
        // （备份名称输入框 / 回车确认）。z 序冻结（SWP_NOZORDER）仍保留。
        wp->flags &= ~SWP_NOACTIVATE;
        return false;                  // 走默认处理链，改写已生效
    }
#endif
    return QDialog::nativeEvent(eventType, message, result);
}
