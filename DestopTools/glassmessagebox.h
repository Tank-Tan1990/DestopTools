/*
 * @file glassmessagebox.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef GLASSMESSAGEBOX_H
#define GLASSMESSAGEBOX_H

#include <QDialog>
#include <QLabel>
#include <QFrame>
#include <QRect>

class GlassMessageBox : public QDialog {
    Q_OBJECT
public:
    enum IconType {
        InfoIcon,
        WarningIcon
    };

    /**
     * @brief 显示一个无边框、玻璃风格的信息提示对话框（替代 QMessageBox::information）。
     */
    // information
    static int information(QWidget* parent, const QString& title, const QString& text);

    /**
     * @brief 显示一个无边框、玻璃风格的警告提示对话框（替代 QMessageBox::warning）。
     */
    // warning
    static int warning(QWidget* parent, const QString& title, const QString& text);

    /**
     * @brief 带自定义按钮的确认框（如「创建收纳盒」← 点击即执行该动作）。
     *        按钮文字用动词而非「OK」，让动作语义直接写在按钮上（与截图一致）。
     * @param okText     确定按钮文字，如「创建收纳盒」
     * @param cancelText 取消按钮文字；为空则只显示一个确定按钮
     * @param okIconName Theme::icon() 的图标名（如 "new_box"）；为空则不带图标
     * @param frameRect  非空时用该矩形（全局逻辑坐标）作为弹框的**位置与大小** —— 就地确认，
     *                   弹框正好落在用户刚画出的那个框上（小于卡片最小需求时退回 360×180，
     *                   并整体夹进该屏幕可用区）；为空则沿用 Qt 默认（居中于 parent / 屏幕）
     * @return QDialog::Accepted 表示用户点了确定按钮
     */
    // confirm
    static int confirm(QWidget* parent, const QString& title, const QString& text,
                       const QString& okText, const QString& cancelText = QString(),
                       const QString& okIconName = QString(), const QRect& frameRect = QRect());

protected:
    // 构造函数：初始化对象
    explicit GlassMessageBox(QWidget* parent, const QString& title, const QString& text,
                             IconType iconType = InfoIcon,
                             const QString& okText = QString(),
                             const QString& cancelText = QString(),
                             const QString& okIconName = QString());
    // 原生事件
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;
    // 鼠标按下事件
    void mousePressEvent(QMouseEvent* event) override;
    // 鼠标移动事件
    void mouseMoveEvent(QMouseEvent* event) override;
    // 鼠标松开事件
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    // 初始化ui
    void setupUi(const QString& title, const QString& text);
    // paint图标
    void paintIcon(IconType iconType);
    // 把弹框重新压到全部 band 窗口（收纳盒/桌面助手/Dock 图标区）之上、正常程序之下，
    // 并清掉 Qt 可能替我们指定的 owner（否则会随 owner 的周期性压底一起沉到桌面图标层之下）。
    // 与 GlassInputDialog（重命名/新建分类弹框）同源，幂等，可反复调用。
    void assertLayerAboveBandWindows();

    QFrame* m_titleBar = nullptr;
    QLabel* m_iconLabel = nullptr;
    IconType m_iconType = InfoIcon;
    // 按钮可定制（confirm() 用；为空时退回 information/warning 的单个「OK」）
    QString m_okText;
    QString m_cancelText;
    QString m_okIconName;
    bool m_dragging = false;
    QPoint m_dragPos;
};

#endif // GLASSMESSAGEBOX_H
