/*
 * @file assistantwindow.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "assistantwindow.h"
#include "sidepanelwidget.h"
#include "desktopmirrorwindow.h"   // clampBandZOrder：nativeEvent 提层守卫
#include "theme.h"
#include "thememanager.h"
#include "settingsmanager.h"
#include "windowsnap.h"            // 自动对齐（格子对齐）+ 窗口互相磁吸
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QCloseEvent>
#include <QShowEvent>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

AssistantWindow::AssistantWindow(QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint) {
    setObjectName(QStringLiteral("AssistantWindow"));
    setWindowTitle(QStringLiteral("桌面助手"));
    setAttribute(Qt::WA_TranslucentBackground, true);
    resize(320, 720);
    setMinimumSize(280, 400);
    // 桌面助手窗口的外框：经 Theme::boxChrome 受「盒子显示边框」开关控制（默认显示）。
    setStyleSheet(Theme::boxChrome(Theme::windowStyle()));

    // 主题色/透明度联动：重绘窗口样式。
    // 2026-09-21：不再有独立的「不透明度」通道 —— 透明度已并入**背景色** alpha
    // （见 theme.h 的 bgAlphaF），所以主题色与透明度变化都走同一条「重刷样式」，窗口自身的
    // 圆角线框与文字不会被淡化（需求：线框透明度不调整），也不再调用 setWindowOpacity。
    // 2026-09-22：本函数同时是「盒子显示边框」开关的重刷出口 —— ThemeManager::
    // reloadAppearanceFlags() 在该开关变化时追加一次 themeChanged，经登记表落到这里。
    auto applyStyle = [this]() {
        this->setStyleSheet(Theme::boxChrome(Theme::windowStyle()));
    };
    ThemeManager::instance()->registerThemeTarget(this, applyStyle);
    applyStyle();     // 初始按当前主题色/透明度应用一次

    // 记录初始展开尺寸，作为展开时的回退尺寸
    m_expandedWidth = width();
    m_expandedHeight = height();

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    m_panel = new SidePanelWidget(this);
    connect(m_panel, &SidePanelWidget::toolTriggered, this, &AssistantWindow::toolTriggered);
    connect(m_panel, &SidePanelWidget::searchTextChanged, this, &AssistantWindow::searchTextChanged);
    connect(m_panel, &SidePanelWidget::collapseToggled, this, &AssistantWindow::onPanelCollapsed);

    // 折叠/展开状态不在构造期应用，统一由 MainWindow::loadLayout 在恢复几何后处理，
    // 避免用默认 320x720 覆盖用户已修改的展开尺寸。

    rootLayout->addWidget(m_panel);
}

// 显示事件
// 作者：谭征
void AssistantWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
#ifdef Q_OS_WIN
    // 设置 WS_EX_NOACTIVATE：本窗口永不被系统激活，因此 Windows“显示桌面”
    // （任务栏最右侧按钮 / Win+D）会直接跳过它，既不最小化也不隐藏，避免可见闪烁。
    if (HWND h = reinterpret_cast<HWND>(winId())) {
        LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
        if (!(ex & WS_EX_NOACTIVATE)) {
            SetWindowLongW(h, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
            SetWindowPos(h, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
        }
    }
#endif
}

// 常态拦截提层：WM_WINDOWPOSCHANGING（同步发送、不走消息队列，全局过滤器看不到）
// 阶段调用 DesktopMirrorWindow::clampBandZOrder，在 z 序变更应用之前冻结「顶层提层」，
// 杜绝「开其它程序后点击 → 助手面板被顶到程序之上再被压回」的一闪而过。
// show desktop 激活期守卫放行，topmost 状态机不受影响。
// 作者：谭征
bool AssistantWindow::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    Q_UNUSED(result)
    MSG* msg = static_cast<MSG*>(message);
    if (msg && msg->message == WM_WINDOWPOSCHANGING) {
        DesktopMirrorWindow::clampBandZOrder(msg->hwnd,
                                             reinterpret_cast<WINDOWPOS*>(msg->lParam));
        return false;   // 改写已就位，走默认处理链
    }
#else
    Q_UNUSED(eventType) Q_UNUSED(message) Q_UNUSED(result)
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

// 面板
// 作者：谭征
SidePanelWidget* AssistantWindow::panel() const {
    return m_panel;
}

// 展开尺寸
// 作者：谭征
QSize AssistantWindow::expandedSize() const {
    return QSize(m_expandedWidth, m_expandedHeight);
}

// 设置展开尺寸
// 作者：谭征
void AssistantWindow::setExpandedSize(const QSize& size) {
    if (size.isValid() && size.width() > 0 && size.height() > 0) {
        m_expandedWidth = size.width();
        m_expandedHeight = size.height();
    }
}

// 标题栏本身不承载任何编辑交互，不会与输入/选择冲突。
// 作者：谭征
void AssistantWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragPos = event->globalPos() - frameGeometry().topLeft();
        event->accept();
    }
    QWidget::mousePressEvent(event);
}

// 鼠标移动事件
// 作者：谭征
void AssistantWindow::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        QPoint target = event->globalPos() - m_dragPos;
        // —— 自动对齐（2026-09-24）——
        // 与收纳盒窗口同一套：先磁吸到其它挂件窗口 / 屏幕边缘，未命中则吸附到桌面图标网格。
        // 每帧从当前鼠标位置重算自由坐标，避免"吸附值被当成新基准"导致的拖不动。
        if (WindowSnap::enabledNow()) {
            target = WindowSnap::resolve(target, size(),
                                         WindowSnap::collectSnapTargets(this),
                                         WindowSnap::gridOrigin(),
                                         nullptr, nullptr,
                                         // 距桌面顶部留出一根折叠收纳盒的高度（不贴顶边）
                                         WindowSnap::topLimitGlobal(QRect(target, size())));
        }
        move(target);
        event->accept();
    }
    QWidget::mouseMoveEvent(event);
}

// 鼠标松开事件
// 作者：谭征
void AssistantWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const bool wasDragging = m_dragging;
        m_dragging = false;
        event->accept();
        // 拖拽移动结束后通知主窗口保存当前几何
        if (wasDragging) {
            emit windowGeometryChanged();
        }
    }
    QWidget::mouseReleaseEvent(event);
}

// 为 false 时仅更新窗口尺寸，供启动恢复阶段使用，避免覆盖已持久化的展开尺寸。
// 作者：谭征
void AssistantWindow::setPanelCollapsed(bool collapsed, bool save) {
    if (collapsed == m_collapsed) return;

    m_collapsed = collapsed;
    if (m_collapsed) {
        // 记录当前展开尺寸，供下次展开恢复
        m_expandedWidth = width();
        m_expandedHeight = height();
        setMinimumHeight(Theme::collapsedHeight());
        resize(width(), Theme::collapsedHeight());
    } else {
        setMinimumHeight(400);
        resize(m_expandedWidth, qMax(m_expandedHeight, 400));
    }

    if (save) {
        // 持久化桌面助手窗口的折叠状态和展开尺寸
        SettingsManager sm;
        sm.saveValue(QStringLiteral("AssistantWindow/collapsed"), m_collapsed);
        sm.saveValue(QStringLiteral("AssistantWindow/expandedSize"), expandedSize());

        // 折叠/展开会改变窗口大小，通知主窗口保存当前几何
        emit windowGeometryChanged();
    }
}

// 响应面板折叠
// 作者：谭征
void AssistantWindow::onPanelCollapsed(bool collapsed) {
    setPanelCollapsed(collapsed, true);
}

// 关闭事件
// 作者：谭征
void AssistantWindow::closeEvent(QCloseEvent* event) {
    // 关闭前通知主窗口保存当前几何
    emit windowGeometryChanged();
    QWidget::closeEvent(event);
}
