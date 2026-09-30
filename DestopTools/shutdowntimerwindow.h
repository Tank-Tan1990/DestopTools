/*
 * @file shutdowntimerwindow.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef SHUTDOWNTIMERWINDOW_H
#define SHUTDOWNTIMERWINDOW_H

#include <QDialog>
#include <QPoint>

QT_BEGIN_NAMESPACE
class QMouseEvent;
class QShowEvent;
class QHideEvent;
class QLabel;
class QFrame;
class QToolButton;
class QPushButton;
class QRadioButton;
class QButtonGroup;
class QComboBox;
class QDateEdit;
class QStackedWidget;
class QScrollArea;
class QVBoxLayout;
class QTimer;
QT_END_NAMESPACE

struct ShutdownTask;
class ShutdownTaskManager;

// 快捷工具「定时关机」对应的窗口（对应参考图：任务列表页 + 任务设置页 两个页面）。
// 页面结构：
// ① 列表页 —— 提醒横幅（有启用任务时显示「最近一次关机将在 xx 小时 xx 分钟 xx 秒后执行 [取消]」）
// + 空态（暂无任务）+ 区头（我的任务 / + 添加任务）+ 任务行（类型 | 下次执行时间 | 重复方式 | 删除 | 开关）
// ② 设置页 —— 返回 / 设置任务执行时间（每天·工作日·周末 + 时·分 ｜ 指定时间点 + 日期 + 时·分）
// / 设置任务类型（关机·强制关机·重启·睡眠·注销·锁定）/ 取消·确定
// 窗口基础设施（与「快速搜索」窗完全同源，新增顶层窗口一律照此三件套）：
// · Qt::Window 而非 Qt::Dialog —— 避免 Qt 把 band 窗口（Dock/收纳盒/助手）设成本窗口的 owner，
// 否则会随 owner 的周期性压底一起沉到桌面图标层之下；
// · clearOwner + raiseAboveBandWindows（错峰多轮）+ nativeEvent 拦 WM_WINDOWPOSCHANGING；
// · 长期存活窗额外用 500ms 看护定时器：只在被「可见的」band 窗口压住时才救回。
// · 无边框窗口没有系统标题栏，拖动自己实现（按住标题栏）。
class ShutdownTimerWindow : public QDialog {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit ShutdownTimerWindow(QWidget* parent = nullptr);
    ~ShutdownTimerWindow();

    // 打开窗口（每次唤起都停在任务列表页，并刷新任务与倒计时）。
    void showWindow();

protected:
    // 显示事件
    void showEvent(QShowEvent* event) override;
    // 隐藏事件
    void hideEvent(QHideEvent* event) override;
    // 原生事件
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;
    // 鼠标按下事件
    void mousePressEvent(QMouseEvent* event) override;
    // 鼠标移动事件
    void mouseMoveEvent(QMouseEvent* event) override;
    // 鼠标松开事件
    void mouseReleaseEvent(QMouseEvent* event) override;

private slots:
    // 响应editorconfirm
    void onEditorConfirm();
    // 响应editor取消
    void onEditorCancel();
    // 响应模式变化信号
    void onModeChanged(int id);
    // 响应type变化信号
    void onTypeChanged();
    // 更新countdown
    void updateCountdown();

private:
    // 初始化ui
    void setupUi();
    // 构建列表页
    QWidget* buildListPage();
    // 构建editor页
    QWidget* buildEditorPage();
    // 构建task行
    QWidget* buildTaskRow(const ShutdownTask& t);
    // 重建tasks
    void rebuildTasks();
    // 显示列表页
    void showListPage();
    // 显示editor页
    void showEditorPage();
    // 同步actionword
    void syncActionWord();
    // 当前typeid
    int  currentTypeId() const;
    static QString repeatText(const ShutdownTask& t);   // 「每天关机」「指定时间关机」…

    // 判断拖拽area在
    bool isDragAreaAt(const QPoint& pos) const;
    // assertlayerabove条带windows
    void assertLayerAboveBandWindows();
    // schedulelayerasserts
    void scheduleLayerAsserts();

    ShutdownTaskManager* m_mgr = nullptr;

    QFrame* m_card = nullptr;
    QFrame* m_titleBar = nullptr;
    QStackedWidget* m_pages = nullptr;

    // 列表页
    QWidget* m_listPage = nullptr;
    QFrame* m_remindCard = nullptr;
    QLabel* m_remindText = nullptr;
    QToolButton* m_remindCancel = nullptr;
    QWidget* m_emptyState = nullptr;
    QScrollArea* m_taskScroll = nullptr;
    QWidget* m_taskHost = nullptr;
    QVBoxLayout* m_taskColumn = nullptr;
    QToolButton* m_addTaskBtn = nullptr;

    // 设置页
    QWidget* m_editorPage = nullptr;
    QButtonGroup* m_modeGroup = nullptr;         // 0=每天起（周期） 1=指定时间点
    QRadioButton* m_modeRepeat = nullptr;
    QRadioButton* m_modeOnce = nullptr;
    QComboBox* m_repeatCombo = nullptr;          // 每天 / 工作日 / 周末
    QLabel* m_actionWord = nullptr;              // 「关机将在」（随任务类型变化）
    QComboBox* m_repeatHour = nullptr;
    QComboBox* m_repeatMinute = nullptr;
    QDateEdit* m_onceDate = nullptr;
    QComboBox* m_onceHour = nullptr;
    QComboBox* m_onceMinute = nullptr;
    QButtonGroup* m_typeGroup = nullptr;         // ShutdownTask::Type 一一对应

    QTimer* m_layerKeeper = nullptr;

    bool m_positioned = false;                   // 首次显示时居中一次，之后记住用户拖到的位置
    bool m_dragging = false;
    QPoint m_dragPos;
};

#endif // SHUTDOWNTIMERWINDOW_H
