/*
 * @file feedbackdialog.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "feedbackdialog.h"
#include "theme.h"
#include "glassmessagebox.h"
#include "thememanager.h"
#include "desktopmirrorwindow.h"   // raiseAboveBandWindows / clampBandZOrder：弹框 z 序（同重命名弹框）
#include "editorwatch.h"           // clearOwner：弹框是独立顶层窗口，不能被 owner 的压底拖着沉下去
#include <QApplication>
#include <QTimer>
#include <QSize>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QMouseEvent>
#include <QStackedWidget>
#include <QTextEdit>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QDesktopServices>
#include <QUrl>
#include <QVector>
#include <QGraphicsDropShadowEffect>

FeedbackDialog::FeedbackDialog(QWidget* parent)
    // 必须是 Qt::Window（独立顶层窗口），不能是 Qt::Dialog（2026-09-16 定论，与 GlassMessageBox /
    // UpdateDialog 同源）。Qt::Dialog + parent 会让 Qt 把 parent（band 窗口，Dock 每秒被压到
    // HWND_BOTTOM）的 HWND 指定成本窗口的 owner，弹框会跟着一起沉到桌面图标层之下。
    : QDialog(parent, Qt::FramelessWindowHint | Qt::Window | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(620, 520);
    setupUi();

    // z 序：弹框 > 收纳盒 / 桌面助手 / Dock 图标区（全部 band 窗口），但 < 其它任意程序
    // 错峰三轮（0/60/200ms），原因同 UpdateDialog：show() 后 Qt 的内部 z 序调整 + Dock 的
    // parkAllTargetsAtBottom（1s 周期）都会改写 z 序。幂等。
#ifdef Q_OS_WIN
    for (int delayMs : {0, 60, 200}) {
        QTimer::singleShot(delayMs, this, [this]() { assertLayerAboveBandWindows(); });
    }
#endif

    // 全局主题色 / 背景透明度：本窗是按需创建的独立窗口，全部样式在 setupUi() 里用
    // Theme::applyTokens(...) 就地生成 —— 每次弹出取的都是当时的主题色与背景透明度，天然最新。
    // 这里【不再】setWindowOpacity(整窗不透明度)：透明度已并入**背景色** alpha（theme.h 的 bgAlphaF），
    // 整窗不透明度会把圆角线框与文字一起淡化，与「圆角线框透明度不调整」的需求冲突。
}

// 把弹框钉在 band 窗口之上（幂等）。三步各自独立：任一失效都单独补齐。
// 作者：谭征
void FeedbackDialog::assertLayerAboveBandWindows() {
#ifdef Q_OS_WIN
    if (!isVisible()) return;
    const HWND h = reinterpret_cast<HWND>(winId());
    if (!h || !IsWindow(h)) return;
    EditorWatch::clearOwner(h);                              // ① 清 owner，避免随其压底而沉下去
    DesktopMirrorWindow::raiseAboveBandWindows(h);           // ② 插到全部 band 窗口之上
    if (GetForegroundWindow() != h) activateWindow();        // ③ 可激活才能收键盘（反馈输入框）
#endif
}

// 初始化ui
// 作者：谭征
void FeedbackDialog::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(0);

    // 玻璃卡片：内容区按图 3 红框位置的颜色（窗口深蓝 11,16,33）作为单层玻璃底，
    // 使「我要反馈」与「反馈记录」两页叠加一致、无双层色差。
    auto* card = new QFrame(this);
    card->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QFrame { background-color: rgba(11,16,33,1.0); border: 1px solid rgba(34,211,238,0.12); border-radius: 16px; }"
    )));

    auto* shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(24);
    shadow->setColor(Theme::shadow());
    shadow->setOffset(0, 4);
    card->setGraphicsEffect(shadow);

    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    // 自定义标题栏：就地覆盖为与卡片完全一致的窗口深蓝 rgba(11,16,33,1.0)，
    // 使整框（标题栏 + 内容区）无颜色接缝、完全统一。
    m_titleBar = new QFrame(card);
    m_titleBar->setObjectName(QStringLiteral("titleBar"));
    m_titleBar->setFixedHeight(Theme::titleBarHeight());
    m_titleBar->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QFrame { background-color: rgba(11,16,33,1.0); border-top-left-radius: 16px; border-top-right-radius: 16px; border-bottom: 1px solid rgba(34,211,238,0.16); }"
    )));
    auto* titleLayout = new QHBoxLayout(m_titleBar);
    titleLayout->setContentsMargins(12, 0, 8, 0);
    titleLayout->setSpacing(6);

    auto* appIcon = new QLabel(m_titleBar);
    appIcon->setPixmap(Theme::icon(QStringLiteral("feedback")).pixmap(QSize(18, 18)));
    appIcon->setStyleSheet(QStringLiteral("background: transparent; border: none;"));

    auto* caption = new QLabel(QStringLiteral("帮助与反馈"), m_titleBar);
    caption->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 13px; font-weight: bold; background: transparent; border: none;"));

    auto* forumBtn = new QPushButton(QStringLiteral("进入论坛"), m_titleBar);
    forumBtn->setCursor(Qt::PointingHandCursor);
    forumBtn->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QPushButton { background: transparent; color: rgba(255,255,255,0.80); border: none; border-radius: 4px; padding: 2px 8px; font-size: 12px; }"
        "QPushButton:hover { color: #FFFFFF; background-color: rgba(34,211,238,0.12); }"
    )));
    connect(forumBtn, &QPushButton::clicked, this, []() {
        // 使用占位地址，默认由系统浏览器打开；后续可替换为真实产品论坛地址
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.bing.com")));
    });

    auto* closeBtn = new QToolButton(m_titleBar);
    closeBtn->setIcon(Theme::icon(QStringLiteral("close")));
    closeBtn->setIconSize(QSize(16, 16));
    closeBtn->setText(QString());
    closeBtn->setFixedSize(Theme::headerButtonSize());
    closeBtn->setStyleSheet(Theme::toolButtonStyle());
    connect(closeBtn, &QToolButton::clicked, this, &QDialog::reject);

    titleLayout->addWidget(appIcon);
    titleLayout->addWidget(caption);
    titleLayout->addStretch();
    titleLayout->addWidget(forumBtn);
    titleLayout->addWidget(closeBtn);

    cardLayout->addWidget(m_titleBar);

    // Tab 栏
    auto* tabBar = new QFrame(card);
    tabBar->setFrameStyle(QFrame::NoFrame);
    tabBar->setFixedHeight(44);
    tabBar->setStyleSheet(QStringLiteral("QFrame { background-color: transparent; border: none; }"));
    auto* tabLayout = new QHBoxLayout(tabBar);
    tabLayout->setContentsMargins(16, 0, 16, 0);
    tabLayout->setSpacing(0);

    const QStringList tabNames = {
        QStringLiteral("我要反馈"),
        QStringLiteral("反馈记录")
    };
    for (int i = 0; i < tabNames.size(); ++i) {
        auto* btn = new QPushButton(tabNames.at(i));
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedHeight(42);
        btn->setStyleSheet(Theme::applyTokens(QStringLiteral(
            "QPushButton { background: transparent; color: #FFFFFF; border: none; border-radius: 0; padding: 8px 16px; font-size: 13px; }"
            "QPushButton:hover { color: #FFFFFF; }"
            "QPushButton:checked { color: #FFFFFF; font-weight: bold; border-bottom: 2px solid #22D3EE; }"
        )));
        connect(btn, &QPushButton::clicked, this, [this, i]() { onTabClicked(i); });
        m_tabBtns.append(btn);
        tabLayout->addWidget(btn);
    }
    tabLayout->addStretch();

    cardLayout->addWidget(tabBar);

    // 堆叠内容区
    m_stack = new QStackedWidget(card);
    m_stack->addWidget(createFeedbackPage());
    m_stack->addWidget(createPlaceholderPage(QStringLiteral("反馈记录")));

    cardLayout->addWidget(m_stack, 1);

    root->addWidget(card);

    onTabClicked(0); // 默认打开「我要反馈」
}

// 创建feedback页
// 作者：谭征
QWidget* FeedbackDialog::createFeedbackPage() {
    auto* page = new QWidget();
    // 内容区不再额外叠加背景色，直接使用 card 的单层玻璃底，
    // 避免与反馈记录页（单底）出现两层叠加不一致的问题。
    page->setStyleSheet(QStringLiteral(
        "QWidget { background-color: transparent; border: none; border-radius: 0px; }"
    ));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 16, 24, 16);
    layout->setSpacing(14);

    // 问题描述标签
    auto* descLabel = new QLabel(QStringLiteral("* 请描述您的问题："));
    descLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 13px; font-weight: bold; background: transparent; border: none;"));
    layout->addWidget(descLabel);

    // 文本编辑框 + 底部工具栏（不再使用外框容器，避免残留矩形线框）
    auto* editContainer = new QFrame();
    editContainer->setFrameStyle(QFrame::NoFrame);
    editContainer->setStyleSheet(QStringLiteral(
        "QFrame { border: none; background-color: transparent; }"
    ));
    auto* editLayout = new QVBoxLayout(editContainer);
    editLayout->setContentsMargins(1, 1, 1, 1);
    editLayout->setSpacing(0);

    m_contentEdit = new QTextEdit();
    m_contentEdit->setPlaceholderText(QStringLiteral("欢迎使用桌面助手，请提出您的建议和意见："));
    m_contentEdit->setMinimumHeight(180);
    m_contentEdit->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QTextEdit { background-color: transparent; color: #FFFFFF; border: 1px solid rgba(34,211,238,0.12); border-radius: 10px; padding: 8px; }"
        "QTextEdit:focus { border-color: rgba(34,211,238,0.35); }"
    )));

    auto* toolbar = new QFrame();
    toolbar->setFrameStyle(QFrame::NoFrame);
    toolbar->setFixedHeight(36);
    toolbar->setStyleSheet(QStringLiteral(
        "QFrame { border: none; background-color: transparent; }"
    ));
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(8, 0, 8, 0);
    toolbarLayout->setSpacing(16);

    m_screenshotCheck = new QCheckBox(QStringLiteral("截图"));
    m_screenshotCheck->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QCheckBox { color: #FFFFFF; spacing: 8px; background: transparent; border: none; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid rgba(34,211,238,0.30); border-radius: 3px; background: transparent; }"
        "QCheckBox::indicator:hover { border-color: #22D3EE; }"
        "QCheckBox::indicator:checked { background: transparent; border: 1px solid rgba(34,211,238,0.30); image: url(:/icons/check_plain_1x.png); }"
        "QCheckBox::indicator:checked:hover { border-color: #22D3EE; }"
    )));
    m_screenshotCheck->setAttribute(Qt::WA_TranslucentBackground, true);
    m_screenshotCheck->setAutoFillBackground(false);

    m_attachBtn = new QPushButton(QStringLiteral("添加文件"));
    m_attachBtn->setCursor(Qt::PointingHandCursor);
    m_attachBtn->setFixedSize(76, 26);
    m_attachBtn->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QPushButton { color: #FFFFFF; background: transparent; border: 1px solid rgba(34,211,238,0.18); border-radius: 8px; padding: 2px 10px; font-size: 12px; }"
        "QPushButton:hover { background: rgba(34,211,238,0.12); border-color: rgba(34,211,238,0.35); }"
    )));
    m_attachLabel = new QLabel();
    m_attachLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 12px; background: transparent; border: none;"));
    connect(m_attachBtn, &QPushButton::clicked, this, [this]() {
        QString file = QFileDialog::getOpenFileName(this,
                                                    QStringLiteral("选择附件"),
                                                    QString(),
                                                    QStringLiteral("图片 (*.png *.jpg *.jpeg);;所有文件 (*.*)"));
        if (!file.isEmpty() && m_attachLabel) {
            m_attachLabel->setText(QFileInfo(file).fileName());
        }
    });

    toolbarLayout->addWidget(m_screenshotCheck);
    toolbarLayout->addWidget(m_attachBtn);
    toolbarLayout->addWidget(m_attachLabel);
    toolbarLayout->addStretch();

    editLayout->addWidget(m_contentEdit, 1);
    editLayout->addWidget(toolbar);

    layout->addWidget(editContainer, 1);

    // 联系方式
    auto* contactRow = new QHBoxLayout();
    contactRow->setSpacing(10);
    auto* contactLabel = new QLabel(QStringLiteral("* 联系方式："));
    contactLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 13px; font-weight: bold; background: transparent; border: none;"));
    m_contactTypeCombo = new QComboBox();
    m_contactTypeCombo->addItem(QStringLiteral("手机"));
    m_contactTypeCombo->addItem(QStringLiteral("QQ"));
    m_contactTypeCombo->addItem(QStringLiteral("邮箱"));
    m_contactTypeCombo->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QComboBox { background-color: transparent; color: #FFFFFF; border: 1px solid rgba(34,211,238,0.18); border-radius: 10px; padding: 5px 10px; min-width: 80px; }"
        "QComboBox:hover { background-color: rgba(34,211,238,0.12); border-color: rgba(34,211,238,0.35); }"
        "QComboBox::drop-down { border: none; width: 24px; }"
        "QComboBox QAbstractItemView { background-color: rgba(17,26,46,1.0); color: #FFFFFF; selection-background-color: rgba(34,211,238,0.18); border: 1px solid rgba(34,211,238,0.18); outline: none; }"
        "QComboBox QAbstractItemView::item { color: #FFFFFF; border-radius: 0px; padding: 6px 10px; }"
        "QComboBox QAbstractItemView::item:hover { background-color: rgba(34,211,238,0.12); }"
        "QComboBox QAbstractItemView::item:selected { background-color: rgba(34,211,238,0.18); border: 1px solid rgba(34,211,238,0.35); }"
    )));
    m_contactEdit = new QLineEdit();
    m_contactEdit->setPlaceholderText(QStringLiteral("请输入联系方式"));
    m_contactEdit->setMinimumWidth(180);
    m_contactEdit->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QLineEdit { background-color: transparent; color: #FFFFFF; border: 1px solid rgba(34,211,238,0.18); border-radius: 10px; padding: 5px 10px; }"
        "QLineEdit:hover { background-color: rgba(34,211,238,0.12); border-color: rgba(34,211,238,0.35); }"
        "QLineEdit:focus { border-color: #22D3EE; }"
        "QLineEdit::placeholder { color: rgba(255,255,255,0.45); }"
    )));

    contactRow->addWidget(contactLabel);
    contactRow->addWidget(m_contactTypeCombo);
    contactRow->addWidget(m_contactEdit);
    contactRow->addStretch();

    layout->addLayout(contactRow);

    // 底部行
    auto* bottomRow = new QHBoxLayout();
    bottomRow->setSpacing(4);

    auto* luckyLabel = new QLabel(QStringLiteral("幸运星：180xxxx2954 贡献价值反馈，获得"));
    luckyLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 12px; background: transparent; border: none;"));
    auto* giftLabel = new QLabel(QStringLiteral("定制版鼠标垫"));
    giftLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 12px; background: transparent; border: none;"));

    bottomRow->addWidget(luckyLabel);
    bottomRow->addWidget(giftLabel);
    bottomRow->addStretch();

    auto* submitBtn = new QPushButton(QStringLiteral("提交问题"));
    submitBtn->setCursor(Qt::PointingHandCursor);
    submitBtn->setFixedSize(100, 34);
    submitBtn->setStyleSheet(Theme::primaryButtonStyle());
    connect(submitBtn, &QPushButton::clicked, this, &FeedbackDialog::onSubmit);

    bottomRow->addWidget(submitBtn);

    layout->addLayout(bottomRow);

    return page;
}

// 创建placeholder页
// 作者：谭征
QWidget* FeedbackDialog::createPlaceholderPage(const QString& title) {
    auto* page = new QWidget();
    // 与「我要反馈」页保持一致的背景处理：透明底，直接透出 card 的单层玻璃底，
    // 避免两页之间出现背景色/深浅不一致。
    page->setStyleSheet(QStringLiteral(
        "QWidget { background-color: transparent; border: none; border-radius: 0px; }"
    ));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 16, 24, 16);
    layout->setSpacing(14);
    auto* label = new QLabel(title + QStringLiteral("（待扩展）"));
    label->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 13px; font-weight: bold; background: transparent; border: none;"));
    layout->addWidget(label);
    layout->addStretch();
    return page;
}

// 设置标签style
// 作者：谭征
void FeedbackDialog::setTabStyle(int activeIndex) {
    // 下划线由 QSS「QPushButton#tabButton:checked」统一渲染，无需在代码里反复追加 styleSheet
    // （原先每次切换都向按钮 styleSheet 末尾拼接 border-bottom，导致字符串无限增长并最终覆盖样式）。
    for (int i = 0; i < m_tabBtns.size(); ++i) {
        m_tabBtns.at(i)->setChecked(i == activeIndex);
    }
}

// 响应标签clicked
// 作者：谭征
void FeedbackDialog::onTabClicked(int index) {
    if (m_stack && index >= 0 && index < m_stack->count()) {
        m_stack->setCurrentIndex(index);
    }
    setTabStyle(index);
}

// 响应submit
// 作者：谭征
void FeedbackDialog::onSubmit() {
    QString content = m_contentEdit ? m_contentEdit->toPlainText().trimmed() : QString();
    QString contact = m_contactEdit ? m_contactEdit->text().trimmed() : QString();

    if (content.isEmpty()) {
        GlassMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请填写问题描述"));
        return;
    }
    if (contact.isEmpty()) {
        GlassMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请填写联系方式"));
        return;
    }

    // 占位：实际可替换为网络提交
    GlassMessageBox::information(this, QStringLiteral("提交成功"), QStringLiteral("感谢您的反馈，我们会尽快处理！"));
    accept();
}

// 标题栏本身不承载任何编辑交互，不会与输入/选择冲突。
// 作者：谭征
void FeedbackDialog::mousePressEvent(QMouseEvent* event) {
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
void FeedbackDialog::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPos() - m_dragPos);
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

// 鼠标松开事件
// 作者：谭征
void FeedbackDialog::mouseReleaseEvent(QMouseEvent* event) {
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
bool FeedbackDialog::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    MSG* msg = reinterpret_cast<MSG*>(message);
    if (msg && msg->message == WM_WINDOWPOSCHANGING) {
        WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(msg->lParam);
        DesktopMirrorWindow::clampBandZOrder(msg->hwnd, wp);
        // 必须清掉 clampBandZOrder 加上的 SWP_NOACTIVATE：弹框要能激活才能收键盘
        // （反馈内容输入框）。z 序冻结（SWP_NOZORDER）仍保留。
        wp->flags &= ~SWP_NOACTIVATE;
        return false;                  // 走默认处理链，改写已生效
    }
#endif
    return QDialog::nativeEvent(eventType, message, result);
}
