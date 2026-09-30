/*
 * @file main.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

// D1（2026-09-21）实测：整屏 WA_TranslucentBackground 分层窗（WS_EX_LAYERED）在
// “子控件 move + 重绘”下，每次 flush 的开销是否随【窗口面积】增长。
// 背景：Dock / 收纳盒都是全屏半透明分层窗。若 Qt 对分层窗的 flush 走 UpdateLayeredWindow
// 提交【整个窗口表面】，那么一次极小的局部重绘也会被放大成“整窗 alpha 合成”
// （1080p≈8MB、4K≈33MB 每帧），拖拽/悬停这类逐像素重绘的操作就会异常昂贵。
// 方法：对 3 种窗口尺寸 × 2 种窗口类型（分层半透明 / 普通不透明），分别测
// 「1 个 72x84 子控件 move 1px + repaint()」的平均每帧耗时（repaint 为同步重绘 + flush）。
// 两种重绘范围都测：整窗 repaint（wholeWindow=true）与只 repaint 该子控件（局部）。
// 判读：
// · D1 成立 ⇔ 分层窗“局部重绘”的每帧耗时随面积显著增长（≈线性），而不透明窗基本恒定；
// · D1 不成立 ⇔ 两者都基本不随面积变化（说明按脏区提交），则无需为它改架构。
// 结果写入 exe 同目录的 d1layer_result.txt。

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <QPalette>
#include <QScreen>
#include <QTextStream>
#include <QThread>
#include <QVector>
#include <QWidget>

// 模拟 Dock 图标：72x84 单元格 + 48px 图标块 + 一行标签
class Box : public QWidget {
public:
    explicit Box(QWidget* parent) : QWidget(parent) {
        setFixedSize(72, 84);
        setAttribute(Qt::WA_TranslucentBackground, true);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(QRect(12, 6, 48, 48), QColor(255, 140, 60, 230));
        p.setPen(Qt::white);
        p.drawText(QRect(0, 58, width(), 22), Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("图标"));
    }
};

class Stage : public QWidget {
public:
    Stage(bool layered, const QSize& sz)
        : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint) {
        setAttribute(Qt::WA_TranslucentBackground, layered);
        if (!layered) {
            setAutoFillBackground(true);
            QPalette pal = palette();
            pal.setColor(QPalette::Window, QColor(28, 32, 40));
            setPalette(pal);
        }
        resize(sz);
        const int cols = qMax(1, sz.width() / 90);
        const int rows = qMax(1, sz.height() / 110);
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                Box* b = new Box(this);
                const QPoint pos(10 + c * 90, 10 + r * 110);
                b->move(pos);
                m_boxes.append(b);
                m_base.append(pos);
            }
        }
        move(0, 0);
    }
    int boxCount() const { return m_boxes.size(); }

    // 一帧：只移动第一个子控件 1px（真实拖拽的最小脏区），然后按指定范围重绘。
    void step(bool wholeWindow) {
        if (m_boxes.isEmpty()) return;
        m_flip = !m_flip;
        Box* b = m_boxes.first();
        b->move(m_base.first().x() + (m_flip ? 1 : 0), m_base.first().y());
        if (wholeWindow) repaint();
        else b->repaint();
    }

private:
    QVector<Box*> m_boxes;
    QVector<QPoint> m_base;
    bool m_flip = false;
};

static double bench(Stage* st, bool wholeWindow, int frames) {
    for (int i = 0; i < 10; ++i) st->step(wholeWindow);   // 预热
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < frames; ++i) {
        st->step(wholeWindow);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
    }
    return double(t.nsecsElapsed()) / 1e6 / double(frames);   // ms/帧
}

// 单个配置：新建窗口 → show → 等稳定 → 测四个量 → close
static void measure(bool layered, const QSize& sz, int frames,
                    double& whole, double& local, int& boxes) {
    Stage st(layered, sz);
    st.show();
    for (int i = 0; i < 30; ++i) QCoreApplication::processEvents();
    QThread::msleep(250);
    QCoreApplication::processEvents();

    boxes = st.boxCount();
    whole = bench(&st, true, frames);
    local = bench(&st, false, frames);

    st.close();
    QCoreApplication::processEvents();
    QThread::msleep(120);
    QCoreApplication::processEvents();
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    QScreen* scr = QApplication::primaryScreen();
    const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1920, 1080);

    QVector<QSize> sizes;
    sizes << QSize(640, 480);
    if (avail.width() >= 1280 && avail.height() >= 720) sizes << QSize(1280, 720);
    if (avail.width() >= 1920 && avail.height() >= 1000) sizes << QSize(1920, 1000);

    const int frames = 120;

    QString out;
    out += QStringLiteral("D1 分层半透明窗 flush 开销实测（2026-09-21）\n");
    out += QStringLiteral("屏幕可用区: %1 x %2\n").arg(avail.width()).arg(avail.height());
    out += QStringLiteral("方法: 每配置 %1 帧「1 个 72x84 子控件 move 1px + repaint()」，"
                          "repaint 为同步重绘（含 flush）；每帧后处理 UpdateRequest。\n").arg(frames);
    out += QStringLiteral("列说明: 整窗 = repaint(整窗)；局部 = repaint(仅该子控件)。数值为平均每帧 ms。\n");
    out += QStringLiteral("归一化 = 局部每帧 ms ÷ (窗口百万像素)，用于判断是否 O(面积)。\n\n");
    out += QStringLiteral("尺寸            子控件  分层/整窗  不透明/整窗  分层/局部  不透明/局部  "
                          "分层归一化  不透明归一化\n");

    for (const QSize& sz : sizes) {
        double lw = 0, ll = 0, ow = 0, ol = 0;
        int lb = 0, ob = 0;
        measure(true, sz, frames, lw, ll, lb);
        measure(false, sz, frames, ow, ol, ob);

        const double mp = double(sz.width()) * double(sz.height()) / 1e6;
        out += QStringLiteral("%1 x %2        %3     %4      %5      %6     %7      %8      %9\n")
                   .arg(sz.width(), 5).arg(sz.height(), -5)
                   .arg(lb, 4)
                   .arg(lw, 8, 'f', 3).arg(ow, 9, 'f', 3)
                   .arg(ll, 9, 'f', 3).arg(ol, 10, 'f', 3)
                   .arg(ll / mp, 10, 'f', 4).arg(ol / mp, 11, 'f', 4);
    }

    out += QStringLiteral("\n判读:\n");
    out += QStringLiteral(" · 若「分层/局部」随尺寸显著增长、而「不透明/局部」基本恒定 → D1 成立"
                          "（局部重绘被放大为整窗分层提交）。\n");
    out += QStringLiteral(" · 若两者都不随尺寸变化（归一化数值接近）→ D1 不成立，无需为渲染层改架构。\n");

    const QString path = QCoreApplication::applicationDirPath() + QStringLiteral("/d1layer_result.txt");
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QTextStream ts(&f);
        ts.setCodec("UTF-8");
        ts << out;
        f.close();
    }
    fflush(stdout);
    fprintf(stdout, "%s", out.toUtf8().constData());
    return 0;
}
