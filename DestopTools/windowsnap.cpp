/*
 * @file windowsnap.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "windowsnap.h"
#include "icongridwindow.h"       // 收纳盒窗口 / 主整理窗口：磁吸候选
#include "assistantwindow.h"      // 桌面助手面板：磁吸候选
#include "desktopmirrorwindow.h"  // Dock（铺满主屏）：磁吸候选
#include "theme.h"                // Theme::collapsedHeight()：网格步长＝一根折叠收纳盒的高度

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QWidget>
#include <climits>

namespace {
// 某个 QWidget 是否属于本程序的"桌面挂件窗口"（磁吸候选）。
bool isBandWidget(QWidget* w) {
    return qobject_cast<IconGridWindow*>(w) != nullptr
        || qobject_cast<AssistantWindow*>(w) != nullptr
        || qobject_cast<DesktopMirrorWindow*>(w) != nullptr;
}
}  // namespace

// （原话「离 dock 桌面顶部，约半个收纳盒（折叠）的距离，约 45 逻辑像素」）。
// 作者：谭征
QSize WindowSnap::gridStep() {
    // 一格 = 一根折叠收纳盒的高度（当前 44）。下限 8 防呆：万一 collapsedHeight() 被改成 0，
    // 也不会把窗口吸成 1px 的死格子（拖动像粘住一样）。
    const int h = qMax(8, Theme::collapsedHeight());
    return QSize(h, h);
}

// `Theme::collapsedHeight()`（44，一根折叠条），但第一条格子线整体下移到 10。
// 作者：谭征
int WindowSnap::topMargin() {
    // 顶部留白（用户 2026-09-24 指定：**10 逻辑像素**）。
    // 注意它与网格步长**已解耦**：网格仍是一格 44（一根折叠条的高度），
    // 但第一条格子线不再贴顶 —— 落在 10，于是格子线为 10 / 54 / 98…，间距仍是 44。
    return 10;
}

// 于是格子线落在 10、54、98… 上 —— "从桌面顶部往下留 10px 起格，之后每格一根折叠条"。
// 作者：谭征
QPoint WindowSnap::gridOrigin() {
    // 全局坐标：第一条格子线落在 y = topMargin()（10），即"离桌面顶部留出 10px"。
    return QPoint(0, topMargin());
}

// 副屏（工作区顶边 ≠ 0）也能算对。
// 作者：谭征
int WindowSnap::topLimitGlobal(const QRect& globalRect) {
    // 取矩形中心所在屏幕（拖动跨屏时以窗口中心为准），拿它的**工作区**顶边 ——
    // 任务栏在顶部时 availableGeometry 的 top 已经不是 0，这里天然算对。
    QScreen* s = QGuiApplication::screenAt(globalRect.center());
    if (!s) s = QGuiApplication::primaryScreen();
    const int top = s ? s->availableGeometry().top() : 0;
    return top + topMargin();
}

// 拖动时是否启用对齐（按住 Alt = 临时关闭）。
// 作者：谭征
bool WindowSnap::enabledNow() {
    // Alt = 临时关闭自动对齐（想精确摆位的逃生口）。拖动过程中逐帧查询，按下/松开即刻生效。
    return !(QApplication::keyboardModifiers() & Qt::AltModifier);
}

// 避免两处各写一遍、日后改一处忘一处。
// 作者：谭征
QString WindowSnap::hintText() {
    return QStringLiteral(
        "拖动时自动对齐：\n"
        "· 与其它收纳盒 / 桌面助手的边缘或中心磁吸对齐；\n"
        "· 没有可对齐的目标时，吸附到网格（每格约 44 逻辑像素，等于一根折叠收纳盒的高度）；\n"
        "· 距桌面顶部始终留出 10 逻辑像素，不会贴住顶边；\n"
        "· 按住 Alt 拖动可临时关闭自动对齐。");
}

// 于是"吸屏幕顶边"与"网格第一行"都不会把窗口贴到顶部。默认 INT_MIN＝不限制。
// 作者：谭征
QPoint WindowSnap::resolve(const QPoint& raw, const QSize& size,
                           const QVector<QRect>& magnets,
                           const QPoint& origin,
                           bool* snappedX, bool* snappedY, int minY) {
    if (snappedX) *snappedX = false;
    if (snappedY) *snappedY = false;

    const int th = magnetThreshold();
    const int w = size.width();
    const int h = size.height();

    int bestDx = INT_MAX, bestDy = INT_MAX;
    int x = raw.x(), y = raw.y();

    // 只在"比已选中的候选更近"时才改写（严格小于）→ 结果稳定、不会在两线之间跳。
    auto considerX = [&](int cand, int dist) {
        if (dist <= th && dist < bestDx) { bestDx = dist; x = cand; }
    };
    auto considerY = [&](int cand, int dist) {
        if (dist <= th && dist < bestDy) { bestDy = dist; y = cand; }
    };

    for (const QRect& m : magnets) {
        if (!m.isValid()) continue;
        const int mRight = m.left() + m.width();    // 右边缘（不含）
        const int mBottom = m.top() + m.height();   // 下边缘（不含）

        // —— 横轴 ——
        considerX(m.left(),             qAbs(m.left() - raw.x()));                 // 左-左
        considerX(mRight - w,           qAbs(mRight - (raw.x() + w)));             // 右-右
        considerX(m.left() - w,         qAbs(m.left() - (raw.x() + w)));           // 右贴左边缘外侧
        considerX(mRight,               qAbs(mRight - raw.x()));                   // 左贴右边缘外侧
        considerX(m.center().x() - w / 2, qAbs(m.center().x() - (raw.x() + w / 2))); // 中心-中心

        // —— 纵轴 ——
        considerY(m.top(),              qAbs(m.top() - raw.y()));                  // 上-上
        considerY(mBottom - h,          qAbs(mBottom - (raw.y() + h)));            // 下-下
        considerY(m.top() - h,          qAbs(m.top() - (raw.y() + h)));            // 下贴上边缘外侧
        considerY(mBottom,              qAbs(mBottom - raw.y()));                  // 上贴下边缘外侧
        considerY(m.center().y() - h / 2, qAbs(m.center().y() - (raw.y() + h / 2))); // 中心-中心
    }

    const QSize step = gridStep();
    if (bestDx == INT_MAX) {
        // 网格兜底：左上角取整到离它最近的格子。
        const int sx = qMax(8, step.width());
        x = origin.x() + qRound(double(raw.x() - origin.x()) / double(sx)) * sx;
    } else if (snappedX) {
        *snappedX = true;
    }

    if (bestDy == INT_MAX) {
        const int sy = qMax(8, step.height());
        y = origin.y() + qRound(double(raw.y() - origin.y()) / double(sy)) * sy;
    } else if (snappedY) {
        *snappedY = true;
    }

    // —— 顶部留白（2026-09-24）——
    // 磁吸与网格都可能把窗口顶到工作区顶边（磁吸尤其：屏幕工作区顶边是一条明确的候选对齐线），
    // 所以这一步必须在两者之后统一兜底：不管走了哪条路，顶端都不许越过这条线。
    if (y < minY) y = minY;

    return QPoint(x, y);
}

// 供顶层窗口（收纳盒窗口 / 桌面助手）直接使用；子控件场景（全屏收纳盒）请自建候选。
// 作者：谭征
QVector<QRect> WindowSnap::collectSnapTargets(const QWidget* exclude) {
    QVector<QRect> out;

    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget* w : tops) {
        if (!w || w == exclude) continue;
        if (!w->isVisible() || w->isMinimized()) continue;
        if (!isBandWidget(w)) continue;
        const QRect g = w->frameGeometry();
        if (g.isValid()) out.append(g);
    }

    // 屏幕工作区：这样"贴屏幕边"与"屏幕居中"也纳入磁吸（Dock 铺满主屏时二者重叠，无副作用）。
    const QList<QScreen*> screens = QApplication::screens();
    for (QScreen* s : screens) {
        if (!s) continue;
        const QRect g = s->availableGeometry();
        if (g.isValid()) out.append(g);
    }

    return out;
}
