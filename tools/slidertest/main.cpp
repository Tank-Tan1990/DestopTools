/*
 * @file main.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

// QSlider 信号顺序实测（供 DestopTools「透明度改了不落盘」根因验证）
// 待验证命题：当 setTracking(false) 时，用户拖动后 Qt 是先发 sliderReleased()、
// 再执行 setValue(d->position) —— 也就是在 sliderReleased 槽里读 value() 拿到的是
// 【拖动前的旧值】。若成立，则「在 sliderReleased 里 saveSettings()」会把旧值写回配置，
// 表现恰好是用户反馈的「拖到 100%，重启又变回旧值」。
// 做法：程序化发送 按下(手柄上)/移动/抬起 三个鼠标事件，把每个信号里读到的
// value()/sliderPosition() 打印出来，最后再打印抬起之后（事件循环跑完）的 value()。

#include <QApplication>
#include <QSlider>
#include <QTimer>
#include <QMouseEvent>
#include <QDebug>
#include <QString>
#include <QFile>
#include <QTextStream>

static QSlider* g_slider = nullptr;
static QFile* g_logFile = nullptr;

// 双通道输出：qDebug 之外同时落文件（Windows 下 qDebug 未必回到控制台，
// 落文件才能保证取证不丢）。
static void emitLine(const QString& line)
{
    qDebug().noquote() << line;
    if (g_logFile && g_logFile->isOpen()) {
        QTextStream ts(g_logFile);
        ts << line << "\n";
        ts.flush();
    }
}

static void log(const char* tag, int sigArg = -1)
{
    const QString pos = QString::number(g_slider->sliderPosition());
    const QString val = QString::number(g_slider->value());
    emitLine(QStringLiteral("[%1] sigArg=%2  value()=%3  sliderPosition()=%4")
                 .arg(QLatin1String(tag),
                      sigArg < 0 ? QStringLiteral("-") : QString::number(sigArg), val, pos));
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    QFile logFile(QStringLiteral("slidertest_result.txt"));
    logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    g_logFile = &logFile;
    emitLine(QStringLiteral("START (Qt %1)").arg(QString::fromLatin1(qVersion())));

    QSlider s(Qt::Horizontal);
    s.setRange(0, 100);
    s.setValue(100);
    s.setFixedWidth(260);
    s.setTracking(false);            // ← 与 DestopTools 透明度滑块同口径
    s.resize(260, 24);
    s.show();
    g_slider = &s;
    emitLine(QStringLiteral("slider geometry = %1x%2, value=%3")
                 .arg(s.width()).arg(s.height()).arg(s.value()));

    QObject::connect(&s, &QSlider::sliderPressed,  []() { log("sliderPressed"); });
    QObject::connect(&s, &QSlider::sliderMoved,    [](int v) { log("sliderMoved", v); });
    QObject::connect(&s, &QSlider::valueChanged,   [](int v) { log("valueChanged", v); });
    QObject::connect(&s, &QSlider::sliderReleased, []() { log("sliderReleased"); });

    auto drag = [&s](int fromX, int toX) {
        const int y = s.height() / 2;
        QMouseEvent press(QEvent::MouseButtonPress, QPoint(fromX, y), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&s, &press);
        QMouseEvent move(QEvent::MouseMove, QPoint(toX, y), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&s, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, QPoint(toX, y), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&s, &release);
    };

    QTimer::singleShot(200, [&]() {
        emitLine(QStringLiteral("==== 拖动 1（手柄起点 → 左端） ===="));
        drag(s.width() - 6, 20);
        emitLine(QStringLiteral("---- 抬起返回后（同一时刻）value()=%1 ----").arg(s.value()));

        QTimer::singleShot(300, [&]() {
            emitLine(QStringLiteral("==== 拖动 2（当前手柄 → 右端 ≈100%） ===="));
            drag(s.width() / 2, s.width() - 6);
            emitLine(QStringLiteral("---- 抬起返回后（同一时刻）value()=%1 ----").arg(s.value()));

            QTimer::singleShot(300, [&]() {
                emitLine(QStringLiteral("==== 事件循环跑完后的最终 value()=%1（这才是应落盘的值） ====")
                             .arg(s.value()));
                emitLine(QStringLiteral("END"));
                app.quit();
            });
        });
    });

    return app.exec();
}
