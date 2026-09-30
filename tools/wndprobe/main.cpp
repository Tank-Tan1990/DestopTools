/*
 * @file main.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

// 窗口属性级排查：QMenu 为什么拿不到 WS_EX_LAYERED（= alpha 合成位）。
// 背景：tools/popupprobe 已证实 —— 所有 QMenu 的 WS_EX_LAYERED 都是 no，其半透明背景
// 只能在**黑色 backing store** 上合成（白底画布上 alpha 变小反而更暗，反推底色 = 黑），
// 因此菜单永远透不出后面的桌面；而普通 QWidget（Qt::Window 或 Qt::Popup）+Translucent
// 都能拿到 LAYERED=yes。说明本机 Qt 的半透明窗口机制是好的，只有 QMenu 拿不到。
// 本程序**不显示任何窗口**，只做"属性级"排查（构造 → 可选设置 → winId() → 读 EX 样式
// 与 QWindow::format()），5 秒出结果，用于定位到底哪种设置组合能拿到 alpha 合成位。
#include <QApplication>
#include <QMenu>
#include <QWidget>
#include <QWindow>
#include <QSurfaceFormat>
#include <QTextStream>

#include <windows.h>

namespace {

int g_bad = 0;

// 观察点：
// priorHandle —— dump 之前 QWindow 是否已存在（若已存在，说明"构造即建窗"，
// 之后任何 WA_TranslucentBackground 都来不及影响窗口创建）
// fmtAlpha / AB —— QWindow 的 surface format 是否带 alpha 通道（Qt 据此决定加 LAYERED）
// LAYERED —— Win32 侧最终结果（= 能否真正做 per-pixel alpha 合成）
void dump(QTextStream& out, const QString& label, QWidget* w) {
    QWindow* prior    = w->windowHandle();
    const bool hadIt  = (prior != nullptr);
    const WId  id     = w->winId();               // 强制创建平台窗口
    const HWND h      = reinterpret_cast<HWND>(id);
    const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    QWindow* wh       = w->windowHandle();

    const bool layered = (ex & WS_EX_LAYERED) != 0;
    if (!layered) ++g_bad;

    const QString s = QStringLiteral("%1 | priorHandle=%2 attr=%3 fmtAlpha=%4 AB=%5 LAYERED=%6")
                          .arg(label, -50)
                          .arg(hadIt ? QStringLiteral("Y") : QStringLiteral("N"))
                          .arg(w->testAttribute(Qt::WA_TranslucentBackground) ? QStringLiteral("Y")
                                                                             : QStringLiteral("N"))
                          .arg((wh && wh->format().hasAlpha()) ? QStringLiteral("Y") : QStringLiteral("N"))
                          .arg(wh ? wh->format().alphaBufferSize() : -999)
                          .arg(layered ? QStringLiteral("Y") : QStringLiteral("N"));
    out << s << "\n";
    out.flush();
}

}   // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTextStream out(stdout);

    QWidget host;   // 只作为 parent，不显示

    // 对照 1：普通顶层窗（与 Dock / 助手窗同型）—— 预期 LAYERED=Y
    {
        auto* w = new QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        w->setAttribute(Qt::WA_TranslucentBackground, true);
        dump(out, QStringLiteral("对照 QWidget(Window|Frameless)"), w);
        delete w;
    }

    // 对照 2：QWidget 直接带 Qt::Popup —— 与菜单的窗口类型一致，预期 LAYERED=Y
    {
        auto* w = new QWidget(nullptr, Qt::Popup | Qt::FramelessWindowHint);
        w->setAttribute(Qt::WA_TranslucentBackground, true);
        dump(out, QStringLiteral("对照 QWidget(Qt::Popup)"), w);
        delete w;
    }

    // A：QMenu 基线（产品现状）
    {
        auto* m = new QMenu(&host);
        dump(out, QStringLiteral("A QMenu 基线"), m);
        delete m;
    }

    // B：QMenu + Translucent
    {
        auto* m = new QMenu(&host);
        m->setAttribute(Qt::WA_TranslucentBackground, true);
        dump(out, QStringLiteral("B QMenu +Translucent"), m);
        delete m;
    }

    // C：QMenu + Translucent + 去系统阴影
    {
        auto* m = new QMenu(&host);
        m->setWindowFlag(Qt::NoDropShadowWindowHint, true);
        m->setAttribute(Qt::WA_TranslucentBackground, true);
        dump(out, QStringLiteral("C QMenu +Translucent +NoDropShadow"), m);
        delete m;
    }

    // D：QMenu 显式重置窗口标志（触发平台窗口重建）后再 Translucent
    {
        auto* m = new QMenu(&host);
        m->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        m->setAttribute(Qt::WA_TranslucentBackground, true);
        dump(out, QStringLiteral("D QMenu 显式 flags 重建 +Translucent"), m);
        delete m;
    }

    // E：QMenu 不带 parent
    {
        auto* m = new QMenu(nullptr);
        m->setAttribute(Qt::WA_TranslucentBackground, true);
        dump(out, QStringLiteral("E QMenu(无 parent) +Translucent"), m);
        delete m;
    }

    // F：先建窗（winId），再设属性，再 hide/show 触发重建
    {
        auto* m = new QMenu(&host);
        m->winId();                                   // 先创建
        m->setAttribute(Qt::WA_TranslucentBackground, true);
        m->hide();
        m->show();
        dump(out, QStringLiteral("F QMenu 先建窗→设属性→hide/show"), m);
        delete m;
    }

    out << QStringLiteral("\n未拿到 LAYERED 的项数 = %1\n").arg(g_bad);
    out.flush();
    return g_bad;
}
