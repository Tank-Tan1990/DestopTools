/*
 * @file todolistwindow.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "todolistwindow.h"
#include "todostore.h"
#include "glassinputdialog.h"
#include "theme.h"
#include "thememanager.h"
#include "desktopmirrorwindow.h"   // raiseAboveBandWindows / clampBandZOrder / ensureLayerAboveBandWindows
#include "editorwatch.h"           // clearOwner：独立顶层窗口不能被 owner 的压底拖着沉下去

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QHideEvent>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

// 条目行圆点按钮样式：未完成=空心圆（有优先级则实心着色）；已完成=灰色实心
QString circleStyle(bool done, int priority) {
    QString bg = QStringLiteral("rgba(255,255,255,0.30)");
    if (!done) {
        switch (priority) {
            case 3: bg = QStringLiteral("#F87171"); break;   // 高=红
            case 2: bg = QStringLiteral("#FBBF24"); break;   // 中=黄
            case 1: bg = QStringLiteral("#34D399"); break;   // 低=绿
            default: bg = QStringLiteral("transparent"); break;
        }
    }
    const QString border = (!done && priority == 0)
        ? QStringLiteral("border: 1px solid rgba(255,255,255,0.55);")
        : QStringLiteral("border: none;");
    return Theme::applyTokens(QStringLiteral(
        "QToolButton { background: %1; %2 border-radius: 7px; padding: 0; }"
        "QToolButton:hover { border: 1px solid %3; }")
        .arg(bg, border, ThemeManager::instance()->accentColor().name()));
}

QString rowFrameStyle() {
    return Theme::applyTokens(QStringLiteral(
        "QFrame { background: transparent; border: none; border-radius: 8px; }"
        "QFrame:hover { background: rgba(34,211,238,0.08); }"));
}

// 底部「+ 添加待办事项」：右侧参考样式——主题色细「+」+ 常规文字
QString addLabelStyle() {
    return Theme::applyTokens(QStringLiteral(
        "QLabel { color: rgba(255,255,255,0.85); background: transparent; border: none; font-size: 12px; }"
        "QLabel:hover { color: %1; }")
        .arg(ThemeManager::instance()->accentColor().name()));
}

QString addLabelHtml() {
    return Theme::applyTokens(QStringLiteral(
        "<span style=\"color:%1; font-size:14px;\">+</span>&nbsp;&nbsp;添加待办事项")
        .arg(ThemeManager::instance()->accentColor().name()));
}

QString filterBtnStyle() {
    return Theme::applyTokens(QStringLiteral(
        "QToolButton { color: rgba(255,255,255,0.65); background: transparent; border: 1px solid transparent;"
        "              border-radius: 10px; padding: 2px 10px; font-size: 12px; }"
        "QToolButton:hover { color: #FFFFFF; background: rgba(34,211,238,0.12); }"
        "QToolButton:checked { color: #FFFFFF; background: rgba(34,211,238,0.18);"
        "                      border-color: rgba(34,211,238,0.45); }"));
}

QString itemTextStyle(bool done, int fontPoint) {
    return Theme::applyTokens(QStringLiteral(
        "QLabel { color: %1; background: transparent; border: none; %2 font-size: %3pt; }")
        .arg(done ? QStringLiteral("rgba(255,255,255,0.45)") : QStringLiteral("#FFFFFF"))
        .arg(done ? QStringLiteral("text-decoration: line-through;") : QString())
        .arg(fontPoint > 0 ? fontPoint : 13));
}

}   // namespace

TodoListWindow::TodoListWindow(QWidget* parent)
    // 必须是 Qt::Window（独立顶层窗口），禁 Qt::Dialog —— 与壁纸窗/快速搜索窗同源，
    // 避免 owner 化后被 band 窗口的周期性压底拖着沉下去。
    : QDialog(parent, Qt::FramelessWindowHint | Qt::Window | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(340, 480);

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

    // 数据变化 → 刷新列表（面板增删改/勾选完成时，清单窗同步）
    connect(&TodoStore::instance(), &TodoStore::itemsChanged, this, [this]() {
        if (isVisible()) refresh();
    });

    // 主题色 / 背景透明度联动：两类变化都重建行内样式（refreshTheme()）。
    // 不再 setWindowOpacity（整窗不透明度会把圆角线框与文字一起淡化）；透明度已并入背景色 alpha。
    ThemeManager::instance()->registerThemeTarget(this, [this]() { refreshTheme(); });
}

// 初始化ui
// 作者：谭征
void TodoListWindow::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(0);

    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("card"));
    m_card->setStyleSheet(Theme::panelStyle());
    // 不要给此卡片挂 QGraphicsDropShadowEffect：Qt 5.14 半透明顶层窗 + 图形效果
    // 的缓存不随动态子部件增删刷新 → 新行不显示、旧行/旧按钮 ghost 叠影，update() 也无效

    auto* cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    // 标题栏：事项清单 - 未完成/已完成 (N) + 视图切换 + 关闭
    m_titleBar = new QFrame(m_card);
    m_titleBar->setObjectName(QStringLiteral("titleBar"));
    m_titleBar->setFixedHeight(Theme::titleBarHeight());
    m_titleBar->setStyleSheet(Theme::titleBarStyle());
    auto* titleLayout = new QHBoxLayout(m_titleBar);
    titleLayout->setContentsMargins(12, 0, 8, 0);
    titleLayout->setSpacing(6);

    // 标题只保留名称；视图状态由右侧「未完成/已完成」切换按钮承担，不再重复显示
    m_caption = new QLabel(QStringLiteral("事项清单"), m_titleBar);
    m_caption->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 13px; font-weight: bold; background: transparent; border: none;"));

    m_undoneBtn = new QToolButton(m_titleBar);
    m_undoneBtn->setText(QStringLiteral("未完成"));
    m_undoneBtn->setCheckable(true);
    m_undoneBtn->setStyleSheet(filterBtnStyle());
    m_undoneBtn->setCursor(Qt::PointingHandCursor);
    connect(m_undoneBtn, &QToolButton::clicked, this, [this]() { setView(false); });

    m_doneBtn = new QToolButton(m_titleBar);
    m_doneBtn->setText(QStringLiteral("已完成"));
    m_doneBtn->setCheckable(true);
    m_doneBtn->setStyleSheet(filterBtnStyle());
    m_doneBtn->setCursor(Qt::PointingHandCursor);
    connect(m_doneBtn, &QToolButton::clicked, this, [this]() { setView(true); });

    auto* closeBtn = new QToolButton(m_titleBar);
    closeBtn->setIcon(Theme::icon("close"));
    closeBtn->setIconSize(QSize(16, 16));
    closeBtn->setText(QString());
    closeBtn->setFixedSize(Theme::headerButtonSize());
    closeBtn->setStyleSheet(Theme::toolButtonStyle());
    connect(closeBtn, &QToolButton::clicked, this, &QDialog::reject);

    titleLayout->addWidget(m_caption);
    titleLayout->addStretch();
    titleLayout->addWidget(m_undoneBtn);
    titleLayout->addWidget(m_doneBtn);
    titleLayout->addWidget(closeBtn);
    cardLayout->addWidget(m_titleBar);

    // 中部：条目列表
    m_listScroll = new QScrollArea(m_card);
    m_listScroll->setWidgetResizable(true);
    m_listScroll->setFrameShape(QFrame::NoFrame);
    m_listScroll->setStyleSheet(QStringLiteral(
        "QScrollArea { background: transparent; border: none; outline: none; }"
        "QScrollArea::viewport { background: transparent; border: none; }")
        + Theme::scrollBarStyle());
    m_listHost = new QWidget();
    m_listHost->setStyleSheet(QStringLiteral("background: transparent;"));
    m_listLayout = new QVBoxLayout(m_listHost);
    m_listLayout->setContentsMargins(10, 8, 10, 8);
    m_listLayout->setSpacing(2);
    m_listLayout->addStretch();
    m_listScroll->setWidget(m_listHost);
    cardLayout->addWidget(m_listScroll, 1);

    // 底部：添加待办事项 + 清空已完成
    m_bottomBar = new QFrame(m_card);
    m_bottomBar->setFixedHeight(36);   // 固定高度，防止内容被上下的卡片边界裁掉
    m_bottomBar->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "background: transparent; border-top: 1px solid rgba(34,211,238,%1);")
        .arg(Theme::alphaF(0.12), 0, 'f', 2)));
    auto* bottomLayout = new QHBoxLayout(m_bottomBar);
    bottomLayout->setContentsMargins(12, 4, 12, 4);
    bottomLayout->setSpacing(6);

    m_addLabel = new QLabel(m_bottomBar);
    qobject_cast<QLabel*>(m_addLabel)->setTextFormat(Qt::RichText);
    qobject_cast<QLabel*>(m_addLabel)->setText(addLabelHtml());
    m_addLabel->setCursor(Qt::PointingHandCursor);
    m_addLabel->setStyleSheet(addLabelStyle());
    m_addLabel->setFixedHeight(24);   // 文本行高固定，避免被裁剪
    m_addLabel->installEventFilter(this);   // click → addViaDialog
    bottomLayout->addWidget(m_addLabel);
    bottomLayout->addStretch();

    m_clearDoneBtn = new QToolButton(m_bottomBar);
    m_clearDoneBtn->setIcon(Theme::icon("delete"));
    m_clearDoneBtn->setIconSize(QSize(16, 16));
    m_clearDoneBtn->setToolTip(QStringLiteral("清空已完成"));
    m_clearDoneBtn->setFixedSize(Theme::headerButtonSize());
    m_clearDoneBtn->setStyleSheet(Theme::toolButtonStyle());
    m_clearDoneBtn->setCursor(Qt::PointingHandCursor);
    connect(m_clearDoneBtn, &QToolButton::clicked, this, []() {
        TodoStore::instance().removeDone();
    });
    bottomLayout->addWidget(m_clearDoneBtn);

    cardLayout->addWidget(m_bottomBar);
    root->addWidget(m_card);

    setView(false);
}

// 打开窗口（每次唤起都停在任务列表页，并刷新任务与倒计时）。
// 作者：谭征
void TodoListWindow::showWindow() {
    refresh();
    show();
    raise();
    scheduleLayerAsserts();
    if (m_layerKeeper && !m_layerKeeper->isActive()) m_layerKeeper->start();
}

// 设置视图
// 作者：谭征
void TodoListWindow::setView(bool showDone) {
    m_showDone = showDone;
    if (m_undoneBtn) m_undoneBtn->setChecked(!showDone);
    if (m_doneBtn) m_doneBtn->setChecked(showDone);
    if (m_clearDoneBtn) m_clearDoneBtn->setVisible(showDone);
    refresh();
}

// 刷新
// 作者：谭征
void TodoListWindow::refresh() {
    if (!m_listLayout) return;
    // 清空旧行（保留末尾 stretch）。必须立即 delete：deleteLater 下一轮才删，
    // 之后的强制重绘会把旧行原样画回缓存里（ghost 残留）
    while (m_listLayout->count() > 1) {
        QLayoutItem* item = m_listLayout->takeAt(0);
        if (item && item->widget()) delete item->widget();
        delete item;
    }

    const QVector<TodoItem> list = m_showDone
        ? TodoStore::instance().doneItems()
        : TodoStore::instance().undoneItems();

    for (const TodoItem& it : list) {
        QWidget* row = buildRow(it.id);
        m_listLayout->insertWidget(m_listLayout->count() - 1, row);
    }

    // 空视图提示
    if (list.isEmpty()) {
        auto* hint = new QLabel(m_showDone ? QStringLiteral("暂无已完成事项")
                                           : QStringLiteral("暂无待办事项，点击下方添加。"),
                                m_listHost);
        hint->setAlignment(Qt::AlignCenter);
        hint->setStyleSheet(Theme::applyTokens(QStringLiteral(
            "color: rgba(255,255,255,0.40); background: transparent; border: none; font-size: 12px;")));
        m_listLayout->insertWidget(m_listLayout->count() - 1, hint);
    }

    // 切换视图后滚动位置残留上个视图的偏移 → 首行被视口裁掉，强制回到顶部
    if (m_listScroll && m_listScroll->verticalScrollBar()) {
        m_listScroll->verticalScrollBar()->setValue(0);
    }
    // 卡片已不挂 QGraphicsDropShadowEffect（见 setupUi 注释），普通重绘即可
    QTimer::singleShot(0, this, [this]() {
        if (m_card) m_card->update();
        if (m_listHost) m_listHost->update();
        if (m_listScroll && m_listScroll->viewport()) m_listScroll->viewport()->update();
    });
}

// 构建行
// 作者：谭征
QWidget* TodoListWindow::buildRow(const QString& id) {
    const TodoItem it = TodoStore::instance().item(id);
    auto* row = new QFrame(m_listHost);
    row->setStyleSheet(rowFrameStyle());
    row->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(row, &QWidget::customContextMenuRequested, row, [this, row, id](const QPoint&) {
        const TodoUi::MenuResult r = TodoUi::execItemMenu(row, TodoStore::instance().item(id));
        TodoUi::applyItemAction(this, id, r);
    });

    auto* lay = new QHBoxLayout(row);
    lay->setContentsMargins(8, 5, 8, 5);
    lay->setSpacing(8);

    // 圆点：点击切换 完成/恢复
    auto* circle = new QToolButton(row);
    circle->setFixedSize(14, 14);
    circle->setCursor(Qt::PointingHandCursor);
    circle->setStyleSheet(circleStyle(it.done, it.priority));
    connect(circle, &QToolButton::clicked, circle, [id, done = it.done]() {
        TodoStore::instance().setDone(id, !done);
    });
    lay->addWidget(circle);

    // 文本
    auto* text = new QLabel(it.text, row);
    text->setStyleSheet(itemTextStyle(it.done, it.fontPoint));
    text->setWordWrap(true);
    if (it.reminder.isValid()) {
        text->setToolTip(QStringLiteral("提醒：%1").arg(it.reminder.toString(QStringLiteral("MM-dd hh:mm"))));
    }
    lay->addWidget(text, 1);

    // 已完成视图：行内删除按钮
    if (it.done) {
        auto* del = new QToolButton(row);
        del->setIcon(Theme::icon("delete"));
        del->setIconSize(QSize(14, 14));
        del->setFixedSize(20, 20);
        del->setToolTip(QStringLiteral("删除条目"));
        del->setStyleSheet(Theme::toolButtonStyle());
        del->setCursor(Qt::PointingHandCursor);
        connect(del, &QToolButton::clicked, del, [id]() {
            TodoStore::instance().remove(id);
        });
        lay->addWidget(del);
    }
    return row;
}

// 添加via对话框
// 作者：谭征
void TodoListWindow::addViaDialog() {
    bool ok = false;
    const QString text = GlassInputDialog::getText(this, QStringLiteral("添加待办事项"),
                                                   QStringLiteral("内容"), QLineEdit::Normal,
                                                   QString(), &ok);
    if (ok && !text.trimmed().isEmpty()) TodoStore::instance().addItem(text);
}

// 刷新主题
// 作者：谭征
void TodoListWindow::refreshTheme() {
    if (m_card) m_card->setStyleSheet(Theme::panelStyle());
    if (m_titleBar) m_titleBar->setStyleSheet(Theme::titleBarStyle());
    if (m_listScroll) {
        m_listScroll->setStyleSheet(QStringLiteral(
            "QScrollArea { background: transparent; border: none; outline: none; }"
            "QScrollArea::viewport { background: transparent; border: none; }")
            + Theme::scrollBarStyle());
    }
    if (m_undoneBtn) m_undoneBtn->setStyleSheet(filterBtnStyle());
    if (m_doneBtn) m_doneBtn->setStyleSheet(filterBtnStyle());
    if (m_bottomBar) {
        m_bottomBar->setStyleSheet(Theme::applyTokens(QStringLiteral(
            "background: transparent; border-top: 1px solid rgba(34,211,238,%1);")
            .arg(Theme::alphaF(0.12), 0, 'f', 2)));
    }
    if (m_addLabel) {
        m_addLabel->setStyleSheet(addLabelStyle());
        if (auto* lbl = qobject_cast<QLabel*>(m_addLabel)) lbl->setText(addLabelHtml());
    }
    if (m_clearDoneBtn) m_clearDoneBtn->setStyleSheet(Theme::toolButtonStyle());
    if (isVisible()) refresh();   // 行内样式按新主题重建
}

// 窗口基础设施（三件套同源）

// 作者：谭征
bool TodoListWindow::isDragAreaAt(const QPoint& pos) const {
    QWidget* hit = childAt(pos);
    if (!hit) return false;   // 卡片外透明阴影留白：不响应
    for (QWidget* p = hit; p && p != this; p = p->parentWidget()) {
        if (p == m_titleBar) return true;   // 标题栏可拖（按钮自己吞按下事件）
    }
    return hit == m_card;
}

// 标题栏本身不承载任何编辑交互，不会与输入/选择冲突。
// 作者：谭征
void TodoListWindow::mousePressEvent(QMouseEvent* event) {
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
void TodoListWindow::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPos() - m_dragPos);
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

// 鼠标松开事件
// 作者：谭征
void TodoListWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        event->accept();
        return;
    }
    QDialog::mouseReleaseEvent(event);
}

// 应用级“点别处即提交”守卫（装在 qApp 上）：见 cpp 中的实现说明。
// 作者：谭征
bool TodoListWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_addLabel && event->type() == QEvent::MouseButtonPress) {
        addViaDialog();
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

// 与 GlassInputDialog（重命名弹框）/ UpdateDialog 同源，幂等，可反复调用。
// 作者：谭征
void TodoListWindow::assertLayerAboveBandWindows() {
#ifdef Q_OS_WIN
    if (!isVisible()) return;
    const HWND h = reinterpret_cast<HWND>(winId());
    if (!h || !IsWindow(h)) return;
    EditorWatch::clearOwner(h);                     // ① 清 owner
    DesktopMirrorWindow::raiseAboveBandWindows(h);  // ② 插到全部 band 窗口之上
    if (GetForegroundWindow() != h) activateWindow();
#endif
}

// schedulelayerasserts
// 作者：谭征
void TodoListWindow::scheduleLayerAsserts() {
#ifdef Q_OS_WIN
    // show() 之后 Qt 仍会沿 parent 链补一次内部 z 序调整，单次断言会被它覆盖，故错峰多轮再钉。
    for (int delayMs : {0, 60, 200, 400, 800}) {
        QTimer::singleShot(delayMs, this, [this]() { assertLayerAboveBandWindows(); });
    }
#endif
}

// 显示事件
// 作者：谭征
void TodoListWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    scheduleLayerAsserts();
    if (m_layerKeeper) m_layerKeeper->start();
}

// 隐藏事件
// 作者：谭征
void TodoListWindow::hideEvent(QHideEvent* event) {
    if (m_layerKeeper) m_layerKeeper->stop();
    m_dragging = false;
    QDialog::hideEvent(event);
}

// 助手面板顶到普通程序之上再被压回的「一闪而过」。show desktop 期放行。
// 作者：谭征
bool TodoListWindow::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    MSG* msg = reinterpret_cast<MSG*>(message);
    if (msg && msg->message == WM_WINDOWPOSCHANGING) {
        WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(msg->lParam);
        DesktopMirrorWindow::clampBandZOrder(msg->hwnd, wp);
        wp->flags &= ~SWP_NOACTIVATE;   // 本窗要能激活以收键盘/弹层
        return false;
    }
#endif
    return QDialog::nativeEvent(eventType, message, result);
}
