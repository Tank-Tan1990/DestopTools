/*
 * @file settingcenterdialog.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "settingcenterdialog.h"
#include "theme.h"
#include "thememanager.h"
#include "settingsmanager.h"
#include "regiondata.h"
#include "backupdialog.h"
#include "updatedialog.h"
#include "shortcutedit.h"
#include "quicktoolswidget.h"
#include "glassmessagebox.h"
#include "appexit.h"
#define NOMINMAX
#include <windows.h>
#include <QtWin>
#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QEvent>
#include <QDateTime>
#include <QGuiApplication>
#include <QFile>
#include <QFileInfo>
#include <QListWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QSlider>
#include <QLineEdit>
#include <QButtonGroup>
#include <QToolButton>
#include <QFileDialog>
#include <QMouseEvent>
#include <QFont>
#include <QColorDialog>
#include <QAbstractButton>
#include <QCheckBox>
#include <QGroupBox>
#include <QRadioButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <QSet>
#include <QMessageBox>
#include <QPainter>
#include <QImage>
#include <QBitmap>
#include <QPainterPath>
#include <QRegion>
#include <QGraphicsDropShadowEffect>
#include <QDir>
#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslError>
#include <QScreen>
#include <QTimer>
#include "desktopmirrorwindow.h"
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
    QPixmap createAboutLogo(int w, int h) {
        QPixmap pix(w, h);
        pix.fill(Qt::transparent);
        QPainter p(&pix);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);

        const int screenX = qRound(w * 0.15);
        const int screenY = qRound(h * 0.08);
        const int screenW = qRound(w * 0.70);
        const int screenH = qRound(h * 0.55);

        // 显示器外框（主题色弱描边玻璃）
        p.setBrush(Theme::windowBgColor());
        p.drawRoundedRect(screenX, screenY, screenW, screenH, 8, 8);
        QColor monitorBorder = ThemeManager::instance()->accentColor();
        monitorBorder.setAlpha(120);
        p.setPen(monitorBorder);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(screenX, screenY, screenW, screenH, 8, 8);

        // 屏幕：青蓝渐变
        QLinearGradient grad(screenX, screenY, screenX, screenY + screenH);
        grad.setColorAt(0, QColor("#1B3A6B"));
        grad.setColorAt(1, Theme::windowBgColor());
        p.setPen(Qt::NoPen);
        p.setBrush(grad);
        p.drawRoundedRect(screenX + 3, screenY + 3, screenW - 6, screenH - 6, 6, 6);

        // 屏幕内左侧小块（「谭」）—— 中文为方形字面，字号按小块短边取比例，保证不溢出
        const int docW = qRound(screenW * 0.26);
        const int docH = qRound(screenH * 0.48);
        const int docX = screenX + qRound(screenW * 0.16);
        const int docY = screenY + qRound(screenH * 0.20);
        p.setBrush(Theme::windowBgColor());
        p.drawRoundedRect(docX, docY, docW, docH, 4, 4);
        p.setPen(ThemeManager::instance()->accentColor());
        p.setFont(QFont(QString(), qMax(10, qRound(qMin(docW, docH) * 0.62)), QFont::Bold));
        p.drawText(QRect(docX, docY, docW, docH), Qt::AlignCenter, QStringLiteral("谭"));

        // 屏幕内右侧小块（「征」）
        p.setPen(Qt::NoPen);
        const int picW = qRound(screenW * 0.28);
        const int picH = qRound(screenH * 0.42);
        const int picX = screenX + qRound(screenW * 0.52);
        const int picY = screenY + qRound(screenH * 0.18);
        p.setBrush(QColor("#13406B"));
        p.drawRoundedRect(picX, picY, picW, picH, 4, 4);
        p.setPen(ThemeManager::instance()->accentColor().lighter(115));
        p.setFont(QFont(QString(), qMax(10, qRound(qMin(picW, picH) * 0.62)), QFont::Bold));
        p.drawText(QRect(picX, picY, picW, picH), Qt::AlignCenter, QStringLiteral("征"));

        // 支架（青色弱描边）
        p.setPen(Qt::NoPen);
        QColor standColor = ThemeManager::instance()->accentColor();
        standColor.setAlpha(60);
        p.setBrush(standColor);
        p.drawRoundedRect(qRound(w * 0.44), qRound(h * 0.66), qRound(w * 0.12), qRound(h * 0.10), 2, 2);

        // 底座
        p.drawRoundedRect(qRound(w * 0.28), qRound(h * 0.76), qRound(w * 0.44), qRound(h * 0.08), 4, 4);

        p.end();
        return pix;
    }

    // 设置/取消 Windows 开机自启动（写入/删除注册表 Run 键）
    // 仅在「目标状态 ≠ 注册表现状」时才写并 sync()。本函数处在 saveSettings() 的路径上，
    // 而 saveSettings 在高频交互中会被反复调用；无条件写注册表 + sync() 强刷（每次都要落盘
    // 并广播注册表变更）是明显的卡顿源，纯属重复劳动。
    void applyAutoStart(bool enabled) {
#ifdef Q_OS_WIN
        const QString runPath = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
        QSettings settings(runPath, QSettings::NativeFormat);
        const QString keyName = QStringLiteral("DestopTools");
        const QString want = enabled
            ? QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()))
            : QString();
        // 缺键 → QVariant 非法 → toString() 为空串，与 want 为空时相等，正好表示"本就未启用"
        if (settings.value(keyName).toString() == want) return;
        if (enabled) settings.setValue(keyName, want);
        else         settings.remove(keyName);
        settings.sync();
#endif
    }

    // 出厂色板：唯一字面量来源，不可变。仅供「恢复默认」还原用。
    const QStringList s_defaultColorNames = {
        QStringLiteral("#22D3EE"), // 青蓝（默认主题色，科技风主色）
        QStringLiteral("#3B82F6"),
        QStringLiteral("#8B5CF6"),
        QStringLiteral("#EC4899"),
        QStringLiteral("#F43F5E"),
        QStringLiteral("#F87171"),
        QStringLiteral("#FB923C"),
        QStringLiteral("#FBBF24"),
        QStringLiteral("#34D399"),
        QStringLiteral("#10B981"),
        QStringLiteral("#14B8A6"),
        QStringLiteral("#06B6D4"),
        QStringLiteral("#0EA5E9"),
        QStringLiteral("#6366F1"),
        QStringLiteral("#A78BFA"),
        QStringLiteral("#FFFFFF")
    };

    // 当前生效色板（**非 const**）：初值为出厂色板；用户右键任一色块可自定义该格颜色，
    // loadPalette() 用已保存的自定义色板覆盖它，onRestoreDefault() 还原为出厂值。
    // 索引即色板顺序；第 0 位是默认主题色（恢复默认与老配置回落都用它），格数恒为 16。
    QStringList s_colorNames = s_defaultColorNames;

}

SettingCenterDialog::SettingCenterDialog(Tab initialTab, QWidget* parent)
    : QDialog(parent, Qt::FramelessWindowHint | Qt::Window | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(820, 620);
    // 深空科技风：与「展示详情」设置中心截图完全一致
    // 16px 圆角玻璃对话框、纯白文字、线性青色描边按钮、选中项青边框微填充
    setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QDialog { background: transparent; border: none; }"
        "QLabel { color: #FFFFFF; background: transparent; }"
        "QListWidget { background-color: rgba(17,26,46,1.0); border: none; color: #FFFFFF; outline: none; padding: 0px; }"
        "QListWidget::item { padding: 12px 16px; border-radius: 10px; color: #FFFFFF; margin: 2px 6px; }"
        "QListWidget::item:selected { background: rgba(34,211,238,0.18); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.45); }"
        "QListWidget::item:hover { background-color: rgba(34,211,238,0.08); color: #FFFFFF; }"
        "QListWidget::item:selected:hover { background: rgba(34,211,238,0.22); }"
        "QComboBox { background-color: rgba(255,255,255,0.06); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.18); border-radius: 10px; padding: 5px 10px; min-width: 120px; }"
        "QComboBox:hover { background-color: rgba(34,211,238,0.12); border-color: rgba(34,211,238,0.35); }"
        "QComboBox:disabled { background-color: rgba(255,255,255,0.03); color: rgba(255,255,255,0.5); border-color: rgba(34,211,238,0.08); }"
        "QComboBox::drop-down { border: none; width: 24px; }"
        "QComboBox QAbstractItemView { background-color: rgba(17,26,46,1.0); color: #FFFFFF; selection-background-color: rgba(34,211,238,0.18); border: 1px solid rgba(34,211,238,0.18); border-radius: 0px; padding: 0px; outline: none; }"
        "QComboBox QAbstractItemView::item { color: #FFFFFF; border-radius: 0px; padding: 6px 10px; }"
        "QComboBox QAbstractItemView::item:hover { background-color: rgba(34,211,238,0.12); }"
        "QComboBox QAbstractItemView::item:selected { background-color: rgba(34,211,238,0.18); border: 1px solid rgba(34,211,238,0.35); }"
        "QComboBox QAbstractItemView QScrollBar:vertical { background: rgba(17,26,46,1.0); width: 8px; border-radius: 0px; margin: 0px; }"
        "QComboBox QAbstractItemView QScrollBar::handle:vertical { background: rgba(34,211,238,0.35); border-radius: 4px; min-height: 24px; }"
        "QComboBox QAbstractItemView QScrollBar::handle:vertical:hover { background: rgba(34,211,238,0.55); }"
        "QComboBox QAbstractItemView QScrollBar::add-line:vertical, QComboBox QAbstractItemView QScrollBar::sub-line:vertical { height: 0px; background: transparent; }"
        "QComboBox QAbstractItemView QScrollBar::add-page:vertical, QComboBox QAbstractItemView QScrollBar::sub-page:vertical { background: rgba(17,26,46,1.0); }"
        "QPushButton { color: #FFFFFF; background: rgba(255,255,255,0.04); border: 1px solid rgba(34,211,238,0.55); border-radius: 10px; padding: 8px 20px; }"
        "QPushButton:hover { background: rgba(34,211,238,0.12); border-color: #22D3EE; }"
        "QPushButton:pressed { background: rgba(34,211,238,0.22); border-color: #67E8F9; }"
        "QPushButton#plainButton { background-color: rgba(255,255,255,0.06); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.18); border-radius: 8px; padding: 5px 10px; }"
        "QPushButton#plainButton:hover { background-color: rgba(34,211,238,0.12); border-color: rgba(34,211,238,0.35); }"
        "QPushButton#plainButton:pressed { background-color: rgba(34,211,238,0.22); border-color: #67E8F9; }"
        "QLineEdit { background-color: rgba(255,255,255,0.06); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.18); border-radius: 12px; padding: 6px 12px; }"
        "QLineEdit:focus { border-color: #22D3EE; }"
        "QLineEdit::placeholder { color: rgba(255,255,255,0.5); }"
        "QSlider::groove:horizontal { height: 6px; background: rgba(255,255,255,0.10); border-radius: 3px; }"
        "QSlider::sub-page:horizontal { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #22D3EE,stop:1 #3B82F6); border-radius: 3px; }"
        "QSlider::handle:horizontal { width: 14px; height: 14px; margin: -4px 0; background: #FFFFFF; border-radius: 7px; }"
        "QGroupBox { color: #FFFFFF; border: 1px solid rgba(34,211,238,0.12); border-radius: 12px; margin-top: 8px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 6px; color: #FFFFFF; }"
    )));

    // 构造/加载期间控件会触发 textChanged/stateChanged，先置 m_isLoading 抑制
    // 这些「加载中」的保存，避免把默认值或中途状态写回磁盘；加载完成后再放开。
    m_isLoading = true;
    setupUi();
    loadSettings();
    m_isLoading = false;

    if (m_sidebar && initialTab >= 0 && initialTab < m_sidebar->count()) {
        m_sidebar->setCurrentRow(initialTab);
        m_stack->setCurrentIndex(initialTab);
    }

    // 关闭即落盘兜底：QDialog 的「确定 / 取消 / [X]」都走 done() → hide()，
    // **不会发 closeEvent**（closeEvent 只在窗口被 close() 时才有）——所以不能只靠 closeEvent 保存。
    // finished 覆盖全部关闭路径，确保「改完直接关」一定持久化；此时控件都还存活，读取安全。
    connect(this, &QDialog::finished, this, [this](int) { saveSettings(); });

    // IP 定位网络管理器（仅用于「自动定位」按钮）
    m_netManager = new QNetworkAccessManager(this);

    // 主题色/背景透明度联动：按已载入主题重绘本对话框样式。
    // 2026-09-21：透明度已并入**背景色** alpha（theme.h 的 bgAlphaF）→ 主题色与透明度
    // 两类变化都走 applyTheme()，不再有单独的「整窗不透明度」通道：setWindowOpacity 会把
    // 15px 圆角线框与全部文字一起淡化，与「圆角线框透明度不调整」的需求冲突。
    // 代价可控：滑块是 setTracking(false) 的 → 拖动期间零下发、松手才下发一次。
    ThemeManager::instance()->registerThemeTarget(this, [this]() { applyTheme(); });
    applyTheme();
}

// 对话框stylesheet
// 作者：谭征
QString SettingCenterDialog::dialogStyleSheet() const {
    // 对话框级样式（replace 在 Theme::applyTokens 中替换青蓝强调色令牌），
    // 与全局样式表共享同一套令牌替换逻辑，保证主题色一致。
    return Theme::applyTokens(QStringLiteral(
        "QDialog { background: transparent; border: none; }"
        "QLabel { color: #FFFFFF; background: transparent; }"
        "QListWidget { background-color: rgba(17,26,46,1.0); border: none; color: #FFFFFF; outline: none; padding: 0px; }"
        "QListWidget::item { padding: 12px 16px; border-radius: 10px; color: #FFFFFF; margin: 2px 6px; }"
        "QListWidget::item:selected { background: rgba(34,211,238,0.18); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.45); }"
        "QListWidget::item:hover { background-color: rgba(34,211,238,0.08); color: #FFFFFF; }"
        "QListWidget::item:selected:hover { background: rgba(34,211,238,0.22); }"
        "QComboBox { background-color: rgba(255,255,255,0.06); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.18); border-radius: 10px; padding: 5px 10px; min-width: 120px; }"
        "QComboBox:hover { background-color: rgba(34,211,238,0.12); border-color: rgba(34,211,238,0.35); }"
        "QComboBox:disabled { background-color: rgba(255,255,255,0.03); color: rgba(255,255,255,0.5); border-color: rgba(34,211,238,0.08); }"
        "QComboBox::drop-down { border: none; width: 24px; }"
        "QComboBox QAbstractItemView { background-color: rgba(17,26,46,1.0); color: #FFFFFF; selection-background-color: rgba(34,211,238,0.18); border: 1px solid rgba(34,211,238,0.18); border-radius: 0px; padding: 0px; outline: none; }"
        "QComboBox QAbstractItemView::item { color: #FFFFFF; border-radius: 0px; padding: 6px 10px; }"
        "QComboBox QAbstractItemView::item:hover { background-color: rgba(34,211,238,0.12); }"
        "QComboBox QAbstractItemView::item:selected { background-color: rgba(34,211,238,0.18); border: 1px solid rgba(34,211,238,0.35); }"
        "QComboBox QAbstractItemView QScrollBar:vertical { background: rgba(17,26,46,1.0); width: 8px; border-radius: 0px; margin: 0px; }"
        "QComboBox QAbstractItemView QScrollBar::handle:vertical { background: rgba(34,211,238,0.35); border-radius: 4px; min-height: 24px; }"
        "QComboBox QAbstractItemView QScrollBar::handle:vertical:hover { background: rgba(34,211,238,0.55); }"
        "QComboBox QAbstractItemView QScrollBar::add-line:vertical, QComboBox QAbstractItemView QScrollBar::sub-line:vertical { height: 0px; background: transparent; }"
        "QComboBox QAbstractItemView QScrollBar::add-page:vertical, QComboBox QAbstractItemView QScrollBar::sub-page:vertical { background: rgba(17,26,46,1.0); }"
        "QPushButton { color: #FFFFFF; background: rgba(255,255,255,0.04); border: 1px solid rgba(34,211,238,0.55); border-radius: 10px; padding: 8px 20px; }"
        "QPushButton:hover { background: rgba(34,211,238,0.12); border-color: #22D3EE; }"
        "QPushButton:pressed { background: rgba(34,211,238,0.22); border-color: #67E8F9; }"
        "QPushButton#plainButton { background-color: rgba(255,255,255,0.06); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.18); border-radius: 8px; padding: 5px 10px; }"
        "QPushButton#plainButton:hover { background-color: rgba(34,211,238,0.12); border-color: rgba(34,211,238,0.35); }"
        "QPushButton#plainButton:pressed { background-color: rgba(34,211,238,0.22); border-color: #67E8F9; }"
        "QLineEdit { background-color: rgba(255,255,255,0.06); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.18); border-radius: 12px; padding: 6px 12px; }"
        "QLineEdit:focus { border-color: #22D3EE; }"
        "QLineEdit::placeholder { color: rgba(255,255,255,0.5); }"
        "QSlider::groove:horizontal { height: 6px; background: rgba(255,255,255,0.10); border-radius: 3px; }"
        "QSlider::sub-page:horizontal { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #22D3EE,stop:1 #3B82F6); border-radius: 3px; }"
        "QSlider::handle:horizontal { width: 14px; height: 14px; margin: -4px 0; background: #FFFFFF; border-radius: 7px; }"
        "QGroupBox { color: #FFFFFF; border: 1px solid rgba(34,211,238,0.12); border-radius: 12px; margin-top: 8px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 6px; color: #FFFFFF; }"
    ));
}

// 主题色/透明度联动：重绘对话框自身硬编码青色样式，并同步窗口不透明度
// 作者：谭征
void SettingCenterDialog::applyTheme() {
    // 主题色/透明度联动：重绘对话框自身硬编码青色样式（令牌替换）并同步窗口不透明度。
    this->setStyleSheet(dialogStyleSheet());

    // 2026-09-22：这里必须补一次窗口遮罩重算 —— 「盒子使用圆角」开关的变化走
    // ThemeManager::reloadAppearanceFlags() → themeChanged → 本窗已登记的 onStyle(本函数)。
    // 遮罩半径由 Theme::radiusPx 决定，若只在这里重刷样式而不重算 mask，
    // 勾/取消该复选时窗口四角要等到下次 showEvent/resizeEvent 才更新（表现为"要重启才生效"）。
    updateRoundedMask();

    if (m_rootFrame) {
        // m_rootFrame 负责设置中心整体圆角外框：15px 圆角 + 主题背景 + 主题色边框。
        // 边框 1px、透明度 0.22 的青色描边；mask / paintEvent 仍 = 15px，保持真 15px
        // 锐利圆角、无透明环带（1px 边框内半落在 15px mask 内，不被裁）。
        QString rootStyle = Theme::applyTokens(QStringLiteral(
            "QFrame { background-color: WINDOW_BG_COLOR; border: 1px solid rgba(34,211,238,ROOT_BORDER_ALPHA); border-radius: 15px; }"
        ));
        rootStyle.replace(QStringLiteral("WINDOW_BG_COLOR"), Theme::windowBgString(1.0));
        rootStyle.replace(QStringLiteral("ROOT_BORDER_ALPHA"), QString::number(Theme::alphaF(0.22), 'f', 2));
        m_rootFrame->setStyleSheet(rootStyle);
    }
    if (m_titleBar) m_titleBar->setStyleSheet(Theme::applyTokens(m_titleBarStyle));
    if (m_stack) {
        QString stackStyle = Theme::applyTokens(QStringLiteral(
            "QStackedWidget { background-color: WINDOW_BG_COLOR; border-radius: 15px; }"
            "QStackedWidget > QWidget { background: transparent; }"
        ));
        stackStyle.replace(QStringLiteral("WINDOW_BG_COLOR"), Theme::windowBgString(1.0));
        m_stack->setStyleSheet(stackStyle);
        // 样式表更新后重新应用圆角裁剪，确保内部四角始终为圆角。
        updateStackMask();
    }
    if (m_bottomBar) {
        QString bbStyle = Theme::applyTokens(QStringLiteral(
            "QFrame { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 rgba(17,26,46,1.0),stop:1 rgba(17,26,46,1.0)); border-bottom-left-radius: 13px; border-bottom-right-radius: 13px; border-top: 1px solid rgba(34,211,238,BOTTOM_BAR_ALPHA); }"
        ));
        bbStyle.replace(QStringLiteral("BOTTOM_BAR_ALPHA"), QString::number(Theme::alphaF(0.12), 'f', 2));
        m_bottomBar->setStyleSheet(bbStyle);
    }
    if (m_appearancePage) m_appearancePage->setStyleSheet(Theme::applyTokens(m_appearancePageStyle));

    // 内部 accent 控件：重绘缓存的青色样式模板（令牌替换为当前主题色），确保对话框打开期间实时联动
    if (!m_weatherComboStyle.isEmpty()) {
        if (m_provinceCombo) m_provinceCombo->setStyleSheet(Theme::applyTokens(m_weatherComboStyle));
        if (m_cityCombo) m_cityCombo->setStyleSheet(Theme::applyTokens(m_weatherComboStyle));
        if (m_districtCombo) m_districtCombo->setStyleSheet(Theme::applyTokens(m_weatherComboStyle));
    }
    if (!m_weatherSwitchBtnStyle.isEmpty() && m_weatherSwitchBtn) m_weatherSwitchBtn->setStyleSheet(Theme::applyTokens(m_weatherSwitchBtnStyle));
    if (!m_weatherAutoLocateStyle.isEmpty() && m_autoLocateBtn) m_autoLocateBtn->setStyleSheet(Theme::applyTokens(m_weatherAutoLocateStyle));
    if (!m_ruleTableStyle.isEmpty() && m_ruleTable) {
        QString tableStyle = Theme::applyTokens(m_ruleTableStyle);
        tableStyle.replace(QStringLiteral("TABLE_BG_COLOR"), Theme::windowBgString(1.0));
        m_ruleTable->setStyleSheet(tableStyle);
    }

    // 桌面整理页单选按钮外矩形线框随主题色/透明度实时刷新
    if (!m_organizeRadioStyle.isEmpty()) {
        QString radioStyle = Theme::applyTokens(m_organizeRadioStyle);
        radioStyle.replace(QStringLiteral("BACKGROUND_COLOR"), Theme::windowBgString(0.10));
        radioStyle.replace(QStringLiteral("ALPHA_BORDER"), QString::number(Theme::alphaF(0.22), 'f', 2));
        if (m_ruleModeRadio) m_ruleModeRadio->setStyleSheet(radioStyle);
        if (m_fixedModeRadio) m_fixedModeRadio->setStyleSheet(radioStyle);
    }

    if (!m_toolsListStyle.isEmpty() && m_toolsList) m_toolsList->setStyleSheet(Theme::applyTokens(m_toolsListStyle));
    if (!m_toolActionBtnStyle.isEmpty()) {
        if (m_toolRestoreBtn) m_toolRestoreBtn->setStyleSheet(Theme::applyTokens(m_toolActionBtnStyle));
        if (m_toolAddBtn) m_toolAddBtn->setStyleSheet(Theme::applyTokens(m_toolActionBtnStyle));
        if (m_toolRemoveBtn) m_toolRemoveBtn->setStyleSheet(Theme::applyTokens(m_toolActionBtnStyle));
    }
    if (!m_agreementBtnStyle.isEmpty()) {
        if (m_agreementBtn) m_agreementBtn->setStyleSheet(Theme::applyTokens(m_agreementBtnStyle));
        if (m_privacyBtn) m_privacyBtn->setStyleSheet(Theme::applyTokens(m_agreementBtnStyle));
    }
    if (!m_userExperienceCheckStyle.isEmpty() && m_userExperienceCheck) m_userExperienceCheck->setStyleSheet(Theme::applyTokens(m_userExperienceCheckStyle));

    // 外观页：色块底色/描边跟随主题色与透明度实时刷新（自定义颜色后也走这里）
    refreshSwatchStyles();

    // 备份页自定义卡片：重绘青色模板（令牌替换为当前主题色），确保对话框打开期间实时联动
    restyleBackupCards();

    // 关于页 Logo：绘制成位图的图形不会自己随主题色变，主题色一变就得重建
    updateAboutLogo();

    // 快捷操作页复选框外矩形线框随主题色/透明度实时刷新
    if (!m_quickCheckStyle.isEmpty()) {
        QString quickStyle = Theme::applyTokens(m_quickCheckStyle);
        quickStyle.replace(QStringLiteral("BACKGROUND_COLOR"), Theme::windowBgString(0.10));
        quickStyle.replace(QStringLiteral("ALPHA_BORDER"), QString::number(Theme::alphaF(0.22), 'f', 2));
        if (m_hideIconsOnDoubleClickCheck) m_hideIconsOnDoubleClickCheck->setStyleSheet(quickStyle);
        if (m_drawBoxOnBlankCheck) m_drawBoxOnBlankCheck->setStyleSheet(quickStyle);
    }

    // 常规设置页复选框背景/矩形线框/勾选框随主题色/透明度实时刷新
    if (!m_generalCheckStyle.isEmpty()) {
        QString checkStyle = Theme::applyTokens(m_generalCheckStyle);
        checkStyle.replace(QStringLiteral("BACKGROUND_COLOR"), Theme::windowBgString(0.10));
        checkStyle.replace(QStringLiteral("ALPHA_BORDER"), QString::number(Theme::alphaF(0.22), 'f', 2));
        for (QCheckBox* check : {m_autoStartCheck, m_largeTimeFontCheck, m_showSecondsCheck,
                                  m_showMainWindowCheck, m_showCompletedCheck, m_showClockWeatherCheck,
                                  m_showWidgetsCheck, m_followStartCheck}) {
            if (check) check->setStyleSheet(checkStyle);
        }
    }

    this->setWindowOpacity(1.0);   //  显式保证整窗不透明度恒为 1（透明度只走背景色 alpha）

    // 主题色/透明度变化时触发重绘，确保 paintEvent 绘制的圆角背景 + 边框实时刷新。
    update();
}

// 关闭事件
// 作者：谭征
void SettingCenterDialog::closeEvent(QCloseEvent* event) {
    // 关闭对话框前，把当前控件状态强制保存并落盘，确保「刚改完就关」也能持久化，
    // 不依赖后续析构时机。m_isLoading 此时早已为 false，saveSettings 会正常执行。
    // 先把尚未下发的透明度补发一次（拖动中途直接关窗时，pending 值还没进过 ThemeManager）。
    flushTransparency();
    saveSettings();
    QDialog::closeEvent(event);
}

// 初始化ui
// 作者：谭征
void SettingCenterDialog::setupUi() {
    // 加载行政区划数据（省/市/区三级联动），供「天气区域设置」使用
    RegionData::load();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // 内容根容器：承担无边框窗口的可视化外框线（主题色 + 透明度联动）
    m_rootFrame = new QFrame(this);
    // 禁用 QFrame 默认的矩形面板绘制，完全由样式表负责圆角背景 + 边框，
    // 避免默认矩形覆盖样式表的圆角，导致四角出现直角。
    m_rootFrame->setFrameStyle(QFrame::NoFrame);
    auto* rootFrameLayout = new QVBoxLayout(m_rootFrame);
    rootFrameLayout->setContentsMargins(1, 1, 1, 1);
    rootFrameLayout->setSpacing(0);

    // 自定义标题栏
    m_titleBar = new QFrame(m_rootFrame);
    m_titleBar->setObjectName(QStringLiteral("titleBar"));
    m_titleBar->setFixedHeight(Theme::titleBarHeight());
    m_titleBarStyle = QStringLiteral(
        // 圆角 13px 而非 15px：标题栏被「外框 1px 描边 + rootFrameLayout 1px 内边距」
        // 共 2px 内缩，只有 15-2=13 才与 m_rootFrame 的 15px 外圆角**同心**。
        // 仍写成 15 的话，圆角处会多出一圈 2~3px 宽的月牙缝（外框底色露出），
        // 看着就是"外框和内容区圆角对不上"（用户 2026-09-22 反馈的那类不平齐）。
        "QFrame { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 rgba(17,26,46,1.0),stop:1 rgba(17,26,46,1.0)); border-top-left-radius: 13px; border-top-right-radius: 13px; border: none; }"
    );
    m_titleBar->setStyleSheet(Theme::applyTokens(m_titleBarStyle));
    auto* titleLayout = new QHBoxLayout(m_titleBar);
    titleLayout->setContentsMargins(16, 0, 12, 0);
    titleLayout->setSpacing(8);

    auto* iconLabel = new QLabel(m_titleBar);
    iconLabel->setPixmap(Theme::icon(QStringLiteral("settings")).pixmap(QSize(18, 18)));
    iconLabel->setStyleSheet(QStringLiteral("background: transparent;"));

    auto* titleLabel = new QLabel(QStringLiteral("设置中心"), m_titleBar);
    QFont tf = titleLabel->font();
    tf.setPointSize(12);
    tf.setBold(true);
    titleLabel->setFont(tf);
    titleLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent;"));

    auto* closeBtn = new QPushButton(m_titleBar);
    closeBtn->setIcon(Theme::icon(QStringLiteral("close")));
    closeBtn->setIconSize(QSize(16, 16));
    closeBtn->setFixedSize(28, 28);
    closeBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none; border-radius: 6px; }"
        "QPushButton:hover { background-color: rgba(248,113,113,0.18); }"
    ));
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);

    titleLayout->addWidget(iconLabel);
    titleLayout->addWidget(titleLabel);
    titleLayout->addStretch();
    titleLayout->addWidget(closeBtn);

    rootFrameLayout->addWidget(m_titleBar);

    // 主体：左侧边栏 + 右侧堆叠页
    auto* body = new QHBoxLayout();
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);

    m_sidebar = new QListWidget(m_rootFrame);
    m_sidebar->setFixedWidth(180);
    m_sidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setupSidebar();
    connect(m_sidebar, &QListWidget::currentRowChanged,
            this, &SettingCenterDialog::onSidebarCurrentRowChanged);

    m_stack = new QStackedWidget(m_rootFrame);
    // 右侧内容区使用主题背景色并加圆角，与外层主题线框的圆角保持一致，
    // 避免四个角出现矩形直角。
    {
        QString stackStyle = Theme::applyTokens(QStringLiteral(
            "QStackedWidget { background-color: WINDOW_BG_COLOR; border-radius: 15px; }"
            "QStackedWidget > QWidget { background: transparent; }"
        ));
        stackStyle.replace(QStringLiteral("WINDOW_BG_COLOR"), Theme::windowBgString(1.0));
        m_stack->setStyleSheet(stackStyle);
    }

    m_stack->addWidget(createGeneralSettingsPage());
    m_stack->addWidget(createDesktopOrganizePage());
    m_stack->addWidget(createAppearancePage());
    m_stack->addWidget(createDesktopBackupPage());
    m_stack->addWidget(createQuickActionsPage());
    m_stack->addWidget(createToolsPage());
    m_stack->addWidget(createAboutPage());

    body->addWidget(m_sidebar);
    body->addWidget(m_stack, 1);

    rootFrameLayout->addLayout(body, 1);

    // 底部操作栏：保留框架（用户要求只移除「+ 新建收纳盒」按钮，不动其它）
    m_bottomBar = new QFrame(m_rootFrame);
    m_bottomBar->setFixedHeight(56);
    m_bottomBar->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QFrame { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 rgba(17,26,46,1.0),stop:1 rgba(17,26,46,1.0)); border-bottom-left-radius: 13px; border-bottom-right-radius: 13px; border-top: 1px solid rgba(34,211,238,0.12); }"
    )));
    auto* bottomLayout = new QHBoxLayout(m_bottomBar);
    bottomLayout->setContentsMargins(16, 0, 16, 0);
    bottomLayout->setSpacing(12);

    bottomLayout->addStretch();

    rootFrameLayout->addWidget(m_bottomBar);

    root->addWidget(m_rootFrame);

    // 用圆角矩形裁剪整个窗口，让四角真正呈现圆角（无边框 + 透明背景的窗口
    // 默认仍是直角矩形，仅样式表 border-radius 不会裁剪原生窗口）。
    updateRoundedMask();
    // 右侧内容区同样用 QRegion 裁剪为圆角，避免子控件覆盖其 border-radius 区域。
    updateStackMask();
}

// 显示事件
// 作者：谭征
void SettingCenterDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // 窗口显示后再应用一次圆角裁剪，确保原生窗口创建后 mask 生效。
    updateRoundedMask();
    // 内容区在显示后尺寸才最终确定，此时应用圆角裁剪。
    updateStackMask();

#ifdef Q_OS_WIN
    // 居中到 Windows 桌面（主屏幕可用区域）
    QScreen *scr = QApplication::primaryScreen();
    if (scr) {
        QRect ag = scr->availableGeometry();
        move(ag.x() + (ag.width() - width()) / 2,
             ag.y() + (ag.height() - height()) / 2);
    }

    // z 序：设置中心 > 收纳盒/桌面助手（全部 band 窗口），但 < 其它任意程序
    // 延迟到事件循环末尾执行（QTimer::singleShot 0ms），避免 Qt 在 showEvent 之后
    // 因 parent 链（SidePanelWidget→AssistantWindow）自动调整 z 序而覆盖我们的插入。
    // 遍历所有非 Dock 的 band 窗口，逐个将设置中心插到其上，
    // 最终落在全部 band 窗口之上；不使用 HWND_TOP/TOPMOST，因此不遮挡其它正常程序。
    QTimer::singleShot(0, this, [this]() {
#ifdef Q_OS_WIN
        DesktopMirrorWindow::raiseAboveBandWindows((HWND)winId());
#endif
    });
#endif
}

// 助手面板顶到普通程序之上再被压回的「一闪而过」。show desktop 期放行。
// 作者：谭征
bool SettingCenterDialog::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    MSG* msg = reinterpret_cast<MSG*>(message);
    // WM_WINDOWPOSCHANGING 是同步发送消息、不走消息队列，必须在本窗口 nativeEvent 拦截
    // （全局 QAbstractNativeEventFilter 看不到）。与收纳盒/桌面助手共用同一守卫：
    // 冻结任何「顶层提层」（HWND_TOP/TOPMOST/NOTOPMOST/外部程序参照），使设置中心
    // 保持当前 band 内位置——即落在全部 band 窗口之上、所有正常程序之下。
    if (msg && msg->message == WM_WINDOWPOSCHANGING) {
        WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(msg->lParam);
        // 文件选择框打开期间：本窗是其 owner，系统需要把 owner（本窗）排到文件框之下，
        // 才能保证文件框在本窗之上。此时若仍冻结本窗 z 序（clampBandZOrder 会加
        // SWP_NOZORDER），owner 拒绝下移 → 排序错乱 → 文件框反被本窗遮住。
        // 故此期间跳过冻结；仅保留"不抢焦点"，把 z 序交还系统按 owner 关系正常处理。
        if (!m_suppressZOrderClamp) {
            DesktopMirrorWindow::clampBandZOrder(msg->hwnd, wp);
        }
        wp->flags |= SWP_NOACTIVATE;   // 不抢焦点
        return false;                  // 走默认处理链，改写已生效
    }
#endif
    return QDialog::nativeEvent(eventType, message, result);
}

static QRegion roundedRectMask(const QRect& rect, int radius)
{
    // 用 1-bit QBitmap 绘制抗锯齿圆角矩形再转 QRegion。
    // 比 QRegion::Ellipse 的矩形近似更精确，mask 边界能紧贴真实圆角，
    // 从而用更小的余量彻底解决圆角被截断的问题。
    if (rect.width() <= 0 || rect.height() <= 0) return QRegion();

    QBitmap bmp(rect.size());
    bmp.fill(Qt::color0);                       // 黑色 = 被裁掉
    QPainter p(&bmp);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::color1);                       // 白色 = 可见
    p.drawRoundedRect(QRectF(0, 0, rect.width(), rect.height()), radius, radius);
    p.end();

    return QRegion(bmp);
}

// 为彻底消除 Windows 下无边框透明窗口的四角直角，改用 paintEvent + setMask 方案。
// 作者：谭征
void SettingCenterDialog::updateRoundedMask() {
    // 彻底清晰方案：窗口 mask 半径直接 = m_rootFrame 的视觉圆角 15px（与 paintEvent 填充
    // 半径、QSS border-radius 三者完全一致）。边界完全重合，无任何延伸环带，圆角最清晰。
    // 用 QBitmap 精确生成的 mask（非 QRegion::Ellipse 整数近似），15px 不会被硬裁。
    // 半径必须走 Theme::radiusPx（= 跟随「盒子使用圆角」开关），**不能写死 15**：
    // 该开关已在 applyTokens 里把 QSS 的 border-radius 全压成 0px，若这里仍按 15px 裁，
    // 就会出现「线框是方角、遮罩是圆角」→ 遮罩把方角线框的四角切出缺口，
    // 视觉上就是"外框圆角 / 内容区方角豁口"两态打架（2026-09-22 设置中心实测）。
    // 取消勾选时半径 0 → clearMask()，整窗就是原生矩形，与方角线框完全一致。
    const int r = Theme::radiusPx(15);
    const QRect rect = this->rect();
    if (rect.width() <= 0 || rect.height() <= 0) return;

    clearMask();
    if (r <= 0) return;      // 方角：不裁，保持原生矩形窗口
    setMask(roundedRectMask(rect, r));
}

// 把堆叠页裁剪为圆角矩形，让内部四角与外层圆角风格保持一致。
// 作者：谭征
void SettingCenterDialog::updateStackMask() {
    if (!m_stack) return;
    // 与窗口外框一致：内容区 mask 半径直接 = m_stack 的视觉圆角 15px，三者完全对齐，
    // 内容区四角圆角最清晰、无延伸环带。
    // 同样必须走 Theme::radiusPx：m_stack（QStackedWidget 继承 QFrame）会从 m_rootFrame
    // 的样式串里继承到那圈 1px 主题色线框，方角态下它是**直角**线框 —— 若这里仍按 15px 裁，
    // 四角就被切出缺口（正是用户看到的"圆角/方角不一致"）。方角态直接 clearMask()。
    const int r = Theme::radiusPx(15);
    const QRect rect = m_stack->rect();
    if (r <= 0 || rect.width() <= 2 * r || rect.height() <= 2 * r) {
        m_stack->clearMask();
        return;
    }

    m_stack->setMask(roundedRectMask(rect, r));
}

// 缩放事件
// 作者：谭征
void SettingCenterDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    // 窗口大小变化后重新应用圆角裁剪，保证 mask 始终覆盖当前窗口区域。
    updateRoundedMask();
    // 内容区 m_stack 也要同步更新圆角裁剪，防止大小变化后内部四角被拉成直角。
    updateStackMask();
}

// paint事件
// 作者：谭征
void SettingCenterDialog::paintEvent(QPaintEvent* event) {
    QDialog::paintEvent(event);
    // 彻底清晰方案：paintEvent 填充半径 = 窗口 mask 半径 = m_rootFrame 视觉圆角（三者同为
    // 15px），边界完全重合，无任何延伸环带；填充色与 m_rootFrame 一致的不透明主题底色。
    // 这样圆角曲线即视觉圆角曲线，最锐利清晰；m_rootFrame 的 1px 主题边框沿 15px 圆角曲线
    // 绘制，落在 mask 边界内，清晰可见。
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // 填充半径同样必须走 Theme::radiusPx（跟随「盒子使用圆角」开关）：本绘制是窗口
    // 底色的唯一来源，若在方角态仍按 15px 圆角填，四角会露出后面的壁纸/桌面，
    // 而 QSS 线框已是方角 → 又一处两态打架。半径 0 时 drawRoundedRect 等价于直角矩形。
    const int fillRadius = Theme::radiusPx(15);
    const QRectF panelRect = QRectF(rect());

    QColor bg = Theme::applyBgAlpha(Theme::windowBgColor());
    // alpha 必须走 applyBgAlpha（= 全局背景透明度），与 m_rootFrame 的 windowBgString(1.0)
    // 保持**同一个值**：写成 setAlpha(255) 会让设置中心自己这一个窗口永远不跟随透明度滑块
    // （其它窗口都跟随 → 观感不一致），而滑块恰好就在本窗里，最容易被看见。
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(panelRect, fillRadius, fillRadius);
}

// 初始化sidebar
// 作者：谭征
void SettingCenterDialog::setupSidebar() {
    const QStringList items = {
        QStringLiteral("常规设置"),
        QStringLiteral("桌面整理"),
        QStringLiteral("外观设置"),
        QStringLiteral("桌面备份"),
        QStringLiteral("快捷操作"),
        QStringLiteral("小工具"),
        QStringLiteral("关于我们")
    };
    for (const QString& item : items) {
        m_sidebar->addItem(item);
    }
}

// 创建general设置页
// 作者：谭征
QWidget* SettingCenterDialog::createGeneralSettingsPage() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(18);

    // 标题
    auto* titleLabel = new QLabel(QStringLiteral("常规设置"));
    QFont tf = titleLabel->font();
    tf.setPointSize(14);
    tf.setBold(true);
    titleLabel->setFont(tf);
    titleLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent; border: none;"));
    layout->addWidget(titleLabel);

    // 基本设置：按截图使用网格式排列，短项可并排，长项独占一行
    auto* basicLayout = new QGridLayout();
    basicLayout->setSpacing(14);
    basicLayout->setContentsMargins(0, 0, 0, 0);
    basicLayout->setColumnStretch(0, 1);
    basicLayout->setColumnStretch(1, 1);

    m_autoStartCheck = new QCheckBox(QStringLiteral("开机启动桌面助手"));
    m_largeTimeFontCheck = new QCheckBox(QStringLiteral("时间大字体显示"));
    m_showSecondsCheck = new QCheckBox(QStringLiteral("时间显示秒"));
    m_showMainWindowCheck = new QCheckBox(QStringLiteral("显示主界面/隐藏主界面"));
    m_showCompletedCheck = new QCheckBox(QStringLiteral("显示已完成事项（主界面）"));
    m_showClockWeatherCheck = new QCheckBox(QStringLiteral("显示时间和天气模块"));
    m_showWidgetsCheck = new QCheckBox(QStringLiteral("显示实用挂件"));
    m_followStartCheck = new QCheckBox(QStringLiteral("组件库跟随桌面助手启动"));

    // 复选框统一背景+边框样式模板：背景使用随主题色派生的窗口背景色，
    // 矩形线框使用主题强调色，构造时应用一次，applyTheme 中随主题色/透明度实时刷新。
    m_generalCheckStyle = QStringLiteral(
        "QCheckBox { color: #FFFFFF; background-color: BACKGROUND_COLOR; border: 1px solid rgba(34,211,238,ALPHA_BORDER); border-radius: 6px; padding: 6px 8px; spacing: 8px; }"
        "QCheckBox::indicator { width: 18px; height: 18px; border-radius: 4px; border: 1px solid rgba(34,211,238,0.30); background: transparent; }"
        "QCheckBox::indicator:checked { background: #22D3EE; border-color: #22D3EE; image: url(:/icons/check_plain_1x.png); }"
    );
    auto applyGeneralCheckStyle = [this](QCheckBox* check) {
        if (!check) return;
        QString s = Theme::applyTokens(m_generalCheckStyle);
        s.replace(QStringLiteral("BACKGROUND_COLOR"), Theme::windowBgString(0.10));
        s.replace(QStringLiteral("ALPHA_BORDER"), QString::number(Theme::alphaF(0.22), 'f', 2));
        check->setStyleSheet(s);
    };
    for (QCheckBox* check : {m_autoStartCheck, m_largeTimeFontCheck, m_showSecondsCheck,
                             m_showMainWindowCheck, m_showCompletedCheck, m_showClockWeatherCheck,
                             m_showWidgetsCheck, m_followStartCheck}) {
        applyGeneralCheckStyle(check);
    }

    int row = 0;
    basicLayout->addWidget(m_autoStartCheck, row, 0, 1, 2); ++row;
    // 显示时间和天气模块紧随「开机启动桌面助手」（用户指定：天气项移到时间项之前）
    basicLayout->addWidget(m_showClockWeatherCheck, row, 0, 1, 2); ++row;
    basicLayout->addWidget(m_largeTimeFontCheck, row, 0);
    basicLayout->addWidget(m_showSecondsCheck, row, 1); ++row;
    basicLayout->addWidget(m_showCompletedCheck, row, 0, 1, 2); ++row;
    // 显示主界面移到「显示实用挂件」之后（用户指定）
    basicLayout->addWidget(m_showWidgetsCheck, row, 0, 1, 2); ++row;
    basicLayout->addWidget(m_showMainWindowCheck, row, 0, 1, 2); ++row;
    basicLayout->addWidget(m_followStartCheck, row, 0, 1, 2);

    layout->addLayout(basicLayout);

    // 连接复选框变化
    connect(m_autoStartCheck, &QCheckBox::stateChanged, this, &SettingCenterDialog::onGeneralCheckChanged);
    connect(m_largeTimeFontCheck, &QCheckBox::stateChanged, this, &SettingCenterDialog::onGeneralCheckChanged);
    connect(m_showSecondsCheck, &QCheckBox::stateChanged, this, &SettingCenterDialog::onGeneralCheckChanged);
    connect(m_showMainWindowCheck, &QCheckBox::stateChanged, this, &SettingCenterDialog::onGeneralCheckChanged);
    connect(m_showCompletedCheck, &QCheckBox::stateChanged, this, &SettingCenterDialog::onGeneralCheckChanged);
    connect(m_showClockWeatherCheck, &QCheckBox::stateChanged, this, &SettingCenterDialog::onGeneralCheckChanged);
    connect(m_showWidgetsCheck, &QCheckBox::stateChanged, this, &SettingCenterDialog::onGeneralCheckChanged);
    connect(m_followStartCheck, &QCheckBox::stateChanged, this, &SettingCenterDialog::onGeneralCheckChanged);

    // 天气区域设置：按截图使用普通标签 + 水平布局，不再使用分组框
    layout->addSpacing(20);
    auto* weatherTitle = new QLabel(QStringLiteral("天气区域设置"));
    weatherTitle->setStyleSheet(QStringLiteral("color: #FFFFFF; font-weight: bold; background: transparent; border: none;"));
    layout->addWidget(weatherTitle);

    // 天气区域控件分两行布局，避免窗口宽度不足时所有控件被挤压折叠
    auto* weatherContainer = new QVBoxLayout();
    weatherContainer->setSpacing(10);
    weatherContainer->setContentsMargins(0, 0, 0, 0);

    auto* comboRow = new QHBoxLayout();
    comboRow->setSpacing(12);
    comboRow->setContentsMargins(0, 0, 0, 0);

    auto* btnRow = new QHBoxLayout();
    btnRow->setSpacing(12);
    btnRow->setContentsMargins(0, 0, 0, 0);

    m_provinceCombo = new QComboBox();
    m_provinceCombo->setMinimumWidth(120);
    m_provinceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QStringList provinces = RegionData::provinces();
    if (provinces.isEmpty()) {
        // JSON 未加载时的兜底，保证仍能显示基础选项
        provinces << QStringLiteral("陕西") << QStringLiteral("北京")
                  << QStringLiteral("上海") << QStringLiteral("广东");
    }
    m_provinceCombo->addItems(provinces);

    m_cityCombo = new QComboBox();
    m_cityCombo->setMinimumWidth(120);
    m_cityCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_districtCombo = new QComboBox();
    m_districtCombo->setMinimumWidth(120);
    m_districtCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // 下拉框样式：与「切换区域」按钮保持同色调；存储青色模板，构造与主题联动均走 applyTokens
    m_weatherComboStyle = QStringLiteral(
        "QComboBox { background-color: rgba(255,255,255,0.06); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.18); border-radius: 10px; padding: 5px 10px; }"
        "QComboBox:hover { background-color: rgba(34,211,238,0.12); border-color: rgba(34,211,238,0.35); }"
        "QComboBox::drop-down { border: none; width: 24px; }"
        "QComboBox QAbstractItemView { background-color: rgba(17,26,46,1.0); color: #FFFFFF; selection-background-color: rgba(34,211,238,0.18); border: 1px solid rgba(34,211,238,0.18); border-radius: 0px; padding: 0px; outline: none; }"
        "QComboBox QAbstractItemView::item { color: #FFFFFF; border-radius: 0px; padding: 6px 10px; }"
        "QComboBox QAbstractItemView::item:hover { background-color: rgba(34,211,238,0.12); }"
        "QComboBox QAbstractItemView::item:selected { background-color: rgba(34,211,238,0.18); border: 1px solid rgba(34,211,238,0.35); }"
        "QComboBox QAbstractItemView QScrollBar:vertical { background: rgba(17,26,46,1.0); width: 8px; border-radius: 0px; margin: 0px; }"
        "QComboBox QAbstractItemView QScrollBar::handle:vertical { background: rgba(34,211,238,0.35); border-radius: 4px; min-height: 24px; }"
        "QComboBox QAbstractItemView QScrollBar::handle:vertical:hover { background: rgba(34,211,238,0.55); }"
        "QComboBox QAbstractItemView QScrollBar::add-line:vertical, QComboBox QAbstractItemView QScrollBar::sub-line:vertical { height: 0px; background: transparent; }"
        "QComboBox QAbstractItemView QScrollBar::add-page:vertical, QComboBox QAbstractItemView QScrollBar::sub-page:vertical { background: rgba(17,26,46,1.0); }"
    );
    m_provinceCombo->setStyleSheet(Theme::applyTokens(m_weatherComboStyle));
    m_cityCombo->setStyleSheet(Theme::applyTokens(m_weatherComboStyle));
    m_districtCombo->setStyleSheet(Theme::applyTokens(m_weatherComboStyle));

    // 初始填充默认区域（陕西 -> 西安 -> 新城），loadGeneralSettings 会按保存值覆盖
    const QString defProvince = QStringLiteral("陕西");
    refreshCityCombo(defProvince);
    if (m_cityCombo->count() == 0) {
        m_cityCombo->addItem(QStringLiteral("西安"));
    }
    m_cityCombo->setCurrentIndex(0);
    refreshDistrictCombo(defProvince, m_cityCombo->currentText());
    if (m_districtCombo->count() == 0) {
        m_districtCombo->addItem(QStringLiteral("新城"));
    }
    m_districtCombo->setCurrentIndex(0);

    connect(m_provinceCombo, QOverload<const QString&>::of(&QComboBox::currentTextChanged),
            this, &SettingCenterDialog::onProvinceChanged);
    connect(m_cityCombo, QOverload<const QString&>::of(&QComboBox::currentTextChanged),
            this, &SettingCenterDialog::onCityChanged);

    m_weatherSwitchBtnStyle = QStringLiteral(
        "QPushButton#weatherSwitchBtn { background-color: rgba(255,255,255,0.06); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.18); border-radius: 8px; padding: 5px 10px; }"
        "QPushButton#weatherSwitchBtn:hover { background-color: rgba(34,211,238,0.12); border-color: rgba(34,211,238,0.35); }"
        "QPushButton#weatherSwitchBtn:pressed { background-color: rgba(34,211,238,0.22); border-color: #67E8F9; }"
    );
    auto* switchBtn = new QPushButton(QStringLiteral("切换区域"));
    switchBtn->setObjectName(QStringLiteral("weatherSwitchBtn"));
    switchBtn->setFixedWidth(100);
    switchBtn->setStyleSheet(Theme::applyTokens(m_weatherSwitchBtnStyle));
    m_weatherSwitchBtn = switchBtn;

    m_weatherAutoLocateStyle = QStringLiteral(
        "QPushButton#weatherAutoLocateBtn { background-color: rgba(255,255,255,0.06); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.18); border-radius: 8px; padding: 5px 10px; }"
        "QPushButton#weatherAutoLocateBtn:hover { background-color: rgba(34,211,238,0.12); border-color: rgba(34,211,238,0.35); }"
        "QPushButton#weatherAutoLocateBtn:pressed { background-color: rgba(34,211,238,0.22); border-color: #67E8F9; }"
        "QPushButton#weatherAutoLocateBtn:disabled { color: rgba(255,255,255,0.4); border-color: rgba(34,211,238, 0.08); }"
    );
    m_autoLocateBtn = new QPushButton(QStringLiteral("自动定位"));
    m_autoLocateBtn->setObjectName(QStringLiteral("weatherAutoLocateBtn"));
    m_autoLocateBtn->setFixedWidth(100);
    m_autoLocateBtn->setToolTip(QStringLiteral("根据当前 IP 自动识别省/市/区"));
    m_autoLocateBtn->setStyleSheet(Theme::applyTokens(m_weatherAutoLocateStyle));
    connect(m_autoLocateBtn, &QPushButton::clicked, this, &SettingCenterDialog::onAutoLocateByIp);

    comboRow->addWidget(m_provinceCombo);
    comboRow->addWidget(m_cityCombo);
    comboRow->addWidget(m_districtCombo);
    comboRow->addStretch();

    btnRow->addWidget(switchBtn);
    btnRow->addWidget(m_autoLocateBtn);
    btnRow->addStretch();

    weatherContainer->addLayout(comboRow);
    weatherContainer->addLayout(btnRow);

    layout->addLayout(weatherContainer);
    layout->addStretch();

    return page;
}

// 创建桌面organize页
// 作者：谭征
QWidget* SettingCenterDialog::createDesktopOrganizePage() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(18);

    // 整块区域进一步统一：单选按钮去除默认深色背景、文字白色；
    // 禁用态下拉框使用与主背景一致的弱玻璃态，避免突兀深色块。
    // 单选按钮 / 下拉框样式交由全局主题样式表（Theme::globalStyleSheet，已在 themeChanged 时实时重设）统一提供，
    // 不再硬编码青色，确保「改主题色 → 桌面整理页全部控件实时联动」。

    // 标题
    auto* titleLabel = new QLabel(QStringLiteral("桌面整理"));
    QFont tf = titleLabel->font();
    tf.setPointSize(14);
    tf.setBold(true);
    titleLabel->setFont(tf);
    titleLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent; border: none;"));
    layout->addWidget(titleLabel);

    // 整理模式
    auto* modeLayout = new QVBoxLayout();
    modeLayout->setSpacing(10);
    modeLayout->setContentsMargins(0, 0, 0, 0);

    auto* modeRow1 = new QHBoxLayout();
    modeRow1->setSpacing(8);
    m_ruleModeRadio = new QRadioButton(QStringLiteral("按照【整理规则】，整理到各个分区"));
    m_ruleModeRadio->setChecked(true);
    modeRow1->addWidget(m_ruleModeRadio);
    modeRow1->addStretch();

    auto* modeRow2 = new QHBoxLayout();
    modeRow2->setSpacing(8);
    m_fixedModeRadio = new QRadioButton(QStringLiteral("整理到固定分区"));
    m_fixedPartitionCombo = new QComboBox();
    m_fixedPartitionCombo->addItem(QStringLiteral("目录"));
    m_fixedPartitionCombo->setEnabled(false);
    modeRow2->addWidget(m_fixedModeRadio);
    modeRow2->addWidget(m_fixedPartitionCombo);
    modeRow2->addStretch();

    modeLayout->addLayout(modeRow1);
    modeLayout->addLayout(modeRow2);
    layout->addLayout(modeLayout);

    // 单选按钮外矩形线框样式模板：背景/边框均随主题色和全局透明度实时联动。
    m_organizeRadioStyle = QStringLiteral(
        "QRadioButton { color: #FFFFFF; spacing: 6px; background-color: BACKGROUND_COLOR; border: 1px solid rgba(34,211,238,ALPHA_BORDER); border-radius: 8px; padding: 6px 10px; }"
        "QRadioButton::indicator { width: 16px; height: 16px; border: 1px solid rgba(34,211,238,0.40); border-radius: 8px; background: rgba(17,26,46,1.0); }"
        "QRadioButton::indicator:checked { background: rgba(34,211,238,0.35); border: 2px solid #22D3EE; }"
    );
    auto applyOrganizeRadioStyle = [this](QRadioButton* radio) {
        if (!radio) return;
        QString s = Theme::applyTokens(m_organizeRadioStyle);
        s.replace(QStringLiteral("BACKGROUND_COLOR"), Theme::windowBgString(0.10));
        s.replace(QStringLiteral("ALPHA_BORDER"), QString::number(Theme::alphaF(0.22), 'f', 2));
        radio->setStyleSheet(s);
    };
    applyOrganizeRadioStyle(m_ruleModeRadio);
    applyOrganizeRadioStyle(m_fixedModeRadio);

    connect(m_ruleModeRadio, &QRadioButton::toggled, this, [this](bool checked) {
        if (m_fixedPartitionCombo) m_fixedPartitionCombo->setEnabled(!checked);
        saveSettings();
    });
    connect(m_fixedModeRadio, &QRadioButton::toggled, this, [this](bool) {
        if (m_fixedPartitionCombo) m_fixedPartitionCombo->setEnabled(m_fixedModeRadio->isChecked());
        saveSettings();
    });
    if (m_fixedPartitionCombo) {
        connect(m_fixedPartitionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &SettingCenterDialog::saveSettings);
    }

    // 规则表
    auto* tableLabel = new QLabel(QStringLiteral("整理规则"));
    tableLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; font-weight: bold; background: transparent; border: none;"));
    layout->addWidget(tableLabel);

    m_ruleTable = new QTableWidget(9, 4);
    m_ruleTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_ruleTable->setFocusPolicy(Qt::NoFocus);
    m_ruleTable->setShowGrid(false);
    m_ruleTable->verticalHeader()->setVisible(false);
    m_ruleTable->horizontalHeader()->setVisible(true);
    m_ruleTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_ruleTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_ruleTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_ruleTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    m_ruleTable->setColumnWidth(2, 110);
    m_ruleTable->setColumnWidth(3, 90);
    // 规则表样式模板：背景色使用 TABLE_BG_COLOR 占位符，构造与 applyTheme 中均替换为
    // 随主题色派生的窗口背景色，确保表格背景实时跟随主题色/透明度联动。
    m_ruleTableStyle = QStringLiteral(
        "QTableWidget { background-color: TABLE_BG_COLOR; border: 1px solid rgba(34,211,238,0.18); gridline-color: transparent; color: #FFFFFF; }"
        "QHeaderView::section { background-color: TABLE_BG_COLOR; color: #FFFFFF; padding: 6px; border: none; border-bottom: 1px solid rgba(34,211,238,0.18); }"
        "QTableWidget::item { background: transparent; border-bottom: 1px solid rgba(255,255,255,0.06); padding: 4px; color: #FFFFFF; }"
        "QTableWidget::item:selected { background: transparent; }"
        "QComboBox { background-color: rgba(255,255,255,0.06); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.18); border-radius: 6px; padding: 2px 6px; }"
        "QComboBox:hover { background-color: rgba(34,211,238,0.12); border-color: rgba(34,211,238,0.35); }"
        "QComboBox:disabled { background-color: rgba(255,255,255,0.03); color: rgba(255,255,255,0.5); border-color: rgba(34,211,238,0.08); }"
        "QComboBox::drop-down { border: none; width: 20px; }"
        "QComboBox QAbstractItemView { background-color: TABLE_BG_COLOR; color: #FFFFFF; selection-background-color: rgba(34,211,238,0.18); border: 1px solid rgba(34,211,238,0.18); border-radius: 0px; padding: 0px; outline: none; }"
        "QComboBox QAbstractItemView::item { color: #FFFFFF; border-radius: 0px; padding: 6px 10px; }"
        "QComboBox QAbstractItemView::item:hover { background-color: rgba(34,211,238,0.12); }"
        "QComboBox QAbstractItemView::item:selected { background-color: rgba(34,211,238,0.18); border: 1px solid rgba(34,211,238,0.35); }"
        "QComboBox QAbstractItemView QScrollBar:vertical { background: TABLE_BG_COLOR; width: 8px; border-radius: 0px; margin: 0px; }"
        "QComboBox QAbstractItemView QScrollBar::handle:vertical { background: rgba(34,211,238,0.35); border-radius: 4px; min-height: 24px; }"
        "QComboBox QAbstractItemView QScrollBar::handle:vertical:hover { background: rgba(34,211,238,0.55); }"
        "QComboBox QAbstractItemView QScrollBar::add-line:vertical, QComboBox QAbstractItemView QScrollBar::sub-line:vertical { height: 0px; background: transparent; }"
        "QComboBox QAbstractItemView QScrollBar::add-page:vertical, QComboBox QAbstractItemView QScrollBar::sub-page:vertical { background: TABLE_BG_COLOR; }"
        "QCheckBox { color: #FFFFFF; spacing: 8px; background: transparent; border: none; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid rgba(34,211,238,0.30); border-radius: 3px; background: transparent; }"
        "QCheckBox::indicator:hover { border-color: #22D3EE; }"
        "QCheckBox::indicator:checked { background: transparent; border: 1px solid rgba(34,211,238,0.30); image: url(:/icons/check_plain_1x.png); }"
        "QCheckBox::indicator:checked:hover { border-color: #22D3EE; }"
    );
    {
        QString tableStyle = Theme::applyTokens(m_ruleTableStyle);
        tableStyle.replace(QStringLiteral("TABLE_BG_COLOR"), Theme::windowBgString(1.0));
        m_ruleTable->setStyleSheet(tableStyle);
    }

    const QStringList partitions = {
        QStringLiteral("目录"), QStringLiteral("文档"), QStringLiteral("压缩"),
        QStringLiteral("图片"), QStringLiteral("快捷方式"), QStringLiteral("网址"),
        QStringLiteral("视频"), QStringLiteral("音频"), QStringLiteral("其它")
    };

    const QStringList typeNames = {
        QStringLiteral("目录"), QStringLiteral("文档"), QStringLiteral("压缩"),
        QStringLiteral("图片"), QStringLiteral("快捷方式"), QStringLiteral("网址"),
        QStringLiteral("视频"), QStringLiteral("音频"), QStringLiteral("其它")
    };
    const QStringList extensions = {
        QStringLiteral("--"),
        QStringLiteral(".doc|.docx|.dot|.dotm|.pdf|.xps|.htm|.html|.mht|.mhtml|.xml|.txt|.rtf|.wtf|.odt|.xls|.xlsx|.xlsb|.xltx|.xltm|.xlt|.csv|.prn|.dif|.slk|.xlam|.xla|.ods|.ppt|.pptx|.pptm|.pot|.potm|.potx|.ppsx|.pps|.ppsm|.ppam|.thmx|.ppa|.xlsm|.wps"),
        QStringLiteral(".001|.7z|.a|.apm|.arj|.bz2|.bzip2|.cab|.cpio|.cramfs|.deb|.dmg|.epub|.esd|.ext|.ext2|.ext3|.ext4|.gpt|.gz|.gzip|.hfs|.hfsx|.hxl|.hxql|.hxr|.hxs|.hxw|.ihex|.img|.iso|.jar|.lha|.lib|.lit|.lzh|.lzma|.lzma86|.ova|.pkg|.pmd|.qcow|.qcow2|.qcow2c|.r00|.rar|.scap|.squashfs|.swm|.tar|.taz|.tbz|.tbz2|.txz|.ueff|.vdi|.vmdk|.wim|.xar|.zip|.xz|.z|.z01|.zipx"),
        QStringLiteral(".bmp|.jpg|.jpeg|.png|.gif|.tif|.tiff|.dib|.psd|.eps|.raw|.pxr|.mac|.tga|.vst|.pcd|.pct|.ai|.fpx|.cal|.wi|.ico|.cr2|.crw|.cur|.ani|.psb|.sai"),
        QStringLiteral(".lnk"),
        QStringLiteral(".url"),
        QStringLiteral(".mp4|.3gp|.avi|.wmv|.mpeg|.mpg|.mov|.flv|.swf|.qsv|.kux|.rm|.ram"),
        QStringLiteral(".mp3|.mp2|.mp1|.wav|.aif|.aiff|.au|.ra|.rm|.ram|.midi|.rmi"),
        QStringLiteral("不包含在任何规则内的文件")
    };
    const QList<bool> enabledDefaults = { true, true, true, true, true, false, false, false, true };

    m_ruleTable->setHorizontalHeaderLabels(QStringList()
        << QStringLiteral("类型") << QStringLiteral("包含后缀") << QStringLiteral("选择桌面分区") << QStringLiteral("启用"));

    for (int row = 0; row < typeNames.size(); ++row) {
        auto* typeItem = new QTableWidgetItem(typeNames.at(row));
        typeItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_ruleTable->setItem(row, 0, typeItem);

        auto* extItem = new QTableWidgetItem(extensions.at(row));
        extItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        // tooltip 中 | 分隔的后缀按每行 6 个多列显示，避免一行过长
        QStringList extParts = extensions.at(row).split(QStringLiteral("|"), Qt::SkipEmptyParts);
        QStringList extLines;
        const int perLine = 6;
        for (int i = 0; i < extParts.size(); i += perLine) {
            extLines.append(extParts.mid(i, perLine).join(QStringLiteral("  ")));
        }
        extItem->setToolTip(extLines.join(QStringLiteral("\n")));
        m_ruleTable->setItem(row, 1, extItem);

        auto* combo = new QComboBox();
        combo->addItems(partitions);
        combo->setCurrentIndex(row);
        if (!enabledDefaults.at(row)) {
            combo->setEnabled(false);
        }
        m_ruleTable->setCellWidget(row, 2, combo);
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &SettingCenterDialog::saveSettings);

        auto* check = new QCheckBox();
        check->setChecked(enabledDefaults.at(row));
        // 复选框样式交由全局主题样式表统一提供（随主题色实时联动），此处不再硬编码青色。
        connect(check, &QCheckBox::toggled, this, [this, row, combo](bool checked) {
            if (combo) combo->setEnabled(checked);
            saveSettings();
        });

        auto* checkContainer = new QWidget();
        checkContainer->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        auto* checkLayout = new QHBoxLayout(checkContainer);
        checkLayout->setContentsMargins(10, 0, 0, 0);
        checkLayout->setAlignment(Qt::AlignCenter);
        checkLayout->addWidget(check);
        m_ruleTable->setCellWidget(row, 3, checkContainer);
    }

    m_ruleTable->horizontalHeader()->setStretchLastSection(false);
    m_ruleTable->setMinimumHeight(320);
    layout->addWidget(m_ruleTable, 1);
    layout->addStretch();

    return page;
}

// 创建appearance页
// 作者：谭征
QWidget* SettingCenterDialog::createAppearancePage() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    // 外观页是 7 页里内容最高的一页。窗口高度固定 820×620（按用户要求不改），
    // 右侧内容区可用高 = 620 − 标题栏42 − 底部栏56 − 外框2 ≈ 520px，扣掉页边距后约 480px。
    // 原值 margins(20,20) + spacing(18) 时该页最小高约 544px > 480px → QVBoxLayout 只能
    // 强行把控件压到最小高之下，表现为：「其他」分组两行复选框重叠、底部提示贴住上一行。
    // 收窄行距与上下边距（spacing 18→12、margins 20/20→14/10）可挤出约 94px，
    // 页面按自然高度排布，多余空间由底部 addStretch 吸收 → 提示文字自然下沉到页面最底部，
    // 且与上方「其他」分组之间留出明显间隔。
    layout->setContentsMargins(24, 14, 24, 10);
    layout->setSpacing(12);

    m_appearancePage = page;
    m_appearancePageStyle = QStringLiteral(
        "QLabel { color: #FFFFFF; background: transparent; border: none; }"
        "QLineEdit { background-color: rgba(255,255,255,0.06); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.18); border-radius: 6px; padding: 5px 8px; }"
        "QLineEdit:focus { border-color: rgba(34,211,238,0.35); }"
        "QSlider { background: transparent; border: none; }"
        "QSlider::groove:horizontal { height: 4px; background: rgba(255,255,255,0.12); border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: rgba(34,211,238,0.45); border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 12px; height: 12px; margin: -4px 0; background: #22D3EE; border-radius: 6px; }"
        "QCheckBox { color: #FFFFFF; spacing: 8px; background: transparent; border: none; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid rgba(34,211,238,0.30); border-radius: 3px; background: transparent; }"
        "QCheckBox::indicator:hover { border-color: #22D3EE; }"
        "QCheckBox::indicator:checked { background: transparent; border: 1px solid rgba(34,211,238,0.30); image: url(:/icons/check_plain_1x.png); }"
        "QCheckBox::indicator:checked:hover { border-color: #22D3EE; }"
        "QRadioButton { color: #FFFFFF; spacing: 8px; background: transparent; border: none; }"
        "QRadioButton::indicator { width: 16px; height: 16px; border: 1px solid rgba(34,211,238,0.30); border-radius: 3px; background: transparent; }"
        "QRadioButton::indicator:hover { border-color: #22D3EE; }"
        "QRadioButton::indicator:checked { background: transparent; border: 1px solid rgba(34,211,238,0.30); image: url(:/icons/check_plain_1x.png); }"
        "QRadioButton::indicator:checked:hover { border-color: #22D3EE; }"
    );
    m_appearancePage->setStyleSheet(Theme::applyTokens(m_appearancePageStyle));

    // 选择分区
    auto* partitionRow = new QHBoxLayout();
    auto* partitionLabel = new QLabel(QStringLiteral("选择分区"));
    partitionLabel->setFixedWidth(80);
    m_partitionCombo = new QComboBox();
    m_partitionCombo->addItem(QStringLiteral("所有界面"));
    m_partitionCombo->addItem(QStringLiteral("左侧图标区"));
    m_partitionCombo->addItem(QStringLiteral("右侧助手区"));

    auto* restoreBtn = new QPushButton(QStringLiteral("恢复默认"));
    restoreBtn->setObjectName(QStringLiteral("plainButton"));
    restoreBtn->setFixedWidth(90);
    connect(restoreBtn, &QPushButton::clicked, this, &SettingCenterDialog::onRestoreDefault);

    partitionRow->addWidget(partitionLabel);
    partitionRow->addWidget(m_partitionCombo);
    partitionRow->addStretch();
    partitionRow->addWidget(restoreBtn);
    layout->addLayout(partitionRow);

    // 颜色
    auto* colorRow = new QHBoxLayout();
    auto* colorLabel = new QLabel(QStringLiteral("颜色"));
    colorLabel->setFixedWidth(80);
    colorRow->addWidget(colorLabel);

    m_colorGroup = new QButtonGroup(this);
    m_colorGroup->setExclusive(true);
    for (int i = 0; i < s_colorNames.size(); ++i) {
        auto* btn = new QToolButton();
        btn->setFixedSize(24, 24);
        btn->setCheckable(true);
        const QString color = s_colorNames.at(i);
        btn->setStyleSheet(Theme::applyTokens(QStringLiteral(
            "QToolButton { background-color: %1; border: 2px solid %2; border-radius: 4px; }"
            "QToolButton:checked { border: 2px solid #22D3EE; }"
        ).arg(color).arg(Theme::panelBgString(0.6))));
        // 右键色块 → 弹出系统色卡，自定义该格颜色并保存。
        // 与左键互不干扰：QToolButton 只对左键响应 clicked，右键不会改变当前选中的主题色。
        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(btn, &QToolButton::customContextMenuRequested,
                this, [this, i](const QPoint&) { onCustomizeSwatch(i); });
        m_colorGroup->addButton(btn, i);
        colorRow->addWidget(btn);
    }
    connect(m_colorGroup, QOverload<int>::of(&QButtonGroup::buttonClicked),
            this, &SettingCenterDialog::onColorSelected);
    colorRow->addStretch();
    layout->addLayout(colorRow);

    // 颜色预览
    auto* previewRow = new QHBoxLayout();
    auto* previewLabel = new QLabel(QStringLiteral("当前主题色"));
    previewLabel->setFixedWidth(80);
    m_colorPreview = new QLabel();
    m_colorPreview->setFixedSize(60, 24);
    m_colorPreview->setStyleSheet(QStringLiteral("background-color: #FFFFFF; border-radius: 4px;"));
    previewRow->addWidget(previewLabel);
    previewRow->addWidget(m_colorPreview);
    previewRow->addStretch();
    layout->addLayout(previewRow);

    // 透明度
    auto* transRow = new QHBoxLayout();
    auto* transLabel = new QLabel(QStringLiteral("透明度"));
    transLabel->setFixedWidth(80);
    m_transparencySlider = new QSlider(Qt::Horizontal);
    m_transparencySlider->setRange(0, 100);
    // 默认档位 100 = 完全不透明（2026-09-21 由 80 改为 100）：程序默认不做整体淡化，
    // 想要透明观感的用户自己把滑块往左拖（值越低越透）。真实值以已存配置为准，
    // loadSettings() 会用持久化的值覆盖这里。
    m_transparencySlider->setValue(100);
    m_transparencySlider->setFixedWidth(260);
    // B：关闭 tracking，拖动期间【不再】发 valueChanged
    // 为什么：一次主题下发是重活（全应用 setStyleSheet 遍历每个 widget repolish 150~330ms
    // ＋ 15 窗各自 applyTheme 的 setStyleSheet ＋ setWindowOpacity，单次合计 300~600ms）。
    // 拖动中每个像素都下发一次 → GUI 线程被占满 → 全系统鼠标僵住（低级钩子超时）。
    // 关掉 tracking 后：拖动过程【零下发】（只由下面的 sliderMoved 更新百分比文字），
    // 松手时才应用一次，手感立刻恢复正常。
    m_transparencySlider->setTracking(false);
    m_transparencyLabel = new QLabel(QStringLiteral("100%"));
    // 拖动中只实时刷新百分比文字，绝不触发任何主题下发
    connect(m_transparencySlider, &QSlider::sliderMoved, this, [this](int value) {
        if (m_transparencyLabel) m_transparencyLabel->setText(QStringLiteral("%1%").arg(value));
    });
    // 合并窗口（详见头文件 m_透明度Timer 说明）：拖动过程中最多约每 120ms 真正下发一次
    m_transparencyTimer = new QTimer(this);
    m_transparencyTimer->setSingleShot(true);
    m_transparencyTimer->setInterval(120);
    connect(m_transparencyTimer, &QTimer::timeout, this, [this]() { flushTransparency(); });
    connect(m_transparencySlider, &QSlider::valueChanged,
            this, &SettingCenterDialog::onTransparencyChanged);
    // 松手：只把尚未下发的透明度补发一次（落盘见下）。
    // 这里【绝对不能】saveSettings() —— 实测见 tools/slidertest（Qt 5.14.2 复现）：
    // [sliderPressed]  value()=100  sliderPosition()=100
    // [sliderMoved]    value()=100  sliderPosition()=6     ← 拖动中只动 position，value 不变
    // [sliderReleased] value()=100  sliderPosition()=6     ←  value() 仍是拖动前的旧值！
    // [valueChanged]   value()=6    sliderPosition()=6     ← Qt 在这之后才 setValue(position)
    // 即 QAbstractSlider::setSliderDown(false) 的顺序是「先 emit sliderReleased()、后 setValue()」。
    // 所以旧代码在此落盘，写回的是【本次拖动之前】的值 —— 表现正是用户反馈的
    // "拖到 100%，重启又变回上次那个值（8%）"：每次拖动都把上一次的值存进去，永远慢一拍。
    // 更糟的是：在滑槽上点击（pageStep 跳格）根本不发 sliderReleased，那种操作一次都不会保存。
    // 正解：落盘挂到 valueChanged（onTransparencyChanged：此处 value 必为最终值）与 QDialog::finished。
    connect(m_transparencySlider, &QSlider::sliderReleased, this, [this]() {
        if (m_transparencyTimer) m_transparencyTimer->stop();
        flushTransparency();
    });
    transRow->addWidget(transLabel);
    transRow->addWidget(m_transparencySlider);
    transRow->addWidget(m_transparencyLabel);
    transRow->addStretch();
    layout->addLayout(transRow);

    // 背景图片
    auto* bgRow = new QHBoxLayout();
    auto* bgLabel = new QLabel(QStringLiteral("背景图片"));
    bgLabel->setFixedWidth(80);
    m_bgImageEdit = new QLineEdit();
    m_bgImageEdit->setPlaceholderText(QStringLiteral("无"));
    auto* chooseBtn = new QPushButton(QStringLiteral("选择图片"));
    chooseBtn->setObjectName(QStringLiteral("plainButton"));
    chooseBtn->setFixedWidth(90);
    connect(chooseBtn, &QPushButton::clicked, this, &SettingCenterDialog::onChooseImage);
    bgRow->addWidget(bgLabel);
    bgRow->addWidget(m_bgImageEdit, 1);
    bgRow->addWidget(chooseBtn);
    layout->addLayout(bgRow);

    // 提醒声音（样式与背景图片行一致：路径框 + 选择按钮；提醒弹框出现时整曲播放）
    auto* soundRow = new QHBoxLayout();
    auto* soundLabel = new QLabel(QStringLiteral("提醒声音"));
    soundLabel->setFixedWidth(80);
    m_remindSoundEdit = new QLineEdit();
    m_remindSoundEdit->setPlaceholderText(QStringLiteral("无"));
    auto* chooseSoundBtn = new QPushButton(QStringLiteral("选择声音"));
    chooseSoundBtn->setObjectName(QStringLiteral("plainButton"));
    chooseSoundBtn->setFixedWidth(90);
    connect(chooseSoundBtn, &QPushButton::clicked, this, &SettingCenterDialog::onChooseRemindSound);
    connect(m_remindSoundEdit, &QLineEdit::textChanged, this, &SettingCenterDialog::saveSettings);
    soundRow->addWidget(soundLabel);
    soundRow->addWidget(m_remindSoundEdit, 1);
    soundRow->addWidget(chooseSoundBtn);
    layout->addLayout(soundRow);

    // 桌面字体设置
    auto* fontRow = new QHBoxLayout();
    auto* fontLabel = new QLabel(QStringLiteral("桌面字体设置"));
    fontLabel->setFixedWidth(80);
    m_fontCombo = new QComboBox();
    m_fontCombo->addItem(QStringLiteral("默认字体"));
    m_fontCombo->addItem(QStringLiteral("微软雅黑"));
    m_fontCombo->addItem(QStringLiteral("宋体"));
    m_fontCombo->addItem(QStringLiteral("Consolas"));
    fontRow->addWidget(fontLabel);
    fontRow->addWidget(m_fontCombo);
    fontRow->addStretch();
    layout->addLayout(fontRow);

    // 分区标签切换（单选：点击切换 / 悬停切换）
    auto* tagRow = new QHBoxLayout();
    auto* tagLabel = new QLabel(QStringLiteral("分区标签切换"));
    tagLabel->setFixedWidth(80);
    m_tagClickRadio = new QRadioButton(QStringLiteral("点击切换"));
    m_tagHoverRadio = new QRadioButton(QStringLiteral("悬停切换"));
    m_tagClickRadio->setChecked(true);
    connect(m_tagClickRadio, &QRadioButton::toggled, this, &SettingCenterDialog::saveSettings);
    connect(m_tagHoverRadio, &QRadioButton::toggled, this, &SettingCenterDialog::saveSettings);
    tagRow->addWidget(tagLabel);
    tagRow->addWidget(m_tagClickRadio);
    tagRow->addWidget(m_tagHoverRadio);
    tagRow->addStretch();
    layout->addLayout(tagRow);

    // 分区菜单标签显示（单选：始终显示 / 鼠标悬停显示）
    auto* menuLabelRow = new QHBoxLayout();
    auto* menuLabel = new QLabel(QStringLiteral("分区菜单标签显示"));
    menuLabel->setFixedWidth(80);
    m_menuAlwaysShowRadio = new QRadioButton(QStringLiteral("始终显示"));
    m_menuHoverShowRadio = new QRadioButton(QStringLiteral("鼠标悬停显示"));
    m_menuAlwaysShowRadio->setChecked(true);
    connect(m_menuAlwaysShowRadio, &QRadioButton::toggled, this, &SettingCenterDialog::saveSettings);
    connect(m_menuHoverShowRadio, &QRadioButton::toggled, this, &SettingCenterDialog::saveSettings);
    menuLabelRow->addWidget(menuLabel);
    menuLabelRow->addWidget(m_menuAlwaysShowRadio);
    menuLabelRow->addWidget(m_menuHoverShowRadio);
    menuLabelRow->addStretch();
    layout->addLayout(menuLabelRow);

    // 两对单选钮必须【各自】装进 QButtonGroup，否则本页 4 个 QRadioButton 是同一父窗口的
    // 兄弟节点，会被 Qt 的 autoExclusive 机制合并成**一个**互斥组 —— 于是 loadSettings()
    // 里后赋值的那对（分区菜单标签显示）会把先赋值的那对（分区标签切换）刚选中的按钮踩掉：
    // 磁盘里明明存着 tagSwitchMode=1，重新打开设置中心却是「两个都没选中」，用户看到的就是
    // 「这个开关的状态没有持久化」。2026-09-21 用真实控件实测复现（tools/scprobe 阶段 F：
    // 4 个单选钮 parentWidget() 指针完全相同；选中「悬停切换」后再给「始终显示」赋值，
    // 悬停立刻被清掉）。装进独立 QButtonGroup 后，组内互斥、组间完全独立。
    if (auto* g = new QButtonGroup(this)) {
        g->addButton(m_tagClickRadio, 0);
        g->addButton(m_tagHoverRadio, 1);
    }
    if (auto* g = new QButtonGroup(this)) {
        g->addButton(m_menuAlwaysShowRadio, 0);
        g->addButton(m_menuHoverShowRadio, 1);
    }

    // 其他
    auto* otherTitle = new QLabel(QStringLiteral("其他"));
    otherTitle->setStyleSheet(QStringLiteral("color: #FFFFFF; font-weight: bold; background: transparent; border: none;"));
    layout->addWidget(otherTitle);

    auto* otherLayout = new QGridLayout();
    otherLayout->setSpacing(12);
    otherLayout->setContentsMargins(0, 0, 0, 0);
    // 给两行复选框定死最小行高：即使窗口/字体把空间压到极限，这两行也不会互相重叠
    // （「其他」分组被压扁是本页最直观的故障症状）。
    otherLayout->setRowMinimumHeight(0, 22);
    otherLayout->setRowMinimumHeight(1, 22);
    otherLayout->setRowMinimumHeight(2, 22);
    m_boxBorderCheck = new QCheckBox(QStringLiteral("盒子显示边框"));
    m_boxRoundCheck = new QCheckBox(QStringLiteral("盒子使用圆角"));
    m_shortcutArrowCheck = new QCheckBox(QStringLiteral("在快捷方式图标上显示箭头"));
    m_autoExpandOnHoverCheck = new QCheckBox(QStringLiteral("收起后，鼠标移动到标题自动展开"));
    // 「闹铃」：待办事项到点提醒时是否播放上面「提醒声音」选中的文件。
    // 勾选＝出声（原有行为，故默认勾选，不回退老用户既有体验）；取消＝提醒静音，
    // 但提醒弹框照常出现。消费端在 SidePanelWidget 的 reminderDue 里（每次到期实时读盘）。
    m_remindSoundCheck = new QCheckBox(QStringLiteral("闹铃"));
    m_remindSoundCheck->setToolTip(QStringLiteral("选中：待办事项到点提醒时播放「提醒声音」选中的文件；\n"
                                                  "未选中：提醒静音（提醒弹框仍会出现）"));
    m_boxBorderCheck->setChecked(true);
    m_shortcutArrowCheck->setChecked(true);
    m_remindSoundCheck->setChecked(true);
    connect(m_boxBorderCheck, &QCheckBox::stateChanged, this, &SettingCenterDialog::saveSettings);
    connect(m_boxRoundCheck, &QCheckBox::stateChanged, this, &SettingCenterDialog::saveSettings);
    connect(m_shortcutArrowCheck, &QCheckBox::stateChanged, this, &SettingCenterDialog::saveSettings);
    connect(m_autoExpandOnHoverCheck, &QCheckBox::stateChanged, this, &SettingCenterDialog::saveSettings);
    connect(m_remindSoundCheck, &QCheckBox::stateChanged, this, &SettingCenterDialog::saveSettings);
    otherLayout->addWidget(m_boxBorderCheck, 0, 0);
    otherLayout->addWidget(m_boxRoundCheck, 0, 1);
    otherLayout->addWidget(m_shortcutArrowCheck, 1, 0);
    otherLayout->addWidget(m_autoExpandOnHoverCheck, 1, 1);
    // 单独一行、跨两列（与上面两行的左列对齐，占满整行宽度）
    otherLayout->addWidget(m_remindSoundCheck, 2, 0, 1, 2);
    layout->addLayout(otherLayout);

    layout->addStretch();

    auto* bottomHint = new QLabel(QStringLiteral("提示：外观设置当前为预览状态，重启后生效"));
    bottomHint->setStyleSheet(QStringLiteral("color: rgba(255,255,255,0.5); font-size: 11px;"));
    layout->addWidget(bottomHint, 0, Qt::AlignLeft);

    return page;
}

// 创建桌面备份页
// 作者：谭征
QWidget* SettingCenterDialog::createDesktopBackupPage() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(14);

    // 标题
    auto* titleLabel = new QLabel(QStringLiteral("桌面布局备份"));
    QFont tf = titleLabel->font();
    tf.setPointSize(14);
    tf.setBold(true);
    titleLabel->setFont(tf);
    titleLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent; border: none;"));
    layout->addWidget(titleLabel);

    // 样式模板（含主题令牌，applyTheme→restyleBackupCards 时重绘）
    // 全部带 id 选择器：备份卡/浮层都是 QFrame，裸选择器会向子控件传播（漏出多余圆角框）。
    m_backupCardStyle = QStringLiteral(
        "QFrame#backupThumbCard { background-color: rgba(255,255,255,0.04); border: 1px solid rgba(34,211,238,0.18); border-radius: 10px; }"
    );
    m_backupOverlayStyle = QStringLiteral(
        "QFrame#backupCardOverlay { background-color: rgba(0,0,0,0.45); border-radius: 8px; }"
    );
    m_backupApplyBtnStyle = QStringLiteral(
        "QPushButton { background-color: #22D3EE; color: #FFFFFF; border: none; border-radius: 6px;"
        " padding: 4px 16px; font-size: 12px; font-weight: bold; }"
        "QPushButton:hover { background-color: #67E8F9; }"
        "QPushButton:pressed { background-color: #3B82F6; }"
    );
    m_backupDelBtnStyle = QStringLiteral(
        "QToolButton { background: transparent; color: rgba(255,255,255,0.85); border: none; font-size: 15px; }"
        "QToolButton:hover { color: #FF6B6B; }"
    );
    m_backupDateLabelStyle = QStringLiteral(
        "QLabel { color: rgba(255,255,255,0.80); font-size: 12px; background: transparent; border: none; }"
    );
    m_backupAddCardStyle = QStringLiteral(
        "QPushButton { background-color: rgba(255,255,255,0.04); border: 1px solid rgba(34,211,238,0.18); border-radius: 10px; }"
        "QPushButton:hover { background-color: rgba(34,211,238,0.10); }"
        "QPushButton:pressed { background-color: rgba(34,211,238,0.16); }"
    );

    // 备份卡网格放在滚动区里（备份多了可以滚）
    m_backupScroll = new QScrollArea(page);
    m_backupScroll->setObjectName(QStringLiteral("backupScroll"));
    m_backupScroll->setWidgetResizable(true);
    m_backupScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_backupScroll->setFrameShape(QFrame::NoFrame);
    // 滚动区/viewport/内容宿主都是 QFrame 系，逐个显式透明（带 id 选择器），
    // 防止从页面父级白捡背景/边框。
    m_backupScroll->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QScrollArea#backupScroll { background: transparent; border: none; }")));
    m_backupScroll->viewport()->setObjectName(QStringLiteral("backupScrollViewport"));
    m_backupScroll->viewport()->setStyleSheet(QStringLiteral(
        "QWidget#backupScrollViewport { background: transparent; border: none; }"));

    m_backupGridHost = new QWidget();
    m_backupGridHost->setObjectName(QStringLiteral("backupGridHost"));
    m_backupGridHost->setStyleSheet(QStringLiteral(
        "QWidget#backupGridHost { background: transparent; border: none; }"));
    m_backupGrid = new QGridLayout(m_backupGridHost);
    m_backupGrid->setContentsMargins(0, 0, 0, 0);
    m_backupGrid->setHorizontalSpacing(20);
    m_backupGrid->setVerticalSpacing(16);
    m_backupGrid->setColumnStretch(2, 1);   // 卡片固定宽，右侧留白
    m_backupScroll->setWidget(m_backupGridHost);
    layout->addWidget(m_backupScroll, 1);

    // 「添加备份」卡（网格末位；rebuildBackupCards 负责摆位）
    auto* addCard = new QPushButton();
    addCard->setFixedSize(220, 150);
    addCard->setCursor(Qt::PointingHandCursor);
    addCard->setFlat(true);
    m_backupAddCard = addCard;
    auto* addLayout = new QVBoxLayout(addCard);
    addLayout->setAlignment(Qt::AlignCenter);
    addLayout->setSpacing(2);
    auto* plusLabel = new QLabel(QStringLiteral("+"));
    plusLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 32px; background: transparent; border: none;"));
    plusLabel->setAlignment(Qt::AlignCenter);
    auto* addText = new QLabel(QStringLiteral("添加备份"));
    addText->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 13px; background: transparent; border: none;"));
    addText->setAlignment(Qt::AlignCenter);
    addLayout->addWidget(plusLabel);
    addLayout->addWidget(addText);

    connect(addCard, &QPushButton::clicked, this, &SettingCenterDialog::onAddBackup);

    // 首次填充：扫描已有备份 + 摆放添加卡
    rebuildBackupCards();
    return page;
}

// thumb.png（备份瞬间的桌面截图）  meta.ini（备份名）
// 作者：谭征
QString SettingCenterDialog::backupRootDir() const {
    // 与 QSettings 落盘文件同根：%APPDATA%/DestopTools/backups
    QSettings cur(QSettings::IniFormat, QSettings::UserScope,
                  QStringLiteral("DestopTools"), QStringLiteral("DestopTools"));
    return QFileInfo(cur.fileName()).absolutePath() + QStringLiteral("/backups");
}

// 重建备份cards
// 作者：谭征
void SettingCenterDialog::rebuildBackupCards() {
    if (!m_backupGrid || !m_backupGridHost) return;

    // 清空网格（「添加备份」卡是成员，保活复用）
    while (QLayoutItem* it = m_backupGrid->takeAt(0)) {
        if (QWidget* w = it->widget()) {
            if (w != m_backupAddCard) w->deleteLater();
        }
        delete it;
    }

    const QString root = backupRootDir();
    const QFileInfoList entries = QDir(root).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);   // 新备份在前

    int row = 0, col = 0;
    for (const QFileInfo& fi : entries) {
        const QString dir = fi.absoluteFilePath();
        const QString stamp = fi.fileName();                  // yyyyMMdd_HHmmss
        const QString dateText = stamp.left(8);               // 仿 360：只显示日期

        // 备份名（meta.ini [meta] name；旧格式/缺失时回退时间戳）
        QString name = stamp;
        QSettings meta(dir + QStringLiteral("/meta.ini"), QSettings::IniFormat);
        const QString metaName = meta.value(QStringLiteral("meta/name")).toString();
        if (!metaName.isEmpty()) name = metaName;

        auto* card = new QFrame(m_backupGridHost);
        card->setObjectName(QStringLiteral("backupThumbCard"));
        card->setFixedSize(220, 150);
        card->setProperty("backupDir", dir);
        card->setProperty("backupMetaName", name);
        card->setProperty("backupStamp", stamp);
        card->setCursor(Qt::PointingHandCursor);
        card->installEventFilter(this);                       // 悬停显隐浮层

        auto* thumb = new QLabel(card);
        thumb->setGeometry(4, 4, 212, 116);
        thumb->setAlignment(Qt::AlignCenter);
        const QPixmap pm = roundedBackupThumb(dir + QStringLiteral("/thumb.png"), QSize(212, 116));
        if (!pm.isNull()) thumb->setPixmap(pm);
        thumb->setProperty("backupDateLabel", false);

        // 悬停浮层：应用备份（居中）+ 删除（右上角）
        auto* overlay = new QFrame(card);
        overlay->setObjectName(QStringLiteral("backupCardOverlay"));
        overlay->setGeometry(4, 4, 212, 116);
        overlay->setAttribute(Qt::WA_StyledBackground, true);
        auto* ovLayout = new QVBoxLayout(overlay);
        ovLayout->setContentsMargins(8, 8, 8, 8);
        ovLayout->addStretch();
        auto* applyBtn = new QPushButton(QStringLiteral("应用备份"), overlay);
        applyBtn->setCursor(Qt::PointingHandCursor);
        applyBtn->setFixedHeight(30);
        connect(applyBtn, &QPushButton::clicked, this, [this, dir]() { applyBackupNow(dir); });
        ovLayout->addWidget(applyBtn, 0, Qt::AlignHCenter);
        ovLayout->addStretch();
        auto* delBtn = new QToolButton(overlay);
        delBtn->setText(QStringLiteral("×"));
        delBtn->setFixedSize(22, 22);
        delBtn->setCursor(Qt::PointingHandCursor);
        delBtn->setGeometry(212 - 26, 2, 22, 22);
        delBtn->raise();
        connect(delBtn, &QToolButton::clicked, this, [this, dir, name]() { deleteBackupNow(dir, name); });

        auto* dateLabel = new QLabel(dateText, card);
        dateLabel->setGeometry(8, 124, 204, 22);
        dateLabel->setProperty("backupDateLabel", true);
        dateLabel->setToolTip(QStringLiteral("%1（%2）").arg(name, stamp));

        m_backupGrid->addWidget(card, row, col);
        if (++col >= 2) { col = 0; ++row; }
    }

    // 添加备份卡摆到末位
    if (m_backupAddCard) {
        m_backupGrid->addWidget(m_backupAddCard, row, col);
        if (++col >= 2) { col = 0; ++row; }
    }
    m_backupGrid->setRowStretch(m_backupGrid->rowCount(), 1);

    restyleBackupCards();
}

// restyle备份cards
// 作者：谭征
void SettingCenterDialog::restyleBackupCards() {
    if (!m_backupAddCardStyle.isEmpty() && m_backupAddCard) {
        m_backupAddCard->setStyleSheet(Theme::applyTokens(m_backupAddCardStyle));
    }
    if (!m_backupGridHost) return;
    const QList<QFrame*> cards = m_backupGridHost->findChildren<QFrame*>(QStringLiteral("backupThumbCard"));
    for (QFrame* card : cards) {
        if (!m_backupCardStyle.isEmpty()) card->setStyleSheet(Theme::applyTokens(m_backupCardStyle));
        if (QFrame* overlay = card->findChild<QFrame*>(QStringLiteral("backupCardOverlay"))) {
            if (!m_backupOverlayStyle.isEmpty()) overlay->setStyleSheet(Theme::applyTokens(m_backupOverlayStyle));
        }
        if (QPushButton* applyBtn = card->findChild<QPushButton*>()) {
            // overlay 里唯一的 QPushButton 就是「应用备份」
            if (applyBtn->parentWidget() && applyBtn->parentWidget()->objectName() == QStringLiteral("backupCardOverlay")
                    && !m_backupApplyBtnStyle.isEmpty()) {
                applyBtn->setStyleSheet(Theme::applyTokens(m_backupApplyBtnStyle));
            }
        }
        const QList<QToolButton*> delBtns = card->findChildren<QToolButton*>();
        for (QToolButton* delBtn : delBtns) {
            if (!m_backupDelBtnStyle.isEmpty()) delBtn->setStyleSheet(Theme::applyTokens(m_backupDelBtnStyle));
        }
        const QList<QLabel*> dateLabels = card->findChildren<QLabel*>();
        for (QLabel* lb : dateLabels) {
            if (lb->property("backupDateLabel").toBool() && !m_backupDateLabelStyle.isEmpty()) {
                lb->setStyleSheet(Theme::applyTokens(m_backupDateLabelStyle));
            }
        }
    }
}

// rounded备份thumb
// 作者：谭征
QPixmap SettingCenterDialog::roundedBackupThumb(const QString& imageFile, const QSize& size) const {
    QImage srcImg(imageFile);
    if (srcImg.isNull()) return QPixmap();
    srcImg.setDevicePixelRatio(1.0);   // 之后一律按物理像素计算，规避源文件自带 DPR 的歧义
    const qreal dpr = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
    const QSize phys(qRound(size.width() * dpr), qRound(size.height() * dpr));
    QImage out(phys, QImage::Format_ARGB32_Premultiplied);
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath path;
    path.addRoundedRect(0, 0, size.width(), size.height(), 8, 8);   // 逻辑坐标（= device / dpr）
    p.setClipPath(path);
    QImage scaled = srcImg.scaled(phys, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const int x = qMax(0, (scaled.width() - phys.width()) / 2);
    const int y = qMax(0, (scaled.height() - phys.height()) / 2);
    p.drawImage(QRectF(0, 0, size.width(), size.height()), scaled,
                QRectF(x, y, phys.width(), phys.height()));
    p.end();
    return QPixmap::fromImage(out);
}

// 响应添加备份
// 作者：谭征
void SettingCenterDialog::onAddBackup() {
    BackupDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    QString name = dlg.backupName();
    if (name.isEmpty()) {
        name = QStringLiteral("桌面备份");
    }
    createBackupNow(name);
}

// 创建备份now
// 作者：谭征
void SettingCenterDialog::createBackupNow(const QString& name) {
    // 备份实体：%APPDATA%/DestopTools/backups/<yyyyMMdd_HHmmss>/
    // DestopTools.ini —— 整份配置（收纳盒 UserBoxes / Dock / 全部设置 / 待办等全在这一个文件里）
    // thumb.png       —— 备份瞬间的桌面截图（做缩略图）
    // meta.ini        —— 备份名
    const QString root = backupRootDir();
    if (!QDir().mkpath(root)) return;

    // 先把运行期尚未刷盘的设置落定，保证拷走的是完整快照
    {
        SettingsManager sm;
        sm.sync();
        QSettings cur(QSettings::IniFormat, QSettings::UserScope,
                      QStringLiteral("DestopTools"), QStringLiteral("DestopTools"));
        cur.sync();
        // 分类映射库（categories.ini）也要落定：它是独立 INI，不属于 SettingsManager。
        QSettings cat(QSettings::IniFormat, QSettings::UserScope,
                      QStringLiteral("DestopTools"), QStringLiteral("categories"));
        cat.sync();
    }

    const QDateTime now = QDateTime::currentDateTime();
    const QString stamp = now.toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString dir = root + QStringLiteral("/") + stamp;
    if (!QDir().mkpath(dir)) return;

    // 应用有两份配置文件，必须一起备份：
    // ① DestopTools.ini —— 主设置（Dock 快照 / 收纳盒 UserBoxes / 待办 / 全部选项 / 已解散分类集合）
    // ② categories.ini  —— 分类映射库 CategoryStore（文件→分类、已归档隐藏于 Dock）
    // 主窗口「分类」中每个图标属于哪个分类，权威数据全在 categories.ini；若只备份
    // DestopTools.ini，则「解散分类」后应用备份无法还原该分类（图标仍留在 Dock 上）。
    QSettings cur(QSettings::IniFormat, QSettings::UserScope,
                  QStringLiteral("DestopTools"), QStringLiteral("DestopTools"));
    const QString cfgDir = QFileInfo(cur.fileName()).absolutePath();
    const QStringList cfgFiles{ QStringLiteral("DestopTools.ini"), QStringLiteral("categories.ini") };
    for (const QString& fn : cfgFiles) {
        const QString src = cfgDir + QStringLiteral("/") + fn;
        const QString dst = dir + QStringLiteral("/") + fn;
        QFile::remove(dst);
        if (QFile::exists(src)) {
            QFile::copy(src, dst);
        } else {
            // 配置文件尚不存在（从未写过设置）：落一个空文件，保证备份目录结构完整
            QFile f(dst);
            f.open(QIODevice::WriteOnly);
        }
    }

    // 桌面截图：用 Win32 BitBlt + CAPTUREBLT 从屏幕 DC 抓取——这样才能抓到 DWM 合成后的
    // 全部可见窗口（其他程序原样进截图）。QScreen::grabWindow(0) 内部只做了 SRCCOPY，在开启
    // DWM 的系统上只抓到桌面壁纸、抓不到其他窗口（正是“其他程序不进截图”的根因）。
    // 截屏期间【绝不 hide()/show() 本窗】，只把本窗「移到屏幕外」。
    // 经实测：本窗是 WA_TranslucentBackground 的无边框对话框，且由 exec() 拉起（应用级模态）。
    // hide() 会让 Qt 丢弃/重置透明后援缓冲，再 show() 时窗口可能整块透明（视觉上完全消失），
    // 而 exec() 的模态循环仍在跑 —— 用户看到的就是「卡片刷新了、设置中心却整个不见了，
    // 同时其他窗口全被模态挡住」，等于死锁在看不见的对话框上。
    // 移到屏幕外同样保证本窗不进截图（其他程序照常进图），但完全不触碰 Qt 的可见性 / 模态 /
    // z 序状态：窗口自始至终"可见且是同一个前台模态窗"，截完立即移回原位，不会丢位置。
    const QPoint savedPos = this->pos();
    this->move(-32000, -32000);
    QApplication::processEvents();
    ::Sleep(120);   // 等 DWM 重新合成（本窗已不在屏幕范围内）

    QImage full;
    HDC hdcScreen = GetDC(NULL);
    if (hdcScreen) {
        const int sw = GetDeviceCaps(hdcScreen, HORZRES);
        const int sh = GetDeviceCaps(hdcScreen, VERTRES);
        HDC hdcMem = CreateCompatibleDC(hdcScreen);
        HBITMAP hbmp = CreateCompatibleBitmap(hdcScreen, sw, sh);
        if (hdcMem && hbmp) {
            HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hbmp);
            BitBlt(hdcMem, 0, 0, sw, sh, hdcScreen, 0, 0, SRCCOPY | CAPTUREBLT);
            SelectObject(hdcMem, hOld);
            QPixmap px = QtWin::fromHBITMAP(hbmp, QtWin::HBitmapNoAlpha);
            full = px.toImage();
        }
        if (hbmp) DeleteObject(hbmp);
        if (hdcMem) DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
    }

    // 移回原位（只 move，不改尺寸，避免多余 resize 事件；本窗全程可见、模态未被触碰）
    this->move(savedPos);
    this->raise();
    this->activateWindow();
    this->update();

    if (!full.isNull()) {
        // 统一按物理像素落盘（480px 宽）
        QImage thumb = full.scaledToWidth(480, Qt::SmoothTransformation);
        thumb.save(dir + QStringLiteral("/thumb.png"), "PNG");
    }

    QSettings meta(dir + QStringLiteral("/meta.ini"), QSettings::IniFormat);
    meta.setValue(QStringLiteral("meta/name"), name);
    meta.setValue(QStringLiteral("meta/created"), now.toString(Qt::ISODate));
    meta.sync();

    rebuildBackupCards();

    // 截屏前后本窗被短暂移出屏幕，确保重建后新卡片立即可见、无需重开页面：
    // 让网格布局重新生效、宿主自适应、滚动区视口重绘，并强制本窗重绘一次。
    if (m_backupGrid) {
        m_backupGrid->invalidate();
        m_backupGrid->activate();
    }
    if (m_backupGridHost) {
        m_backupGridHost->adjustSize();
        m_backupGridHost->updateGeometry();
        m_backupGridHost->update();
    }
    if (m_backupScroll && m_backupScroll->viewport()) m_backupScroll->viewport()->update();
    this->update();
    this->repaint();
    QApplication::processEvents();
}

// 应用备份now
// 作者：谭征
void SettingCenterDialog::applyBackupNow(const QString& dir) {
    if (dir.isEmpty() || !QFileInfo::exists(dir + QStringLiteral("/DestopTools.ini"))) {
        GlassMessageBox::information(this, QStringLiteral("应用备份"),
            QStringLiteral("备份文件缺失（DestopTools.ini 不存在），无法应用。"));
        return;
    }
    // 编排（见 appexit.h / main.cpp）：本实例退出、全部窗口的落盘动作都停止之后，
    // main() 才用备份覆盖现行配置并拉起新实例 —— 退出路径上的任何 saveValue
    // 都不可能把恢复好的配置再盖回去。这里只置位 + 提示 + 退出。
    g_pendingBackupDir = dir;
    g_restartAfterQuit = true;
    QSettings meta(dir + QStringLiteral("/meta.ini"), QSettings::IniFormat);
    QString name = meta.value(QStringLiteral("meta/name")).toString();
    if (name.isEmpty()) name = QFileInfo(dir).fileName();
    GlassMessageBox::information(this, QStringLiteral("应用备份"),
        QStringLiteral("已选择备份「%1」，程序将自动重启并应用该备份的桌面布局与全部配置。")
            .arg(name));
    // 本对话框是 MainWindow 以 exec() 拉起的【嵌套模态】。若直接 qApp->quit()，Qt 只会退出
    // 最内层（对话框）事件循环，MainWindow 所在的主事件循环仍在跑 → a.exec() 不返回 →
    // main() 里「覆盖配置 + 重启」的还原块永不执行，备份等于没应用。
    // 正确做法：先 accept() 关闭本对话框（其 exec() 返回），再用单次定时器在【主事件循环】
    // 上下文再发一次 quit，确保主循环真正退出、还原块得以运行。
    accept();
    QTimer::singleShot(0, qApp, &QApplication::quit);
}

// 删除备份now
// 作者：谭征
void SettingCenterDialog::deleteBackupNow(const QString& dir, const QString& name) {
    const int r = GlassMessageBox::warning(this, QStringLiteral("删除备份"),
        QStringLiteral("确定删除备份「%1」吗？删除后不可恢复。").arg(name));
    if (r != QDialog::Accepted) return;
    QDir(dir).removeRecursively();
    rebuildBackupCards();
}

// 应用级“点别处即提交”守卫（装在 qApp 上）：见 cpp 中的实现说明。
// 作者：谭征
bool SettingCenterDialog::eventFilter(QObject* watched, QEvent* event) {
    // 备份卡悬停：浮层（应用备份 / 删除）随鼠标进出显隐。
    // 浮层是卡片的子件，光标不会因浮层出现而触发 Leave。
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        QWidget* card = qobject_cast<QWidget*>(watched);
        if (card && !card->property("backupDir").toString().isEmpty()) {
            if (QFrame* overlay = card->findChild<QFrame*>(QStringLiteral("backupCardOverlay"))) {
                if (event->type() == QEvent::Enter) {
                    overlay->raise();
                    overlay->show();
                } else {
                    overlay->hide();
                }
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

// 创建quickactions页
// 作者：谭征
QWidget* SettingCenterDialog::createQuickActionsPage() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(18);

    // 标题
    auto* titleLabel = new QLabel(QStringLiteral("快捷操作"));
    QFont tf = titleLabel->font();
    tf.setPointSize(14);
    tf.setBold(true);
    titleLabel->setFont(tf);
    titleLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent; border: none;"));
    layout->addWidget(titleLabel);

    // 本页复选框 / 输入框 / 表格样式交由全局主题样式表（Theme::globalStyleSheet，已在 themeChanged 时实时重设）统一提供，
    // 不再硬编码青色，确保「改主题色 → 快捷操作页全部控件实时联动」。

    // 快捷功能
    auto* funcTitle = new QLabel(QStringLiteral("快捷功能"));
    funcTitle->setStyleSheet(QStringLiteral("color: #FFFFFF; font-weight: bold; background: transparent; border: none;"));
    layout->addWidget(funcTitle);

    m_hideIconsOnDoubleClickCheck = new QCheckBox(QStringLiteral("双击桌面空白处隐藏桌面图标"));
    m_hideIconsOnDoubleClickCheck->setChecked(true);
    layout->addWidget(m_hideIconsOnDoubleClickCheck);

    m_drawBoxOnBlankCheck = new QCheckBox(QStringLiteral("在桌面空白处绘制创建收纳盒"));
    m_drawBoxOnBlankCheck->setChecked(true);
    layout->addWidget(m_drawBoxOnBlankCheck);

    // 快捷功能复选框外矩形线框样式模板：背景/边框随主题色和全局透明度实时联动。
    m_quickCheckStyle = QStringLiteral(
        "QCheckBox { color: #FFFFFF; background-color: BACKGROUND_COLOR; border: 1px solid rgba(34,211,238,ALPHA_BORDER); border-radius: 8px; padding: 6px 10px; spacing: 8px; }"
        "QCheckBox::indicator { width: 18px; height: 18px; border-radius: 4px; border: 1px solid rgba(34,211,238,0.30); background: transparent; }"
        "QCheckBox::indicator:checked { background: #22D3EE; border-color: #22D3EE; image: url(:/icons/check_plain_1x.png); }"
    );
    auto applyQuickCheckStyle = [this](QCheckBox* check) {
        if (!check) return;
        QString s = Theme::applyTokens(m_quickCheckStyle);
        s.replace(QStringLiteral("BACKGROUND_COLOR"), Theme::windowBgString(0.10));
        s.replace(QStringLiteral("ALPHA_BORDER"), QString::number(Theme::alphaF(0.22), 'f', 2));
        check->setStyleSheet(s);
    };
    applyQuickCheckStyle(m_hideIconsOnDoubleClickCheck);
    applyQuickCheckStyle(m_drawBoxOnBlankCheck);

    auto* linkLabel = new QLabel(QStringLiteral("如何从快速隐藏中排除收纳盒或图标 ?"));
    linkLabel->setObjectName(QStringLiteral("linkLabel"));
    linkLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; padding-left: 24px; background: transparent; border: none;"));
    linkLabel->setCursor(Qt::PointingHandCursor);
    layout->addWidget(linkLabel);

    // 快捷键
    auto* shortcutTitle = new QLabel(QStringLiteral("快捷键"));
    shortcutTitle->setStyleSheet(QStringLiteral("color: #FFFFFF; font-weight: bold; background: transparent; border: none;"));
    layout->addWidget(shortcutTitle);

    const QStringList shortcutNames = {
        QStringLiteral("截图"),
        QStringLiteral("本地搜索"),
        QStringLiteral("计算器"),
        QStringLiteral("记事本"),
        QStringLiteral("桌面整理"),
        QStringLiteral("上网"),
        QStringLiteral("添加事项"),
        QStringLiteral("锁屏")
    };
    const QStringList shortcutValues = {
        QStringLiteral("无"),
        QStringLiteral("无"),
        QStringLiteral("无"),
        QStringLiteral("无"),
        QStringLiteral("无"),
        QStringLiteral("无"),
        QStringLiteral("无"),
        QStringLiteral("无")
    };

    m_shortcutTable = new QTableWidget(shortcutNames.size(), 2);
    m_shortcutTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_shortcutTable->setFocusPolicy(Qt::NoFocus);
    m_shortcutTable->setShowGrid(false);
    m_shortcutTable->setFrameStyle(QFrame::NoFrame);                // 去掉表格整体外框
    m_shortcutTable->verticalHeader()->setVisible(false);
    m_shortcutTable->horizontalHeader()->setVisible(false);
    m_shortcutTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_shortcutTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_shortcutTable->setColumnWidth(0, 70);                         // 名称列稍窄，给输入框更多空间
    m_shortcutTable->verticalHeader()->setDefaultSectionSize(32);   // 行高紧凑但足够显示文字
    m_shortcutTable->setMinimumHeight(260);
    m_shortcutTable->setStyleSheet(Theme::applyTokens(QStringLiteral(
        "QTableWidget { background: transparent; border: none; outline: none; }"
        "QAbstractScrollArea::viewport { background: transparent; border: none; }"
        "QTableWidget::item { background: transparent; padding: 1px 3px; border: none; color: #FFFFFF; }"
    )));

    for (int row = 0; row < shortcutNames.size(); ++row) {
        auto* nameItem = new QTableWidgetItem(shortcutNames.at(row));
        nameItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        nameItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_shortcutTable->setItem(row, 0, nameItem);

        auto* edit = new ShortcutEdit();
        edit->setShortcutText(shortcutValues.at(row));
        // 三重保存触发：textChanged（文本变化时）、editingFinished（焦点离开时）、
        // shortcutCommitted（ShortcutEdit 显式提交快捷键时），确保用户输入的
        // 组合键在任何情况下都能立即持久化。
        connect(edit, &ShortcutEdit::textChanged, this, &SettingCenterDialog::saveSettings);
        connect(edit, &ShortcutEdit::editingFinished, this, &SettingCenterDialog::saveSettings);
        connect(edit, &ShortcutEdit::shortcutCommitted, this, &SettingCenterDialog::saveSettings);
        m_shortcutTable->setCellWidget(row, 1, edit);
    }

    layout->addWidget(m_shortcutTable, 1);
    layout->addStretch();

    // 连接变化保存
    connect(m_hideIconsOnDoubleClickCheck, &QCheckBox::stateChanged, this, &SettingCenterDialog::saveSettings);
    connect(m_drawBoxOnBlankCheck, &QCheckBox::stateChanged, this, &SettingCenterDialog::saveSettings);

    return page;
}

// 创建tools页
// 作者：谭征
QWidget* SettingCenterDialog::createToolsPage() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(18);

    // 标题：我的功能
    auto* titleLabel = new QLabel(QStringLiteral("我的功能"));
    QFont tf = titleLabel->font();
    tf.setPointSize(14);
    tf.setBold(true);
    titleLabel->setFont(tf);
    titleLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent; border: none;"));
    layout->addWidget(titleLabel);

    // 小工具列表：透明玻璃风格（与「常规设置」页色调一致）+ 彩色图标 + 名称；仅选中行显示 ↑/↓ 排序按钮
    m_toolsList = new QListWidget(page);
    m_toolsList->setSpacing(0);
    // 严格覆盖全局 theme.h 中 QListWidget 的兜底样式，避免残留边框/背景/边距
    m_toolsListStyle = QStringLiteral(
        "QListWidget {"
        "  background: transparent;"
        "  border: none;"
        "  outline: none;"
        "  padding: 0px;"
        "}"
        "QListWidget::item {"
        "  background-color: transparent;"
        "  border: none;"
        "  border-radius: 0px;"
        "  margin: 0px;"
        "  padding: 0px 0px 0px 12px;"
        "  color: #FFFFFF;"
        "}"
        "QListWidget::item:selected {"
        "  background-color: transparent;"
        "  border: none;"
        "  border-left: 3px solid #22D3EE;"
        "  border-radius: 0px;"
        "  margin: 0px;"
        "  padding: 0px 0px 0px 9px;"
        "  color: #FFFFFF;"
        "}"
        "QListWidget::item:hover {"
        "  background-color: rgba(34,211,238,0.08);"
        "  border-radius: 6px;"
        "}"
        "QListWidget::item:selected:hover {"
        "  background-color: rgba(34,211,238,0.10);"
        "  border-radius: 6px;"
        "}"
        "QScrollBar:vertical { background: transparent; width: 6px; border-radius: 3px; margin: 0px; }"
        "QScrollBar::handle:vertical { background: rgba(255,255,255,0.20); min-height: 30px; border-radius: 3px; }"
        "QScrollBar::handle:vertical:hover { background: rgba(255,255,255,0.35); }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"
    );
    m_toolsList->setStyleSheet(Theme::applyTokens(m_toolsListStyle));
    m_toolsList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_toolsList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_toolsList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_toolsList->setFrameStyle(QFrame::NoFrame);
    m_toolsList->viewport()->setAutoFillBackground(false);

    // 启用内部拖拽排序
    m_toolsList->setDragEnabled(true);
    m_toolsList->setAcceptDrops(true);
    m_toolsList->setDragDropMode(QAbstractItemView::InternalMove);
    m_toolsList->setDefaultDropAction(Qt::MoveAction);
    m_toolsList->setDropIndicatorShown(false);

    connect(m_toolsList, &QListWidget::itemSelectionChanged,
            this, &SettingCenterDialog::onToolsSelectionChanged);

    connect(m_toolsList->model(), &QAbstractItemModel::rowsMoved,
            this, [this]() {
        if (m_isRefreshingToolsList) return;
        QStringList newOrder;
        newOrder.reserve(m_toolsList->count());
        for (int i = 0; i < m_toolsList->count(); ++i) {
            auto* item = m_toolsList->item(i);
            if (item) newOrder << item->data(Qt::UserRole).toString();
        }
        if (newOrder == m_toolsOrder) return;
        m_toolsOrder = newOrder;
        saveSettings();
        emit quickToolsOrderChanged(m_toolsOrder);
    });

    layout->addWidget(m_toolsList, 1);

    // 底部操作按钮（绿色文字风格）
    auto* bottomLayout = new QHBoxLayout();
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(12);
    bottomLayout->addStretch();

    // 底部操作按钮改为与「常规设置」页一致的青蓝描边玻璃按钮风格
    m_toolActionBtnStyle = QStringLiteral(
        "QPushButton { color: #FFFFFF; background: rgba(255,255,255,0.04); border: 1px solid rgba(34,211,238,0.55); border-radius: 10px; padding: 6px 14px; font-size: 13px; }"
        "QPushButton:hover { background: rgba(34,211,238,0.12); border-color: #22D3EE; }"
        "QPushButton:pressed { background: rgba(34,211,238,0.22); }"
    );

    auto* restoreBtn = new QPushButton(QStringLiteral("恢复默认排序"));
    restoreBtn->setCursor(Qt::PointingHandCursor);
    restoreBtn->setStyleSheet(Theme::applyTokens(m_toolActionBtnStyle));
    m_toolRestoreBtn = restoreBtn;
    connect(restoreBtn, &QPushButton::clicked, this, &SettingCenterDialog::onRestoreToolsOrder);

    auto* addBtn = new QPushButton(QStringLiteral("+ 添加小工具"));
    addBtn->setCursor(Qt::PointingHandCursor);
    addBtn->setStyleSheet(Theme::applyTokens(m_toolActionBtnStyle));
    m_toolAddBtn = addBtn;
    connect(addBtn, &QPushButton::clicked, this, &SettingCenterDialog::onAddTool);

    auto* removeBtn = new QPushButton(QStringLiteral("- 移除小工具"));
    removeBtn->setCursor(Qt::PointingHandCursor);
    removeBtn->setStyleSheet(Theme::applyTokens(m_toolActionBtnStyle));
    m_toolRemoveBtn = removeBtn;
    connect(removeBtn, &QPushButton::clicked, this, &SettingCenterDialog::onRemoveTool);

    bottomLayout->addWidget(restoreBtn);
    bottomLayout->addWidget(addBtn);
    bottomLayout->addWidget(removeBtn);
    layout->addLayout(bottomLayout);

    return page;
}

// 刷新tools列表
// 作者：谭征
void SettingCenterDialog::refreshToolsList() {
    if (!m_toolsList) return;
    m_isRefreshingToolsList = true;

    // 清理旧 item widget，避免内存泄漏
    for (int i = m_toolsList->count() - 1; i >= 0; --i) {
        auto* item = m_toolsList->item(i);
        QWidget* w = m_toolsList->itemWidget(item);
        if (w) {
            m_toolsList->removeItemWidget(item);
            delete w;
        }
    }
    m_toolsList->clear();

    for (int idx = 0; idx < m_toolsOrder.size(); ++idx) {
        const QString& name = m_toolsOrder.at(idx);
        auto* item = new QListWidgetItem(m_toolsList);
        // 不设置 item 默认文本，避免与自定义 widget 中的 nameLabel 重叠
        item->setData(Qt::UserRole, name);
        item->setSizeHint(QSize(0, 40));

        auto* row = new QWidget();
        row->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 8, 12, 8);
        h->setSpacing(12);

        auto* iconLabel = new QLabel();
        // 使用与快捷工具面板完全一致的原始图标，不做单色覆盖，确保与 QuickToolsWidget 显示一致
        QIcon toolIcon = QuickToolsWidget::toolIcon(name, this);
        iconLabel->setPixmap(toolIcon.pixmap(20, 20));
        iconLabel->setStyleSheet(QStringLiteral("background: transparent;"));
        iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents);

        auto* nameLabel = new QLabel(name);
        QFont nf = nameLabel->font();
        nf.setPointSize(12);
        nameLabel->setFont(nf);
        nameLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        nameLabel->setMinimumHeight(20);
        nameLabel->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent;"));
        nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents);

        // 右侧排序按钮区：仅选中行显示；简单白色箭头、无背景框
        auto* btnContainer = new QWidget();
        btnContainer->setObjectName(QStringLiteral("btnContainer"));
        btnContainer->setStyleSheet(QStringLiteral("background: transparent;"));
        auto* btnLayout = new QHBoxLayout(btnContainer);
        btnLayout->setContentsMargins(0, 0, 0, 0);
        btnLayout->setSpacing(6);

        auto* upBtn = new QToolButton();
        upBtn->setText(QStringLiteral("↑"));
        upBtn->setFixedSize(20, 20);
        upBtn->setToolTip(QStringLiteral("上移"));
        upBtn->setCursor(Qt::PointingHandCursor);
        upBtn->setEnabled(idx > 0);
        upBtn->setStyleSheet(Theme::applyTokens(QStringLiteral(
            "QToolButton { background: transparent; color: rgba(255,255,255,0.70); border: none; font-size: 13px; }"
            "QToolButton:hover { color: #FFFFFF; }"
            "QToolButton:pressed { color: #67E8F9; }"
            "QToolButton:disabled { color: rgba(255,255,255,0.20); }"
        )));
        connect(upBtn, &QToolButton::clicked, this, [this, name]() { moveTool(name, -1); });

        auto* downBtn = new QToolButton();
        downBtn->setText(QStringLiteral("↓"));
        downBtn->setFixedSize(20, 20);
        downBtn->setToolTip(QStringLiteral("下移"));
        downBtn->setCursor(Qt::PointingHandCursor);
        downBtn->setEnabled(idx < m_toolsOrder.size() - 1);
        downBtn->setStyleSheet(Theme::applyTokens(QStringLiteral(
            "QToolButton { background: transparent; color: rgba(255,255,255,0.70); border: none; font-size: 13px; }"
            "QToolButton:hover { color: #FFFFFF; }"
            "QToolButton:pressed { color: #67E8F9; }"
            "QToolButton:disabled { color: rgba(255,255,255,0.20); }"
        )));
        connect(downBtn, &QToolButton::clicked, this, [this, name]() { moveTool(name, 1); });

        btnLayout->addWidget(upBtn);
        btnLayout->addWidget(downBtn);

        h->addWidget(iconLabel, 0, Qt::AlignVCenter);
        h->addWidget(nameLabel, 1, Qt::AlignVCenter);
        h->addWidget(btnContainer, 0, Qt::AlignVCenter);

        m_toolsList->setItemWidget(item, row);
    }

    // 默认选中第一行，使 ↑/↓ 按钮可见（与目标截图一致）
    if (m_toolsList->count() > 0) {
        m_toolsList->setCurrentRow(0);
    }

    m_isRefreshingToolsList = false;
}

// 移动工具
// 作者：谭征
void SettingCenterDialog::moveTool(const QString& name, int delta) {
    int idx = m_toolsOrder.indexOf(name);
    if (idx < 0) return;
    int newIdx = idx + delta;
    if (newIdx < 0 || newIdx >= m_toolsOrder.size()) return;

    m_toolsOrder.swap(idx, newIdx);
    refreshToolsList();

    // 保持被移动项处于选中状态
    for (int i = 0; i < m_toolsList->count(); ++i) {
        auto* item = m_toolsList->item(i);
        if (item && item->data(Qt::UserRole).toString() == name) {
            m_toolsList->setCurrentRow(i);
            break;
        }
    }

    saveSettings();
    emit quickToolsOrderChanged(m_toolsOrder);
}

// 响应tools选中变化信号
// 作者：谭征
void SettingCenterDialog::onToolsSelectionChanged() {
    if (!m_toolsList) return;
    for (int i = 0; i < m_toolsList->count(); ++i) {
        auto* item = m_toolsList->item(i);
        auto* row = m_toolsList->itemWidget(item);
        if (!row) continue;
        auto* btnContainer = row->findChild<QWidget*>(QStringLiteral("btnContainer"));
        if (!btnContainer) continue;
        btnContainer->setVisible(item && item->isSelected());
    }
}

// 响应还原tools顺序
// 作者：谭征
void SettingCenterDialog::onRestoreToolsOrder() {
    m_toolsOrder = QuickToolsWidget::defaultToolNames();
    refreshToolsList();
    saveSettings();
    emit quickToolsOrderChanged(m_toolsOrder);
}

// 响应添加工具
// 作者：谭征
void SettingCenterDialog::onAddTool() {
    const QStringList allTools = QuickToolsWidget::defaultToolNames();
    QStringList missing;
    for (const QString& name : allTools) {
        if (!m_toolsOrder.contains(name)) {
            missing << name;
        }
    }

    if (missing.isEmpty()) {
        GlassMessageBox::information(this, QStringLiteral("添加小工具"),
                                 QStringLiteral("当前已包含全部可用小工具。"));
        return;
    }

    m_toolsOrder.append(missing);
    refreshToolsList();
    saveSettings();
    emit quickToolsOrderChanged(m_toolsOrder);
}

// 响应移除工具
// 作者：谭征
void SettingCenterDialog::onRemoveTool() {
    if (!m_toolsList) return;
    int row = m_toolsList->currentRow();
    if (row < 0 || row >= m_toolsOrder.size()) {
        GlassMessageBox::information(this, QStringLiteral("移除小工具"),
                                 QStringLiteral("请先选择要移除的小工具。"));
        return;
    }

    m_toolsOrder.removeAt(row);
    refreshToolsList();
    saveSettings();
    emit quickToolsOrderChanged(m_toolsOrder);
}

// 响应创建新建盒子
// 作者：谭征
void SettingCenterDialog::onCreateNewBox() {
    emit requestCreateNewBox();
}

// 关于页 Logo：按当前主题色重建位图。
// 为什么需要它：createAboutLogo() 把颜色用 QPainter **烤进了位图**（描边/「谭」字/支架
// 全部取 accentColor()），QPixmap 不会自己跟着 ThemeManager 走。若只在 createAboutPage()
// 里生成一次，用户在设置中心里「改主题色 → 关于页 Logo 仍是打开对话框那一刻的旧色」，
// 与同一窗口内已实时联动的 QSS（侧边选中项、边框）自相矛盾。故构造时 + applyTheme() 各调一次。
// 作者：谭征
void SettingCenterDialog::updateAboutLogo() {
    if (!m_aboutLogo) return;              // 构造早期 applyTheme() 先于 createAboutPage() 时安全跳过
    // 按屏幕缩放比超采样再回缩：125%/150% 缩放的屏幕上按逻辑像素出图会发虚。
    // 逻辑尺寸恒为 160×120（与布局一致），DPR 只影响位图分辨率。
    qreal dpr = 1.0;
    if (QScreen* sc = this->screen()) dpr = sc->devicePixelRatio();
    if (dpr < 1.0) dpr = 1.0;
    QPixmap pm = createAboutLogo(qRound(160 * dpr), qRound(120 * dpr));
    pm.setDevicePixelRatio(dpr);
    m_aboutLogo->setPixmap(pm);
    m_aboutLogo->setFixedSize(160, 120);
}

// 创建about页
// 作者：谭征
QWidget* SettingCenterDialog::createAboutPage() {
    auto* page = new QWidget();
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(18);

    // 复选框样式交由全局主题样式表（Theme::globalStyleSheet，已在 themeChanged 时实时重设）统一提供，
    // 不再硬编码青色，确保「改主题色 → 关于页全部控件实时联动」。

    // 页面标题：与「常规设置」页保持相同的左上角大标题风格
    auto* pageTitle = new QLabel(QStringLiteral("关于我们"));
    QFont tf = pageTitle->font();
    tf.setPointSize(14);
    tf.setBold(true);
    pageTitle->setFont(tf);
    pageTitle->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent; border: none;"));
    root->addWidget(pageTitle);

    // 内容区：分段式垂直布局，移除原来的全宽底栏，融入右侧统一玻璃背景
    auto* content = new QVBoxLayout();
    content->setSpacing(18);
    content->setContentsMargins(0, 0, 0, 0);

    // ---- 应用信息 ----
    auto* infoTitle = new QLabel(QStringLiteral("应用信息"));
    infoTitle->setStyleSheet(QStringLiteral("color: #FFFFFF; font-weight: bold; background: transparent; border: none;"));
    content->addWidget(infoTitle);

    auto* logoLabel = new QLabel();
    m_aboutLogo = logoLabel;                 // 交给 updateAboutLogo 管理（主题色变化时重建）
    logoLabel->setAlignment(Qt::AlignCenter);
    content->addWidget(logoLabel, 0, Qt::AlignCenter);
    updateAboutLogo();

    auto* appName = new QLabel(QStringLiteral("桌面助手"));
    QFont nameFont = appName->font();
    nameFont.setPointSize(20);
    nameFont.setBold(true);
    appName->setFont(nameFont);
    appName->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent; border: none;"));
    appName->setAlignment(Qt::AlignCenter);
    content->addWidget(appName, 0, Qt::AlignCenter);

    auto* subTitle = new QLabel(QStringLiteral("轻巧智桌面 · 提效省时间"));
    subTitle->setStyleSheet(QStringLiteral("color: #FFFFFF; background: transparent; border: none; font-size: 13px;"));
    subTitle->setAlignment(Qt::AlignCenter);
    content->addWidget(subTitle, 0, Qt::AlignCenter);

    auto* versionLayout = new QHBoxLayout();
    versionLayout->setSpacing(12);
    versionLayout->setContentsMargins(0, 8, 0, 0);

    // 版本号 + 作者：左侧竖排两行，右侧「检查更新」与版本号一行对齐
    auto* versionCol = new QVBoxLayout();
    versionCol->setSpacing(4);
    versionCol->setContentsMargins(0, 0, 0, 0);

    auto* versionLabel = new QLabel(QStringLiteral("当前版本：0.1.0"));
    versionLabel->setStyleSheet(QStringLiteral("color: rgba(255,255,255,0.5); background: transparent; border: none; font-size: 12px;"));
    versionCol->addWidget(versionLabel);

    auto* authorLabel = new QLabel(QStringLiteral("作者：谭征"));
    authorLabel->setStyleSheet(QStringLiteral("color: rgba(255,255,255,0.5); background: transparent; border: none; font-size: 12px;"));
    versionCol->addWidget(authorLabel);

    auto* contactLabel = new QLabel(QStringLiteral("联系方式：QQ 906548370"));
    contactLabel->setStyleSheet(QStringLiteral("color: rgba(255,255,255,0.5); background: transparent; border: none; font-size: 12px;"));
    versionCol->addWidget(contactLabel);

    auto* updateBtn = new QPushButton(QStringLiteral("检查更新"));
    updateBtn->setObjectName(QStringLiteral("plainButton"));
    updateBtn->setFixedWidth(100);
    updateBtn->setCursor(Qt::PointingHandCursor);
    connect(updateBtn, &QPushButton::clicked, this, [this]() {
        UpdateDialog dlg(false, QString(), this);
        dlg.exec();
    });

    versionLayout->addLayout(versionCol);
    versionLayout->addStretch();
    versionLayout->addWidget(updateBtn, 0, Qt::AlignTop);
    content->addLayout(versionLayout);

    content->addSpacing(10);

    // ---- 参与计划 ----
    auto* planTitle = new QLabel(QStringLiteral("参与计划"));
    planTitle->setStyleSheet(QStringLiteral("color: #FFFFFF; font-weight: bold; background: transparent; border: none;"));
    content->addWidget(planTitle);

    m_userExperienceCheck = new QCheckBox(QStringLiteral("用户体验改善计划"));
    m_userExperienceCheckStyle = QStringLiteral(
        "QCheckBox { color: #FFFFFF; font-size: 12px; background: transparent; border: none; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border-radius: 3px; border: 1px solid rgba(34,211,238,0.30); background: transparent; }"
        "QCheckBox::indicator:hover { border-color: #22D3EE; }"
        "QCheckBox::indicator:checked { background: transparent; border: 1px solid rgba(34,211,238,0.30); image: url(:/icons/check_plain_1x.png); }"
        "QCheckBox::indicator:checked:hover { border-color: #22D3EE; }"
    );
    m_userExperienceCheck->setStyleSheet(Theme::applyTokens(m_userExperienceCheckStyle));
    connect(m_userExperienceCheck, &QCheckBox::stateChanged, this, &SettingCenterDialog::saveSettings);
    content->addWidget(m_userExperienceCheck);

    content->addSpacing(10);

    // ---- 法律信息 ----
    auto* legalTitle = new QLabel(QStringLiteral("法律信息"));
    legalTitle->setStyleSheet(QStringLiteral("color: #FFFFFF; font-weight: bold; background: transparent; border: none;"));
    content->addWidget(legalTitle);

    auto* legalLayout = new QHBoxLayout();
    legalLayout->setSpacing(16);
    legalLayout->setContentsMargins(0, 0, 0, 0);

    m_agreementBtnStyle = QStringLiteral(
        "QPushButton { background: transparent; color: #FFFFFF; border: none; font-size: 12px; padding: 0; }"
        "QPushButton:hover { color: #22D3EE; text-decoration: underline; }"
    );
    auto* agreementBtn = new QPushButton(QStringLiteral("许可使用协议"));
    agreementBtn->setCursor(Qt::PointingHandCursor);
    agreementBtn->setStyleSheet(Theme::applyTokens(m_agreementBtnStyle));
    m_agreementBtn = agreementBtn;
    connect(agreementBtn, &QPushButton::clicked, this, [this]() {
        GlassMessageBox::information(this, QStringLiteral("许可使用协议"), QStringLiteral("许可使用协议内容待完善。"));
    });

    auto* privacyBtn = new QPushButton(QStringLiteral("隐私保护说明"));
    privacyBtn->setCursor(Qt::PointingHandCursor);
    privacyBtn->setStyleSheet(Theme::applyTokens(m_agreementBtnStyle));
    m_privacyBtn = privacyBtn;
    connect(privacyBtn, &QPushButton::clicked, this, [this]() {
        GlassMessageBox::information(this, QStringLiteral("隐私保护说明"), QStringLiteral("隐私保护说明内容待完善。"));
    });

    legalLayout->addWidget(agreementBtn);
    legalLayout->addWidget(privacyBtn);
    legalLayout->addStretch();
    content->addLayout(legalLayout);

    root->addLayout(content);
    root->addStretch();

    return page;
}

// 响应sidebar当前行变化信号
// 作者：谭征
void SettingCenterDialog::onSidebarCurrentRowChanged(int row) {
    if (m_stack && row >= 0 && row < m_stack->count()) {
        m_stack->setCurrentIndex(row);
    }
}

// 按当前色板（含用户自定义的颜色）重刷全部色块的底色 / 选中描边 / 悬停提示。
// applyTheme（主题色或透明度变化）与自定义颜色之后都要调用，保证显示与色板实时一致。
// 作者：谭征
void SettingCenterDialog::refreshSwatchStyles() {
    if (!m_colorGroup) return;
    for (int i = 0; i < s_colorNames.size(); ++i) {
        QWidget* b = m_colorGroup->button(i);
        if (!b) continue;
        const QString color = s_colorNames.at(i);
        b->setStyleSheet(Theme::applyTokens(QStringLiteral(
            "QToolButton { background-color: %1; border: 2px solid %2; border-radius: 4px; }"
            "QToolButton:checked { border: 2px solid #22D3EE; }"
        ).arg(color).arg(Theme::panelBgString(0.6))));
        b->setToolTip(QStringLiteral("%1\n左键：应用为主题色\n右键：自定义此格颜色").arg(color));
    }
}

// 以本窗为 owner 弹出系统色卡（阻塞），返回 "#RRGGBB"；用户取消返回空串。
// 与 pickFileModal 完全同一套 z 序方案：打开期间停用 nativeEvent 里的 z 序冻结，
// 让 Windows 得以把 owner（本窗）重排到色卡之下，从而保证色卡恒在本窗之上、不被遮挡。
// 作者：谭征
QString SettingCenterDialog::pickColorModal(const QString& initialHex, const QString& title) {
    m_suppressZOrderClamp = true;
    const QColor picked = QColorDialog::getColor(QColor(initialHex), this, title);
    m_suppressZOrderClamp = false;
    return picked.isValid() ? picked.name(QColor::HexRgb).toUpper() : QString();
}

// 右键色块：自定义第 index 格颜色。若该格正是当前生效的主题色，同步预览块与全局 UI；
// 最后由 saveSettings() 落盘（含整块色板）。用户取消则不做任何改动。
// 作者：谭征
void SettingCenterDialog::onCustomizeSwatch(int index) {
    if (index < 0 || index >= s_colorNames.size()) return;

    const QString picked = pickColorModal(s_colorNames.at(index), QStringLiteral("自定义颜色"));
    if (picked.isEmpty()) return;

    s_colorNames[index] = picked;
    refreshSwatchStyles();

    if (m_colorGroup && m_colorGroup->checkedId() == index) {
        if (m_colorPreview) {
            m_colorPreview->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 4px;")
                                              .arg(s_colorNames.at(index)));
        }
        // 主题色联动：更新全局 ThemeManager，触发「全部 UI 界面」颜色重绘
        ThemeManager::instance()->setAccentColor(QColor(s_colorNames.at(index)));
    }
    saveSettings();
}

// 读取用户自定义色板（Appearance/palette：逗号分隔的 16 个 #RRGGBB）。
// 为空（从未自定义）、格数与当前色板不符（跨版本）或全部非法时，整体忽略、沿用出厂色板。
// 作者：谭征
void SettingCenterDialog::loadPalette() {
    SettingsManager sm;
    const QString raw = sm.loadValue(QStringLiteral("Appearance/palette")).toString();
    if (raw.isEmpty()) return;
    const QStringList parts = raw.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.size() != s_colorNames.size()) return;
    for (int i = 0; i < parts.size(); ++i) {
        const QColor c(parts.at(i).trimmed());
        if (c.isValid()) s_colorNames[i] = c.name(QColor::HexRgb).toUpper();
    }
}

// 响应颜色selected
// 作者：谭征
void SettingCenterDialog::onColorSelected(int id) {
    if (id < 0 || id >= s_colorNames.size()) return;
    QString color = s_colorNames.at(id);
    if (m_colorPreview) {
        m_colorPreview->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 4px;").arg(color));
    }
    // 主题色联动：更新全局 ThemeManager，触发「全部 UI 界面」颜色重绘
    ThemeManager::instance()->setAccentColor(QColor(color));
    saveSettings();
}

// 响应透明度变化信号
// 作者：谭征
void SettingCenterDialog::onTransparencyChanged(int value) {
    if (m_transparencyLabel) {
        m_transparencyLabel->setText(QStringLiteral("%1%").arg(value));
    }
    m_pendingTransparency = value;
    // 节流（详见头文件 m_透明度Timer）：不在静默窗口内就立刻下发一次，保证首帧即时响应；
    // 窗口内的后续高频变化只记值，由窗口结束时的 timeout → flushTransparency() 合并补发。
    if (!m_transparencyTimer || !m_transparencyTimer->isActive()) {
        flushTransparency();
        if (m_transparencyTimer) m_transparencyTimer->start();
    }
    // 落盘挂这里（而不是 sliderReleased）：valueChanged 携带的一定是用户最终选定的值。
    // 注意滑块是 setTracking(false) 的 → 拖动过程中该信号只发一次（松手时），滑槽点击也只发一次，
    // 所以写盘频率仍被限制在「一次操作一次」，不会回到"每像素写盘"的卡顿老路。
    saveSettings();
}

// 把「待下发」的透明度真正交给 ThemeManager（触发全 UI 不透明度重绘）。
// 作者：谭征
void SettingCenterDialog::flushTransparency() {
    if (m_pendingTransparency < 0) return;
    const int value = m_pendingTransparency;
    m_pendingTransparency = -1;
    ThemeManager::instance()->setTransparency(value);
}

// 以本对话框为 owner 弹出系统原生文件选择框。
// 关键：打开期间把 m_suppressZOrderClamp 置真，令 nativeEvent 里的 z 序冻结（clampBandZOrder）
// 失效。原因——本窗是文件框的 owner，系统需要把 owner（本窗）重排到文件框之下，来满足
// "被 owner 的窗口恒在 owner 之上"；若此时仍冻结本窗 z 序（clampBandZOrder 会加 SWP_NOZORDER），
// owner 拒绝下移 → 二者排序错乱 → 文件框反而落到本窗之后被遮住（历史多次反馈的"选择框被设置中心遮住"）。
// 交还 z 序给系统按 owner 关系处理后，文件框自然浮在本窗之上，且仍非 TOPMOST，不遮挡其它程序。
// 作者：谭征
QString SettingCenterDialog::pickFileModal(const QString& title, const QString& filter) {
    m_suppressZOrderClamp = true;
    const QString file = QFileDialog::getOpenFileName(this, title, QString(), filter);
    m_suppressZOrderClamp = false;
    return file;
}

// 响应choose图像
// 作者：谭征
void SettingCenterDialog::onChooseImage() {
    const QString file = pickFileModal(QStringLiteral("选择背景图片"),
                                       QStringLiteral("图片文件 (*.png *.jpg *.jpeg *.bmp)"));
    if (!file.isEmpty() && m_bgImageEdit) {
        m_bgImageEdit->setText(file);
        saveSettings();
    }
}

// 响应chooseremindsound
// 作者：谭征
void SettingCenterDialog::onChooseRemindSound() {
    const QString file = pickFileModal(QStringLiteral("选择提醒声音"),
                                       QStringLiteral("声音文件 (*.wav *.mp3 *.wma *.m4a *.flac)"));
    if (!file.isEmpty() && m_remindSoundEdit) {
        m_remindSoundEdit->setText(file);
        saveSettings();
    }
}

// 响应还原默认
// 作者：谭征
void SettingCenterDialog::onRestoreDefault() {
    if (m_partitionCombo) m_partitionCombo->setCurrentIndex(0);
    // 色板整块恢复出厂（清掉用户右键自定义过的格子），并重刷色块显示
    s_colorNames = s_defaultColorNames;
    refreshSwatchStyles();
    if (m_colorGroup) {
        QAbstractButton* defaultBtn = m_colorGroup->button(0);
        if (defaultBtn) defaultBtn->setChecked(true);
    }
    // 预览块同步为默认主题色（第 0 格）。此前写死 #FFFFFF，与第 0 格实际颜色不符。
    if (m_colorPreview) {
        m_colorPreview->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 4px;")
                                          .arg(s_colorNames.value(0, QStringLiteral("#22D3EE"))));
    }
    // 恢复默认 = 完全不透明（默认档位 100；透明度由用户自行调整）
    if (m_transparencySlider) m_transparencySlider->setValue(100);
    if (m_transparencyLabel) m_transparencyLabel->setText(QStringLiteral("100%"));
    if (m_bgImageEdit) m_bgImageEdit->clear();
    if (m_remindSoundEdit) m_remindSoundEdit->clear();
    if (m_fontCombo) m_fontCombo->setCurrentIndex(0);
    if (m_tagClickRadio) m_tagClickRadio->setChecked(true);
    if (m_menuAlwaysShowRadio) m_menuAlwaysShowRadio->setChecked(true);
    if (m_boxBorderCheck) m_boxBorderCheck->setChecked(true);
    if (m_boxRoundCheck) m_boxRoundCheck->setChecked(false);
    if (m_shortcutArrowCheck) m_shortcutArrowCheck->setChecked(true);
    if (m_autoExpandOnHoverCheck) m_autoExpandOnHoverCheck->setChecked(false);
    if (m_remindSoundCheck) m_remindSoundCheck->setChecked(true);
    saveSettings();
}

// 加载设置
// 作者：谭征
void SettingCenterDialog::loadSettings() {
    loadGeneralSettings();
    // 先恢复用户自定义色板：后面的 colorIndex / accent 解析都基于它，
    // 否则被自定义过的那一格会被当成出厂色，预览块与实际主题色不一致。
    loadPalette();

    SettingsManager sm;
    int colorIndex = sm.loadValue(QStringLiteral("Appearance/colorIndex"), 0).toInt();
    int transparency = sm.loadValue(QStringLiteral("Appearance/transparency"), 100).toInt();
    QString bgImage = sm.loadValue(QStringLiteral("Appearance/bgImage")).toString();
    QString remindSound = sm.loadValue(QStringLiteral("Appearance/remindSound")).toString();
    int fontIndex = sm.loadValue(QStringLiteral("Appearance/fontIndex"), 0).toInt();
    int partitionIndex = sm.loadValue(QStringLiteral("Appearance/partitionIndex"), 0).toInt();
    int tagSwitchMode = sm.loadValue(QStringLiteral("Appearance/tagSwitchMode"), 0).toInt();
    int menuLabelShow = sm.loadValue(QStringLiteral("Appearance/menuLabelShow"), 0).toInt();
    bool boxBorder = sm.loadValue(QStringLiteral("Appearance/boxBorder"), true).toBool();
    bool boxRound = sm.loadValue(QStringLiteral("Appearance/boxRound"), false).toBool();
    bool shortcutArrow = sm.loadValue(QStringLiteral("Appearance/shortcutArrow"), true).toBool();
    bool autoExpandOnHover = sm.loadValue(QStringLiteral("Appearance/autoExpandOnHover"), false).toBool();
    // 「闹铃」默认 true＝出声（保持未引入该开关前的行为）
    bool remindSoundEnabled = sm.loadValue(QStringLiteral("Appearance/remindSoundEnabled"), true).toBool();

    bool hideIconsOnDoubleClick = sm.loadValue(QStringLiteral("QuickActions/hideIconsOnDoubleClick"), true).toBool();
    bool drawBoxOnBlank = sm.loadValue(QStringLiteral("QuickActions/drawBoxOnBlank"), true).toBool();

    if (m_colorGroup) {
        QAbstractButton* btn = m_colorGroup->button(colorIndex);
        if (btn) btn->setChecked(true);
        else if (m_colorGroup->button(0)) m_colorGroup->button(0)->setChecked(true);
    }
    // 启动时把已保存的主题色同步到 ThemeManager，确保「外观设置」与全局主题一致
    if (colorIndex >= 0 && colorIndex < s_colorNames.size()) {
        ThemeManager::instance()->setAccentColor(QColor(s_colorNames.at(colorIndex)));
    }
    if (m_colorPreview && colorIndex >= 0 && colorIndex < s_colorNames.size()) {
        m_colorPreview->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 4px;").arg(s_colorNames.at(colorIndex)));
    }
    if (m_transparencySlider) m_transparencySlider->setValue(qBound(0, transparency, 100));
    if (m_transparencyLabel) m_transparencyLabel->setText(QStringLiteral("%1%").arg(qBound(0, transparency, 100)));
    if (m_bgImageEdit) m_bgImageEdit->setText(bgImage);
    if (m_remindSoundEdit) m_remindSoundEdit->setText(remindSound);
    if (m_fontCombo) m_fontCombo->setCurrentIndex(qBound(0, fontIndex, m_fontCombo->count() - 1));
    if (m_partitionCombo) m_partitionCombo->setCurrentIndex(qBound(0, partitionIndex, m_partitionCombo->count() - 1));
    if (m_tagClickRadio) m_tagClickRadio->setChecked(tagSwitchMode == 0);
    if (m_tagHoverRadio) m_tagHoverRadio->setChecked(tagSwitchMode == 1);
    if (m_menuAlwaysShowRadio) m_menuAlwaysShowRadio->setChecked(menuLabelShow == 0);
    if (m_menuHoverShowRadio) m_menuHoverShowRadio->setChecked(menuLabelShow == 1);
    if (m_boxBorderCheck) m_boxBorderCheck->setChecked(boxBorder);
    if (m_boxRoundCheck) m_boxRoundCheck->setChecked(boxRound);
    if (m_shortcutArrowCheck) m_shortcutArrowCheck->setChecked(shortcutArrow);
    if (m_autoExpandOnHoverCheck) m_autoExpandOnHoverCheck->setChecked(autoExpandOnHover);
    if (m_remindSoundCheck) m_remindSoundCheck->setChecked(remindSoundEnabled);
    if (m_hideIconsOnDoubleClickCheck) m_hideIconsOnDoubleClickCheck->setChecked(hideIconsOnDoubleClick);
    if (m_drawBoxOnBlankCheck) m_drawBoxOnBlankCheck->setChecked(drawBoxOnBlank);

    const QStringList defaultShortcuts = {
        QStringLiteral("无"), QStringLiteral("无"), QStringLiteral("无"),
        QStringLiteral("无"), QStringLiteral("无"), QStringLiteral("无"),
        QStringLiteral("无"), QStringLiteral("无")
    };
    QStringList savedShortcuts = sm.loadValue(QStringLiteral("QuickActions/shortcuts"), defaultShortcuts).toStringList();
    if (m_shortcutTable) {
        for (int row = 0; row < m_shortcutTable->rowCount(); ++row) {
            auto* edit = qobject_cast<ShortcutEdit*>(m_shortcutTable->cellWidget(row, 1));
            if (edit) {
                // 加载时阻塞信号，避免 setShortcutText 触发 textChanged/editingFinished
                // 导致在 m_isLoading 保护之外意外回写默认值。
                edit->blockSignals(true);
                edit->setShortcutText(row < savedShortcuts.size() ? savedShortcuts.at(row) : QString());
                edit->blockSignals(false);
            }
        }
    }

    if (m_userExperienceCheck) {
        m_userExperienceCheck->setChecked(sm.loadValue(QStringLiteral("About/userExperiencePlan"), true).toBool());
    }

    // 桌面整理页：恢复整理模式、固定分区及各规则行分区/启用状态
    const QVariant modeValue = sm.loadValue(QStringLiteral("Organize/mode"), QVariant());
    const bool modeKeyExisted = !modeValue.isNull();
    int organizeMode = modeKeyExisted ? modeValue.toInt() : 0;
    int fixedPartition = sm.loadValue(QStringLiteral("Organize/fixedPartition"), 0).toInt();
    if (m_ruleModeRadio && m_fixedModeRadio) {
        m_ruleModeRadio->setChecked(organizeMode == 0);
        m_fixedModeRadio->setChecked(organizeMode == 1);
    }
    if (m_fixedPartitionCombo) {
        m_fixedPartitionCombo->setCurrentIndex(qBound(0, fixedPartition, m_fixedPartitionCombo->count() - 1));
    }
    QVariantList rulePartitions = sm.loadValue(QStringLiteral("Organize/rulePartitions"), QVariantList()).toList();
    QVariantList ruleEnabled = sm.loadValue(QStringLiteral("Organize/ruleEnabled"), QVariantList()).toList();
    if (m_ruleTable) {
        const int rowCount = m_ruleTable->rowCount();
        for (int row = 0; row < rowCount; ++row) {
            auto* combo = qobject_cast<QComboBox*>(m_ruleTable->cellWidget(row, 2));
            if (combo && row < rulePartitions.size()) {
                combo->setCurrentIndex(qBound(0, rulePartitions.at(row).toInt(), combo->count() - 1));
            }
            auto* container = qobject_cast<QWidget*>(m_ruleTable->cellWidget(row, 3));
            QCheckBox* check = container ? container->findChild<QCheckBox*>() : nullptr;
            if (check && row < ruleEnabled.size()) {
                check->setChecked(ruleEnabled.at(row).toInt() != 0);
            }
        }
    }

    // 首次打开设置中心且尚未保存整理策略时，把当前界面上的默认值持久化。
    // 这样用户即使在“桌面整理”页没有手动改动，回到主界面点击“桌面整理”也能立即按规则生效。
    if (!modeKeyExisted && m_ruleTable) {
        QVariantList defaultPartitions;
        QVariantList defaultEnabled;
        const int rowCount = m_ruleTable->rowCount();
        for (int row = 0; row < rowCount; ++row) {
            defaultPartitions << row;
            auto* container = qobject_cast<QWidget*>(m_ruleTable->cellWidget(row, 3));
            QCheckBox* check = container ? container->findChild<QCheckBox*>() : nullptr;
            defaultEnabled << ((check && check->isChecked()) ? 1 : 0);
        }
        sm.saveValue(QStringLiteral("Organize/mode"), organizeMode);
        sm.saveValue(QStringLiteral("Organize/fixedPartition"), fixedPartition);
        sm.saveValue(QStringLiteral("Organize/rulePartitions"), defaultPartitions);
        sm.saveValue(QStringLiteral("Organize/ruleEnabled"), defaultEnabled);
        sm.sync();
    }

    // 加载小工具顺序：只保留默认工具集合中存在的项，不再自动补全，
    // 保证「移除小工具」后设置中心与主界面快捷工具内容严格一致。
    m_toolsOrder = sm.loadValue(QStringLiteral("QuickTools/order"), QuickToolsWidget::defaultToolNames()).toStringList();
    QSet<QString> valid = QuickToolsWidget::defaultToolNames().toSet();
    QStringList filtered;
    for (const QString& name : m_toolsOrder) {
        if (valid.contains(name)) {
            filtered << name;
            valid.remove(name);
        }
    }
    m_toolsOrder = filtered;
    // 若配置为空（例如此前已全部移除小工具并保存），回退到默认集合，
    // 避免「我的功能」一片空白；与主界面 QuickToolsWidget::setupUi 的
    // 空值回退保持一致，确保设置中心与主界面快捷工具内容相同。
    if (m_toolsOrder.isEmpty()) {
        m_toolsOrder = QuickToolsWidget::defaultToolNames();
    }
    refreshToolsList();
}

// 保存设置
// 作者：谭征
void SettingCenterDialog::saveSettings() {
    // 加载/构造期间（m_isLoading=true）控件会批量触发 textChanged/stateChanged，
    // 此时直接返回，避免把「尚未加载完成」的半成品状态写回磁盘。
    if (m_isLoading) {
        return;
    }
    saveGeneralSettings();

    SettingsManager sm;
    int colorIndex = m_colorGroup ? m_colorGroup->checkedId() : 0;
    sm.saveValue(QStringLiteral("Appearance/colorIndex"), colorIndex);
    // 整块色板一并落盘（含用户右键自定义过的格子；未自定义时写的即出厂值，无副作用）。
    // 存整块而非逐格键：格子数变化时只需比对数量即可整体忽略，不会残留错位。
    sm.saveValue(QStringLiteral("Appearance/palette"), s_colorNames.join(QLatin1Char(',')));
    // 同时保存所选主题色 hex，供 main.cpp 启动时作为 ThemeManager 初始主题色
    if (colorIndex >= 0 && colorIndex < s_colorNames.size()) {
        sm.saveValue(QStringLiteral("Appearance/accent"), QColor(s_colorNames.at(colorIndex)).name());
    }
    sm.saveValue(QStringLiteral("Appearance/transparency"), m_transparencySlider ? m_transparencySlider->value() : 100);
    sm.saveValue(QStringLiteral("Appearance/bgImage"), m_bgImageEdit ? m_bgImageEdit->text() : QString());
    sm.saveValue(QStringLiteral("Appearance/remindSound"), m_remindSoundEdit ? m_remindSoundEdit->text() : QString());
    sm.saveValue(QStringLiteral("Appearance/fontIndex"), m_fontCombo ? m_fontCombo->currentIndex() : 0);
    sm.saveValue(QStringLiteral("Appearance/partitionIndex"), m_partitionCombo ? m_partitionCombo->currentIndex() : 0);
    int tagSwitchMode = (m_tagHoverRadio && m_tagHoverRadio->isChecked()) ? 1 : 0;
    int menuLabelShow = (m_menuHoverShowRadio && m_menuHoverShowRadio->isChecked()) ? 1 : 0;
    sm.saveValue(QStringLiteral("Appearance/tagSwitchMode"), tagSwitchMode);
    // 写盘后立即广播：已打开的收纳盒/主窗口就地热更新（重读该键即可），不必重开窗口或重启。
    // 只广播本项 —— 收不到自己 key 的窗口会直接返回，零副作用。
    ThemeManager::notifySettingChanged(QStringLiteral("Appearance/tagSwitchMode"), tagSwitchMode);
    sm.saveValue(QStringLiteral("Appearance/menuLabelShow"), menuLabelShow);
    // 「分区菜单标签显示」同样写盘后立即广播：已打开的收纳盒窗口头部按钮 + 桌面助手标题栏按钮
    // 就地切换为始终显示/悬停显示，不必重开窗口或重启。
    ThemeManager::notifySettingChanged(QStringLiteral("Appearance/menuLabelShow"), menuLabelShow);
    // 「其他」区 4 个外观复选（2026-09-22 接通）
    // 四项此前都是**空设置**：只有本文件在读写在自娱自乐，全项目无消费端 → 勾了毫无反应。
    // 现在一律「先写盘、再广播」，由 ThemeManager::notifySettingChanged 触发就地热更新：
    // · boxBorder / boxRound —— 值写进样式串，ThemeManager 会顺带重下发一次样式（reloadAppearanceFlags）；
    // · shortcutArrow        —— DesktoScanner 图标缓存 + Dock/各收纳盒窗口就地重取图标；
    // · autoExpandOnHover    —— 各窗口 hover 分支实时读取，无需重刷。
    const bool boxBorder = m_boxBorderCheck ? m_boxBorderCheck->isChecked() : true;
    const bool boxRound = m_boxRoundCheck ? m_boxRoundCheck->isChecked() : false;
    const bool shortcutArrow = m_shortcutArrowCheck ? m_shortcutArrowCheck->isChecked() : true;
    const bool autoExpandOnHover = m_autoExpandOnHoverCheck ? m_autoExpandOnHoverCheck->isChecked() : false;
    sm.saveValue(QStringLiteral("Appearance/boxBorder"), boxBorder);
    ThemeManager::notifySettingChanged(QStringLiteral("Appearance/boxBorder"), boxBorder);
    sm.saveValue(QStringLiteral("Appearance/boxRound"), boxRound);
    ThemeManager::notifySettingChanged(QStringLiteral("Appearance/boxRound"), boxRound);
    sm.saveValue(QStringLiteral("Appearance/shortcutArrow"), shortcutArrow);
    ThemeManager::notifySettingChanged(QStringLiteral("Appearance/shortcutArrow"), shortcutArrow);
    sm.saveValue(QStringLiteral("Appearance/autoExpandOnHover"), autoExpandOnHover);
    ThemeManager::notifySettingChanged(QStringLiteral("Appearance/autoExpandOnHover"), autoExpandOnHover);
    // 「闹铃」（待办提醒是否出声）：消费端在提醒到期时才读盘（SidePanelWidget::reminderDue），
    // 广播只为口径一致，让人不必重启即可生效。
    const bool remindSoundEnabled = m_remindSoundCheck ? m_remindSoundCheck->isChecked() : true;
    sm.saveValue(QStringLiteral("Appearance/remindSoundEnabled"), remindSoundEnabled);
    ThemeManager::notifySettingChanged(QStringLiteral("Appearance/remindSoundEnabled"), remindSoundEnabled);
    sm.saveValue(QStringLiteral("QuickActions/hideIconsOnDoubleClick"), m_hideIconsOnDoubleClickCheck ? m_hideIconsOnDoubleClickCheck->isChecked() : true);
    sm.saveValue(QStringLiteral("QuickActions/drawBoxOnBlank"), m_drawBoxOnBlankCheck ? m_drawBoxOnBlankCheck->isChecked() : true);
    // 消费端是常驻的 Dock（DesktopMirrorWindow）：它在构造期读一次并缓存（鼠标钩子路径禁做
    // 文件 I/O），所以改完必须广播出去，否则要重启才生效（＝“勾了没反应”）。
    ThemeManager::notifySettingChanged(QStringLiteral("QuickActions/hideIconsOnDoubleClick"),
                                       m_hideIconsOnDoubleClickCheck ? m_hideIconsOnDoubleClickCheck->isChecked() : true);
    ThemeManager::notifySettingChanged(QStringLiteral("QuickActions/drawBoxOnBlank"),
                                       m_drawBoxOnBlankCheck ? m_drawBoxOnBlankCheck->isChecked() : true);

    QStringList shortcutValues;
    if (m_shortcutTable) {
        for (int row = 0; row < m_shortcutTable->rowCount(); ++row) {
            auto* edit = qobject_cast<ShortcutEdit*>(m_shortcutTable->cellWidget(row, 1));
            shortcutValues << (edit ? edit->shortcutText() : QString());
        }
    }
    sm.saveValue(QStringLiteral("QuickActions/shortcuts"), shortcutValues);

    // 桌面整理页：保存整理模式、固定分区及各规则行分区/启用状态
    if (m_ruleModeRadio && m_fixedModeRadio) {
        sm.saveValue(QStringLiteral("Organize/mode"), m_fixedModeRadio->isChecked() ? 1 : 0);
    }
    if (m_fixedPartitionCombo) {
        sm.saveValue(QStringLiteral("Organize/fixedPartition"), m_fixedPartitionCombo->currentIndex());
    }
    if (m_ruleTable) {
        QVariantList rulePartitions;
        QVariantList ruleEnabled;
        const int rowCount = m_ruleTable->rowCount();
        for (int row = 0; row < rowCount; ++row) {
            auto* combo = qobject_cast<QComboBox*>(m_ruleTable->cellWidget(row, 2));
            rulePartitions << (combo ? combo->currentIndex() : 0);
            auto* container = qobject_cast<QWidget*>(m_ruleTable->cellWidget(row, 3));
            QCheckBox* check = container ? container->findChild<QCheckBox*>() : nullptr;
            ruleEnabled << (check ? (check->isChecked() ? 1 : 0) : 0);
        }
        sm.saveValue(QStringLiteral("Organize/rulePartitions"), rulePartitions);
        sm.saveValue(QStringLiteral("Organize/ruleEnabled"), ruleEnabled);
    }

    sm.saveValue(QStringLiteral("About/userExperiencePlan"),
                  m_userExperienceCheck ? m_userExperienceCheck->isChecked() : true);

    // 保存小工具顺序
    sm.saveValue(QStringLiteral("QuickTools/order"), m_toolsOrder);

    // 显式刷写到磁盘：QSettings 默认在对象析构时才落盘，对话框快速关闭或
    // 程序退出时可能来不及写入，此处强制 sync 以保证配置立即持久化。
    sm.sync();
}

// 加载general设置
// 作者：谭征
void SettingCenterDialog::loadGeneralSettings() {
    SettingsManager sm;
    if (m_autoStartCheck) m_autoStartCheck->setChecked(sm.loadValue(QStringLiteral("General/autoStart"), true).toBool());
    if (m_largeTimeFontCheck) m_largeTimeFontCheck->setChecked(sm.loadValue(QStringLiteral("General/largeTimeFont"), true).toBool());
    if (m_showSecondsCheck) m_showSecondsCheck->setChecked(sm.loadValue(QStringLiteral("General/showSeconds"), true).toBool());
    if (m_showMainWindowCheck) m_showMainWindowCheck->setChecked(sm.loadValue(QStringLiteral("General/showMainWindow"), true).toBool());
    if (m_showCompletedCheck) m_showCompletedCheck->setChecked(sm.loadValue(QStringLiteral("General/showCompleted"), true).toBool());
    if (m_showClockWeatherCheck) m_showClockWeatherCheck->setChecked(sm.loadValue(QStringLiteral("General/showClockWeather"), true).toBool());
    if (m_showWidgetsCheck) m_showWidgetsCheck->setChecked(sm.loadValue(QStringLiteral("General/showWidgets"), true).toBool());
    if (m_followStartCheck) m_followStartCheck->setChecked(sm.loadValue(QStringLiteral("General/followStart"), false).toBool());

    QString province = sm.loadValue(QStringLiteral("General/weatherProvinceName"), QStringLiteral("陕西")).toString();
    QString city = sm.loadValue(QStringLiteral("General/weatherCityName"), QString()).toString();
    QString district = sm.loadValue(QStringLiteral("General/weatherDistrictName"), QString()).toString();
    // 首次使用未保存城市/区县时，回退到该省第一个市、该市第一个区县
    if (city.isEmpty()) {
        QStringList cs = RegionData::cities(province);
        if (!cs.isEmpty()) city = cs.first();
    }
    if (district.isEmpty()) {
        QStringList ds = RegionData::districts(province, city);
        if (!ds.isEmpty()) district = ds.first();
    }
    applyRegionSelection(province, city, district);
}

// 保存general设置
// 作者：谭征
void SettingCenterDialog::saveGeneralSettings() {
    SettingsManager sm;
    // 记录天气区域旧值，用于判断是否需要通知面板重新获取天气
    QString oldProvince = sm.loadValue(QStringLiteral("General/weatherProvinceName"), QString()).toString();
    QString oldCity = sm.loadValue(QStringLiteral("General/weatherCityName"), QString()).toString();
    QString oldDistrict = sm.loadValue(QStringLiteral("General/weatherDistrictName"), QString()).toString();

    sm.saveValue(QStringLiteral("General/autoStart"), m_autoStartCheck ? m_autoStartCheck->isChecked() : true);
    sm.saveValue(QStringLiteral("General/largeTimeFont"), m_largeTimeFontCheck ? m_largeTimeFontCheck->isChecked() : true);
    sm.saveValue(QStringLiteral("General/showSeconds"), m_showSecondsCheck ? m_showSecondsCheck->isChecked() : true);
    sm.saveValue(QStringLiteral("General/showMainWindow"), m_showMainWindowCheck ? m_showMainWindowCheck->isChecked() : true);
    sm.saveValue(QStringLiteral("General/showCompleted"), m_showCompletedCheck ? m_showCompletedCheck->isChecked() : true);
    sm.saveValue(QStringLiteral("General/showClockWeather"), m_showClockWeatherCheck ? m_showClockWeatherCheck->isChecked() : true);
    sm.saveValue(QStringLiteral("General/showWidgets"), m_showWidgetsCheck ? m_showWidgetsCheck->isChecked() : true);
    sm.saveValue(QStringLiteral("General/followStart"), m_followStartCheck ? m_followStartCheck->isChecked() : false);
    sm.saveValue(QStringLiteral("General/weatherProvinceName"), m_provinceCombo ? m_provinceCombo->currentText() : QString());
    sm.saveValue(QStringLiteral("General/weatherCityName"), m_cityCombo ? m_cityCombo->currentText() : QString());
    sm.saveValue(QStringLiteral("General/weatherDistrictName"), m_districtCombo ? m_districtCombo->currentText() : QString());

    // 天气区域（省/市/区县）发生变化时，通知桌面助手面板重新获取天气
    QString newProvince = m_provinceCombo ? m_provinceCombo->currentText() : QString();
    QString newCity = m_cityCombo ? m_cityCombo->currentText() : QString();
    QString newDistrict = m_districtCombo ? m_districtCombo->currentText() : QString();
    if (newProvince != oldProvince || newCity != oldCity || newDistrict != oldDistrict) {
        emit weatherRegionChanged();
    }

    // 同步 Windows 注册表中的开机启动项
    applyAutoStart(m_autoStartCheck ? m_autoStartCheck->isChecked() : true);

    // 强制立即落盘：QSettings 默认在对象析构时才 flush，若对话框快速关闭
    // 或程序被异常终止，可能导致 General 配置回退为旧值。
    sm.sync();
}

// 刷新市级下拉：根据所选省列出对应全部市（清空并重新填充）
// 作者：谭征
void SettingCenterDialog::refreshCityCombo(const QString& province) {
    if (!m_cityCombo) return;
    m_cityCombo->clear();
    m_cityCombo->addItems(RegionData::cities(province));
}

// 刷新区县级下拉：根据所选省、市列出对应全部区/县
// 作者：谭征
void SettingCenterDialog::refreshDistrictCombo(const QString& province, const QString& city) {
    if (!m_districtCombo) return;
    m_districtCombo->clear();
    m_districtCombo->addItems(RegionData::districts(province, city));
}

// 联动：省变化时刷新市，并默认选中第一个市（进而刷新区县）
// 作者：谭征
void SettingCenterDialog::onProvinceChanged(const QString& province) {
    if (!m_cityCombo) return;
    refreshCityCombo(province);
    if (m_cityCombo->count() > 0) {
        m_cityCombo->setCurrentIndex(0);   // 触发 onCityChanged 刷新区县
        refreshDistrictCombo(province, m_cityCombo->currentText());
    } else {
        refreshDistrictCombo(province, QString());
    }
    // 用户手动切换区域，标记为已设置，避免启动时 IP 自动定位再次覆盖
    SettingsManager sm;
    sm.saveValue(QStringLiteral("General/weatherRegionSet"), true);
    sm.sync();
    saveGeneralSettings();
}

// 联动：市变化时刷新对应区县
// 作者：谭征
void SettingCenterDialog::onCityChanged(const QString& city) {
    if (!m_provinceCombo) return;
    refreshDistrictCombo(m_provinceCombo->currentText(), city);
    // 用户手动切换区域，标记为已设置
    SettingsManager sm;
    sm.saveValue(QStringLiteral("General/weatherRegionSet"), true);
    sm.sync();
    saveGeneralSettings();
}

// 按保存的文本统一设置三级选择（block 信号避免级联触发）
// 作者：谭征
void SettingCenterDialog::applyRegionSelection(const QString& province,
                                               const QString& city,
                                               const QString& district) {
    if (!m_provinceCombo) return;
    m_provinceCombo->blockSignals(true);
    if (m_cityCombo) m_cityCombo->blockSignals(true);
    if (m_districtCombo) m_districtCombo->blockSignals(true);

    int pi = m_provinceCombo->findText(province);
    if (pi < 0) pi = 0;
    m_provinceCombo->setCurrentIndex(pi);

    refreshCityCombo(m_provinceCombo->currentText());

    int ci = m_cityCombo ? m_cityCombo->findText(city) : -1;
    if (ci < 0) ci = 0;
    if (m_cityCombo) m_cityCombo->setCurrentIndex(ci);

    refreshDistrictCombo(m_provinceCombo->currentText(),
                         m_cityCombo ? m_cityCombo->currentText() : QString());

    int di = m_districtCombo ? m_districtCombo->findText(district) : -1;
    if (di < 0) di = 0;
    if (m_districtCombo) m_districtCombo->setCurrentIndex(di);

    m_provinceCombo->blockSignals(false);
    if (m_cityCombo) m_cityCombo->blockSignals(false);
    if (m_districtCombo) m_districtCombo->blockSignals(false);
}

// 响应general检查变化信号
// 作者：谭征
void SettingCenterDialog::onGeneralCheckChanged() {
    // 构造/加载期间 setChecked 会触发 stateChanged，此时控件尚未全部初始化完成
    // （如省/市/区下拉框还没加载到保存值），保存会覆盖原有配置；此处用 m_isLoading 抑制。
    if (m_isLoading) return;
    saveGeneralSettings();
    if (m_showSecondsCheck) emit timeShowSecondsChanged(m_showSecondsCheck->isChecked());
    if (m_largeTimeFontCheck) emit timeLargeFontChanged(m_largeTimeFontCheck->isChecked());
    if (m_showClockWeatherCheck) emit showClockWeatherChanged(m_showClockWeatherCheck->isChecked());
    if (m_showCompletedCheck) emit showCompletedChanged(m_showCompletedCheck->isChecked());
    if (m_showMainWindowCheck) emit showMainWindowChanged(m_showMainWindowCheck->isChecked());
    emit settingChanged(QStringLiteral("General"), QVariant());
}

static QString stripRegionSuffix(const QString& name);

SettingCenterDialog::IpGeoInfo SettingCenterDialog::parseIp9Location(const QByteArray& json) {
    IpGeoInfo info;
    QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return info;
    QJsonObject root = doc.object();
    if (root.value(QStringLiteral("ret")).toInt() != 200) return info;

    QJsonObject data = root.value(QStringLiteral("data")).toObject();
    if (data.isEmpty()) return info;

    info.province = stripRegionSuffix(data.value(QStringLiteral("prov")).toString());
    info.city = stripRegionSuffix(data.value(QStringLiteral("city")).toString());
    info.district = stripRegionSuffix(data.value(QStringLiteral("area")).toString());
    // lat/lng 在 ip9 接口中以字符串返回
    info.lat = data.value(QStringLiteral("lat")).toString().toDouble();
    info.lon = data.value(QStringLiteral("lng")).toString().toDouble();
    info.valid = !info.province.isEmpty() && !info.city.isEmpty();
    return info;
}

// 去除行政区划名称常见的「省/市/自治区/区/县/旗」等后缀，
// 使其能与 region.json 中的键（如「陕西」「西安」「雁塔」）匹配。
static QString stripRegionSuffix(const QString& name) {
    if (name.isEmpty()) return name;
    const QStringList suffixes = {
        QStringLiteral("自治区"), QStringLiteral("自治州"),
        QStringLiteral("省"), QStringLiteral("市"),
        QStringLiteral("区"), QStringLiteral("县"),
        QStringLiteral("旗"), QStringLiteral("地区")
    };
    QString r = name;
    for (const QString& suf : suffixes) {
        if (r.endsWith(suf) && r.size() > suf.size()) {
            r.chop(suf.size());
            break;
        }
    }
    return r;
}

// 开始ipgeorequest
// 作者：谭征
void SettingCenterDialog::startIpGeoRequest(bool useHttps) {
    if (!m_netManager) return;

    QUrl url(useHttps ? QStringLiteral("https://ip9.com.cn/get")
                      : QStringLiteral("http://ip9.com.cn/get"));
    QNetworkReply* reply = m_netManager->get(QNetworkRequest(url));

    // 忽略证书错误（如证书过期/自签），保证尽可能连通
    connect(reply, &QNetworkReply::sslErrors, this, [reply](const QList<QSslError>&) {
        reply->ignoreSslErrors();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        onIpGeoFinished(reply);
    });
}

// 响应自动locatebyip
// 作者：谭征
void SettingCenterDialog::onAutoLocateByIp() {
    if (!m_netManager || m_ipGeoPending) return;
    m_ipGeoPending = true;
    m_ipGeoTryHttpFallback = false;
    if (m_autoLocateBtn) {
        m_autoLocateBtn->setEnabled(false);
        m_autoLocateBtn->setText(QStringLiteral("定位中…"));
    }

    startIpGeoRequest(true); // 先 HTTPS；失败后再自动 fallback 到 HTTP
}

// 响应ipgeofinished
// 作者：谭征
void SettingCenterDialog::onIpGeoFinished(QNetworkReply* reply) {
    // HTTPS 失败且无 HTTP fallback 过，自动降级到 HTTP 再试一次
    if (reply && reply->error() != QNetworkReply::NoError &&
        reply->url().scheme() == QStringLiteral("https") && !m_ipGeoTryHttpFallback) {
        m_ipGeoTryHttpFallback = true;
        startIpGeoRequest(false); // HTTP fallback
        return;
    }

    m_ipGeoPending = false;
    m_ipGeoTryHttpFallback = false;
    if (m_autoLocateBtn) {
        m_autoLocateBtn->setEnabled(true);
        m_autoLocateBtn->setText(QStringLiteral("自动定位"));
    }

    if (!reply || reply->error() != QNetworkReply::NoError) {
        QString reason = reply ? reply->errorString() : QStringLiteral("未知错误");
        GlassMessageBox::information(this, QStringLiteral("自动定位失败"),
                                     QStringLiteral("无法获取当前 IP 位置：%1").arg(reason));
        return;
    }

    IpGeoInfo info = parseIp9Location(reply->readAll());
    if (!info.valid) {
        GlassMessageBox::information(this, QStringLiteral("自动定位失败"),
                                     QStringLiteral("IP 定位结果解析失败，请手动选择区域。"));
        return;
    }

    // 仅支持 region.json 内置的国内行政区；若 IP 归属地不在列表中（如海外），
    // 不强行切换，避免下拉框回退到第一条错误数据。
    if (!RegionData::provinces().contains(info.province)) {
        GlassMessageBox::information(this, QStringLiteral("自动定位失败"),
                                     QStringLiteral("当前 IP 归属地不在支持的城市列表中，请手动选择区域。"));
        return;
    }

    // 若接口返回的城市不在该省列表中，回退到该省第一个市
    QStringList cs = RegionData::cities(info.province);
    if (!cs.isEmpty() && !cs.contains(info.city)) {
        info.city = cs.first();
    }

    // 若接口未返回区县，回退到该市第一个区县
    QString district = info.district;
    if (district.isEmpty()) {
        QStringList ds = RegionData::districts(info.province, info.city);
        if (!ds.isEmpty()) district = ds.first();
    }

    // 记录旧区域，用于判断本次是否发生变化
    QString oldProvince = m_provinceCombo ? m_provinceCombo->currentText() : QString();
    QString oldCity = m_cityCombo ? m_cityCombo->currentText() : QString();
    QString oldDistrict = m_districtCombo ? m_districtCombo->currentText() : QString();

    // 更新下拉框并保存
    applyRegionSelection(info.province, info.city, district);
    saveGeneralSettings();

    // 标记已设置，避免首次启动的异步 IP 定位再次覆盖用户手动定位
    SettingsManager sm;
    sm.saveValue(QStringLiteral("General/weatherRegionSet"), true);
    sm.saveValue(QStringLiteral("Weather/lat"), info.lat);
    sm.saveValue(QStringLiteral("Weather/lon"), info.lon);
    sm.saveValue(QStringLiteral("Weather/city"), info.city);
    sm.sync();

    QString newProvince = m_provinceCombo ? m_provinceCombo->currentText() : QString();
    QString newCity = m_cityCombo ? m_cityCombo->currentText() : QString();
    QString newDistrict = m_districtCombo ? m_districtCombo->currentText() : QString();
    const bool regionChanged = (newProvince != oldProvince || newCity != oldCity || newDistrict != oldDistrict);

    GlassMessageBox::information(this, QStringLiteral("定位成功"),
                                 QStringLiteral("已切换到：%1 · %2 · %3").arg(info.province, info.city, district));
    // 区域未变化时 saveGeneralSettings() 不会 emit，这里补发一次保证天气刷新
    if (!regionChanged) {
        emit weatherRegionChanged();
    }
}

// 标题栏本身不承载任何编辑交互，不会与输入/选择冲突。
// 作者：谭征
void SettingCenterDialog::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        // 仅在自定义标题栏区域拖动（点击标题栏内任意子控件均可拖动窗口）
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
void SettingCenterDialog::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPos() - m_dragPos);
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

// 鼠标松开事件
// 作者：谭征
void SettingCenterDialog::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        event->accept();
    }
    QDialog::mouseReleaseEvent(event);
}
