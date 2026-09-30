/*
 * @file windowsnap.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef WINDOWSNAP_H
#define WINDOWSNAP_H

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <climits>   // INT_MIN：resolve() 的 minY 默认值（= 不做顶部限制）

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

// 桌面挂件窗口的「自动对齐（格子对齐）」共享实现 —— 收纳盒窗口 / 桌面助手 / 全屏收纳盒通用。
// 拖动窗口时做两层吸附，**磁吸优先、网格兜底**：
// ① 窗口磁吸：本窗的左/右边缘、上/下边缘、水平/垂直中心线，与**其它挂件窗口**及**屏幕工作区**
// 的对齐线距离在阈值内 → 吸附过去。含四种边缘配对（左-左、右-右、右-左贴左、左-右贴右）
// 与中心-中心，所以既能"贴边对齐"也能"居中"。
// ② 网格吸附：该轴若没有任何磁吸命中，则把左上角取整到固定步长的网格格子（格子对齐）。
// 为什么磁吸优先：磁吸是"我要跟这个窗对齐"的明确意图；网格只是"别摆得太乱"的兜底。
// 逃生口：拖动过程中按住 **Alt** 可临时完全关闭对齐（想精确摆位时用）。
class WindowSnap {
public:
    // 网格步长（逻辑像素）＝**折叠收纳盒的高度**（`Theme::collapsedHeight()`，当前 44）。
    // 为什么用它而不是 Windows 桌面图标间距（用户 2026-09-24 指定）：
    // 本机实测桌面图标间距是 76×101 物理像素，**垂直 101** 的跳格太大；而收纳盒与桌面助手
    // 最常见的形态就是一根 44 高的折叠细条。取"一根折叠条的高度"当格子，折叠条就能
    // **一格一根地贴在一起堆叠**（间距正好等于自身高度，不多不少），也正好是用户要的量级
    // （原话「离 dock 桌面顶部，约半个收纳盒（折叠）的距离，约 45 逻辑像素」）。
    static QSize gridStep();

    // 网格原点（屏幕全局逻辑坐标）＝ (0, topMargin())。
    // 于是格子线落在 10、54、98… 上 —— "从桌面顶部往下留 10px 起格，之后每格一根折叠条"。
    static QPoint gridOrigin();

    // 顶部留白（逻辑像素，当前 **10**）。
    // 用户 2026-09-24 指定：收纳盒 / 桌面助手**不许贴到桌面（屏幕工作区）顶边**，
    // 顶端至少留出这一距离。它与网格步长**已解耦** —— 网格仍是一格
    // `Theme::collapsedHeight()`（44，一根折叠条），但第一条格子线整体下移到 10。
    static int topMargin();

    // 屏幕全局坐标下的"顶部限制线"＝该矩形所在屏幕的**工作区顶边 + topMargin()**。
    // 供顶层挂件窗口（收纳盒窗口 / 桌面助手）直接传给 resolve() 的 minY；
    // 副屏（工作区顶边 ≠ 0）也能算对。
    static int topLimitGlobal(const QRect& globalRect);

    // 磁吸触发阈值（逻辑像素）。Qt 逻辑像素已按 DPI 归一，故无需再乘 DPR ——
    // 8 逻辑像素在任何缩放下都是同一"手感"距离。
    static int magnetThreshold() { return 8; }

    // 拖动时是否启用对齐（按住 Alt = 临时关闭）。
    static bool enabledNow();

    // 供 UI 提示（标题栏 tooltip）使用的说明文案。两份 UI（收纳盒 / 桌面助手）共用同一份口径，
    // 避免两处各写一遍、日后改一处忘一处。
    static QString hintText();

    // 把"自由拖动得到的左上角"换算成最终左上角。
    // raw      —— 未经处理的左上角（调用方坐标系）
    // size     —— 窗口尺寸
    // magnets  —— 磁吸候选矩形（必须与 raw 同一坐标系）
    // origin   —— 网格原点（必须与 raw 同一坐标系）
    // snappedX / snappedY —— 出参：该轴是否命中磁吸（false = 走的是网格兜底）
    // minY     —— 该坐标系下的**最小 y**（顶部留白）。磁吸/网格算完后统一抬到不低于它，
    // 于是"吸屏幕顶边"与"网格第一行"都不会把窗口贴到顶部。默认 INT_MIN＝不限制。
    static QPoint resolve(const QPoint& raw, const QSize& size,
                          const QVector<QRect>& magnets,
                          const QPoint& origin,
                          bool* snappedX = nullptr, bool* snappedY = nullptr,
                          int minY = INT_MIN);

    // 收集磁吸候选：当前所有**可见挂件窗口**的屏幕逻辑矩形（排除 exclude）
    // ＋ 所有屏幕的工作区矩形（于是"贴屏幕边/屏幕居中"也顺手成立）。
    // 供顶层窗口（收纳盒窗口 / 桌面助手）直接使用；子控件场景（全屏收纳盒）请自建候选。
    static QVector<QRect> collectSnapTargets(const QWidget* exclude);
};

#endif // WINDOWSNAP_H
