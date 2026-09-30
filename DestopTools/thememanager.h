/*
 * @file thememanager.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QObject>
#include <QColor>
#include <QPointer>
#include <QString>
#include <QVariant>
#include <QVector>
#include <functional>

class QWidget;
class QTimer;

// 全局主题状态：保存当前「主题色（强调色）」与「透明度」，
// 二者变化后发出 themeChanged()，供各窗口/对话框实时重绘样式与窗口不透明度，
// 从而实现「改主题色 → 全部界面颜色联动；改透明度 → 全部界面透明度联动」。
class ThemeManager : public QObject {
    Q_OBJECT
public:
    // instance
    static ThemeManager* instance();

    QColor accentColor() const { return m_accent; }
    // 设置主题色
    void setAccentColor(const QColor& c);

    int transparency() const { return m_transparency; }   // 0~100
    // 设置透明度
    void setTransparency(int t);

    // 窗口「界面背景不透明度」：透明度 100 → 1.0（背景最实），0 → 0.15（背景最透，保底可见）。
    // 它**只作用在背景色上**（见 theme.h 的 bgAlphaF / applyBgAlpha），不再用于 setWindowOpacity：
    // 整窗不透明度会把圆角线框与文字一起淡化，与「线框透明度不调整」的需求冲突。
    double effectiveOpacity() const;

    // E：主题刷新「登记表」
    // 取代各窗口各自 connect(ThemeManager::themeChanged, …)。集中之后才能按可见性分流：
    // 程序内同时存在十几个顶层窗，一次下发把不可见的也全部重刷纯属浪费
    // （setStyleSheet 的代价随该窗控件数线性增长）。
    // 分流规则（见 dispatchTargets）：
    // · 可见目标 → 立即 onStyle()；
    // · 不可见目标 → 记为 pending，在「用户停手 300ms 后」由 m_deferTimer 合并补刷一次
    // （补刷期间自动摘低级钩子），因此隐藏窗口重新显示时样式不会过期 —— 各窗口无需自己处理 showEvent。
    // onStyle 可以留空（表示该窗样式完全由全局 QSS 承担、无需自己重刷，例如按需创建的模态对话框）。
    // 宿主销毁无需手工注销：条目持有 QPointer，下次下发时自动剪除。
    // 2026-09-21：**透明度变化也必须重刷样式** —— 背景 alpha 已由 bgAlphaF() 写进样式串，
    // 不再有"只改整窗不透明度、无需重刷样式"的捷径（那条路会连边框一起淡化，已废弃）。
    // 性能上无碍：滑块是 setTracking(false) 的 → 拖动期间零下发，松手才下发一次。
    void registerThemeTarget(QWidget* host,
                             std::function<void()> onStyle);
    // 注销主题目标
    void unregisterThemeTarget(const QWidget* host);

    // 外观开关（2026-09-22）
    // 「设置中心 → 外观设置 → 其他」里的 4 个复选，此前是**空设置**（只有设置中心自己读写，
    // 全项目无消费端 → 勾了毫无反应）。现统一由本类持有并消费：
    // ① Appearance/boxBorder        「盒子显示边框」         → Theme::boxChrome()
    // ② Appearance/boxRound         「盒子使用圆角」         → Theme::applyTokens()
    // ③ Appearance/shortcutArrow    「在快捷方式图标上显示箭头」→ Theme::shortcutArrowOverlayScale()
    // ④ Appearance/autoExpandOnHover「收起后，鼠标移动到标题自动展开」→ 各窗口 hover 分支自行读取
    // 取值时机：构造期读一次（applyTokens 在 main 之前就会被调到，必须早于首次生成样式表）；
    // 之后任何 `notifySettingChanged("Appearance/…")` 都会顺带重读（见 .cpp）。
    // ① ② 变化必须真的**重下发样式**（它们的值写进了样式串），见 reloadAppearanceFlags()。
    bool boxBorderEnabled() const { return m_boxBorder; }
    bool boxRoundedEnabled() const { return m_boxRounded; }
    bool shortcutArrowEnabled() const { return m_shortcutArrow; }
    bool autoExpandOnHoverEnabled() const { return m_autoExpandOnHover; }

    // 从 SettingManager 重读这 4 项；若 ①/② 真的变了，则追加一次 themeChanged()+dispatchTargets()
    // （样式串内容已变，不重下发就会「设置里勾了、界面上没反应」）。
    void reloadAppearanceFlags();

    // 轻量设置变更广播（2026-09-21）
    // 与 themeChanged() 的区别：**不触发任何样式重刷**，只把「某个设置项刚被改」这件事
    // 广播出去，供各窗口就地热更新（例如「分区标签切换」改完立刻生效，不必重开窗口或重启）。
    // 为什么需要它：设置中心的 saveSettings() 只写盘；消费者若只在构造/showEvent 里读，
    // 已打开的窗口就永远停在旧值上 —— 表现为「设置里选了、界面上没反应」。
    // 用法：写盘方 notifySettingChanged(key, value)；消费方 connect(instance(), &settingChanged, …)
    // 在槽里按 key 过滤（别的 key 一律不理）。
    static void notifySettingChanged(const QString& key, const QVariant& value);

signals:
    // 主题变化信号
    void themeChanged();
    // 设置变化信号
    void settingChanged(const QString& key, const QVariant& value);

private:
    // 构造函数：初始化对象
    explicit ThemeManager(QObject* parent = nullptr);

    void dispatchTargets();                    // 下发：可见目标立即刷，隐藏目标记待刷
    void flushDeferredTargets();               // 停手后补偿刷：把待刷目标（含隐藏窗口）补上

    struct Target {
        QPointer<QWidget> host;           // 代表窗口：判可见性 + 生命周期（QPointer 自动置空）
        std::function<void()> onStyle;    // 重刷样式（主题色变化、透明度变化都要调）
        bool pending = false;             // 隐藏期间错过刷新 → 待补刷
    };

    QVector<Target> m_targets;
    QTimer* m_deferTimer = nullptr;       // 待刷合并定时器（300ms，单次）

    QColor m_accent = QColor(QStringLiteral("#22D3EE")); // 默认科技青蓝
    // 默认 100 = 完全不透明（2026-09-21 由 80 改为 100）：程序默认不做任何整体淡化，
    // 需要透明观感的用户自己在「外观设置 → 透明度」里调低（越低越透）。
    int m_transparency = 100;

    // 外观开关缓存（默认值与设置中心一致，见 settingcenterdialog.cpp::loadSettings 的回退值）
    bool m_boxBorder = true;              // 盒子显示边框
    bool m_boxRounded = false;            // 盒子使用圆角（默认方角）
    bool m_shortcutArrow = true;          // 快捷方式箭头
    bool m_autoExpandOnHover = false;     // 收起后悬停自动展开
};

#endif // THEMEMANAGER_H
