/*
 * @file fencebox.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "fencebox.h"
#include "desktopiconbutton.h"
#include "theme.h"
#include "thememanager.h"
#include "shellops.h"       // 删除的唯一落盘通道（与 Dock / 收纳盒网格共用）
#include "categorystore.h"  // 删除后清理本地持久化的分类记录
#include "windowsnap.h"    // 自动对齐（格子对齐）+ 盒子互相磁吸
#include <QGridLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <QDir>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QFrame>
#include <QFontMetrics>
#include <climits>

namespace {
// 盒内图标格与边距（DesktopIconButton 固定 72x84）
const int kCellW = 72;
const int kCellH = 84;
const int kCellGap = kCellW / 4;   // 间隔 = 图标格子宽度 / 4 (=18)
const int kInnerMargin = 8;
const int kEdgeMargin = 6;    // 边框缩放热区宽度

// 图标条目的稳定键：归一化分隔符 + 小写。与 IconGridWindow 的选中接力 / 顺序持久化同口径，
// 保证"盒内复用按钮"时能按同一个键认出"这就是原来那个图标"。
QString itemKey(const QString& path) {
    return QDir::fromNativeSeparators(path).toLower();
}
}

FenceBox::FenceBox(const QString& category, QWidget* parent)
    : QWidget(parent), m_category(category) {
    setObjectName(QStringLiteral("FenceBox"));
    setAcceptDrops(true);
    setMouseTracking(true);
    setMinimumSize(minimumBoxSize());

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_title = new QLabel(m_category, this);
    m_title->setObjectName(QStringLiteral("fenceTitle"));
    m_title->setFixedHeight(titleBarHeight());
    // 标题与内容区都让鼠标事件穿透到盒子本身，才能实现“拖标题移动 / 拖边框缩放”；
    // 注意该属性不影响其子控件（图标按钮仍可正常点击与拖拽）。
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    m_inner = new QWidget(this);
    m_inner->setObjectName(QStringLiteral("fenceInner"));
    m_inner->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_innerLayout = new QGridLayout(m_inner);
    m_innerLayout->setSpacing(kCellGap);
    m_innerLayout->setContentsMargins(kInnerMargin, kInnerMargin, kInnerMargin, kInnerMargin);
    m_innerLayout->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    root->addWidget(m_title);
    root->addWidget(m_inner, 1);

    // 标题右侧的图标数量
    m_countLabel = new QLabel(this);
    m_countLabel->setObjectName(QStringLiteral("fenceCount"));
    m_countLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_countLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    // 空盒子占位提示
    m_emptyHint = new QLabel(QStringLiteral("把图标拖到这里"), m_inner);
    m_emptyHint->setObjectName(QStringLiteral("fenceEmptyHint"));
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_emptyHint->hide();

    // 双击标题就地重命名用的输入框（默认隐藏，覆盖在标题栏上）
    m_titleEdit = new QLineEdit(this);
    m_titleEdit->setObjectName(QStringLiteral("fenceTitleEdit"));
    m_titleEdit->hide();
    m_titleEdit->installEventFilter(this);
    connect(m_titleEdit, &QLineEdit::returnPressed, this, &FenceBox::commitInlineRename);
    connect(m_titleEdit, &QLineEdit::editingFinished, this, &FenceBox::commitInlineRename);

    // 盒内拖拽插入位置指示线：覆盖在 m_inner 之上、鼠标穿透，仅在拖拽悬停时显示。
    m_insertMarker = new QFrame(m_inner);
    m_insertMarker->setObjectName(QStringLiteral("fenceInsertMarker"));
    m_insertMarker->setFrameShape(QFrame::NoFrame);
    m_insertMarker->setFixedWidth(3);
    m_insertMarker->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_insertMarker->raise();
    m_insertMarker->setStyleSheet(QStringLiteral("background-color: #2f7bff; border-radius: 1px;"));
    m_insertMarker->hide();

    auto applySheet = [this]() {
        // 透明面板：图标直接浮在壁纸上；盒子与标题只给一点半透明底 + 细描边，贴近 Fences 观感。
        // 颜色随主题色联动（panelBgString 基于当前 accentColor）。
        // 外观开关（2026-09-22，设置中心 → 外观设置 → 其他）
        // · 「盒子显示边框」关闭 → 盒子常态与 hover 态都不画那圈 1px 线框；
        // · 「盒子使用圆角」关闭 → 盒子与标题的圆角压成 0（方角）。
        // 只按**盒子自身**的声明做替换，而不是对整串跑正则：本串里
        // `QLineEdit#fenceTitleEdit` 的 `border` 是**内联编辑框这个控件**的描边，
        // 不在「盒子外框」口径内 —— 它不随 boxBorder 消失；而它的 `border-radius`
        // 要与全项目其它输入框一致（都经 applyTokens 归零），所以跟随 boxRound。
        // 两种属性、两种口径混在同一串里，用显式拼接最不容易出错。
        // 本函数已登记为 ThemeManager 的 onStyle：开关变化时 ThemeManager 会追加一次
        // themeChanged，自动落到这里重刷，无需另接广播。
        const bool showBorder = ThemeManager::instance()->boxBorderEnabled();
        const bool rounded = ThemeManager::instance()->boxRoundedEnabled();
        const QString radius = rounded ? QStringLiteral("12px") : QStringLiteral("0px");
        const QString borderNormal = showBorder
            ? QStringLiteral("border: 1px solid rgba(255,255,255,0.10);") : QString();
        const QString borderHover = showBorder
            ? QStringLiteral("border: 1px solid rgba(255,255,255,0.18);") : QString();

        const QString qss =
            QStringLiteral("QWidget#FenceBox { background: %1; border-radius: ")
                .arg(Theme::panelBgString(0.10)) + radius + QLatin1String("; ") + borderNormal + QLatin1Char('}')
          + QStringLiteral("QWidget#FenceBox:hover { background: %1; ")
                .arg(Theme::panelBgString(0.18)) + borderHover + QLatin1Char('}')
          + QStringLiteral("QLabel#fenceTitle { color: #FFFFFF; font-weight: bold; font-size: 13px;"
                           "   padding: 2px 10px; background: %1;"
                           "   border-top-left-radius: ").arg(Theme::panelBgString(0.35))
                + radius + QLatin1String("; border-top-right-radius: ") + radius + QLatin1String("; }")
          + QStringLiteral("QLabel#fenceCount { color: rgba(255,255,255,0.55); font-size: 11px; padding-right: 10px;"
                           "   background: transparent; }"
                           "QLabel#fenceEmptyHint { color: rgba(255,255,255,0.42); font-size: 12px;"
                           "   background: transparent; }"
                           "QLineEdit#fenceTitleEdit { color: #FFFFFF; font-weight: bold; font-size: 13px;"
                           "   background: rgba(0,0,0,0.55); border: 1px solid #2f7bff;"
                           "   border-radius: ")
          + (rounded ? QStringLiteral("6px") : QStringLiteral("0px"))
          + QStringLiteral("; padding: 1px 8px; selection-background-color: #2f7bff; }"
                           "QWidget#fenceInner { background: transparent; }");
        setStyleSheet(qss);
    };
    applySheet();
    // E：仅登记「样式」通道 —— 盒子样式跟随主题色；整窗不透明度由所属窗口统一承担。
    ThemeManager::instance()->registerThemeTarget(this, applySheet);
}

// 盒子允许的最小尺寸（一列图标 + 标题栏）
// 作者：谭征
QSize FenceBox::minimumBoxSize() {
    return QSize(kInnerMargin * 2 + kCellW + 32,
                 titleBarHeight() + kInnerMargin * 2 + kCellH);
}

// 按当前图标数量推荐的“自然尺寸”，供窗口首次自动排布使用
// 作者：谭征
QSize FenceBox::naturalSize() const {
    const int count = m_items.size();
    const int cols = count > 0 ? qBound(1, qMin(count, 5), 5) : 3;
    const int rows = count > 0 ? (count + cols - 1) / cols : 1;
    const int w = kInnerMargin * 2 + cols * kCellW + (cols - 1) * kCellGap;
    const int h = titleBarHeight() + kInnerMargin * 2 + rows * kCellH + (rows - 1) * kCellGap;
    return QSize(qMax(w, 200), qMax(h, minimumBoxSize().height()));
}

// 设置条目
// 作者：谭征
void FenceBox::setItems(const QVector<DesktopItem>& items) {
    m_items = items;
    rebuild();
}

// 顺序变了、集合没变 ⇒ 原地重排：只把**现有按钮**按新顺序放回布局。
// 与 rebuild() 的关键差别：不销毁、不新建任何控件 —— 图标位图、信号连接、按钮状态原地保留。
// 用户感受就是"两个图标换了个位置"，而不是"整个盒子重新加载了一遍"。
// 键口径与 IconGridWindow 的选中接力 / 顺序持久化保持一致：sourcePath 归一化 + 小写。
// 任何一处对不上（增/删/改名/跨盒移动）立即返回 false，由调用方退回 setItems() 全量路径 ——
// 本函数**只做优化，不承担正确性**：不适用时行为与从前完全一致。
// 作者：谭征
bool FenceBox::reorderItemsInPlace(const QVector<DesktopItem>& items) {
    if (!m_innerLayout) return false;
    if (items.size() != m_items.size()) return false;   // 数量不同 ⇒ 必然要增删按钮，交给 rebuild()

    // 现有按钮建索引：sourcePath 为主键；shellPath（Dock 镜像层才赋值的备用路径）另登记一份，
    // 避免两套路径口径不一致时被误判成"集合变了"而白白退回全量重建。
    QHash<QString, DesktopIconButton*> byKey;
    QSet<DesktopIconButton*> allBtns;
    for (int i = 0; i < m_innerLayout->count(); ++i) {
        QLayoutItem* li = m_innerLayout->itemAt(i);
        auto* b = li ? qobject_cast<DesktopIconButton*>(li->widget()) : nullptr;
        if (!b) continue;
        allBtns.insert(b);
        const DesktopItem& bi = b->item();
        if (!bi.sourcePath.isEmpty())
            byKey.insert(QDir::fromNativeSeparators(bi.sourcePath).toLower(), b);
        if (!bi.shellPath.isEmpty())
            byKey.insert(QDir::fromNativeSeparators(bi.shellPath).toLower(), b);
    }
    // 布局里还有别的控件 / 按钮数对不上 ⇒ 结构未知，不冒险
    if (allBtns.size() != items.size()) return false;

    QVector<DesktopIconButton*> ordered;
    ordered.reserve(items.size());
    QSet<DesktopIconButton*> used;
    for (const DesktopItem& it : items) {
        DesktopIconButton* b = nullptr;
        if (!it.sourcePath.isEmpty())
            b = byKey.value(QDir::fromNativeSeparators(it.sourcePath).toLower(), nullptr);
        if (!b && !it.shellPath.isEmpty())
            b = byKey.value(QDir::fromNativeSeparators(it.shellPath).toLower(), nullptr);
        if (!b) return false;                 // 出现新条目 ⇒ 需要新建按钮，交回 rebuild()
        if (used.contains(b)) return false;   // 同一按钮被要求出现两次 ⇒ 数据有重，交回 rebuild()
        used.insert(b);
        ordered.append(b);
    }
    if (used.size() != allBtns.size()) return false;   // 有按钮没被新顺序覆盖 ⇒ 交回 rebuild()

    // —— 到这里可确认：新顺序只是现有这批按钮的一个排列 ⇒ 原地重排 ——
    m_items = items;   // 同步数据（relayoutIcons / 计数标签 / 尺寸口径都读它）
    // 摘下布局项但**不碰按钮**（与 relayoutIcons 同一手法：只 delete item）
    while (m_innerLayout->count()) {
        QLayoutItem* it = m_innerLayout->takeAt(0);
        // 只删 item、**不删按钮**（按钮引用已收在 ordered 里，等下按新顺序放回）；
        // 非按钮控件（理论上有的话）就地销毁，避免留下不在布局中的孤儿。
        if (it && it->widget() && !qobject_cast<DesktopIconButton*>(it->widget()))
            it->widget()->deleteLater();
        delete it;
    }
    m_innerCols = columnsForWidth();   // 与 rebuild() 同序：清空后再算列数
    for (int i = 0; i < ordered.size(); ++i) {
        m_innerLayout->addWidget(ordered[i], i / m_innerCols, i % m_innerCols,
                                 Qt::AlignTop | Qt::AlignLeft);
    }
    const int rows = ordered.isEmpty() ? 0 : (ordered.size() + m_innerCols - 1) / m_innerCols;
    applyInnerCompactPolicy(rows, m_innerCols);
    if (m_emptyHint) {
        m_emptyHint->setVisible(m_items.isEmpty());
        m_emptyHint->raise();
    }
    if (m_insertMarker) m_insertMarker->raise();
    updateTitleText();
    layoutOverlays();
    // 数量没变 ⇒ 尺寸不变；但按钮是"摘下再放回"，显式刷一次内层更稳妥
    if (m_inner) m_inner->update();
    return true;
}

// 增量应用：**复用**已有按钮，只为新增项新建按钮、把移除项的按钮销毁，再按新顺序重排。
// 与 rebuild() 的分工：rebuild = 整套换掉（每个按钮的图标位图全部重取）；本函数 = 只动差异项。
// 「从 Dock 拖一个图标进收纳盒」走的正是这里：目标盒只多一个按钮，其余按钮原地保留。
// 判定从严，任何不确定都返回 false，由调用方退回全量路径 —— 本函数只做优化、不承担正确性。
// 作者：谭征
bool FenceBox::applyItemsDelta(const QVector<DesktopItem>& items) {
    if (!m_innerLayout) return false;
    // 增删幅度过大 ⇒ 往往等于"换了一整批"，全量重建更干净（也避免把"重排"误判成大量增删）
    if (qAbs(items.size() - m_items.size()) > 8) return false;

    // 现有按钮按路径建索引（sourcePath 为主键，shellPath 兜底 —— 与 reorderItemsInPlace 同口径）
    QHash<QString, DesktopIconButton*> byKey;
    QVector<DesktopIconButton*> existing;
    existing.reserve(m_innerLayout->count());
    for (int i = 0; i < m_innerLayout->count(); ++i) {
        QLayoutItem* li = m_innerLayout->itemAt(i);
        auto* b = li ? qobject_cast<DesktopIconButton*>(li->widget()) : nullptr;
        if (!b) continue;
        existing.append(b);
        const DesktopItem& bi = b->item();
        if (!bi.sourcePath.isEmpty()) byKey.insert(itemKey(bi.sourcePath), b);
        if (!bi.shellPath.isEmpty())   byKey.insert(itemKey(bi.shellPath), b);
    }

    QVector<DesktopIconButton*> ordered;
    ordered.reserve(items.size());
    QSet<DesktopIconButton*> used;
    for (const DesktopItem& it : items) {
        DesktopIconButton* b = nullptr;
        if (!it.sourcePath.isEmpty())      b = byKey.value(itemKey(it.sourcePath), nullptr);
        if (!b && !it.shellPath.isEmpty()) b = byKey.value(itemKey(it.shellPath), nullptr);
        if (b && !used.contains(b)) {          // 命中已有按钮 ⇒ 原样复用（图标位图不动）
            used.insert(b);
            ordered.append(b);
            continue;
        }
        ordered.append(createItemButton(it));  // 新增项：只为它建一个按钮
    }

    // 未被复用的旧按钮 = 本次被移除的项（或数据里出现了重复路径）→ 销毁
    for (DesktopIconButton* b : existing) {
        if (used.contains(b)) continue;
        b->hide();
        b->deleteLater();
    }

    m_items = items;
    // 先在布局里摘下复用按钮（只删 item、不删按钮），再按新顺序连新增按钮一起放回
    while (m_innerLayout->count()) {
        QLayoutItem* li = m_innerLayout->takeAt(0);
        if (li && li->widget() && !qobject_cast<DesktopIconButton*>(li->widget()))
            li->widget()->deleteLater();
        delete li;
    }
    m_innerCols = columnsForWidth();
    for (int i = 0; i < ordered.size(); ++i) {
        m_innerLayout->addWidget(ordered[i], i / m_innerCols, i % m_innerCols,
                                 Qt::AlignTop | Qt::AlignLeft);
        ordered[i]->show();   // 新增按钮需显式 show；复用按钮本就是可见的，重复调用幂等
    }
    const int rows = ordered.isEmpty() ? 0 : (ordered.size() + m_innerCols - 1) / m_innerCols;
    applyInnerCompactPolicy(rows, m_innerCols);
    if (m_emptyHint) {
        m_emptyHint->setVisible(m_items.isEmpty());
        m_emptyHint->raise();
    }
    if (m_insertMarker) m_insertMarker->raise();
    updateTitleText();    // 数量变了 ⇒ 标题右侧计数也要跟着变
    layoutOverlays();
    if (m_inner) m_inner->update();
    return true;
}

// 列for宽度
// 作者：谭征
int FenceBox::columnsForWidth() const {
    if (!m_inner) return m_innerCols;
    const int avail = m_inner->width() - kInnerMargin * 2;
    if (avail <= 0) return m_innerCols;
    return qMax(1, (avail + kCellGap) / (kCellW + kCellGap));
}

// 构造一个盒内图标按钮并接好全部信号。
// 为什么必须抽成函数：rebuild()（全量）与 applyItemsDelta()（增量）两条路径都要建按钮，
// 各写一份接线漏掉任何一个 connect，都会让那条路径下的 F2 / Delete / 右键菜单 / 改名静默失效。
// —— 盒内图标的操作语义一律原样上抛给 IconGridWindow ——
// 作者：谭征
DesktopIconButton* FenceBox::createItemButton(const DesktopItem& item) {
    auto* btn = new DesktopIconButton(item, m_inner);
    btn->setProperty("category", m_category.toUtf8().constData());
    // 盒子内图标拖出到其它盒子/桌面后，由网格窗口统一刷新
    connect(btn, &DesktopIconButton::fileMovedOut, this, &FenceBox::fileMovedOut);
    connect(btn, &DesktopIconButton::selectionRequested,
            this, &FenceBox::iconSelectionRequested);
    connect(btn, &DesktopIconButton::selectionRequestedForMenu,
            this, &FenceBox::iconSelectionForMenu);
    connect(btn, &DesktopIconButton::deleteRequested,
            this, &FenceBox::iconDeleteRequested);
    connect(btn, &DesktopIconButton::contextMenuVisibleChanged,
            this, &FenceBox::iconContextMenuVisibleChanged);
    connect(btn, &DesktopIconButton::renameCommitted,
            this, &FenceBox::iconRenameCommitted);
    // 盒内图标被拖到所有收纳盒窗口之外（放回 Dock）：上抛窗口判定与执行。
    connect(btn, &DesktopIconButton::dragDroppedOutside,
            this, &FenceBox::iconDragDroppedOutside);
    // 编辑态需要带上"是哪个按钮"，故用 lambda 转发（信号本身只带 bool）。
    connect(btn, &DesktopIconButton::editingChanged, this, [this, btn](bool on) {
        emit iconEditingChanged(btn, on);
    });
    return btn;
}

// 重建
// 作者：谭征
void FenceBox::rebuild() {
    while (m_innerLayout->count()) {
        QLayoutItem* it = m_innerLayout->takeAt(0);
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }
    m_innerCols = columnsForWidth();
    for (int i = 0; i < m_items.size(); ++i) {
        auto* btn = createItemButton(m_items[i]);
        m_innerLayout->addWidget(btn, i / m_innerCols, i % m_innerCols,
                                 Qt::AlignTop | Qt::AlignLeft);
    }
    const int rows = m_items.isEmpty() ? 0 : (m_items.size() + m_innerCols - 1) / m_innerCols;
    applyInnerCompactPolicy(rows, m_innerCols);
    if (m_emptyHint) {
        m_emptyHint->setVisible(m_items.isEmpty());
        m_emptyHint->raise();
    }
    if (m_insertMarker) m_insertMarker->raise();
    updateTitleText();
    layoutOverlays();
}

// 重排图标
// 作者：谭征
void FenceBox::relayoutIcons(bool force) {
    if (!m_innerLayout) return;
    const int cols = columnsForWidth();
    if (!force && cols == m_innerCols) return;
    m_innerCols = cols;

    QVector<QWidget*> btns;
    btns.reserve(m_innerLayout->count());
    while (m_innerLayout->count()) {
        QLayoutItem* it = m_innerLayout->takeAt(0);
        if (it->widget()) btns.append(it->widget());
        delete it;
    }
    for (int i = 0; i < btns.size(); ++i) {
        m_innerLayout->addWidget(btns[i], i / cols, i % cols,
                                 Qt::AlignTop | Qt::AlignLeft);
    }
    const int rows = btns.isEmpty() ? 0 : (btns.size() + cols - 1) / cols;
    applyInnerCompactPolicy(rows, cols);
    if (m_emptyHint) m_emptyHint->raise();
    if (m_insertMarker) m_insertMarker->raise();
}

// 应用innercompactpolicy
// 作者：谭征
void FenceBox::applyInnerCompactPolicy(int rows, int cols) {
    if (!m_innerLayout) return;
    // 清理旧的拉伸策略，避免列数变化后残留 stretch 导致图标被拉散
    for (int c = 0; c < cols + 1; ++c) {
        m_innerLayout->setColumnStretch(c, 0);
        m_innerLayout->setColumnMinimumWidth(c, 0);
    }
    for (int r = 0; r < rows + 2; ++r) {
        m_innerLayout->setRowStretch(r, 0);
        m_innerLayout->setRowMinimumHeight(r, 0);
    }
    // 已用单元格固定为图标大小，防止 QGridLayout 自动拉宽/拉高后图标居中悬浮
    for (int c = 0; c < cols; ++c) m_innerLayout->setColumnMinimumWidth(c, kCellW);
    for (int r = 0; r < rows; ++r) m_innerLayout->setRowMinimumHeight(r, kCellH);
    // 右侧与下方的虚拟列/行吸收剩余空间，使图标从左上角紧凑排列
    if (cols > 0) m_innerLayout->setColumnStretch(cols, 1);
    if (rows >= 0) m_innerLayout->setRowStretch(rows + 1, 1);
}

// 更新title文本
// 作者：谭征
void FenceBox::updateTitleText() {
    if (!m_title) return;
    if (m_countLabel) m_countLabel->setText(QString::number(m_items.size()));
    const int countW = m_countLabel ? qMax(18, m_countLabel->sizeHint().width()) : 0;
    const int avail = qMax(20, width() - 20 - countW - 4);
    const QFontMetrics fm(m_title->font());
    m_title->setText(fm.elidedText(m_category, Qt::ElideRight, avail));
    m_title->setToolTip(m_category);
}

// 布局overlays
// 作者：谭征
void FenceBox::layoutOverlays() {
    if (m_countLabel) {
        const int cw = qMax(18, m_countLabel->sizeHint().width());
        m_countLabel->setGeometry(width() - cw - 2, 0, cw, titleBarHeight());
        m_countLabel->raise();
    }
    if (m_titleEdit && m_titleEdit->isVisible()) {
        m_titleEdit->setGeometry(6, 3, qMax(60, width() - 12), titleBarHeight() - 6);
        m_titleEdit->raise();
    }
    if (m_emptyHint && m_inner) {
        m_emptyHint->setGeometry(0, 0, m_inner->width(), m_inner->height());
        if (m_emptyHint->isVisible()) m_emptyHint->raise();
    }
}

// 进入就地重命名编辑态（双击标题触发，也可由外部调用）
// 作者：谭征
void FenceBox::beginInlineRename() {
    if (!m_titleEdit) return;
    m_renaming = true;
    m_titleEdit->setText(m_category);
    m_titleEdit->show();
    layoutOverlays();
    m_titleEdit->setFocus(Qt::MouseFocusReason);
    m_titleEdit->selectAll();
}

// 内联重命名：提交（Enter/失焦）/取消（Esc）。cleanupEditor 负责销毁编辑框并恢复标签绘制。
// 作者：谭征
void FenceBox::commitInlineRename() {
    if (!m_renaming || !m_titleEdit) return;
    m_renaming = false;                       // 先清标志，避免 editingFinished 二次进入
    const QString text = m_titleEdit->text().trimmed();
    m_titleEdit->hide();
    if (!text.isEmpty() && text != m_category) {
        emit renameCommitted(m_category, text);
    }
}

// 取消inline重命名
// 作者：谭征
void FenceBox::cancelInlineRename() {
    if (!m_titleEdit) return;
    m_renaming = false;
    m_titleEdit->hide();
}

// 应用级“点别处即提交”守卫（装在 qApp 上）：见 cpp 中的实现说明。
// 作者：谭征
bool FenceBox::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_titleEdit && event->type() == QEvent::KeyPress) {
        if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            cancelInlineRename();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ---------------------------------------------------------------- 拖动 / 缩放

Qt::Edges FenceBox::edgesAt(const QPoint& pos) const {
    Qt::Edges edges;
    if (pos.x() <= kEdgeMargin) edges |= Qt::LeftEdge;
    if (pos.x() >= width() - kEdgeMargin) edges |= Qt::RightEdge;
    if (pos.y() <= kEdgeMargin) edges |= Qt::TopEdge;
    if (pos.y() >= height() - kEdgeMargin) edges |= Qt::BottomEdge;
    return edges;
}

Qt::CursorShape FenceBox::cursorForEdges(Qt::Edges edges) {
    const bool l = edges & Qt::LeftEdge, r = edges & Qt::RightEdge;
    const bool t = edges & Qt::TopEdge, b = edges & Qt::BottomEdge;
    if ((l && t) || (r && b)) return Qt::SizeFDiagCursor;
    if ((r && t) || (l && b)) return Qt::SizeBDiagCursor;
    if (l || r) return Qt::SizeHorCursor;
    if (t || b) return Qt::SizeVerCursor;
    return Qt::ArrowCursor;
}

// 标题栏本身不承载任何编辑交互，不会与输入/选择冲突。
// 作者：谭征
void FenceBox::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
    if (m_renaming) cancelInlineRename();

    const Qt::Edges edges = edgesAt(e->pos());
    if (edges != Qt::Edges()) {
        m_boxResizing = true;
        m_resizeEdges = edges;
        m_startGeom = geometry();
        m_startGlobal = e->globalPos();
        setCursor(cursorForEdges(edges));
        raise();
        e->accept();
        return;
    }
    // 标题栏或空白内容区按下 -> 整盒拖动（图标本身仍走 DesktopIconButton 的拖拽）
    m_moving = true;
    m_startGeom = geometry();
    m_startGlobal = e->globalPos();
    setCursor(Qt::SizeAllCursor);
    raise();
    e->accept();
}

// 鼠标移动事件
// 作者：谭征
void FenceBox::mouseMoveEvent(QMouseEvent* e) {
    const QRect bounds = parentWidget() ? parentWidget()->rect() : QRect();

    if (m_moving && (e->buttons() & Qt::LeftButton)) {
        QPoint tl = m_startGeom.topLeft() + (e->globalPos() - m_startGlobal);
        // —— 自动对齐（2026-09-24）——
        // 全屏收纳模式下盒子是父容器（铺满工作区的收纳窗口）的子控件，坐标是**父局部坐标**，
        // 故候选 = 同父的其它盒子 + 父容器四边；网格原点从屏幕网格换算到父局部，
        // 这样这里的"格子"与 Dock / 收纳盒窗口吸附的是同一套格子，窗口之间不会错位。
        if (WindowSnap::enabledNow()) {
            QVector<QRect> magnets;
            QPoint localOrigin = WindowSnap::gridOrigin();
            int localMinY = INT_MIN;   // 父局部坐标下的顶部留白线
            if (QWidget* p = parentWidget()) {
                magnets.append(p->rect());
                const auto sibs = p->findChildren<FenceBox*>(QString(), Qt::FindDirectChildrenOnly);
                for (FenceBox* b : sibs) {
                    if (b && b != this && b->isVisible()) magnets.append(b->geometry());
                }
                const QPoint pGlobal = p->mapToGlobal(QPoint(0, 0));
                localOrigin -= pGlobal;
                // 顶部留白线从"父容器所在屏幕的工作区顶边"换算到父局部坐标；
                // 父容器铺满工作区时它正好等于 WindowSnap::topMargin()。
                localMinY = WindowSnap::topLimitGlobal(QRect(pGlobal, p->size())) - pGlobal.y();
            }
            tl = WindowSnap::resolve(tl, size(), magnets, localOrigin, nullptr, nullptr, localMinY);
        }
        if (bounds.isValid()) {
            tl.setX(qBound(0, tl.x(), qMax(0, bounds.width() - width())));
            tl.setY(qBound(0, tl.y(), qMax(0, bounds.height() - height())));
        }
        move(tl);
        e->accept();
        return;
    }
    if (m_boxResizing && (e->buttons() & Qt::LeftButton)) {
        const QPoint d = e->globalPos() - m_startGlobal;
        const QSize minS = minimumBoxSize();
        QRect g = m_startGeom;
        if (m_resizeEdges & Qt::LeftEdge) {
            int nx = m_startGeom.left() + d.x();
            if (bounds.isValid()) nx = qMax(0, nx);
            g.setLeft(qMin(nx, m_startGeom.right() - minS.width() + 1));
        }
        if (m_resizeEdges & Qt::RightEdge) {
            int nr = m_startGeom.right() + d.x();
            if (bounds.isValid()) nr = qMin(nr, bounds.width() - 1);
            g.setRight(qMax(nr, m_startGeom.left() + minS.width() - 1));
        }
        if (m_resizeEdges & Qt::TopEdge) {
            int ny = m_startGeom.top() + d.y();
            if (bounds.isValid()) ny = qMax(0, ny);
            g.setTop(qMin(ny, m_startGeom.bottom() - minS.height() + 1));
        }
        if (m_resizeEdges & Qt::BottomEdge) {
            int nb = m_startGeom.bottom() + d.y();
            if (bounds.isValid()) nb = qMin(nb, bounds.height() - 1);
            g.setBottom(qMax(nb, m_startGeom.top() + minS.height() - 1));
        }
        setGeometry(g);
        e->accept();
        return;
    }
    // 悬停：边缘显示缩放光标，标题栏显示移动光标。
    // C3：加变化检测 —— 本函数在鼠标移动时逐像素被调用，而 QWidget::setCursor() /
    // unsetCursor() 在同值时并不会短路（会走到平台层重复设置光标），属于纯浪费。
    // 只在“本次应显示的光标”与上次不同时才真正写一次。
    const Qt::Edges edges = edgesAt(e->pos());
    int wantKey = 0;                       // 0 = 默认光标（unsetCursor）
    Qt::CursorShape wantShape = Qt::ArrowCursor;
    if (edges != Qt::Edges()) {
        wantKey = 1 + static_cast<int>(edges);
        wantShape = cursorForEdges(edges);
    } else if (e->pos().y() < titleBarHeight()) {
        wantKey = 1000;
        wantShape = Qt::SizeAllCursor;
    }
    if (wantKey != m_lastHoverCursorKey) {
        m_lastHoverCursorKey = wantKey;
        if (wantKey == 0) unsetCursor();
        else setCursor(wantShape);
    }
    QWidget::mouseMoveEvent(e);
}

// 鼠标松开事件
// 作者：谭征
void FenceBox::mouseReleaseEvent(QMouseEvent* e) {
    if (m_moving || m_boxResizing) {
        const bool wasResize = m_boxResizing;
        m_moving = false;
        m_boxResizing = false;
        m_resizeEdges = Qt::Edges();
        unsetCursor();
        if (wasResize) relayoutIcons(true);   // 缩放结束按新宽度重排图标
        emit geometryEdited(m_category, geometry());
        e->accept();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

// 鼠标双击click事件
// 作者：谭征
void FenceBox::mouseDoubleClickEvent(QMouseEvent* e) {
    // 双击标题栏 -> 就地重命名分类
    if (e->button() == Qt::LeftButton && edgesAt(e->pos()) == Qt::Edges()
        && e->pos().y() < titleBarHeight()) {
        beginInlineRename();
        e->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}

// 离开事件
// 作者：谭征
void FenceBox::leaveEvent(QEvent* e) {
    if (!m_moving && !m_boxResizing) unsetCursor();
    QWidget::leaveEvent(e);
}

// 缩放事件
// 作者：谭征
void FenceBox::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    updateTitleText();
    layoutOverlays();
    if (!m_boxResizing) relayoutIcons(false);   // 缩放过程中不逐帧重排，释放时统一处理
}

// ---------------------------------------------------------------- 插入指示线

// 作者：谭征
int FenceBox::targetIndexFromPos(const QPoint& pos) const {
    if (m_innerLayout->count() == 0) return 0;
    int closest = -1;
    int minDist = INT_MAX;
    for (int i = 0; i < m_innerLayout->count(); ++i) {
        QWidget* w = m_innerLayout->itemAt(i)->widget();
        if (!w) continue;
        const int d = (w->geometry().center() - pos).manhattanLength();
        if (d < minDist) { minDist = d; closest = i; }
    }
    if (closest < 0) return m_innerLayout->count();
    QWidget* w = m_innerLayout->itemAt(closest)->widget();
    const QPoint c = w->geometry().center();
    const bool before = (pos.y() < c.y()) || (pos.y() == c.y() && pos.x() < c.x());
    return before ? closest : closest + 1;
}

// 更新插入指示线
// 作者：谭征
void FenceBox::updateInsertMarker(int targetIndex) {
    if (!m_insertMarker || m_innerLayout->count() == 0) { hideInsertMarker(); return; }
    const int count = m_innerLayout->count();
    const int cols = qMax(1, m_innerCols);
    QRect cell;
    if (targetIndex < count) {
        const int row = targetIndex / cols;
        const int col = targetIndex % cols;
        cell = m_innerLayout->cellRect(row, col);
    } else {
        // 追加到末尾：指示线画在最后一个图标的右侧
        const int lastIdx = count - 1;
        const int row = lastIdx / cols;
        const int col = lastIdx % cols;
        cell = m_innerLayout->cellRect(row, col);
        if (!cell.isValid()) { hideInsertMarker(); return; }
        cell.moveLeft(cell.right() + m_innerLayout->spacing() + 1);
    }
    if (!cell.isValid()) { hideInsertMarker(); return; }
    m_insertMarker->setGeometry(cell.left() - 1, cell.top(), 3, cell.height());
    m_insertMarker->raise();
    m_insertMarker->show();
}

// 隐藏插入指示线
// 作者：谭征
void FenceBox::hideInsertMarker() {
    if (m_insertMarker) m_insertMarker->hide();
}

// ------------------------------------------------- 外部拖拽悬停（Dock → 盒内插入位）
// 与自身 dragMoveEvent 的差异：这条路径上鼠标事件被**发起方窗口**（Dock）持有，
// 本盒收不到 QDragMoveEvent，所以插入下标与指示线必须能由全局坐标按需计算。
// 作者：谭征
bool FenceBox::showExternalDropMarker(const QPoint& globalPos, int* outIndex) {
    const int idx = externalDropIndexAt(globalPos);
    if (idx < 0) { hideInsertMarker(); return false; }
    updateInsertMarker(idx);
    if (outIndex) *outIndex = idx;
    return true;
}

// 只查询插入下标（松手时用），-1 = 落点不在本盒的图标网格上；不改变指示线显示
// 作者：谭征
int FenceBox::externalDropIndexAt(const QPoint& globalPos) const {
    if (!isVisible() || !m_inner || m_inner->size().isEmpty()) return -1;
    const QRect innerRect(m_inner->mapToGlobal(QPoint(0, 0)), m_inner->size());
    if (!innerRect.contains(globalPos)) return -1;
    return targetIndexFromPos(m_inner->mapFromGlobal(globalPos));
}

// 收起外部拖拽指示线（悬停离开 / 拖拽结束）
// 作者：谭征
void FenceBox::hideExternalDropMarker() {
    hideInsertMarker();
}

// ---------------------------------------------------------------- 拖放

// 作者：谭征
void FenceBox::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasFormat(QStringLiteral("application/x-desktopitem-id"))) {
        e->acceptProposedAction();
    } else if (e->mimeData()->hasUrls()) {
        // 外部文件（资源管理器/真实桌面）拖入：本程序只登记归属，绝不搬动真实文件。
        // 必须显式 Copy —— Explorer 同盘拖拽的默认动作是 MoveAction，
        // acceptProposedAction() 等于告诉系统「接收方要移走文件」（2026-09-14 用户投诉）。
        e->setDropAction(Qt::CopyAction);
        e->accept();
    } else {
        e->ignore();
    }
}

// 拖拽移动事件
// 作者：谭征
void FenceBox::dragMoveEvent(QDragMoveEvent* e) {
    if (e->mimeData()->hasFormat(QStringLiteral("application/x-desktopitem-id")) ||
        e->mimeData()->hasUrls()) {
        // 外部文件（不带 x-desktopitem-id）一律降级为 Copy：只登记归属，绝不搬动真实文件。
        // 内部拖拽（带私有 MIME）保留 Move 语义，与 QDrag::exec 的默认动作一致。
        if (!e->mimeData()->hasFormat(QStringLiteral("application/x-desktopitem-id"))) {
            e->setDropAction(Qt::CopyAction);
            e->accept();
        } else {
            e->acceptProposedAction();
        }
        const QPoint localPos = m_inner->mapFromParent(e->pos());
        updateInsertMarker(targetIndexFromPos(localPos));
    } else {
        e->ignore();
    }
}

// 拖拽离开事件
// 作者：谭征
void FenceBox::dragLeaveEvent(QDragLeaveEvent* e) {
    Q_UNUSED(e);
    hideInsertMarker();
}

// 投放事件
// 作者：谭征
void FenceBox::dropEvent(QDropEvent* e) {
    hideInsertMarker();
    const QMimeData* md = e->mimeData();
    if (md->hasFormat(QStringLiteral("application/x-desktopitem-id"))) {
        const QUuid id = QUuid::fromString(QString::fromUtf8(
            md->data(QStringLiteral("application/x-desktopitem-id"))));
        const QString sourceCat = QString::fromUtf8(
            md->data(QStringLiteral("application/x-source-category")));
        const QPoint localPos = m_inner->mapFromParent(e->pos());
        const int targetIndex = targetIndexFromPos(localPos);
        emit itemDropped(id, sourceCat, m_category, targetIndex);
        e->acceptProposedAction();
    } else if (md->hasUrls()) {
        QStringList paths;
        for (const QUrl& u : md->urls()) {
            if (u.isLocalFile()) paths.append(u.toLocalFile());
        }
        if (!paths.isEmpty()) emit filesDropped(paths, m_category);
        // 必须显式声明「只读（Copy）」语义。
        // 这里是外部文件拖入（资源管理器 / 真实桌面）。本程序对这类拖放只做一件事：
        // 在 CategoryStore 里记下「这个路径归属本分类」——**绝不搬动、绝不删除真实文件**。
        // 若照搬 acceptProposedAction()：Explorer 同盘拖拽的默认动作是 MoveAction，
        // 等于告诉系统「接收方要移走文件」，就可能产生真实移动（2026-09-14 用户投诉）。
        e->setDropAction(Qt::CopyAction);
        e->accept();
    } else {
        e->ignore();
    }
}
