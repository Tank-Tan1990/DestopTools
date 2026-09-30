/*
 * @file thememanager.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "thememanager.h"
#include "desktopmirrorwindow.h"   // C：主题下发期间临时摘掉 Dock 的低级钩子
#include "icongridwindow.h"        // C：同上（网格窗一套钩子）
#include "settingsmanager.h"       // 外观开关（Appearance/*）的取值来源
#include "diagtrace.h"             // 透明度换算的诊断行（默认关闭，非阻塞）
#include <QObject>
#include <QWidget>
#include <QTimer>
#include <QColor>
#include <algorithm>

namespace {

// C：主题下发期间的「摘钩护栏」
// 为什么必须在这里做：一次主题下发是重活 —— 全应用 a.setStyleSheet(15KB QSS) 会遍历进程内
// 每个 widget 重解析 + repolish（150~330ms），加上十余个窗口各自 applyTheme() 的 setStyleSheet
// （其中数个是全屏分层窗），单次合计可达 300~600ms。
// 而 WH_KEYBOARD_LL / WH_MOUSE_LL 是系统同步回调到本 GUI 线程的：这 300~600ms 内 Windows 会
// 阻塞全局鼠标输入等我们返回 → 用户看到的就是「拖透明度时鼠标卡的不动」。
// 摘钩后这些重活照旧耗时，但【不再冻结系统鼠标】。
// 放在 emit / dispatch 两侧（而非某个调用点）是为了覆盖全部主题变化来源：拖动滑块、点主题色、
// 恢复默认、启动时载入配置……不必在每个调用点重复。
// 注意 m_deferTimer 的补偿刷也会自己再取一次本护栏（那次不在 set* 的调用栈里）。
class HotkeyHookGuard {
public:
    HotkeyHookGuard() {
        DesktopMirrorWindow::suspendHotkeyHooks();
        IconGridWindow::suspendGridHooks();
    }
    ~HotkeyHookGuard() {
        IconGridWindow::resumeGridHooks();      // 顺序无关：两套钩子互不依赖
        DesktopMirrorWindow::resumeHotkeyHooks();
    }
    Q_DISABLE_COPY(HotkeyHookGuard)
};

// 外观开关的读取（2026-09-22）
// 抽成独立函数：构造期（只取值、**不**重下发）与运行期 reload（取值 + 必要时重下发）共用同一份
// key/default，避免两处默认值写歪（默认值必须与 settingcenterdialog.cpp::loadSettings 一致）。
struct AppearanceFlags {
    bool border = true;        // Appearance/boxBorder
    bool round = false;        // Appearance/boxRound
    bool arrow = true;         // Appearance/shortcutArrow
    bool autoExpand = false;   // Appearance/autoExpandOnHover
};

AppearanceFlags readAppearanceFlags() {
    SettingsManager sm;
    AppearanceFlags f;
    f.border     = sm.loadValue(QStringLiteral("Appearance/boxBorder"), true).toBool();
    f.round      = sm.loadValue(QStringLiteral("Appearance/boxRound"), false).toBool();
    f.arrow      = sm.loadValue(QStringLiteral("Appearance/shortcutArrow"), true).toBool();
    f.autoExpand = sm.loadValue(QStringLiteral("Appearance/autoExpandOnHover"), false).toBool();
    return f;
}

} // namespace

// instance
// 作者：谭征
ThemeManager* ThemeManager::instance() {
    static ThemeManager s_instance;
    return &s_instance;
}

ThemeManager::ThemeManager(QObject* parent) : QObject(parent) {
    // 构造期就要把 4 个外观开关读进来：applyTokens() 在 main() 设全局样式表之前就会被调用
    // （各窗口构造里都在生成样式串），若此时开关还是硬编码默认值，用户上次勾的"方角/无边框"
    // 在**首次生成**的样式串里就不生效，且未必会有第二次机会重刷。
    // 这里只取值、**不**触发重下发：此刻各窗口尚未创建（m_targets 为空）、main() 还没把
    // themeChanged 接到全局 QSS 下发上，emit 出去既没人接，又白跑一次低级钩子摘/装。
    const AppearanceFlags f = readAppearanceFlags();
    m_boxBorder = f.border;
    m_boxRounded = f.round;
    m_shortcutArrow = f.arrow;
    m_autoExpandOnHover = f.autoExpand;
}

// 从设置重读 4 个外观开关。① ② 直接决定样式串内容 → 变化时必须重下发样式；
// ③ ④ 不进样式串（分别由取图标路径与 hover 分支即时读取）→ 不需要重刷。
// 作者：谭征
void ThemeManager::reloadAppearanceFlags() {
    const AppearanceFlags f = readAppearanceFlags();
    const bool styleChanged = (f.border != m_boxBorder) || (f.round != m_boxRounded);
    m_boxBorder = f.border;
    m_boxRounded = f.round;
    m_shortcutArrow = f.arrow;
    m_autoExpandOnHover = f.autoExpand;
    if (!styleChanged) return;

    // 与 setAccentColor 同款：重下发前后摘/装低级钩子。一次全应用 repolish 是重活，
    // 若不摘钩，GUI 线程被占住的这段时间 Windows 会阻塞全局鼠标键盘输入（用户看到"鼠标卡住"）。
    HotkeyHookGuard guard;
    emit themeChanged();      // main.cpp 唯一的全局 QSS 下发入口（缓存键含 boxRounded，会真的重生成）
    dispatchTargets();        // 各已登记窗口的 onStyle()
}

// 轻量设置变更广播：不重刷样式，只通知「某个设置项刚被改」。
// 不变量：本函数与 themeChanged 链路完全独立 —— 消费方只做自己的局部更新（如重读一个 int），
// 因此可以放心在 saveSettings() 里每次写盘后调用，不会有 repolish 级别的开销。
// 作者：谭征
void ThemeManager::notifySettingChanged(const QString& key, const QVariant& value) {
    if (key.isEmpty()) return;
    ThemeManager* self = instance();
    // 外观开关必须在这里顺带重读：写盘方（设置中心）只负责 saveValue + notify，
    // 开关缓存若不同步更新，后续生成的样式串/取的图标仍是旧值。
    // 放在 emit 之前 → 订阅者收到 settingChanged 时开关已是新值（顺序敏感）。
    if (key.startsWith(QLatin1String("Appearance/"))) self->reloadAppearanceFlags();
    emit self->settingChanged(key, value);
}

// 设置主题色
// 作者：谭征
void ThemeManager::setAccentColor(const QColor& c) {
    if (!c.isValid() || c == m_accent) return;
    m_accent = c;
    HotkeyHookGuard guard;                // C：下发前后摘/装低级钩子，避免重活期间冻结系统鼠标
    emit themeChanged();                  // 全局 QSS（main.cpp 唯一的样式下发入口）
    dispatchTargets();
}

// 设置透明度
// 作者：谭征
void ThemeManager::setTransparency(int t) {
    t = qBound(0, t, 100);
    if (t == m_transparency) return;
    m_transparency = t;
    // 诊断（默认关闭，仅 INI [Diagnostics] traceEnabled=true 才写盘）：
    // 一行说清"这次透明度换算成了什么背景 alpha"。用户若觉得观感不对，凭这一行即可区分
    // "滑块没生效"与"生效了但值不符合预期"，不必靠猜。
    DiagTrace::log(QStringLiteral("[theme] 背景透明度=%1% -> 背景 alpha=%2（圆角线框 alpha 恒为设计值，不随本滑块变化）")
                       .arg(t)
                       .arg(effectiveOpacity(), 0, 'f', 2));
    HotkeyHookGuard guard;                // C：同上（透明度的全应用 repolish 同样必须避开低级钩子）
    emit themeChanged();                  // 背景 alpha 在样式串里（bgAlphaF）→ 这条链路必须真的重下发
    dispatchTargets();
}

// 整窗不透明度会把圆角线框与文字一起淡化，与「线框透明度不调整」的需求冲突。
// 作者：谭征
double ThemeManager::effectiveOpacity() const {
    return qBound(0.15, m_transparency / 100.0, 1.0);
}

// E：登记 / 注销
// 作者：谭征
void ThemeManager::registerThemeTarget(QWidget* host,
                                       std::function<void()> onStyle) {
    if (!host) return;
    unregisterThemeTarget(host);          // 幂等：同一宿主重复登记只保留最后一次
    Target t;
    t.host = host;
    t.onStyle = std::move(onStyle);
    m_targets.append(t);
}

// 注销主题目标
// 作者：谭征
void ThemeManager::unregisterThemeTarget(const QWidget* host) {
    for (int i = m_targets.size() - 1; i >= 0; --i) {
        // 顺带剪除已销毁宿主（QPointer 已置空）的条目，避免登记表随弹框反复创建而无限增长。
        if (m_targets.at(i).host.isNull() || m_targets.at(i).host.data() == host) {
            m_targets.remove(i);
        }
    }
}

// E：下发
// 主题色与透明度**都要重刷样式**（背景 alpha 由 bgAlphaF 写进样式串，二者不可分割）。
// 隐藏目标不当场刷：既省下它的 repolish，又不会漏（记 pending，停手后由 deferTimer 补刷）。
// 作者：谭征
void ThemeManager::dispatchTargets() {
    bool needDefer = false;
    for (int i = m_targets.size() - 1; i >= 0; --i) {
        Target& t = m_targets[i];
        if (t.host.isNull()) { m_targets.remove(i); continue; }   // 宿主已销毁 → 剪除
        if (t.host->isVisible()) {
            if (t.onStyle) t.onStyle();
        } else {
            t.pending = true;
            needDefer = true;
        }
    }
    if (needDefer) {
        if (!m_deferTimer) {
            m_deferTimer = new QTimer(this);
            m_deferTimer->setSingleShot(true);
            connect(m_deferTimer, &QTimer::timeout, this, &ThemeManager::flushDeferredTargets);
        }
        m_deferTimer->start(300);   // 重复 start 自动重新计时 → 连续操作只在"最后一次变化"后 300ms 才补刷一次
    }
}

// 补偿刷：把隐藏期间错过刷新的目标（含隐藏窗口）补上一次。
// 只在用户停手 300ms 后触发一次，所以拖动过程中不会有任何隐藏窗口参与 → 拖手感不受影响；
// 而隐藏窗口重新显示时样式必然是最新的（不需要各窗口自己处理 showEvent）。
// 作者：谭征
void ThemeManager::flushDeferredTargets() {
    bool any = false;
    for (const Target& t : m_targets) {
        if (t.pending && !t.host.isNull()) { any = true; break; }
    }
    if (!any) return;

    HotkeyHookGuard guard;   // C：补偿刷同样是重活 → 先摘钩，绝不冻结系统鼠标
    for (int i = m_targets.size() - 1; i >= 0; --i) {
        Target& t = m_targets[i];
        if (t.host.isNull()) { m_targets.remove(i); continue; }
        if (!t.pending) continue;
        t.pending = false;
        if (t.onStyle) t.onStyle();
    }
}
