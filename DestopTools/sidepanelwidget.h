/*
 * @file sidepanelwidget.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef SIDEPANELWIDGET_H
#define SIDEPANELWIDGET_H

#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QFrame;
class QToolButton;
class QuickToolsWidget;
class QTimer;
class QLineEdit;
class QMenu;
class QNetworkAccessManager;
class QNetworkReply;
class QScrollArea;
class QVBoxLayout;
class TodoListWindow;
QT_END_NAMESPACE

// 360 桌面助手右侧助手面板：标题栏 + 大时钟 + 日期 + 快捷工具
class SidePanelWidget : public QWidget {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit SidePanelWidget(QWidget* parent = nullptr);

    // 清除搜索
    void clearSearch();
    // 判断折叠
    bool isCollapsed() const;
    QString currentSearchText() const;   // 当前搜索框文本（供「快速搜索」按钮带入初始关键字）
    // 设置中心「小工具」增删/排序后刷新面板（public：MainWindow 的「工具管理」按钮
    // 打开设置中心时也要 connect 这条信号；此前仅 sidepanelwidget 内部打开时可达）
    void updateToolOrder(const QStringList& order);

signals:
    // 工具触发信号
    void toolTriggered(const QString& name);
    // 搜索文本变化信号
    void searchTextChanged(const QString& text);
    void requestFileSearch(const QString& text);   // 搜索框回车/放大镜 → 唤起「快速搜索」文件搜索窗口
    // 菜单请求信号
    void menuRequested();
    // 折叠toggled
    void collapseToggled(bool collapsed);
    // request创建新建盒子
    void requestCreateNewBox();
    void requestDockReset();         // dock 复位：清空 dock 图标并重新扫描 Windows 桌面图标位置
    void requestToggleNativeDesktop(); // 显示/隐藏系统桌面图标层
    void mainWindowVisibilityChanged(bool show); // 「显示主界面/隐藏主界面」转发给 MainWindow 显隐助手面板

protected:
    // 事件过滤
    bool eventFilter(QObject* watched, QEvent* event) override;
    void enterEvent(QEvent* event) override;   // 悬停显示模式：鼠标移入即亮出标题栏菜单按钮
    void leaveEvent(QEvent* event) override;   // 悬停显示模式：鼠标移出即收起来

private:
    // 初始化titlebar
    void setupTitleBar(QWidget* parent);
    // 初始化clock
    QFrame* setupClock(QWidget* parent);
    // 更新time
    void updateTime();
    // 切换折叠
    void toggleCollapse();
    // 显示菜单
    void showMenu();
    void applyGeneralSettings();     // 仅把成员状态套到 UI（不回读设置，避免覆盖信号刚下发的新值）
    void loadGeneralSettings();      // 构造期一次性从设置载入 m_showSeconds / m_largeTimeFont 成员
    // 保存generalclock设置
    void saveGeneralClockSettings();
    // 同步tools从设置
    void syncToolsFromSettings();
    // 创建market图标
    QPixmap createMarketIcon() const;

    // 标题栏菜单按钮「始终显示 ↔ 悬停显示」（设置中心 → 外观设置 → 分区菜单标签显示）。
    // 与收纳盒窗口共用同一个键 `Appearance/menuLabelShow`：0=始终显示（常驻） 1=悬停显示。
    void loadMenuRevealMode();
    // reload菜单显示模式
    void reloadMenuRevealMode();
    void applyMenuRevealVisibility();     // **唯一**显隐出口

    // 天气（Open-Meteo，免费、无需 key）
    void fetchWeather();          // 根据已保存城市发起地理编码 + 天气请求
    void fetchWeatherData();      // 用已解析的经纬度请求实时天气
    // 响应geocodefinished
    void onGeocodeFinished(QNetworkReply* reply);
    // 响应weatherfinished
    void onWeatherFinished(QNetworkReply* reply);
    static QString weatherDescription(int wmoCode); // WMO 天气代码 → 中文描述
    static QPixmap weatherPixmap(int wmoCode, int size); // WMO 代码 → 晴/雨/雪等图标
    void applyWeatherCache();     // 启动时先用缓存显示，再请求最新
    void refreshWeatherIfCityChanged(); // 设置中心切换城市后重取
    void applyTheme();            // 主题色/透明度联动：重绘硬编码青色样式
    void updateTimeLabelMinWidth(); // 按最宽样本(88:88:88)预留时间标签宽度，防「显示秒」末位被挤掉
    void fetchIpLocation();       // 首次启动时根据 IP 自动定位天气区域
    void startIpGeoRequest(bool useHttps); // 发起 IP 定位请求（支持 HTTPS→HTTP fallback）
    // 响应ipgeofinished
    void onIpGeoFinished(QNetworkReply* reply);

    // 待办事项：面板内未完成条目列表 + 底部待办栏（TodoListWindow 为展开的「事项清单」独立窗）
    void rebuildTodoList();       // TodoStore 数据 → 面板条目行（未完成）
    // 构建todo行
    QWidget* buildTodoRow(const QString& id);
    void addTodoViaDialog();      // 「添加待办事项」→ GlassInputDialog
    void openTodoListWindow();    // 「清单」按钮 → TodoListWindow

    QLabel* m_timeLabel = nullptr;
    QLabel* m_dateLabel = nullptr;
    QLabel* m_weekdayLabel = nullptr;
    QLabel* m_lunarLabel = nullptr;
    QLabel* m_weatherTempLabel = nullptr; // 温度文字（图标左侧）
    QLabel* m_weatherIcon = nullptr;      // 天气图标（随晴/雨/雪切换）
    QuickToolsWidget* m_toolsWidget = nullptr;
    QTimer* m_timer = nullptr;
    QTimer* m_weatherTimer = nullptr;     // 每 3 分钟刷新天气
    QNetworkAccessManager* m_netManager = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QToolButton* m_searchBtn = nullptr; // 标题栏搜索按钮（跟随主题色）
    QWidget* m_content = nullptr;
    QFrame* m_card = nullptr;        // 主玻璃面板，需在主题变化时重刷样式
    QFrame* m_clockFrame = nullptr;
    QToolButton* m_moreBtn = nullptr;
    QToolButton* m_menuBtn = nullptr;
    QFrame* m_todoBar = nullptr;       // 底部待办栏（硬编码青色边框，需跟随主题）
    QLabel* m_todoIcon = nullptr;      // 待办栏左侧「+」号，需跟随主题色
    QToolButton* m_todoListBtn = nullptr; // 待办栏右侧「清单」入口按钮
    QScrollArea* m_todoScroll = nullptr;  // 面板内待办条目滚动区（无条目时隐藏）
    QWidget* m_todoHost = nullptr;        // 待办条目容器
    QVBoxLayout* m_todoListLayout = nullptr; // 待办条目纵向布局（末尾 stretch）
    TodoListWindow* m_todoListWindow = nullptr; // 「事项清单」独立窗（懒创建）
    QWidget* m_titleBar = nullptr;     // 标题栏（硬编码青色下边线）
    QToolButton* m_layersBtn = nullptr; // 组件市场按钮（硬编码青色描边）
    // 标题栏红框那排按钮（搜索/市场/菜单/展开）的**定宽容器**：悬停显示时只隐藏里面的按钮，
    // 容器仍占住原宽度 → 左侧搜索框不会随鼠标进出反复伸缩。
    QWidget* m_menuBtnBox = nullptr;
    int m_menuRevealMode = 0;      // Appearance/menuLabelShow：0=始终显示 1=悬停显示
    bool m_pointerInside = false;  // 鼠标是否在面板内（Enter/Leave 维护）
    bool m_collapsed = false;
    bool m_showSeconds = true;
    bool m_largeTimeFont = true;
    bool m_showCompletedItems = true;   // 「显示已完成事项（主界面）」：面板待办列表是否含已完成条目
    QString m_weatherCity;       // 最近一次请求对应的城市
    double m_weatherLat = 0.0;   // 已解析的纬度
    double m_weatherLon = 0.0;   // 已解析的经度
    bool m_geocodePending = false; // 地理编码请求进行中，避免重复
    bool m_weatherPending = false;  // 天气请求进行中
    bool m_ipGeoPending = false;    // IP 定位请求进行中
    bool m_ipGeoTryHttpFallback = false; // HTTPS 失败后是否已尝试 HTTP fallback
};

#endif // SIDEPANELWIDGET_H
