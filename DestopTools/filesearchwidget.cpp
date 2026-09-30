/*
 * @file filesearchwidget.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "filesearchwidget.h"
#include "filesearchworker.h"       // 后台文件枚举 worker（独立 QThread）
#include "theme.h"
#include "thememanager.h"
#include "desktopmirrorwindow.h"   // raiseAboveBandWindows / clampBandZOrder：窗口 z 序（同重命名弹框）
#include "editorwatch.h"           // clearOwner：独立顶层窗口，不能被 owner 的压底拖着沉下去
#include <QApplication>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QFrame>
#include <QStackedWidget>
#include <QButtonGroup>
#include <QLineEdit>
#include <QComboBox>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPainter>
#include <QGraphicsDropShadowEffect>
#include <QDesktopServices>
#include <QUrl>
#include <QUrlQuery>
#include <QDir>
#include <QFileInfo>
#include <QFileIconProvider>
#include <QSet>
#include <QStorageInfo>
#include <QStandardPaths>
#include <QShowEvent>
#include <QHideEvent>
#include <QThread>
#include <QMetaObject>
#include <atomic>


FileSearchWidget::FileSearchWidget(QWidget* parent)
    : QDialog(parent, Qt::FramelessWindowHint | Qt::Window | Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(560, 520);
    setupUi();

    // 后台搜索线程
    m_thread = new QThread(this);
    m_worker = new FileSearchWorker;
    m_worker->moveToThread(m_thread);
    connect(m_worker, &FileSearchWorker::resultsReady, this, &FileSearchWidget::onResultBatch);
    connect(m_worker, &FileSearchWorker::searchFinished, this, &FileSearchWidget::onSearchFinished);
    m_thread->start();

    // z 序：钉在全部 band 窗口之上（与 GlassInputDialog「重命名弹框」/ UpdateDialog 同源，幂等）
    scheduleLayerAsserts();

    // 层级看护：窗口可见期间每 1s 复检一次「是否被收纳盒/助手压住」，**只在被压住时才救回**。
    // 本窗口与弹框的关键差别是长期存活（一次 show 后一直留在屏幕上、可能开着几十分钟），
    // 只靠 show 时刻的错峰断言覆盖不了整段生命周期，故再加这层兜底。
    // B5：500ms → 1000ms。看护要沿 z 序链遍历，且被压住是低频事件，
    // 1s 的检测延迟对“被遮挡后自动救回”的体感没有影响。
    m_layerKeeper = new QTimer(this);
    m_layerKeeper->setInterval(1000);
    connect(m_layerKeeper, &QTimer::timeout, this, [this]() {
#ifdef Q_OS_WIN
        if (isVisible())
            DesktopMirrorWindow::ensureLayerAboveBandWindows(reinterpret_cast<HWND>(winId()));
#endif
    });

    // 主题色 / 背景透明度联动：本窗样式全部由 applyTheme() 生成，两类变化都走它。
    // 不再 setWindowOpacity（整窗不透明度会把圆角线框与文字一起淡化）；透明度已并入背景色 alpha。
    ThemeManager::instance()->registerThemeTarget(this, [this]() { applyTheme(); });
}

// show() 之后按 0/60/200/400/800ms 错峰多轮再断言。
// 为什么不是「一次就够」（与 UpdateDialog 同一结论，2026-09-16）：
// · Qt 在 show() 之后仍会沿 parent 链补一次内部 z 序调整，单次插入会被它覆盖；
// · Dock 的 parkAllTargetsAtBottom（1s 周期）与各 band 窗口的 z 序守卫会持续改写 z 序，
// 窗口必须趁这些改写的空隙把位置重新钉住。
// assertLayerAboveBandWindows() 幂等（已贴在 band 之上时 raiseAboveBandWindows 直接返回），
// 重复调用零副作用、也不会闪烁。
// 作者：谭征
void FileSearchWidget::scheduleLayerAsserts() {
#ifdef Q_OS_WIN
    for (int delayMs : {0, 60, 200, 400, 800}) {
        QTimer::singleShot(delayMs, this, [this]() { assertLayerAboveBandWindows(); });
    }
#endif
}

FileSearchWidget::~FileSearchWidget() {
    if (m_worker) m_worker->m_activeToken.fetch_add(1);   // 让正在进行的遍历尽快退出
    if (m_thread) {
        m_thread->quit();
        m_thread->wait(2000);
    }
}

// 打开窗口并（可选）带入初始关键字：keyword 非空则直接进入文件搜索页并开始搜索。
// 作者：谭征
void FileSearchWidget::showSearch(const QString& keyword) {
    switchTab(0);   // 默认停在文件搜索页
    show();
    // show() 已触发 showEvent（内部排了一轮错峰断言）；这里再排一轮，覆盖「窗口已可见时
    // 再次被唤起」的路径 —— 此时 showEvent 不会再触发，但仍然需要重新钉一次层级。
    scheduleLayerAsserts();
    if (m_layerKeeper && !m_layerKeeper->isActive()) m_layerKeeper->start();
    if (!keyword.isEmpty()) {
        m_fileEdit->setText(keyword);
        startSearch(keyword);
    } else {
        m_fileEdit->setFocus();
        m_fileEdit->selectAll();
    }
}

// 初始化ui
// 作者：谭征
void FileSearchWidget::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(0);

    m_card = new QFrame(this);
    auto* card = m_card;
    card->setObjectName(QStringLiteral("card"));
    card->setStyleSheet(Theme::panelStyle());
    auto* shadow = new QGraphicsDropShadowEffect(card);
    shadow->setBlurRadius(24);
    shadow->setColor(Theme::shadow());
    shadow->setOffset(0, 4);
    card->setGraphicsEffect(shadow);

    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    // 标题栏
    m_titleBar = new QFrame(card);
    m_titleBar->setObjectName(QStringLiteral("titleBar"));
    m_titleBar->setFixedHeight(Theme::titleBarHeight());
    m_titleBar->setStyleSheet(Theme::titleBarStyle());
    auto* titleLayout = new QHBoxLayout(m_titleBar);
    titleLayout->setContentsMargins(12, 0, 8, 0);
    titleLayout->setSpacing(6);
    auto* caption = new QLabel(QStringLiteral("桌面助手 · 快速搜索"), m_titleBar);
    caption->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 13px; background: transparent; border: none;"));
    auto* closeBtn = new QToolButton(m_titleBar);
    closeBtn->setIcon(Theme::icon("close"));
    closeBtn->setIconSize(QSize(16, 16));
    closeBtn->setText(QString());
    closeBtn->setFixedSize(Theme::headerButtonSize());
    closeBtn->setStyleSheet(Theme::toolButtonStyle());
    connect(closeBtn, &QToolButton::clicked, this, &QDialog::reject);
    titleLayout->addWidget(caption);
    titleLayout->addStretch();
    titleLayout->addWidget(closeBtn);
    cardLayout->addWidget(m_titleBar);

    // 标签页切换
    auto* tabBar = new QHBoxLayout();
    tabBar->setContentsMargins(12, 12, 12, 4);
    tabBar->setSpacing(8);
    m_tabFile = new QToolButton(card);
    m_tabFile->setText(QStringLiteral("文件搜索"));
    m_tabFile->setCheckable(true);
    m_tabFile->setChecked(true);
    m_tabFile->setStyleSheet(Theme::tabButtonStyle());
    m_tabWeb = new QToolButton(card);
    m_tabWeb->setText(QStringLiteral("网页搜索"));
    m_tabWeb->setCheckable(true);
    m_tabWeb->setStyleSheet(Theme::tabButtonStyle());
    auto* tabGroup = new QButtonGroup(card);
    tabGroup->addButton(m_tabFile, 0);
    tabGroup->addButton(m_tabWeb, 1);
    tabGroup->setExclusive(true);
    connect(tabGroup, QOverload<int>::of(&QButtonGroup::buttonClicked), this, &FileSearchWidget::switchTab);
    tabBar->addWidget(m_tabFile);
    tabBar->addWidget(m_tabWeb);
    tabBar->addStretch();
    cardLayout->addLayout(tabBar);

    // 页面堆叠
    m_pages = new QStackedWidget(card);
    m_pages->setStyleSheet(QStringLiteral("QStackedWidget { background: transparent; border: none; }"));

    // 文件搜索页
    m_filePage = new QWidget(card);
    auto* filePage = m_filePage;
    auto* fileLayout = new QVBoxLayout(filePage);
    fileLayout->setContentsMargins(12, 4, 12, 12);
    fileLayout->setSpacing(10);

    auto* searchRow = new QHBoxLayout();
    searchRow->setSpacing(8);
    m_fileEdit = new QLineEdit(filePage);
    m_fileEdit->setPlaceholderText(QStringLiteral("输入文件名或关键字，回车开始搜索"));
    m_fileEdit->setMinimumHeight(34);
    Theme::applyLineEditStyle(m_fileEdit);
    m_scopeCombo = new QComboBox(filePage);
    m_scopeCombo->setMinimumHeight(34);
    m_scopeCombo->setStyleSheet(Theme::lineEditStyle());
    populateScopeCombo();
    searchRow->addWidget(m_fileEdit, 1);
    searchRow->addWidget(m_scopeCombo);
    fileLayout->addLayout(searchRow);

    m_statusLabel = new QLabel(QStringLiteral("输入关键字以搜索本机文件"), filePage);
    m_statusLabel->setStyleSheet(QStringLiteral("color: rgba(255,255,255,0.55); font-size: 12px; background: transparent; border: none;"));
    fileLayout->addWidget(m_statusLabel);

    m_resultList = new QListWidget(filePage);
    m_resultList->setStyleSheet(Theme::listWidgetStyle());
    m_resultList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_resultList, &QListWidget::itemActivated, this, &FileSearchWidget::onResultActivated);
    fileLayout->addWidget(m_resultList, 1);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    connect(m_debounce, &QTimer::timeout, this, [this]() { startSearch(m_fileEdit->text()); });
    connect(m_fileEdit, &QLineEdit::textChanged, this, &FileSearchWidget::onFileKeywordChanged);
    connect(m_scopeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &FileSearchWidget::onScopeChanged);
    connect(m_fileEdit, &QLineEdit::returnPressed, this, [this]() { startSearch(m_fileEdit->text()); });

    // 网页搜索页
    m_webPage = new QWidget(card);
    auto* webPage = m_webPage;
    auto* webLayout = new QVBoxLayout(webPage);
    webLayout->setContentsMargins(12, 4, 12, 12);
    webLayout->setSpacing(10);
    auto* webRow = new QHBoxLayout();
    webRow->setSpacing(8);
    m_webEdit = new QLineEdit(webPage);
    m_webEdit->setPlaceholderText(QStringLiteral("输入要搜索的内容，回车打开浏览器"));
    m_webEdit->setMinimumHeight(34);
    Theme::applyLineEditStyle(m_webEdit);
    m_engineCombo = new QComboBox(webPage);
    m_engineCombo->setMinimumHeight(34);
    m_engineCombo->setStyleSheet(Theme::lineEditStyle());
    m_engineCombo->addItem(QStringLiteral("百度"), QStringLiteral("baidu"));
    m_engineCombo->addItem(QStringLiteral("Bing"), QStringLiteral("bing"));
    m_engineCombo->addItem(QStringLiteral("Google"), QStringLiteral("google"));
    webRow->addWidget(m_webEdit, 1);
    webRow->addWidget(m_engineCombo);
    webLayout->addLayout(webRow);
    auto* webHint = new QLabel(QStringLiteral("回车或点击下方按钮，使用系统默认浏览器打开搜索结果页"), webPage);
    webHint->setStyleSheet(QStringLiteral("color: rgba(255,255,255,0.55); font-size: 12px; background: transparent; border: none;"));
    webLayout->addWidget(webHint);
    auto* webBtn = new QPushButton(QStringLiteral("搜索"), webPage);
    webBtn->setCursor(Qt::PointingHandCursor);
    webBtn->setFixedSize(96, 34);
    webBtn->setStyleSheet(Theme::primaryButtonStyle());
    connect(webBtn, &QPushButton::clicked, this, &FileSearchWidget::onWebSearch);
    connect(m_webEdit, &QLineEdit::returnPressed, this, &FileSearchWidget::onWebSearch);
    auto* webBtnRow = new QHBoxLayout();
    webBtnRow->addWidget(webBtn);
    webBtnRow->addStretch();
    webLayout->addLayout(webBtnRow);
    webLayout->addStretch();

    m_pages->addWidget(filePage);
    m_pages->addWidget(webPage);
    cardLayout->addWidget(m_pages, 1);

    root->addWidget(card);
}

// populatescopecombo
// 作者：谭征
void FileSearchWidget::populateScopeCombo() {
    m_scopeCombo->blockSignals(true);
    m_scopeCombo->clear();
    m_scopeCombo->addItem(QStringLiteral("此电脑"), QStringLiteral("computer"));
    // 固定/可移动盘符
    const QList<QStorageInfo> vols = QStorageInfo::mountedVolumes();
    for (const QStorageInfo& v : vols) {
        if (!v.isValid() || !v.isReady() || v.isReadOnly()) continue;
        const QString root = v.rootPath();
        if (root.startsWith(QStringLiteral("//"))) continue;   // 跳过网络映射根
        QString name = v.name();
        const QString label = name.isEmpty()
            ? QStringLiteral("%1:").arg(root.at(0))
            : QStringLiteral("%1: (%2)").arg(root.at(0)).arg(name);
        m_scopeCombo->addItem(label, root);
    }
    // 常用目录优先
    auto addCommon = [&](QStandardPaths::StandardLocation loc, const QString& zh) {
        const QString p = QStandardPaths::writableLocation(loc);
        if (!p.isEmpty() && QFile::exists(p)) m_scopeCombo->addItem(QStringLiteral("%1 (%2)").arg(zh).arg(p), p);
    };
    addCommon(QStandardPaths::DesktopLocation, QStringLiteral("桌面"));
    addCommon(QStandardPaths::DocumentsLocation, QStringLiteral("文档"));
    addCommon(QStandardPaths::DownloadLocation, QStringLiteral("下载"));
    m_scopeCombo->blockSignals(false);
}

// rootsforscope
// 作者：谭征
QStringList FileSearchWidget::rootsForScope(const QString& scopeKey) const {
    if (scopeKey == QStringLiteral("computer")) {
        QStringList roots;
        const QList<QStorageInfo> vols = QStorageInfo::mountedVolumes();
        for (const QStorageInfo& v : vols) {
            if (!v.isValid() || !v.isReady() || v.isReadOnly()) continue;
            const QString root = v.rootPath();
            if (root.startsWith(QStringLiteral("//"))) continue;
            roots.append(root);
        }
        if (roots.isEmpty()) roots.append(QDir::rootPath());
        return roots;
    }
    const QString p = m_scopeCombo->currentData().toString();
    if (!p.isEmpty() && QFile::exists(p)) return QStringList{p};
    return QStringList{QDir::rootPath()};
}

// 响应文件keyword变化信号
// 作者：谭征
void FileSearchWidget::onFileKeywordChanged(const QString& text) {
    // 防抖：停止输入 350ms 后再搜，避免逐字触发全盘遍历
    m_debounce->start(350);
    Q_UNUSED(text);
}

// 响应scope变化信号
// 作者：谭征
void FileSearchWidget::onScopeChanged(int /*index*/) {
    if (!m_currentKeyword.isEmpty()) startSearch(m_currentKeyword);
}

// 开始搜索
// 作者：谭征
void FileSearchWidget::startSearch(const QString& keyword) {
    m_currentKeyword = keyword.trimmed();
    if (m_currentKeyword.isEmpty()) {
        m_resultList->clear();
        m_statusLabel->setText(QStringLiteral("输入关键字以搜索本机文件"));
        return;
    }
    ++m_searchToken;
    m_resultList->clear();
    m_statusLabel->setText(QStringLiteral("搜索中…"));
    const QString scopeKey = m_scopeCombo->currentData().toString();
    const QStringList roots = rootsForScope(scopeKey);
    QMetaObject::invokeMethod(m_worker, "doSearch",
                              Qt::QueuedConnection,
                              Q_ARG(qint64, m_searchToken),
                              Q_ARG(QStringList, roots),
                              Q_ARG(QString, m_currentKeyword),
                              Q_ARG(int, 2000));
}

// 响应resultbatch
// 作者：谭征
void FileSearchWidget::onResultBatch(qint64 token, const QStringList& paths, const QList<bool>& isDirs, int runningTotal) {
    if (token != m_searchToken) return;   // 过期批次，丢弃
    QFileIconProvider iconProv;
    const int n = qMin(paths.size(), isDirs.size());
    for (int i = 0; i < n; ++i) {
        const QString path = paths.at(i);
        auto* item = new QListWidgetItem(m_resultList);
        item->setData(Qt::UserRole, path);
        auto* row = new QWidget;
        auto* hl = new QHBoxLayout(row);
        hl->setContentsMargins(8, 6, 8, 6);
        hl->setSpacing(10);
        auto* iconLbl = new QLabel(row);
        iconLbl->setFixedSize(28, 28);
        QIcon ic = iconProv.icon(isDirs.at(i) ? QFileIconProvider::Folder : QFileIconProvider::File);
        iconLbl->setPixmap(ic.pixmap(28, 28));
        const QString name = path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
        auto* vl = new QVBoxLayout;
        vl->setContentsMargins(0, 0, 0, 0);
        vl->setSpacing(2);
        auto* nameLbl = new QLabel(name, row);
        nameLbl->setStyleSheet(QStringLiteral("color: #FFFFFF; font-size: 13px; background: transparent; border: none;"));
        auto* pathLbl = new QLabel(path, row);
        pathLbl->setStyleSheet(QStringLiteral("color: rgba(255,255,255,0.50); font-size: 11px; background: transparent; border: none;"));
        pathLbl->setWordWrap(false);
        vl->addWidget(nameLbl);
        vl->addWidget(pathLbl);
        hl->addWidget(iconLbl);
        hl->addLayout(vl, 1);
        m_resultList->setItemWidget(item, row);
    }
    m_statusLabel->setText(QStringLiteral("已找到 %1 个文件…").arg(runningTotal));
}

// 响应搜索finished
// 作者：谭征
void FileSearchWidget::onSearchFinished(qint64 token, int total) {
    if (token != m_searchToken) return;
    if (total == 0) {
        m_statusLabel->setText(QStringLiteral("未找到匹配的文件"));
    } else {
        m_statusLabel->setText(QStringLiteral("共匹配 %1 个文件").arg(total));
    }
}

// 响应resultactivated
// 作者：谭征
void FileSearchWidget::onResultActivated(QListWidgetItem* item) {
    if (!item) return;
    const QString path = item->data(Qt::UserRole).toString();
    if (path.isEmpty()) return;
    // 双击/回车：用系统关联程序打开（目录会用资源管理器打开）
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

// 响应web搜索
// 作者：谭征
void FileSearchWidget::onWebSearch() {
    const QString q = m_webEdit->text().trimmed();
    if (q.isEmpty()) return;
    const QString engine = m_engineCombo->currentData().toString();
    QDesktopServices::openUrl(QUrl(searchUrl(engine, q)));
}

// switch标签
// 作者：谭征
void FileSearchWidget::switchTab(int tab) {
    m_pages->setCurrentIndex(tab);
    m_tabFile->setChecked(tab == 0);
    m_tabWeb->setChecked(tab == 1);
    if (tab == 0) {
        m_fileEdit->setFocus();
        if (!m_currentKeyword.isEmpty()) startSearch(m_currentKeyword);
    } else {
        m_webEdit->setFocus();
    }
}

// 主题色/透明度联动：重绘对话框自身硬编码青色样式，并同步窗口不透明度
// 作者：谭征
void FileSearchWidget::applyTheme() {
    // 主题色/透明度变化：重刷卡片、标题栏与可重绘控件样式
    auto* card = findChild<QFrame*>(QStringLiteral("card"));
    if (card) card->setStyleSheet(Theme::panelStyle());
    if (m_titleBar) m_titleBar->setStyleSheet(Theme::titleBarStyle());
    Theme::applyLineEditStyle(m_fileEdit);
    m_scopeCombo->setStyleSheet(Theme::lineEditStyle());
    Theme::applyLineEditStyle(m_webEdit);
    m_engineCombo->setStyleSheet(Theme::lineEditStyle());
    m_resultList->setStyleSheet(Theme::listWidgetStyle());
    m_tabFile->setStyleSheet(Theme::tabButtonStyle());
    m_tabWeb->setStyleSheet(Theme::tabButtonStyle());
}

// 搜索url
// 作者：谭征
QString FileSearchWidget::searchUrl(const QString& engine, const QString& query) {
    const QString q = QString::fromUtf8(QUrl::toPercentEncoding(query));
    if (engine == QStringLiteral("bing")) {
        return QStringLiteral("https://www.bing.com/search?q=%1").arg(q);
    } else if (engine == QStringLiteral("google")) {
        return QStringLiteral("https://www.google.com/search?q=%1").arg(q);
    }
    return QStringLiteral("https://www.baidu.com/s?wd=%1").arg(q);   // 默认百度
}

// 无边框窗口拖动（项目统一方案，与 GlassInputDialog 重命名弹框 / UpdateDialog / SettingCenterDialog 同款）
// Qt::FramelessWindowHint 的窗口没有系统标题栏，Windows 不会替我们移动它，必须自己实现：
// mousePressEvent 判定「是否落在可拖区域」并记下按下点相对窗口左上角的偏移；
// mouseMoveEvent 用 (全局坐标 - 偏移) 反推窗口新位置。
// 可拖区域 = 标题栏（连其上的标题文字）+ 卡片/页面容器的空白留白。
// 输入框 / 下拉框 / 结果列表 / 按钮等交互控件一律不参与判定，避免「想选中文本却把窗口拖走」。
// 注：move() 会触发 WM_WINDOWPOSCHANGING，但本窗口 nativeEvent 里的 clampBandZOrder 只钳制 z 序、
// 不改 x/y（且 Qt 移动时带 SWP_NOZORDER），所以拖动位置不会被层级守卫吞掉。
// 作者：谭征
bool FileSearchWidget::isDragAreaAt(const QPoint& pos) const {
    QWidget* hit = childAt(pos);
    if (!hit) return false;   // 窗口外围 12px 透明留白（WA_TranslucentBackground 的阴影边距）：不响应，避免误触
    for (QWidget* p = hit; p && p != this; p = p->parentWidget()) {
        if (p == m_titleBar) return true;   // 标题栏区域；关闭按钮是 QToolButton、会自己吞掉按下事件，走不到这里
    }
    return hit == m_card || hit == m_pages || hit == m_filePage || hit == m_webPage;
}

// 标题栏本身不承载任何编辑交互，不会与输入/选择冲突。
// 作者：谭征
void FileSearchWidget::mousePressEvent(QMouseEvent* event) {
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
void FileSearchWidget::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPos() - m_dragPos);
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

// 鼠标松开事件
// 作者：谭征
void FileSearchWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        event->accept();
        return;
    }
    QDialog::mouseReleaseEvent(event);
}

// 与 GlassInputDialog（重命名弹框）/ UpdateDialog 同源，幂等，可反复调用。
// 作者：谭征
void FileSearchWidget::assertLayerAboveBandWindows() {
#ifdef Q_OS_WIN
    if (!isVisible()) return;
    const HWND h = reinterpret_cast<HWND>(winId());
    if (!h || !IsWindow(h)) return;
    EditorWatch::clearOwner(h);
    DesktopMirrorWindow::raiseAboveBandWindows(h);
    if (GetForegroundWindow() != h) activateWindow();
#endif
}

// 显示事件
// 作者：谭征
void FileSearchWidget::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // 错峰多轮再断言（与 UpdateDialog 同款结论）：单次断言会被 Qt 在 show() 之后补做的那次
    // 内部 z 序调整覆盖，必须等这次调整落地后再钉一遍、并再多钉几轮。
    scheduleLayerAsserts();
    if (m_layerKeeper) m_layerKeeper->start();
}

// 隐藏事件
// 作者：谭征
void FileSearchWidget::hideEvent(QHideEvent* event) {
    // 不可见时停止看护：既省掉每 500ms 的窗口层级查询，也避免对隐藏窗口做无意义的 z 序写入。
    if (m_layerKeeper) m_layerKeeper->stop();
    m_dragging = false;   // 拖动中途被隐藏（Esc 等）时清掉手势状态，避免下次显示残留半拖动
    QDialog::hideEvent(event);
}

// 助手面板顶到普通程序之上再被压回的「一闪而过」。show desktop 期放行。
// 作者：谭征
bool FileSearchWidget::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    MSG* msg = reinterpret_cast<MSG*>(message);
    if (msg && msg->message == WM_WINDOWPOSCHANGING) {
        WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(msg->lParam);
        DesktopMirrorWindow::clampBandZOrder(msg->hwnd, wp);
        wp->flags &= ~SWP_NOACTIVATE;
        return false;
    }
#endif
    return QDialog::nativeEvent(eventType, message, result);
}
