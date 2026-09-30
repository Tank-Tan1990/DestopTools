/*
 * @file settingcenterdialog.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef SETTINGCENTERDIALOG_H
#define SETTINGCENTERDIALOG_H

#include <QDialog>
#include <QCloseEvent>

QT_BEGIN_NAMESPACE
class QListWidget;
class QStackedWidget;
class QSlider;
class QLineEdit;
class QComboBox;
class QLabel;
class QButtonGroup;
class QFrame;
class QCheckBox;
class QRadioButton;
class QTableWidget;
class QVBoxLayout;
class QGridLayout;
class QScrollArea;
class QPushButton;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
QT_END_NAMESPACE

class SettingCenterDialog : public QDialog {
    Q_OBJECT
public:
    enum Tab {
        TabGeneral = 0,
        TabDesktopOrganize,
        TabAppearance,
        TabDesktopBackup,
        TabQuickActions,
        TabTools,
        TabAbout
    };

    // 构造函数：初始化对象
    explicit SettingCenterDialog(Tab initialTab = TabGeneral, QWidget* parent = nullptr);

    // IP 定位结果（ip9.com.cn 返回的省/市/区与经纬度）
    struct IpGeoInfo {
        QString province;
        QString city;
        QString district;
        double lat = 0.0;
        double lon = 0.0;
        bool valid = false;
    };

    // 解析 ip9.com.cn JSON 响应，供设置对话框与桌面助手共用
    static IpGeoInfo parseIp9Location(const QByteArray& json);

signals:
    // time显示seconds变化信号
    void timeShowSecondsChanged(bool showSeconds);
    // timelarge字体变化信号
    void timeLargeFontChanged(bool largeFont);
    // 显示clockweather变化信号
    void showClockWeatherChanged(bool show);
    void showCompletedChanged(bool show);   // 「显示已完成事项（主界面）」联动面板待办列表
    void showMainWindowChanged(bool show);  // 「显示主界面/隐藏主界面」勾选=显示助手面板，取消=隐藏
    // 设置变化信号
    void settingChanged(const QString& key, const QVariant& value);
    // quicktools顺序变化信号
    void quickToolsOrderChanged(const QStringList& order);
    // request创建新建盒子
    void requestCreateNewBox();
    void weatherRegionChanged(); // 天气区域（省/市/区县）变化时通知面板重新获取天气

protected:
    // 鼠标按下事件
    void mousePressEvent(QMouseEvent* event) override;
    // 鼠标移动事件
    void mouseMoveEvent(QMouseEvent* event) override;
    // 鼠标松开事件
    void mouseReleaseEvent(QMouseEvent* event) override;
    // 关闭事件
    void closeEvent(QCloseEvent* event) override;
    // 显示事件
    void showEvent(QShowEvent* event) override;
    // 缩放事件
    void resizeEvent(QResizeEvent* event) override;
    // 事件过滤
    bool eventFilter(QObject* watched, QEvent* event) override;
    // paint事件
    void paintEvent(QPaintEvent* event) override;
    // 与收纳盒/桌面助手相同的 Z 序守卫：拦截 WM_WINDOWPOSCHANGING，冻结提层，
    // 使设置中心持久钉在桌面 band 层（Dock 之上、正常程序之下），不被 band 窗口覆盖。
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;

private slots:
    // 响应sidebar当前行变化信号
    void onSidebarCurrentRowChanged(int row);
    // 响应颜色selected
    void onColorSelected(int id);
    // 响应透明度变化信号
    void onTransparencyChanged(int value);
    // 响应choose图像
    void onChooseImage();
    // 响应chooseremindsound
    void onChooseRemindSound();
    // 响应还原默认
    void onRestoreDefault();
    // 响应general检查变化信号
    void onGeneralCheckChanged();
    // 响应添加备份
    void onAddBackup();
    // 响应tools选中变化信号
    void onToolsSelectionChanged();
    // 响应还原tools顺序
    void onRestoreToolsOrder();
    // 响应添加工具
    void onAddTool();
    // 响应移除工具
    void onRemoveTool();
    // 响应province变化信号
    void onProvinceChanged(const QString& province);
    // 响应city变化信号
    void onCityChanged(const QString& city);
    // 响应创建新建盒子
    void onCreateNewBox();
    // 响应自动locatebyip
    void onAutoLocateByIp();
    // 响应ipgeofinished
    void onIpGeoFinished(QNetworkReply* reply);
    // 开始ipgeorequest
    void startIpGeoRequest(bool useHttps);

    // 主题色/透明度联动：重绘对话框自身硬编码青色样式，并同步窗口不透明度
    void applyTheme();
    // 对话框stylesheet
    QString dialogStyleSheet() const;

private:
    // 初始化ui
    void setupUi();
    // m_rootFrame 用样式表负责圆角背景 + 主题色边框，整体外框随主题色/透明度联动。
    // 为彻底消除 Windows 下无边框透明窗口的四角直角，改用 paintEvent + setMask 方案。
    void updateRoundedMask();
    // 内容区 m_stack 的 QSS border-radius 不会自动裁剪子控件，需额外用 QRegion
    // 把堆叠页裁剪为圆角矩形，让内部四角与外层圆角风格保持一致。
    void updateStackMask();
    // 初始化sidebar
    void setupSidebar();
    // 创建general设置页
    QWidget* createGeneralSettingsPage();
    // 创建桌面organize页
    QWidget* createDesktopOrganizePage();
    // 创建appearance页
    QWidget* createAppearancePage();
    // 创建桌面备份页
    QWidget* createDesktopBackupPage();
    // 创建quickactions页
    QWidget* createQuickActionsPage();
    // 创建tools页
    QWidget* createToolsPage();
    // 创建about页
    QWidget* createAboutPage();
    // 关于页 Logo 重绘（主题色联动）。 必须显式调用：Logo 是 QPainter 一次性画进
    // QPixmap 的**位图**，不像 QSS 那样能自己去读 ThemeManager；主题色变了它不会变色，
    // 只在 createAboutPage() 里生成一次 → 表现为「对话框开着时改了主题色，界面其他部分
    // 都跟着变，唯独关于页 Logo 还是打开那一刻的旧颜色」。故 applyTheme() 里也要重建。
    void updateAboutLogo();
    // 刷新tools列表
    void refreshToolsList();
    // 移动工具
    void moveTool(const QString& name, int delta);
    // 刷新citycombo
    void refreshCityCombo(const QString& province);
    // 刷新districtcombo
    void refreshDistrictCombo(const QString& province, const QString& city);
    // 应用区域选中
    void applyRegionSelection(const QString& province, const QString& city, const QString& district);
    // 加载设置
    void loadSettings();
    // 保存设置
    void saveSettings();
    // 加载general设置
    void loadGeneralSettings();
    // 保存general设置
    void saveGeneralSettings();

    // 以本对话框为 owner 弹出系统原生文件选择框（阻塞）。打开期间临时停用
    // nativeEvent 里的 z 序冻结，让 Windows 得以把 owner（本窗）排到文件框之下，
    // 从而保证文件框恒在本窗之上、不被遮挡（详见 nativeEvent / m_suppressZOrderClamp）。
    QString pickFileModal(const QString& title, const QString& filter);

    // 以本窗为 owner 弹出系统色卡（阻塞），返回 "#RRGGBB"；用户取消返回空串。
    // 与 pickFileModal 同一套 z 序方案（打开期间停用 z 序冻结），色卡同样恒浮于本窗之上。
    QString pickColorModal(const QString& initialHex, const QString& title);

    // 右键色块 → 自定义该格颜色 → 立即生效并持久化（Appearance/palette）
    void onCustomizeSwatch(int index);

    // 按当前色板（含用户自定义）重刷全部色块的底色 / 选中描边 / 悬停提示
    void refreshSwatchStyles();

    // 读取用户自定义色板（Appearance/palette）；空 / 非法 / 格数不符时沿用出厂色板
    void loadPalette();

    QListWidget* m_sidebar = nullptr;
    QStackedWidget* m_stack = nullptr;
    QSlider* m_transparencySlider = nullptr;
    QLabel* m_transparencyLabel = nullptr;
    QLineEdit* m_bgImageEdit = nullptr;
    QLineEdit* m_remindSoundEdit = nullptr;   // 提醒声音文件路径（提醒弹框出现时整曲播放）
    QComboBox* m_fontCombo = nullptr;
    QComboBox* m_partitionCombo = nullptr;
    QButtonGroup* m_colorGroup = nullptr;
    QLabel* m_colorPreview = nullptr;
    QFrame* m_titleBar = nullptr;
    QFrame* m_rootFrame = nullptr;   // 设置中心整体内容容器，提供窗口外框线
    QString m_titleBarStyle;

    QRadioButton* m_tagClickRadio = nullptr;
    QRadioButton* m_tagHoverRadio = nullptr;
    QRadioButton* m_menuAlwaysShowRadio = nullptr;
    QRadioButton* m_menuHoverShowRadio = nullptr;
    QCheckBox* m_boxBorderCheck = nullptr;
    QCheckBox* m_boxRoundCheck = nullptr;
    QCheckBox* m_shortcutArrowCheck = nullptr;
    QCheckBox* m_autoExpandOnHoverCheck = nullptr;
    QCheckBox* m_remindSoundCheck = nullptr;   // 「闹铃」：待办提醒是否出声（未勾选＝静音，弹框照常出现）

    QCheckBox* m_autoStartCheck = nullptr;
    QCheckBox* m_largeTimeFontCheck = nullptr;
    QCheckBox* m_showSecondsCheck = nullptr;
    QCheckBox* m_showMainWindowCheck = nullptr;
    QCheckBox* m_showCompletedCheck = nullptr;
    QCheckBox* m_showClockWeatherCheck = nullptr;
    QCheckBox* m_showWidgetsCheck = nullptr;
    QCheckBox* m_followStartCheck = nullptr;
    QComboBox* m_provinceCombo = nullptr;
    QComboBox* m_cityCombo = nullptr;
    QComboBox* m_districtCombo = nullptr;

    QRadioButton* m_ruleModeRadio = nullptr;
    QRadioButton* m_fixedModeRadio = nullptr;
    QString m_organizeRadioStyle;       // 桌面整理页单选按钮外框样式模板（含主题/透明度令牌）
    QComboBox* m_fixedPartitionCombo = nullptr;
    QTableWidget* m_ruleTable = nullptr;

    // —— 桌面备份页（仿 360 布局：缩略图卡片网格 + 悬停「应用备份」）——
    // 备份实体 = %APPDATA%/DestopTools/backups/<yyyyMMdd_HHmmss>/：
    // DestopTools.ini（整份配置：UserBoxes/Dock/全部设置/待办等）
    // thumb.png（备份瞬间的桌面截图）  meta.ini（备份名）
    QString backupRootDir() const;
    void rebuildBackupCards();          // 扫描备份目录重建网格（含「添加备份」卡）
    // 创建备份now
    void createBackupNow(const QString& name);
    // 应用备份now
    void applyBackupNow(const QString& dir);
    // 删除备份now
    void deleteBackupNow(const QString& dir, const QString& name);
    void restyleBackupCards();          // 主题色/透明度联动：重刷卡片样式
    // rounded备份thumb
    QPixmap roundedBackupThumb(const QString& imageFile, const QSize& size) const;

    QScrollArea* m_backupScroll = nullptr;
    QWidget* m_backupGridHost = nullptr;         // 滚动区内容宿主（透明，无样式）
    QGridLayout* m_backupGrid = nullptr;

    QCheckBox* m_hideIconsOnDoubleClickCheck = nullptr;
    QCheckBox* m_drawBoxOnBlankCheck = nullptr;
    QString m_quickCheckStyle;       // 快捷操作页复选框外框样式模板（含主题/透明度令牌）
    QTableWidget* m_shortcutTable = nullptr;

    QListWidget* m_toolsList = nullptr;
    QStringList m_toolsOrder;
    bool m_isRefreshingToolsList = false;

    QCheckBox* m_userExperienceCheck = nullptr;
    QLabel* m_aboutLogo = nullptr;   // 关于页 Logo（主题色联动重建，见 updateAboutLogo）

    // 备份页自定义卡片（缩略图备份卡 + 悬停浮层），随主题色实时联动
    QPushButton* m_backupAddCard = nullptr;
    QString m_backupAddCardStyle;
    QString m_backupCardStyle;       // 缩略图卡片样式模板（含主题令牌）
    QString m_backupOverlayStyle;    // 悬停浮层样式模板
    QString m_backupApplyBtnStyle;   // 「应用备份」按钮样式模板
    QString m_backupDelBtnStyle;     // 删除按钮样式模板
    QString m_backupDateLabelStyle;  // 日期标签样式模板

    bool m_dragging = false;
    QPoint m_dragPos;
    bool m_isLoading = false;

    // 为 true 时，nativeEvent 跳过 clampBandZOrder 的 z 序冻结。
    // 仅在对本窗拥有的系统文件选择框打开期间置位：此时系统需要把 owner（本窗）重排到
    // 文件框之下（"子框在父框之上"），若仍冻结本窗 z 序，owner 拒绝移动 → 排序错乱 →
    // 文件框反被本窗遮住。置位期间仅保留"不抢焦点"，不再冻结 z 序。
    bool m_suppressZOrderClamp = false;

    // —— 透明度滑块节流（"拖透明度时鼠标卡死"根治）——
    // QSlider::valueChanged 是连续信号：0→100 拖动一次可触发上百次回调。若每次回调都
    // setTransparency → themeChanged → 全应用 setStyleSheet 重建（Qt 遍历进程内每个 widget
    // 重新解析十几 KB QSS）+ 16 个窗口 applyTheme + 写盘，GUI 线程会被整段钉住；而低级鼠标
    // 钩子是系统同步回调到本线程的，线程一堵全系统鼠标就僵。故：
    // · 标签百分比仍实时刷新（零成本，手感即时）；
    // · 真正下发给 ThemeManager 按 ~120ms 窗口合并（窗口首、尾各一次），最多约 16 次/秒；
    // · saveSettings() 从拖动路径彻底移出，只在松手（sliderReleased）与关闭时落盘。
    void flushTransparency();
    QTimer* m_transparencyTimer = nullptr;
    int m_pendingTransparency = -1;

    // IP 自动定位
    QNetworkAccessManager* m_netManager = nullptr;
    QPushButton* m_autoLocateBtn = nullptr;
    bool m_ipGeoPending = false;
    bool m_ipGeoTryHttpFallback = false; // HTTPS 失败后是否已尝试 HTTP fallback

    // 主题联动缓存：外观页指针 / 底部栏指针 / 外观页原始样式（含主题令牌），用于实时重绘
    QWidget* m_appearancePage = nullptr;
    QFrame* m_bottomBar = nullptr;
    QString m_appearancePageStyle;

    // 常规设置页复选框统一背景样式模板（含主题/透明度令牌），构造与 applyTheme 均刷新
    QString m_generalCheckStyle;

    // 主题联动：对话框内部 accent 控件的青色样式模板（含主题令牌），构造与 applyTheme 均用 Theme::applyTokens 重绘
    QPushButton* m_weatherSwitchBtn = nullptr;
    QPushButton* m_toolRestoreBtn = nullptr;
    QPushButton* m_toolAddBtn = nullptr;
    QPushButton* m_toolRemoveBtn = nullptr;
    QPushButton* m_agreementBtn = nullptr;
    QPushButton* m_privacyBtn = nullptr;
    QString m_weatherComboStyle;
    QString m_weatherSwitchBtnStyle;
    QString m_weatherAutoLocateStyle;
    QString m_ruleTableStyle;
    QString m_toolsListStyle;
    QString m_toolActionBtnStyle;
    QString m_agreementBtnStyle;
    QString m_userExperienceCheckStyle;
};

#endif // SETTINGCENTERDIALOG_H