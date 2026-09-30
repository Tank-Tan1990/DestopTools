/*
 * @file wallpaperwindow.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "wallpaperwindow.h"
#include "theme.h"
#include "thememanager.h"
#include "desktopmirrorwindow.h"   // raiseAboveBandWindows / clampBandZOrder / ensureLayerAboveBandWindows
#include "editorwatch.h"           // clearOwner：独立顶层窗口不能被 owner 的压底拖着沉下去
#include "glassmessagebox.h"

#include <Windows.h>

#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QDesktopWidget>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QHideEvent>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

const int kThumbW = 150;
const int kThumbH = 88;
const int kMaxThumbs = 60;

// 扫描系统自带壁纸目录（C:\Windows\Web 递归），返回图片绝对路径列表。
QStringList scanSystemWallpapers() {
    QStringList out;
    const QString web = QStringLiteral("C:/Windows/Web");
    if (!QDir(web).exists()) return out;
    QDirIterator it(web, QStringList() << QStringLiteral("*.jpg") << QStringLiteral("*.jpeg")
                                       << QStringLiteral("*.png") << QStringLiteral("*.bmp"),
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && out.size() < kMaxThumbs) {
        out << it.next();
    }
    return out;
}

// make标签
// 作者：谭征
QLabel* makeLabel(const QString& text, QWidget* parent, const QString& extra = QString()) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 12px; background: transparent; border: none;")
                     + extra);
    return l;
}

// 主题色/透明度切换时这些样式会被 refreshTheme() 重新生成，
// 新增控件样式请放进对应函数，别在 setupUi 里写死一次性样式串。

// 「填充方式」下拉框
QString fillComboStyle() {
    return Theme::applyTokens(QStringLiteral(
        "QComboBox { color: #FFFFFF; background: rgba(255,255,255,0.06); border: 1px solid rgba(34,211,238,0.22);"
        "            border-radius: 6px; padding: 4px 8px; font-size: 12px; }"
        "QComboBox:hover { border-color: rgba(34,211,238,0.42); }"
        "QComboBox::drop-down { border: none; width: 18px; }"
        "QComboBox QAbstractItemView { background: rgba(17,26,46,1.0); color: #FFFFFF; outline: none;"
        "            border: 1px solid rgba(34,211,238,0.25); selection-background-color: rgba(34,211,238,0.22); }"));
}

// 「选择本地图片」磁贴
QString pickTileStyle() {
    return Theme::applyTokens(QStringLiteral(
        "QToolButton { color: #FFFFFF; background: rgba(34,211,238,0.08);"
        "              border: 1px dashed rgba(34,211,238,0.40); border-radius: 8px; font-size: 12px; }"
        "QToolButton:hover { background: rgba(34,211,238,0.16); border-color: #22D3EE; }"));
}

// 壁纸缩略图磁贴
QString thumbTileStyle() {
    return Theme::applyTokens(QStringLiteral(
        "QToolButton { color: rgba(255,255,255,0.85); background: rgba(255,255,255,0.04);"
        "              border: 1px solid rgba(255,255,255,0.10); border-radius: 8px; font-size: 11px; }"
        "QToolButton:hover { background: rgba(34,211,238,0.14); border-color: rgba(34,211,238,0.45); }"));
}

}   // namespace

WallpaperWindow::WallpaperWindow(QWidget* parent)
    // 必须是 Qt::Window（独立顶层窗口），与「快速搜索」窗 / 定时关机窗 同源：
    // Qt::Dialog + parent 会让 Qt 把 parent 设成本窗口 owner，弹框会随其周期性压底一起沉下去。
    : QDialog(parent, Qt::FramelessWindowHint | Qt::Window | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    // 宽度 = 4 列缩略图（4*162 + 3*8 = 672）+ 右栏 232 + 布局边距/间距 292 + 竖向滚动条余量
    setFixedSize(990, 440);

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

    // 全局主题色 / 背景透明度联动：重新生成全部子控件样式（refreshTheme()）。
    // 不再 setWindowOpacity（整窗不透明度会把圆角线框与文字一起淡化）；透明度已并入背景色 alpha。
    ThemeManager::instance()->registerThemeTarget(this, [this]() { refreshTheme(); });
}

WallpaperWindow::~WallpaperWindow() = default;

// 打开窗口（每次唤起都停在任务列表页，并刷新任务与倒计时）。
// 作者：谭征
void WallpaperWindow::showWindow() {
    buildGallery();          // 每次唤起重新扫描系统壁纸
    selectPath(QString());   // 复位选择
    show();
    raise();
    scheduleLayerAsserts();
    if (m_layerKeeper && !m_layerKeeper->isActive()) m_layerKeeper->start();
}

// UI

// 作者：谭征
void WallpaperWindow::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(0);

    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("card"));
    m_card->setStyleSheet(Theme::panelStyle());
    auto* shadow = new QGraphicsDropShadowEffect(m_card);
    shadow->setBlurRadius(24);
    shadow->setColor(Theme::shadow());
    shadow->setOffset(0, 4);
    m_card->setGraphicsEffect(shadow);

    auto* cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    // 标题栏
    m_titleBar = new QFrame(m_card);
    m_titleBar->setObjectName(QStringLiteral("titleBar"));
    m_titleBar->setFixedHeight(Theme::titleBarHeight());
    m_titleBar->setStyleSheet(Theme::titleBarStyle());
    auto* titleLayout = new QHBoxLayout(m_titleBar);
    titleLayout->setContentsMargins(12, 0, 8, 0);
    titleLayout->setSpacing(6);
    auto* caption = makeLabel(QStringLiteral("壁纸"), m_titleBar, QStringLiteral("font-size: 13px;"));
    auto* closeBtn = new QToolButton(m_titleBar);
    closeBtn->setIcon(Theme::icon("close"));
    closeBtn->setIconSize(QSize(16, 16));
    closeBtn->setText(QString());
    closeBtn->setFixedSize(Theme::headerButtonSize());
    closeBtn->setStyleSheet(Theme::toolButtonStyle());
    connect(closeBtn, &QToolButton::clicked, this, &QDialog::reject);
    titleLayout->addWidget(caption);
    titleLayout->addWidget(makeLabel(QStringLiteral("更换桌面壁纸"), m_titleBar,
                                     QStringLiteral("color: rgba(255,255,255,0.55); font-size: 12px;")));
    titleLayout->addStretch();
    titleLayout->addWidget(closeBtn);
    cardLayout->addWidget(m_titleBar);

    // 主体：左画廊 + 右预览/操作
    auto* body = new QHBoxLayout();
    body->setContentsMargins(12, 12, 12, 12);
    body->setSpacing(12);

    // 左：画廊
    m_galleryScroll = new QScrollArea(m_card);
    m_galleryScroll->setWidgetResizable(true);
    m_galleryScroll->setFrameShape(QFrame::NoFrame);
    m_galleryScroll->setStyleSheet(QStringLiteral(
        "QScrollArea { background: transparent; border: none; outline: none; }"
        "QScrollArea::viewport { background: transparent; border: none; }")
        + Theme::scrollBarStyle());
    m_galleryHost = new QWidget();
    m_galleryHost->setStyleSheet(QStringLiteral("background: transparent;"));
    m_galleryGrid = new QGridLayout(m_galleryHost);
    m_galleryGrid->setContentsMargins(0, 0, 0, 0);
    m_galleryGrid->setSpacing(8);
    m_galleryScroll->setWidget(m_galleryHost);
    body->addWidget(m_galleryScroll, 1);

    // 右：预览 + 填充方式 + 应用
    auto* right = new QVBoxLayout();
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(10);
    right->setAlignment(Qt::AlignTop);

    m_preview = new QLabel(m_card);
    m_preview->setFixedSize(232, 132);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setStyleSheet(QStringLiteral(
        "QLabel { background: rgba(0,0,0,0.28); border: 1px solid rgba(255,255,255,0.12);"
        "          border-radius: 8px; color: rgba(255,255,255,0.45); font-size: 12px; }"));
    m_preview->setText(QStringLiteral("未选择壁纸"));
    right->addWidget(m_preview);

    right->addWidget(makeLabel(QStringLiteral("填充方式"), m_card, QStringLiteral("font-size: 12px;")));
    m_fillCombo = new QComboBox(m_card);
    m_fillCombo->setStyleSheet(fillComboStyle());
    m_fillCombo->addItem(QStringLiteral("填充"), Fill);
    m_fillCombo->addItem(QStringLiteral("适应"), Fit);
    m_fillCombo->addItem(QStringLiteral("拉伸"), Stretch);
    m_fillCombo->addItem(QStringLiteral("平铺"), Tile);
    m_fillCombo->addItem(QStringLiteral("居中"), Center);
    m_fillCombo->setCurrentIndex(0);
    m_fillCombo->setFixedWidth(232);
    right->addWidget(m_fillCombo);
    connect(m_fillCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &WallpaperWindow::onFillChanged);

    m_applyBtn = new QPushButton(QStringLiteral("应用"), m_card);
    m_applyBtn->setCursor(Qt::PointingHandCursor);
    m_applyBtn->setFixedSize(232, 34);
    m_applyBtn->setStyleSheet(Theme::primaryButtonStyle());
    m_applyBtn->setEnabled(false);
    connect(m_applyBtn, &QPushButton::clicked, this, &WallpaperWindow::onApply);
    right->addWidget(m_applyBtn);

    m_hint = makeLabel(QStringLiteral("点击左侧缩略图预览，再点「应用」即可更换桌面壁纸。"), m_card,
                       QStringLiteral("color: rgba(255,255,255,0.55); font-size: 11px;"));
    m_hint->setWordWrap(true);
    m_hint->setFixedWidth(232);
    right->addWidget(m_hint);
    right->addStretch();

    body->addLayout(right, 0);
    cardLayout->addLayout(body, 1);

    root->addWidget(m_card);
}

// 构建gallery
// 作者：谭征
void WallpaperWindow::buildGallery() {
    // 清空旧缩略图（含「选择本地图片」磁贴）
    while (m_galleryGrid->count() > 0) {
        QLayoutItem* item = m_galleryGrid->takeAt(0);
        if (item && item->widget()) {
            item->widget()->hide();
            item->widget()->setParent(nullptr);
            item->widget()->deleteLater();
        }
        delete item;
    }
    m_scanned.clear();

    const int cols = 4;
    int row = 0, col = 0;

    // 顶部：「选择本地图片」磁贴
    auto* pick = new QToolButton(m_galleryHost);
    pick->setIcon(Theme::icon("tool_files"));
    pick->setIconSize(QSize(28, 28));
    pick->setText(QStringLiteral("选择本地图片"));
    pick->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    pick->setFixedSize(kThumbW + 12, kThumbH + 30);
    pick->setProperty("tileRole", QStringLiteral("pick"));   // 主题刷新时区分磁贴样式
    pick->setStyleSheet(pickTileStyle());
    connect(pick, &QToolButton::clicked, this, &WallpaperWindow::onPickLocal);
    m_galleryGrid->addWidget(pick, row, col);
    if (++col >= cols) { col = 0; ++row; }

    // 系统壁纸缩略图
    const QStringList imgs = scanSystemWallpapers();
    for (const QString& path : imgs) {
        QImageReader reader(path);
        reader.setScaledSize(QSize(kThumbW, kThumbH));
        const QPixmap pm = QPixmap::fromImageReader(&reader);
        if (pm.isNull()) continue;

        const QString name = QFileInfo(path).baseName();
        auto* btn = new QToolButton(m_galleryHost);
        btn->setIcon(QIcon(pm));
        btn->setIconSize(QSize(kThumbW, kThumbH));
        btn->setText(name);
        btn->setToolTip(path);
        btn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        btn->setFixedSize(kThumbW + 12, kThumbH + 30);
        btn->setStyleSheet(thumbTileStyle());
        connect(btn, &QToolButton::clicked, this, [this, path]() { onThumbClicked(path); });
        m_galleryGrid->addWidget(btn, row, col);
        m_scanned << path;
        if (++col >= cols) { col = 0; ++row; }
    }
}

// 刷新主题
// 作者：谭征
void WallpaperWindow::refreshTheme() {
    // 主题色/透明度变化 → 重新生成全部子控件样式（卡片、标题栏、下拉框、按钮、画廊磁贴、滚动条）
    if (m_card) m_card->setStyleSheet(Theme::panelStyle());
    if (m_titleBar) m_titleBar->setStyleSheet(Theme::titleBarStyle());
    if (m_galleryScroll) {
        m_galleryScroll->setStyleSheet(QStringLiteral(
            "QScrollArea { background: transparent; border: none; outline: none; }"
            "QScrollArea::viewport { background: transparent; border: none; }")
            + Theme::scrollBarStyle());
    }
    if (m_fillCombo) m_fillCombo->setStyleSheet(fillComboStyle());
    if (m_applyBtn) m_applyBtn->setStyleSheet(Theme::primaryButtonStyle());
    refreshGalleryStyles();
}

// 刷新gallerystyles
// 作者：谭征
void WallpaperWindow::refreshGalleryStyles() {
    if (!m_galleryGrid) return;
    for (int i = 0; i < m_galleryGrid->count(); ++i) {
        QLayoutItem* item = m_galleryGrid->itemAt(i);
        QWidget* w = item ? item->widget() : nullptr;
        if (!w) continue;
        if (w->property("tileRole").toString() == QLatin1String("pick"))
            w->setStyleSheet(pickTileStyle());
        else
            w->setStyleSheet(thumbTileStyle());
    }
}

// 响应thumbclicked
// 作者：谭征
void WallpaperWindow::onThumbClicked(const QString& path) {
    selectPath(path);
}

// 响应pick本地
// 作者：谭征
void WallpaperWindow::onPickLocal() {
    const QString home = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择壁纸图片"),
                                                     home.isEmpty() ? QString() : home,
                                                     QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp)"));
    if (path.isEmpty()) return;
    if (!m_scanned.contains(path)) m_scanned << path;   // 记住，便于双击/回填
    selectPath(path);
}

// 选择路径
// 作者：谭征
void WallpaperWindow::selectPath(const QString& path) {
    m_currentPath = path;
    if (path.isEmpty()) {
        m_preview->setPixmap(QPixmap());
        m_preview->setText(QStringLiteral("未选择壁纸"));
        m_applyBtn->setEnabled(false);
        m_hint->setText(QStringLiteral("点击左侧缩略图预览，再点「应用」即可更换桌面壁纸。"));
        return;
    }
    const QPixmap full(path);
    const QPixmap scaled = full.scaled(m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_preview->setPixmap(scaled);
    m_applyBtn->setEnabled(true);
    m_hint->setText(QStringLiteral("已选：%1\n点「应用」更换（或修改填充方式即时生效）。")
                       .arg(QFileInfo(path).fileName()));
}

// 响应fill变化信号
// 作者：谭征
void WallpaperWindow::onFillChanged(int /*index*/) {
    // 已选壁纸时，改填充方式立即重设（否则更改无视觉效果）
    if (!m_currentPath.isEmpty()) onApply();
}

// 响应应用
// 作者：谭征
void WallpaperWindow::onApply() {
    if (m_currentPath.isEmpty()) return;
    const FillMode mode = FillMode(m_fillCombo->currentData().toInt());
    if (applyWallpaper(m_currentPath, mode)) {
        m_hint->setText(QStringLiteral("已应用：%1").arg(QFileInfo(m_currentPath).fileName()));
    } else {
        GlassMessageBox::warning(this, QStringLiteral("更换壁纸"),
                                 QStringLiteral("设置桌面壁纸失败，请确认图片路径有效且未被占用。"));
    }
}

// 清除选中
// 作者：谭征
void WallpaperWindow::clearSelection() {
    selectPath(QString());
}

// 写入系统壁纸：注册表填充方式 + SystemParametersInfoW。
// 作者：谭征
bool WallpaperWindow::applyWallpaper(const QString& path, FillMode mode) {
    const QString abs = QFileInfo(path).absoluteFilePath();
    if (!QFile::exists(abs)) return false;

    int style = 0, tile = 0;
    switch (mode) {
    case Stretch: style = 2; tile = 0; break;
    case Tile:    style = 0; tile = 1; break;
    case Center:  style = 0; tile = 0; break;
    case Fit:     style = 6; tile = 0; break;
    case Fill:    style = 10; tile = 0; break;
    }

    QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Control Panel\\Desktop"), QSettings::NativeFormat);
    reg.setValue(QStringLiteral("WallpaperStyle"), QString::number(style));
    reg.setValue(QStringLiteral("TileWallpaper"), QString::number(tile));

    const std::wstring wpath = abs.toStdWString();
    const BOOL ok = SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0,
                                          const_cast<LPWSTR>(wpath.c_str()),
                                          SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    return ok != FALSE;
}

// 窗口基础设施

// 作者：谭征
bool WallpaperWindow::isDragAreaAt(const QPoint& pos) const {
    QWidget* hit = childAt(pos);
    if (!hit) return false;   // 卡片外 12px 透明阴影留白：不响应，避免误触
    for (QWidget* p = hit; p && p != this; p = p->parentWidget()) {
        if (p == m_titleBar) return true;   // 关闭按钮是 QToolButton、自己吞掉按下事件，走不到这里
    }
    return hit == m_card;
}

// 标题栏本身不承载任何编辑交互，不会与输入/选择冲突。
// 作者：谭征
void WallpaperWindow::mousePressEvent(QMouseEvent* event) {
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
void WallpaperWindow::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPos() - m_dragPos);
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

// 鼠标松开事件
// 作者：谭征
void WallpaperWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        event->accept();
        return;
    }
    QDialog::mouseReleaseEvent(event);
}

// 与 GlassInputDialog（重命名弹框）/ UpdateDialog 同源，幂等，可反复调用。
// 作者：谭征
void WallpaperWindow::assertLayerAboveBandWindows() {
#ifdef Q_OS_WIN
    if (!isVisible()) return;
    const HWND h = reinterpret_cast<HWND>(winId());
    if (!h || !IsWindow(h)) return;
    EditorWatch::clearOwner(h);                    // ① 清 owner，避免随其压底而沉下去
    DesktopMirrorWindow::raiseAboveBandWindows(h);  // ② 插到全部 band 窗口之上
    if (GetForegroundWindow() != h) activateWindow();
#endif
}

// schedulelayerasserts
// 作者：谭征
void WallpaperWindow::scheduleLayerAsserts() {
#ifdef Q_OS_WIN
    // show() 之后 Qt 仍会沿 parent 链补一次内部 z 序调整，单次断言会被它覆盖，故错峰多轮再钉。
    for (int delayMs : {0, 60, 200, 400, 800}) {
        QTimer::singleShot(delayMs, this, [this]() { assertLayerAboveBandWindows(); });
    }
#endif
}

// 显示事件
// 作者：谭征
void WallpaperWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    scheduleLayerAsserts();
    if (m_layerKeeper) m_layerKeeper->start();
}

// 隐藏事件
// 作者：谭征
void WallpaperWindow::hideEvent(QHideEvent* event) {
    if (m_layerKeeper) m_layerKeeper->stop();
    m_dragging = false;
    QDialog::hideEvent(event);
}

// 助手面板顶到普通程序之上再被压回的「一闪而过」。show desktop 期放行。
// 作者：谭征
bool WallpaperWindow::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    MSG* msg = reinterpret_cast<MSG*>(message);
    if (msg && msg->message == WM_WINDOWPOSCHANGING) {
        WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(msg->lParam);
        DesktopMirrorWindow::clampBandZOrder(msg->hwnd, wp);
        wp->flags &= ~SWP_NOACTIVATE;   // 本窗要能激活以收键盘/弹层；z 序冻结（SWP_NOZORDER）保留
        return false;
    }
#endif
    return QDialog::nativeEvent(eventType, message, result);
}
