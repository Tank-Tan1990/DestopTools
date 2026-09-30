/*
 * @file theme.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef THEME_H
#define THEME_H

#include <QColor>
#include <QFont>
#include <QFontInfo>
#include <QIcon>
#include <QString>
#include <QStringList>
#include <QSize>
#include <QPixmap>
#include <QPainter>
#include <QTransform>
#include <QMenu>
#include <QLineEdit>
#include <QPalette>
#include <QEvent>
#include <QPointer>
#include <QRegularExpression>
#include "thememanager.h"

// 高级科技感主题：深空蓝黑 + 青蓝霓虹辉光 + 玻璃拟态（半透明）面板
// 统一规范：
// 1) 所有字体为白色            —— textPrimary / textSecondary 均为纯白，弱文本用白色半透明
// 2) 按钮为线性（描边）样式    —— 透明底 + 青色描边 + 白色文字，悬停淡青辉光
// 3) 所有界面背景为半透明状态  —— 窗口/面板/输入框均使用 rgba 半透明玻璃，并开启 WA_TranslucentBackground
namespace Theme {

// 透明度有【两条互不相干】的通道（2026-09-21 需求定稿）：
// ① alphaF(base)   —— **线框 / 文字 / 填充色** 的固定不透明度。**永不随全局透明度滑块变化。**
// 需求原文：「所有的圆角线框透明度不调整」。
// 所有 border、divider、hover/selected 填充、文字灰阶一律走这里。
// ② bgAlphaF(base) —— **背景色**专用：base × 全局透明度（effectiveOpacity）。
// 需求原文：「透明度调节的是所有 UI 界面、弹框、菜单弹框等所有程序界面的背景」。
// 只允许 windowBgString / panelBgString / deepBgString 与各 glass 取色函数使用。
// 全应用所有背景色字面量都是 `rgba(17,26,46,1.0)` / `rgba(11,16,33,1.0)`，且**全部**经 applyTokens()
// 替换为 windowBgString(1.0) / panelBgString(1.0)。因此只要这两个函数改走 bgAlphaF()，
// 全局样式表、各窗口样式、QMenu / QComboBox 弹框、日历弹框、滚动条槽…… 背景会**一次性全部**跟随滑块，
// 而所有 rgba(34,211,238,x) 描边保持原样。这是本需求唯一需要改的"接缝"。
// 历史沿革（勿回退）：
// · 曾用 setWindowOpacity(整窗不透明度) 承担透明度 —— 它会把**边框和文字一起**淡化，
// 与「线框不调」冲突，且作用不到 QMenu/QComboBox 弹框（弹框不是登记目标）；
// · 现行方案把 alpha 放回背景色本身，整窗不透明度恒为 1.0（不再调用 setWindowOpacity）。
// 成本（已核算，勿被历史注释误导）：拖滑块本身**不触发任何下发** —— 滑块是 setTracking(false) 的，
// 拖动期间只由 sliderMoved 刷新百分比文字；松手时 valueChanged 才发一次 →
// ThemeManager 下发一次 → 全应用 repolish 一次。即"一次操作一次重刷"，不是"每像素一次"。

// ① 线框 / 文字 / 填充：固定不透明度（只夹取，不乘全局透明度）。
inline qreal alphaF(qreal base) {
    return qBound(0.0, base, 1.0);
}

// ② 背景色专用：基础 alpha × 全局界面背景透明度（100 → ×1.0，0 → ×0.15 保底可见）。
inline qreal bgAlphaF(qreal base) {
    return qBound(0.0, base * ThemeManager::instance()->effectiveOpacity(), 1.0);
}

// 将当前主题色处理成「深色背景色」：保留主题色相，但大幅降低饱和度/明度，
// 使背景不抢眼，同时整体色调随主题色变化。
inline QColor makeThemedBg(const QColor& accent, int maxSaturation, int targetLightness) {
    int h = 0, s = 0, l = 0;
    accent.getHsl(&h, &s, &l);
    if (h < 0) h = 0;
    int ns = qBound(0, int(s * 0.20), maxSaturation);
    if (ns < 5) ns = 5; // 至少保留一点色相倾向
    int nl = qBound(0, targetLightness, 255);
    QColor c;
    c.setHsl(h, ns, nl);
    return c;
}

inline QColor windowBgColor() { return makeThemedBg(ThemeManager::instance()->accentColor(), 55, 20); }
inline QColor panelBgColor()  { return makeThemedBg(ThemeManager::instance()->accentColor(), 50, 14); }
inline QColor deepBgColor()   { return makeThemedBg(ThemeManager::instance()->accentColor(), 45, 10); }

// 注意：这三个是**背景色**字符串 → 用 bgAlphaF（随全局透明度）；线框请一律用 alphaF。
inline QString windowBgString(qreal baseAlpha = 1.0) {
    const QColor c = windowBgColor();
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red()).arg(c.green()).arg(c.blue())
        .arg(bgAlphaF(baseAlpha), 0, 'f', 2);
}
// 面板bgstring
inline QString panelBgString(qreal baseAlpha = 1.0) {
    const QColor c = panelBgColor();
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red()).arg(c.green()).arg(c.blue())
        .arg(bgAlphaF(baseAlpha), 0, 'f', 2);
}
// deepbgstring
inline QString deepBgString(qreal baseAlpha = 1.0) {
    const QColor c = deepBgColor();
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red()).arg(c.green()).arg(c.blue())
        .arg(bgAlphaF(baseAlpha), 0, 'f', 2);
}

// ---------- 动态主题：把硬编码的「青蓝强调色 / 辅助蓝 / 深蓝灰背景」替换为当前主题色 ----------
// 所有 Theme::*Style() 与 globalStyleSheet() 的返回值都会经过本函数，
// 因此改 ThemeManager 的主题色/透明度后，重新生成样式表即可让全部界面颜色联动。
inline QString applyTokens(QString s) {
    const ThemeManager* tm = ThemeManager::instance();
    const QColor accent = tm->accentColor();
    const QString accentRgb = QString::fromLatin1("%1,%2,%3")
        .arg(accent.red()).arg(accent.green()).arg(accent.blue());
    const QString accentHex = accent.name();
    // 悬停高亮与辅助蓝由主题色派生，保持同色系层次
    const QString hoverHex = accent.lighter(115).name();
    const QColor darker = accent.darker(115);
    const QString darkerHex = darker.name();
    const QString darkerRgb = QString::fromLatin1("%1,%2,%3")
        .arg(darker.red()).arg(darker.green()).arg(darker.blue());

    s.replace(QStringLiteral("#22D3EE"), accentHex, Qt::CaseInsensitive);
    s.replace(QStringLiteral("34,211,238"), accentRgb);
    s.replace(QStringLiteral("#67E8F9"), hoverHex, Qt::CaseInsensitive);
    s.replace(QStringLiteral("#3B82F6"), darkerHex, Qt::CaseInsensitive);
    s.replace(QStringLiteral("59,130,246"), darkerRgb);

    // 把写死的深蓝灰背景替换为随主题色变化的深色背景
    s.replace(QStringLiteral("rgba(17,26,46,1.0)"), windowBgString(1.0));
    s.replace(QStringLiteral("rgba(11,16,33,1.0)"), panelBgString(1.0));

    // 「盒子使用圆角」开关（Appearance/boxRound，2026-09-22）
    // 需求：「选中则所有界面、弹框、弹框菜单等所有使用的线框显示**圆角**矩形线框，
    // 取消选中则显示**方角**矩形线框」。
    // 放在 applyTokens 里是唯一正确的位置：全项目所有 Theme::*Style() 与
    // buildGlobalStyleSheet() 的返回值都必经本函数 → 一处改动即覆盖
    // 「面板 / 对话框 / 弹出菜单 / 输入框 / 按钮 / 滚动条 / 复选框指示符 …」全部线框。
    // 逐个控件去改既改不全，也一定会随新增控件再次漏掉。
    // 语义刻意做成「勾选 = 不动原有值」：各控件圆角本就不同（面板 16 / 按钮 10 / 菜单 12 /
    // 指示符 3 …），勾选时保留这些既有值，取消时统一压成 0 —— 两态之间只差"圆角有无"，
    // 不会顺手改动任何已定稿的视觉层次。
    // 匹配范围**不含分号与右花括号**（`[^;}]+`），所以替换后两者都原样保留
    // → `border-radius: 16px;` ⇒ `border-radius: 0px;`。
    // 五个属性一次匹配：单个 border-radius 与四个按角的 *-radius 都要压平，
    // 否则标题栏（用的是 border-top-left/right-radius）会残留圆角。
    // 终止符写成 `[^;}]+`（**不是** `[^;]+`）：属性值后面偶尔会直接跟 `}` 而没有分号
    // （如 `border-radius: 6px }`），只用分号收尾的话 `[^;]+` 会一路吃到串尾，
    // 把后面的 `}` 甚至下一段声明一起替换掉 —— 那就不再是"改圆角"而是"改坏了样式"。
    if (!tm->boxRoundedEnabled()) {
        static const QRegularExpression reRadius(QStringLiteral(
            "(border-radius|border-top-left-radius|border-top-right-radius|"
            "border-bottom-left-radius|border-bottom-right-radius)\\s*:\\s*[^;}]+"));
        s.replace(reRadius, QStringLiteral("\\1: 0px"));
    }
    return s;
}

// 「盒子显示边框」开关（Appearance/boxBorder，2026-09-22）
// 需求：「选中则**收纳盒和桌面助手界面外**显示线框边框，取消选中则不显示线框边框」。
// 只在**盒子级**容器上调用（收纳盒窗口 IconGridWindow / 桌面助手 AssistantWindow 与
// 其面板 SidePanelWidget 的窗口层 + 卡片层），**不要**把它当成 panelStyle() 的替代品 —
// panelStyle() 还被十余个对话框（消息框/输入框/备份/待办/壁纸/文件搜索…）当卡片用，
// 套上本函数会让那些对话框一并丢边框，与需求范围不符。
// 语义：true 原样返回（applyTokens 后的样式）；false 删掉那一条 `border: 1px solid …;`
// 背景、圆角、字色全部不变 —— 只是不画那圈线。
// 幂等：applyTokens 对已替换过的串是 no-op（替换目标已不存在），重复调用安全。
inline QString boxChrome(QString s) {
    s = applyTokens(s);
    if (!ThemeManager::instance()->boxBorderEnabled()) {
        // `border: 1px solid rgba(...);` 整条删除；不带分号的部分（如 `border: none`）不受影响。
        static const QRegularExpression reBorder(
            QStringLiteral("border\\s*:\\s*1px\\s+solid\\s+[^;]*;"));
        s.remove(reBorder);
    }
    return s;
}

// 收纳盒内部两块区域的**常驻**线框（2026-09-22）
// 现象：在设置中心取消勾选「盒子显示边框」后，**分类标签栏**与**图标区**的外线框
// 也一起消失了（按口径只该消失最外层那一圈）。
// 根因：这两圈线过去是**无意中**由卡片样式表"传播"来的。IconGridWindow 的 card 用的是
// `panelStyle()` 这类**无选择器**规则，而 Qt 的样式表会把无选择器的声明套到该
// widget 的**每一个后代**上 —— 于是裸 `QWidget` 的 m_categoryBar、以及
// `QScrollArea` 的 viewport 各自白捡到一份
// `background + border: 1px solid … + border-radius: 16px`，凭空显出圆角线框
// （实测该框高度 34px，与 m_categoryBar 的 setFixedHeight(34) 完全吻合）。
// 一旦 `boxChrome()` 在「盒子显示边框」关闭时从 card 串里删掉那条 border，
// 所有"白捡"到它的子控件就同时丢框 —— 与"只控制盒子最外层"的口径不符。
// 口径（2026-09-22 用户明确）：这**两个内部框与 boxBorder 无关，必须常驻**。
// ⇒ 显式给出**带选择器**的规则（带选择器 ⇒ 只匹配自身、不再向子控件传播），
// 颜色/圆角照旧随主题色、透明度、「盒子使用圆角」联动（都经 applyTokens）。
// 两处调用点都不要图省事改回裸 `background: 透明;`，否则又会退回上面的传播机制。
inline QString categoryStripStyle() {
    return applyTokens(QStringLiteral(
        "QWidget#iconGridCategoryBar {"
        " background: transparent;"
        " border: 1px solid rgba(34,211,238,0.22);"
        " border-radius: 16px;"
        "}"
    ));
}

inline QString iconAreaStyle() {
    return applyTokens(QStringLiteral(
        "QWidget#iconGridViewport {"
        " background: transparent;"
        " border: 1px solid rgba(34,211,238,0.22);"
        " border-radius: 16px;"
        "}"
    ));
}

// 「盒子使用圆角」在 C++ 侧的**唯一**取半径出口（2026-09-22）
// applyTokens 只压得动 QSS 里的 border-radius；而**窗口遮罩（setMask/QRegion）与自绘圆角
// （paintEvent 的 drawRoundedRect）根本不走 QSS** —— 它们写死 15px 时，两态就会打架：
// 取消勾选 → 线框全部被压成方角，但遮罩仍是 15px 圆角，于是遮罩把方角线框的四角
// **切出缺口**（看到的是"外框像圆角、内容区像方角豁口"），正是 2026-09-22 设置中心的实测现象。
// ⇒ 凡 C++ 侧要画圆角或裁圆角，一律用本函数取半径，禁止再写死数字。
// base 由调用方按该处原有视觉值给出（如设置中心 = 15），勾选时原样返回、取消时归零。
inline int radiusPx(int base) {
    return ThemeManager::instance()->boxRoundedEnabled() ? base : 0;
}

// 「在快捷方式图标上显示箭头」开关（Appearance/shortcutArrow，2026-09-22）
// 需求：「选中则 dock/收纳盒内所有的快捷方式显示快捷方式箭头，取消选中所有快捷方式图标
// 不显示快捷方式箭头」。
// 实现上刻意复用**既有的 arrowScale 通道**而不是新增一个 bool 参数：
// DesktopScanner::loadIcon 的进程内图标缓存键已经含 arrowScale
// （`parsename|effSize|arrowScale`），用 0 表示"不要箭头"后，开关两态各自命中各自的缓存条目，
// 切换回来时能直接复用上次的图，不存在"开关变了但命中旧图"的串味问题。
// DesktopScanner::composeShortcutOverlay 对 scale<=0 直接原图返回（见其实现）。
inline double shortcutArrowOverlayScale(double scale) {
    return ThemeManager::instance()->shortcutArrowEnabled() ? scale : 0.0;
}

// 把某个「背景基色」套上当前界面背景透明度（**背景专用**；线框/文字请勿使用本函数）。
inline QColor applyBgAlpha(const QColor& c) {
    QColor r = c;
    r.setAlpha(qRound(255.0 * bgAlphaF(1.0)));
    return r;
}

// ---------- 玻璃背景常量（底色不透明度 = 全局透明度；由「外观设置 → 透明度」控制） ----------
inline QColor windowGlass()  { return applyBgAlpha(windowBgColor()); }
inline QColor panelGlass()   { return applyBgAlpha(windowBgColor()); }
inline QColor dialogGlass()  { return applyBgAlpha(windowBgColor()); }
inline QColor inputGlass()   { return applyBgAlpha(windowBgColor()); }
inline QColor cardGlass()    { return applyBgAlpha(windowBgColor()); }

// 与展示详情截图「设置中心」面板完全一致的视觉 token
inline QColor screenshotPanelBg()      { return applyBgAlpha(windowBgColor()); }
inline QColor screenshotPanelBorder()  { QColor c = ThemeManager::instance()->accentColor(); c.setAlpha(56); return c; }   // 柔和主题描边
inline QColor screenshotItemSelected() { QColor c = ThemeManager::instance()->accentColor(); c.setAlpha(46); return c; }   // 选中项微填充
inline QColor screenshotItemBorder()   { QColor c = ThemeManager::instance()->accentColor(); c.setAlpha(115); return c; }  // 选中项边框

// ---------- 色板（兼容旧接口名） ----------
inline QColor windowBg()        { return windowGlass(); }
inline QColor windowBgDeep()    { return applyBgAlpha(deepBgColor()); }
inline QColor panelBg()         { return panelGlass(); }
inline QColor panelBgGlass()    { return applyBgAlpha(windowBgColor()); }
inline QColor panelBorder()     { QColor c = ThemeManager::instance()->accentColor(); c.setAlpha(110); return c; } // 主题色辉光边框
inline QColor panelBorderWeak() { return QColor(255, 255, 255, 40); }
inline QColor panelHover()      { QColor c = ThemeManager::instance()->accentColor(); c.setAlpha(60); return c; }
inline QColor panelPress()      { QColor c = ThemeManager::instance()->accentColor(); c.setAlpha(110); return c; }

// 所有字体统一为白色；弱文本用白色半透明，仍属白色家族
inline QColor textPrimary()     { return QColor("#FFFFFF"); }
inline QColor textSecondary()   { return QColor("#FFFFFF"); }
inline QColor textMuted()       { return QColor(255, 255, 255, 140); } // rgba(255,255,255,0.55)

inline QColor accentCyan()      { return ThemeManager::instance()->accentColor(); }
inline QColor accentBlue()      { return ThemeManager::instance()->accentColor().darker(115); }
inline QColor accentHover()     { return ThemeManager::instance()->accentColor().lighter(115); }
inline QColor accentGlow()      { QColor c = ThemeManager::instance()->accentColor(); c.setAlpha(90); return c; }

inline QColor danger()          { return QColor("#F87171"); }
inline QColor success()         { return QColor("#34D399"); }
inline QColor warning()         { return QColor("#FBBF24"); }

inline QColor shadow()          { return QColor(0, 0, 0,   120); }
inline QColor divider()         { return QColor(255, 255, 255, 36); }
inline QColor inputBg()         { return inputGlass(); }

// ---------- 尺寸常量（统一所有窗口/对话框，避免出现标题栏高度、按钮尺寸不一致） ----------
inline int titleBarHeight()       { return 42; }   // 所有窗口/对话框统一标题栏高度
inline int collapsedHeight()       { return 44; }   // 折叠后仅保留标题栏时的最小高度（>= titleBarHeight）
inline QSize headerButtonSize()    { return QSize(24, 24); }  // 标题栏内方形工具按钮
inline QSize collapseButtonSize()  { return QSize(22, 22); }  // 折叠/展开三角按钮

// ---------- 字体 ----------
inline QFont appFont() {
    static const QStringList candidates = {
        QStringLiteral("Microsoft YaHei"),
        QStringLiteral("PingFang SC"),
        QStringLiteral("SimHei"),
        QStringLiteral("SimSun")
    };
    for (const QString& family : candidates) {
        QFont f(family);
        if (QFontInfo(f).exactMatch()) {
            f.setPointSize(9);
            return f;
        }
    }
    return QFont();
}

// ---------- 图标资源（线性切图） ----------
inline QIcon icon(const QString& name) {
    QIcon ico;
    ico.addFile(QStringLiteral(":/icons/%1_1x.png").arg(name), QSize(24, 24));
    ico.addFile(QStringLiteral(":/icons/%1_2x.png").arg(name), QSize(48, 48));
    return ico;
}

inline QString iconPath(const QString& name, int scale = 1) {
    return QStringLiteral(":/icons/%1_%2x.png").arg(name).arg(scale);
}

// 将现有图标重绘为指定颜色，保留透明区域，用于把彩色切图转成纯白/主题色版本
inline QPixmap recolorPixmap(const QPixmap& src, const QColor& color) {
    QPixmap dst(src.size());
    dst.fill(Qt::transparent);
    QPainter p(&dst);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);
    p.drawPixmap(0, 0, src);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(dst.rect(), color);
    p.end();
    return dst;
}

// 返回一个纯白版本的图标（保留 alpha），用于标题栏等需要中性白色的场景
inline QIcon whiteIcon(const QString& name) {
    const QIcon src = icon(name);
    QIcon white;
    const auto sizes = src.availableSizes();
    if (sizes.isEmpty()) {
        for (int sz : {18, 24, 36, 48}) {
            QPixmap pm = src.pixmap(sz, sz);
            if (pm.isNull()) continue;
            white.addPixmap(recolorPixmap(pm, Qt::white));
        }
    } else {
        for (const QSize& s : sizes) {
            QPixmap pm = src.pixmap(s);
            if (pm.isNull()) continue;
            white.addPixmap(recolorPixmap(pm, Qt::white));
        }
    }
    return white;
}

// 统一折叠/展开指示图标（上三角 ↔ 下三角）：各窗口不再各自实现旋转逻辑，保证完全一致
inline QPixmap collapsePixmap(bool collapsed, int size = 14) {
    QPixmap pm = Theme::icon("collapse").pixmap(size, size);
    if (!collapsed) return pm;
    QTransform t;
    t.rotate(180);
    return pm.transformed(t, Qt::SmoothTransformation);
}

// ---------- 通用样式串 ----------
// 窗口玻璃：半透明深底 + 青色辉光描边（无圆角，作为 HUD 矩形面板）
inline QString windowStyle() {
    return applyTokens(QStringLiteral(
        "background-color: rgba(17,26,46,1.0);"
        "border: 1px solid rgba(34,211,238,0.20);"
    ));
}

// 面板玻璃：与展示详情截图一致（alpha 1.0、柔和青描边、16px 大圆角）
inline QString panelStyle() {
    return applyTokens(QStringLiteral(
        "background-color: rgba(17,26,46,1.0);"
        "border: 1px solid rgba(34,211,238,0.22);"
        "border-radius: 16px;"
    ));
}

inline QString panelStyleNoBorder() {
    return applyTokens(QStringLiteral(
        "background-color: rgba(17,26,46,1.0); border-radius: 16px;"
    ));
}

// 设置中心截图同款面板样式（可用于统一命名对象）
inline QString settingsPanelStyle() {
    return applyTokens(QStringLiteral(
        "QFrame#settingsPanel {"
        "background-color: rgba(17,26,46,1.0);"
        "border: 1px solid rgba(34,211,238,0.22);"
        "border-radius: 16px;"
        "}"
    ));
}

inline QString cardStyle() {
    return applyTokens(QStringLiteral(
        "QFrame {"
        "background-color: rgba(17,26,46,1.0);"
        "border: 1px solid rgba(34,211,238,0.18);"
        "border-radius: 12px;"
        "}"
    ));
}

inline QString scrollBarStyle() {
    return applyTokens(QStringLiteral(
        "QScrollBar:vertical { background: rgba(17,26,46,1.0); width: 8px; border-radius: 4px; margin: 2px; }"
        "QScrollBar::handle:vertical { background: rgba(34,211,238,0.40); border-radius: 4px; min-height: 32px; }"
        "QScrollBar::handle:vertical:hover { background: rgba(34,211,238,0.60); }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
        "QScrollBar:horizontal { background: rgba(17,26,46,1.0); height: 8px; border-radius: 4px; margin: 2px; }"
        "QScrollBar::handle:horizontal { background: rgba(34,211,238,0.40); border-radius: 4px; min-width: 32px; }"
        "QScrollBar::handle:horizontal:hover { background: rgba(34,211,238,0.60); }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }"
    ));
}

// 图标按钮（桌面项/线性）：透明底，白色文字，青色悬停辉光
inline QString iconButtonStyle() {
    return applyTokens(QStringLiteral(
        "DesktopIconButton { color: #FFFFFF; background: transparent; border: 1px solid transparent; border-radius: 10px; padding: 4px; }"
        "DesktopIconButton:hover { background: rgba(34,211,238,0.12); border-color: rgba(34,211,238,0.20); }"
        "DesktopIconButton:pressed { background: rgba(34,211,238,0.20); }"
    ));
}

inline QString listWidgetStyle() {
    return applyTokens(QStringLiteral(
        "QListWidget { background: rgba(17,26,46,1.0); border: 1px solid rgba(34,211,238,0.12); border-radius: 12px; color: #FFFFFF; outline: none; padding: 6px; }"
        "QListWidget::item { padding: 12px; border-radius: 10px; color: #FFFFFF; margin: 2px 0px; }"
        "QListWidget::item:selected { background: rgba(34,211,238,0.18); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.45); }"
        "QListWidget::item:hover { background: rgba(34,211,238,0.08); }"
        "QListWidget::item:selected:hover { background: rgba(34,211,238,0.22); }"
    ));
}

// 线性（描边）按钮：与截图一致——透明底 + 青色描边 + 白色文字 + 10px 圆角
inline QString pushButtonStyle() {
    return applyTokens(QStringLiteral(
        "QPushButton { color: #FFFFFF; background: rgba(255,255,255,0.04); border: 1px solid rgba(34,211,238,0.55); border-radius: 10px; padding: 8px 20px; }"
        "QPushButton:hover { background: rgba(34,211,238,0.12); border-color: #22D3EE; }"
        "QPushButton:pressed { background: rgba(34,211,238,0.22); border-color: #67E8F9; }"
    ));
}

// 主要按钮（默认/提交）：仍保持线性描边，仅描边更亮、字重更大
inline QString primaryButtonStyle() {
    return applyTokens(QStringLiteral(
        "QPushButton { color: #FFFFFF; background: rgba(34,211,238,0.10); border: 1px solid #22D3EE; border-radius: 10px; padding: 8px 20px; font-weight: bold; }"
        "QPushButton:hover { background: rgba(34,211,238,0.22); border-color: #67E8F9; }"
        "QPushButton:pressed { background: rgba(34,211,238,0.32); border-color: #67E8F9; }"
    ));
}

inline QString toolButtonStyle() {
    return applyTokens(QStringLiteral(
        "QToolButton { color: #FFFFFF; border: 1px solid transparent; border-radius: 10px; background: transparent; font-weight: bold; }"
        "QToolButton:hover { background: rgba(34,211,238,0.12); border-color: rgba(34,211,238,0.20); color: #FFFFFF; }"
        "QToolButton:pressed { background: rgba(34,211,238,0.20); color: #FFFFFF; }"
        "QToolButton:checked { background: rgba(34,211,238,0.16); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.30); }"
    ));
}

inline QString tabButtonStyle() {
    return applyTokens(QStringLiteral(
        "QToolButton { color: rgba(255,255,255,0.72); border: 1px solid transparent; border-radius: 16px; padding: 6px 18px; background: transparent; }"
        "QToolButton:hover { background: rgba(34,211,238,0.12); color: #FFFFFF; border-color: rgba(34,211,238,0.18); }"
        "QToolButton:checked { background: rgba(34,211,238,0.18); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.45); }"
        "QToolButton:pressed { background: rgba(34,211,238,0.26); color: #FFFFFF; }"
    ));
}

inline QString lineEditStyle() {
    // `QLineEdit::placeholder` 是**死规则**：Qt5 样式表没有 `::placeholder` 选择器（写了也不报错、但永不匹配）。
    // 此处保留（无害、且能表达意图），真正的占位色由下面 applyPlaceholderColor() 显式写 palette 决定。
    return applyTokens(QStringLiteral(
        "QLineEdit { color: #FFFFFF; background: rgba(17,26,46,1.0); border: 1px solid rgba(34,211,238,0.22); border-radius: 14px; padding: 4px 10px; }"
        "QLineEdit:focus { border-color: #22D3EE; background: rgba(17,26,46,1.0); }"
        "QLineEdit::placeholder { color: rgba(255,255,255,0.45); }"
    ));
}

// 输入框占位文字色（2026-09-21 定稿）
// 需求：「这个字体是禁用时的那个灰色，不要跟随主题颜色而变成黑色」。
// 根因（截图像素实测）：占位色**从来不由上面的样式表决定**——Qt5 QSS 没有 `::placeholder` 选择器，
// 真正生效的是 `QPalette::PlaceholderText`，其默认值 = `Text` 角色取 **50% alpha**（即半透明黑）。
// 深色背景下 `0.5 × 黑` 合成出来是「比背景更黑」的暗字，而不是灰：
// 产品实测 搜索框底色 rgb(16,23,22) → 笔画 rgb(8,11,11)（恰为底色 × 50%）。
// 又因为 `Text` 角色是随主题/样式表走的，占位色就成了"跟随主题色"的活值。
// 修法：把 `PlaceholderText` 显式钉成固定「禁用灰」= 与 `QMenu::item:disabled` 同款的 rgba(255,255,255,0.40)，
// 与主题色/背景透明度完全解耦。**禁再用样式表去调占位色。**
inline QColor placeholderTextColor() {
    return QColor(255, 255, 255, qRound(alphaF(0.40) * 255));   // 固定禁用灰，不随主题色
}

// 占位色守卫：**任何一次重抛光之后立刻把占位色钉回固定禁用灰**。
// 为什么必须有守卫（用户实测症状：「改主题色后这个字就变成黑色」）：
// 改主题色 / 拖透明度会触发全局重刷 —— `a.setStyleSheet(globalStyleSheet())` +
// 各窗口 onStyle() 重设样式表 + Qt 的 repolish。这条链路上控件的调色板会被重新推导，
// `PlaceholderText` 一旦落回默认值（= `Text` 的 50% alpha = **半透明黑**），
// 深色底上就合成成"变黑"；而同样的规则刷新（字符串没变时 Qt 会跳过）不一定再把它改回来，
// 于是黑住不动 —— 这正是"只在改完主题色之后才变黑"的成因。
// 只在创建时设一次 palette 挡不住这条路，必须监听 StyleChange / PaletteChange 后重设。
// 幂等：只在当前值不等于目标值时写，且用 m_busy 挡住 setPalette 引发的嵌套事件，不会来回抖动。
class PlaceholderColorGuard : public QObject {
public:
    explicit PlaceholderColorGuard(QLineEdit* edit) : QObject(edit), m_edit(edit) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        switch (event->type()) {
        case QEvent::StyleChange:
        case QEvent::PaletteChange:
        case QEvent::ApplicationPaletteChange:
        case QEvent::Polish:
            reassert();
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);   // 只看不改，绝不吞事件
    }

private:
    void reassert() {
        if (m_busy || !m_edit) return;
        const QColor want = placeholderTextColor();
        if (m_edit->palette().color(QPalette::PlaceholderText) == want) return;   // 已是目标色：不写
        m_busy = true;
        QPalette pal = m_edit->palette();
        pal.setColor(QPalette::PlaceholderText, want);
        m_edit->setPalette(pal);
        m_busy = false;
    }

    QPointer<QLineEdit> m_edit;
    bool                m_busy = false;
};

inline void applyPlaceholderColor(QLineEdit* edit) {
    if (!edit) return;
    QPalette pal = edit->palette();
    pal.setColor(QPalette::PlaceholderText, placeholderTextColor());
    edit->setPalette(pal);
    if (!edit->property("dt_placeholderGuard").toBool()) {   // 守卫只装一次
        edit->setProperty("dt_placeholderGuard", true);
        edit->installEventFilter(new PlaceholderColorGuard(edit));   // 父对象 = edit，随其析构
    }
}

// 输入框统一样式入口：**样式表 + 占位色必须一起设**。
// 顺序不能反：`setStyleSheet()` 会触发 QStyleSheetStyle 重新 polish 并回写 widget palette，
// 先设 palette 会被覆盖掉。所以调用方一律用本函数，别再单独调 `setStyleSheet(lineEditStyle())`。
inline void applyLineEditStyle(QLineEdit* edit) {
    if (!edit) return;
    edit->setStyleSheet(lineEditStyle());
    applyPlaceholderColor(edit);
}

inline QString quickToolButtonStyle() {
    return applyTokens(QStringLiteral(
        "QToolButton { color: #FFFFFF; border: 1px solid transparent; border-radius: 10px; background: transparent; padding: 3px; }"
        "QToolButton:hover { background: rgba(34,211,238,0.14); border-color: rgba(34,211,238,0.22); }"
        "QToolButton:pressed { background: rgba(34,211,238,0.22); }"
    ));
}

inline QString menuStyle() {
    return applyTokens(QStringLiteral(
        "QMenu { background-color: rgba(17,26,46,1.0); border: 1px solid rgba(34,211,238,0.28); border-radius: 10px; padding: 6px; }"
        "QMenu::item { color: #FFFFFF; padding: 7px 26px; border-radius: 6px; }"
        "QMenu::item:selected { background: rgba(34,211,238,0.20); color: #FFFFFF; }"
        "QMenu::item:disabled { color: rgba(255,255,255,0.40); }"
        "QMenu::separator { background: rgba(255,255,255,0.12); height: 1px; margin: 6px 10px; }"
        "QMenu::icon { padding-left: 8px; }"
    ));
}

// 菜单弹层必须用本函数应用样式 —— **不要**只写 setStyleSheet(menuStyle())。
// 2026-09-21 实测定论（tools/menualpha：把菜单弹在**纯白画布**上读真实像素，判据是
// 「透明度 100 → 0 时像素是否变亮（与白底混合）」）：
// · 只 setStyleSheet(menuStyle()) 时，QMenu 的 WS_EX_LAYERED = **no**。
// 没有 alpha 合成位 → 它的半透明背景只能合成到**黑色 backing store** 上：
// t=100→rgb(16,22,23)，t=0→rgb(4,6,6) —— 越透明**越黑**，永远透不出后面的桌面。
// 这就是用户反馈「这些菜单红框内的背景没被透明度控制」的真相：
// alpha 其实一直在变（字符串级也一直是对的），但底层是黑，画面上看不出来。
// · 同机同 Qt，普通 QWidget（Qt::Window **与** Qt::Popup）+ WA_TranslucentBackground
// 都能拿到 LAYERED=yes → 说明本机 Qt 的半透明窗口机制是好的，只有 QMenu 拿不到。
// · 拿得到的唯一路径：**先 setWindowFlags 触发平台窗口重建，再设 WA_TranslucentBackground**。
// 原因：QMenu 在构造 / 首次 winId() 时就把平台窗口 CreateWindowEx 建好了，属性设晚了
// 影响不到已存在的窗口；而 Qt 只在**建窗那一刻**按该属性决定 alpha buffer 与
// WS_EX_LAYERED（QWindow::format().hasAlpha()）。
// · flags 必须与 QMenu 默认值**不同**才触发重建。QMenu 默认 flags 实测 0x8800f009
// （Window|Popup|TitleHint|SystemMenuHint|Min|Max|CloseButton|FullscreenButton），
// **不含 FramelessWindowHint**；因此写成 setWindowFlags(windowFlags()) 会被 Qt 判等
// 直接 return（实测 LAYERED 仍为 no，无效）。
// · 修复后实测：t=100→rgb(16,22,23)，t=0→rgb(219,220,220)，
// 与「深底 alpha 0.15 × 白底」的理论值 (219,221,224) 吻合 → 真的透出桌面了。
// 取舍：显式 FramelessWindowHint 会去掉弹出窗原有的系统投影（与产品其它无边框窗一致，
// 各对话框同样用 FramelessWindowHint | NoDropShadowWindowHint）。圆角外区域从
// 原来的「黑角」变成真正透明，观感更干净。
// 调用时机：**必须在首次 popup()/exec() 之前**（本函数会触发窗口重建并 hide 一次）。
inline void applyMenuStyle(QMenu* menu) {
    if (!menu) return;
    // 保留调用方已有的 flags（如 WindowStaysOnTopHint），只"确保"含这三项。
    // 这样无论调用方在前后怎么调 setWindowFlag()，都不会丢掉顶层提示、也不会跳过重建。
    menu->setWindowFlags(menu->windowFlags()
                         | Qt::Popup
                         | Qt::FramelessWindowHint
                         | Qt::NoDropShadowWindowHint);
    menu->setAttribute(Qt::WA_TranslucentBackground, true);
    menu->setStyleSheet(menuStyle());

    // 必须在**每次弹出前**重刷样式表（2026-09-21 补，勿删）
    // 反例实测（tools/menualpha case ⑪）：对于「一次创建、长期持有」的菜单 —— 典型就是挂在
    // QToolButton 上的「+」按钮菜单（icongridwindow.cpp 的 createAddMenu()，全项目唯一一处
    // setMenu()）—— 上面这行 setStyleSheet 只在**创建那一刻**执行一次，之后改主题色 / 拖
    // 透明度滑块都不会再更新它：
    // t=100 → rgb(16,22,23)，t=0 → rgb(16,22,23)   ← 采样值**一模一样**，纹丝不动
    // 这正是用户反馈「这个菜单没有控制上 / 不要一下写死了」的真相：渲染层没问题
    // （LAYERED=Y），纯粹是样式字符串停留在创建那一刻。（对照 ⑩：每次弹出前重刷 → 跟随 ）
    // 挂在 aboutToShow 上即可覆盖两种情况，且**幂等**：
    // · 长期持有的菜单 → 每次弹出前自动拿到最新 theme；
    // · 每次弹出新建的局部 QMenu（其余所有菜单）→ 本就是最新的，重刷一次无副作用。
    // aboutToShow 在平台窗口显示**之前**发出，此刻 setStyleSheet 不会隐藏/重建窗口。
    // 注意 Qt::UniqueConnection 对 lambda 无效（无法比较仿函数）→ 用动态属性做幂等标记。
    if (!menu->property("dt_menuStyleHooked").toBool()) {
        menu->setProperty("dt_menuStyleHooked", true);
        QObject::connect(menu, &QMenu::aboutToShow, menu,
                         [menu]() { menu->setStyleSheet(menuStyle()); });
    }
}

// 对话框玻璃：半透明深底 + 青色辉光描边 + 16px 大圆角（与面板统一）
inline QString dialogStyle() {
    return applyTokens(QStringLiteral(
        "QDialog { background-color: rgba(17,26,46,1.0); border: 1px solid rgba(34,211,238,0.28); border-radius: 16px; }"
    ));
}

inline QString titleBarStyle() {
    return applyTokens(QStringLiteral(
        "QFrame { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 rgba(17,26,46,1.0),stop:1 rgba(17,26,46,1.0)); border-top-left-radius: 16px; border-top-right-radius: 16px; border-bottom: 1px solid rgba(34,211,238,0.16); }"
    ));
}

// ---------- 全局样式表：兜底所有原生组件 ----------
// 统一：白色字体 + 线性描边按钮 + 半透明玻璃背景
// 真正生成全局样式表（15KB 级 QSS，含数十次 replace）。外部请调用下方的
// globalStyleSheet()（带记忆化），不要直接调用本函数。
inline QString buildGlobalStyleSheet() {
    return applyTokens(QStringLiteral(
        // 通用
        "QWidget { color: #FFFFFF; selection-background-color: rgba(34,211,238,0.30); selection-color: #FFFFFF; outline: none; }"
        "QToolTip { color: #FFFFFF; background-color: rgba(17,26,46,1.0); border: 1px solid rgba(34,211,238,0.30); border-radius: 6px; padding: 4px 8px; }"

        // 对话框/窗口
        "QDialog { background-color: rgba(17,26,46,1.0); border: 1px solid rgba(34,211,238,0.24); border-radius: 16px; }"
        "QMainWindow::separator { background: rgba(255,255,255,0.08); width: 1px; height: 1px; }"

        // 按钮：线性描边风格
        "QPushButton { color: #FFFFFF; background: rgba(255,255,255,0.04); border: 1px solid rgba(34,211,238,0.55); border-radius: 10px; padding: 8px 20px; }"
        "QPushButton:hover { background: rgba(34,211,238,0.12); border-color: #22D3EE; }"
        "QPushButton:pressed { background: rgba(34,211,238,0.22); }"
        "QPushButton:default { color: #FFFFFF; background: rgba(34,211,238,0.10); border: 1px solid #22D3EE; border-radius: 10px; padding: 8px 20px; font-weight: bold; }"
        "QPushButton:default:hover { background: rgba(34,211,238,0.22); border-color: #67E8F9; }"

        "QToolButton { color: #FFFFFF; border: 1px solid transparent; border-radius: 10px; background: transparent; }"
        "QToolButton:hover { background: rgba(34,211,238,0.12); border-color: rgba(34,211,238,0.20); color: #FFFFFF; }"
        "QToolButton:pressed { background: rgba(34,211,238,0.20); color: #FFFFFF; }"

        // 输入框
        "QLineEdit { color: #FFFFFF; background: rgba(17,26,46,1.0); border: 1px solid rgba(34,211,238,0.22); border-radius: 12px; padding: 6px 12px; }"
        "QLineEdit:focus { border-color: #22D3EE; background: rgba(17,26,46,1.0); }"
        "QLineEdit::placeholder { color: rgba(255,255,255,0.45); }"
        "QTextEdit { color: #FFFFFF; background: rgba(17,26,46,1.0); border: 1px solid rgba(34,211,238,0.18); border-radius: 10px; padding: 6px; }"
        "QTextEdit:focus { border-color: #22D3EE; }"
        "QComboBox { color: #FFFFFF; background: rgba(17,26,46,1.0); border: 1px solid rgba(34,211,238,0.22); border-radius: 10px; padding: 5px 10px; }"
        "QComboBox:hover { border-color: rgba(34,211,238,0.35); }"
        "QComboBox::drop-down { border: none; width: 24px; }"
        "QComboBox QAbstractItemView { background: rgba(17,26,46,1.0); border: 1px solid rgba(34,211,238,0.25); selection-background-color: rgba(34,211,238,0.20); color: #FFFFFF; }"
        "QSpinBox, QDoubleSpinBox { color: #FFFFFF; background: rgba(17,26,46,1.0); border: 1px solid rgba(34,211,238,0.22); border-radius: 10px; padding: 5px; }"

        // 菜单
        "QMenu { background-color: rgba(17,26,46,1.0); border: 1px solid rgba(34,211,238,0.28); border-radius: 12px; padding: 6px; }"
        "QMenu::item { color: #FFFFFF; padding: 7px 26px; border-radius: 6px; }"
        "QMenu::item:selected { background: rgba(34,211,238,0.20); color: #FFFFFF; }"
        "QMenu::item:disabled { color: rgba(255,255,255,0.40); }"
        "QMenu::separator { background: rgba(255,255,255,0.12); height: 1px; margin: 6px 10px; }"

        // 列表/表格
        "QListWidget { background: rgba(17,26,46,1.0); border: 1px solid rgba(34,211,238,0.12); border-radius: 12px; color: #FFFFFF; outline: none; padding: 6px; }"
        "QListWidget::item { padding: 12px; border-radius: 10px; color: #FFFFFF; margin: 2px 0px; }"
        "QListWidget::item:selected { background: rgba(34,211,238,0.18); color: #FFFFFF; border: 1px solid rgba(34,211,238,0.45); }"
        "QListWidget::item:hover { background: rgba(34,211,238,0.08); }"
        "QListWidget::item:selected:hover { background: rgba(34,211,238,0.22); }"
        "QTableWidget { background: rgba(17,26,46,1.0); border: 1px solid rgba(34,211,238,0.14); border-radius: 10px; color: #FFFFFF; gridline-color: rgba(255,255,255,0.10); }"
        "QTableWidget::item { padding: 6px; border-radius: 4px; color: #FFFFFF; }"
        "QTableWidget::item:selected { background: rgba(34,211,238,0.18); color: #FFFFFF; }"
        "QHeaderView::section { background: rgba(17,26,46,1.0); color: #FFFFFF; padding: 6px; border: none; border-bottom: 1px solid rgba(34,211,238,0.16); }"
        "QTreeView { background: rgba(17,26,46,1.0); border: 1px solid rgba(34,211,238,0.14); border-radius: 8px; color: #FFFFFF; }"
        "QTreeView::item:selected { background: rgba(34,211,238,0.18); color: #FFFFFF; }"

        // 复选框（线性描边指示符，统一 16x16 / 圆角 3px / 青蓝渐变勾选，与各对话框覆盖样式保持一致）
        "QCheckBox { color: #FFFFFF; spacing: 8px; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid rgba(34,211,238,0.30); border-radius: 3px; background: rgba(17,26,46,1.0); }"
        "QCheckBox::indicator:checked { background: qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #22D3EE,stop:1 #3B82F6); border: none; }"
        "QRadioButton { color: #FFFFFF; spacing: 6px; }"
        "QRadioButton::indicator { width: 16px; height: 16px; border: 1px solid rgba(34,211,238,0.40); border-radius: 8px; background: rgba(17,26,46,1.0); }"
        "QRadioButton::indicator:checked { background: rgba(34,211,238,0.35); border: 2px solid #22D3EE; }"

        // 分组框/标签
        "QGroupBox { color: #FFFFFF; border: 1px solid rgba(34,211,238,0.16); border-radius: 12px; margin-top: 8px; padding-top: 8px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 6px; color: #FFFFFF; }"
        "QLabel { color: #FFFFFF; }"
        "QFrame { border: none; }"

        // 滚动条
        "QScrollBar:vertical { background: rgba(17,26,46,1.0); width: 8px; border-radius: 4px; margin: 2px; }"
        "QScrollBar::handle:vertical { background: rgba(34,211,238,0.40); border-radius: 4px; min-height: 32px; }"
        "QScrollBar::handle:vertical:hover { background: rgba(34,211,238,0.60); }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
        "QScrollBar:horizontal { background: rgba(17,26,46,1.0); height: 8px; border-radius: 4px; margin: 2px; }"
        "QScrollBar::handle:horizontal { background: rgba(34,211,238,0.40); border-radius: 4px; min-width: 32px; }"
        "QScrollBar::handle:horizontal:hover { background: rgba(34,211,238,0.60); }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }"

        // Tab 标签
        "QTabWidget::pane { border: 1px solid rgba(34,211,238,0.16); border-radius: 12px; background: rgba(17,26,46,1.0); }"
        "QTabBar::tab { color: rgba(255,255,255,0.72); background: transparent; border: 1px solid transparent; border-radius: 10px; padding: 6px 14px; margin-right: 4px; }"
        "QTabBar::tab:hover { background: rgba(34,211,238,0.12); color: #FFFFFF; }"
        "QTabBar::tab:selected { color: #FFFFFF; background: rgba(34,211,238,0.18); border: 1px solid rgba(34,211,238,0.45); }"

        // 分隔线
        "QSplitter::handle { background: rgba(255,255,255,0.12); }"
        "QProgressBar { border: 1px solid rgba(34,211,238,0.25); border-radius: 6px; background: rgba(17,26,46,1.0); text-align: center; color: #FFFFFF; }"
        "QProgressBar::chunk { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 rgba(34,211,238,0.8),stop:1 rgba(59,130,246,0.8)); border-radius: 5px; }"
    ));
}

// 记忆化包装：
// 本函数返回 15KB 级 QSS，生成过程含数十次 QString::replace()（每次都要扫全文）。
// 缓存键必须同时含【主题色 + 透明度】：2026-09-21 起背景色 alpha 由 bgAlphaF() 提供、
// 随全局透明度变化 —— 若仍只按主题色缓存，改完透明度会把旧串原样返回，
// main.cpp 的字符串比对会"命中未变化"，透明度永远不生效。
// 2026-09-22 再补【盒子使用圆角】：boxRound 会在 applyTokens 里把全部 border-radius 压成 0，
// 它是样式串内容的直接输入 —— 漏进缓存键的话，勾/取消该复选时这里会把旧串原样吐回去，
// main.cpp 的比对同样判定"未变化"，圆角开关将只在重启后才生效。
// （boxBorder 不进本键：全局 QSS 里没有"盒子外框"，那条线只由 boxChrome() 作用于具体窗口。）
inline QString globalStyleSheet() {
    static QColor  s_cachedAccent;
    static int     s_cachedTransparency = -1;
    static bool    s_cachedRounded = false;
    static QString s_cachedQss;

    const ThemeManager* tm = ThemeManager::instance();
    const QColor accent = tm->accentColor();
    const int transparency = tm->transparency();
    const bool rounded = tm->boxRoundedEnabled();
    if (s_cachedQss.isEmpty() || accent != s_cachedAccent
        || transparency != s_cachedTransparency || rounded != s_cachedRounded) {
        s_cachedAccent = accent;
        s_cachedTransparency = transparency;
        s_cachedRounded = rounded;
        s_cachedQss = buildGlobalStyleSheet();
    }
    return s_cachedQss;
}

} // namespace Theme

#endif // THEME_H
