/*
 * @file filesearchwidget.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef FILESEARCHWIDGET_H
#define FILESEARCHWIDGET_H

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QList>
#include <QIcon>
#include <QPoint>
#include <QPointer>

QT_BEGIN_NAMESPACE
class QMouseEvent;
class QLineEdit;
class QComboBox;
class QListWidget;
class QListWidgetItem;
class QLabel;
class QToolButton;
class QFrame;
class QStackedWidget;
class QThread;
class QButtonGroup;
class QTimer;
QT_END_NAMESPACE

class FileSearchWorker;

// 「快速搜索」对应的全局文件搜索窗口（图2：文件搜索 / 网页搜索 两个标签页）。
// 设计要点：
// · 独立顶层窗口（Qt::Window），z 序钉在所有 band 窗口（Dock/收纳盒/助手）之上、但低于其它程序，
// 与 GlassInputDialog（重命名弹框）/ UpdateDialog / GlassMessageBox 完全同一套方案：
// Qt::Window（不被 owner 化）+ clearOwner + raiseAboveBandWindows + nativeEvent 拦截 WM_WINDOWPOSCHANGING。
// ·  本窗口与弹框的关键差别：它**长期存活**（一次 show 后一直留在屏幕上），而弹框是 show 完就交互。
// 因此两层保护都要有：① 每次 show() 后按 0/60/200/400/800ms 错峰再断言（Qt 会在 show 之后
// 沿 parent 链补一次内部 z 序调整，单次断言会被它覆盖）；② 可见期间由 500ms 看护定时器
// 「只在被 band 窗口压住时」把窗口救回 band 之上 —— 保证长时间开着也不会莫名沉到收纳盒/助手下面。
// · 文件搜索走后台线程（QDirIterator 递归枚举），边搜边出结果、不卡 UI；关键字大小写不敏感；
// 全盘（此电脑）搜索上限封顶，避免无限遍历。
// · 网页搜索回车即用系统默认浏览器打开搜索引擎结果页（不内嵌浏览器）。
class FileSearchWidget : public QDialog {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit FileSearchWidget(QWidget* parent = nullptr);
    ~FileSearchWidget();

    // 打开窗口并（可选）带入初始关键字：keyword 非空则直接进入文件搜索页并开始搜索。
    void showSearch(const QString& keyword = QString());

protected:
    // 显示事件
    void showEvent(QShowEvent* event) override;
    // 隐藏事件
    void hideEvent(QHideEvent* event) override;
    // 原生事件
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;
    // 无边框窗口（Qt::FramelessWindowHint）没有系统标题栏，拖动必须自己实现。
    // 按项目统一方案（GlassInputDialog 重命名弹框 / UpdateDialog / SettingCenterDialog 同款）：
    // 只用「按住标题栏」这一条拖动通道 —— 标题栏内是标题文字与关闭按钮，
    // 标题栏本身不承载任何编辑交互，不会与输入/选择冲突。
    void mousePressEvent(QMouseEvent* event) override;
    // 鼠标移动事件
    void mouseMoveEvent(QMouseEvent* event) override;
    // 鼠标松开事件
    void mouseReleaseEvent(QMouseEvent* event) override;

private slots:
    // 响应文件keyword变化信号
    void onFileKeywordChanged(const QString& text);
    // 响应scope变化信号
    void onScopeChanged(int index);
    // 响应resultbatch
    void onResultBatch(qint64 token, const QStringList& paths, const QList<bool>& isDirs, int runningTotal);
    // 响应搜索finished
    void onSearchFinished(qint64 token, int total);
    // 响应resultactivated
    void onResultActivated(QListWidgetItem* item);
    // 响应web搜索
    void onWebSearch();
    // switch标签
    void switchTab(int tab);
    // 应用主题
    void applyTheme();

private:
    // 初始化ui
    void setupUi();
    // populatescopecombo
    void populateScopeCombo();
    // 开始搜索
    void startSearch(const QString& keyword);
    // 按下点是否落在「可拖动」区域（标题栏或其上的标题文字、卡片/页面容器的空白留白）。
    // 输入框 / 下拉框 / 结果列表 / 按钮等交互控件一律排除，避免误拖。
    bool isDragAreaAt(const QPoint& pos) const;
    // assertlayerabove条带windows
    void assertLayerAboveBandWindows();
    void scheduleLayerAsserts();          // show 之后 0/60/200/400/800ms 错峰多轮再断言（幂等）
    // rootsforscope
    QStringList rootsForScope(const QString& scopeKey) const;
    // 搜索url
    static QString searchUrl(const QString& engine, const QString& query);

    QLineEdit* m_fileEdit = nullptr;
    QComboBox* m_scopeCombo = nullptr;
    QListWidget* m_resultList = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLineEdit* m_webEdit = nullptr;
    QComboBox* m_engineCombo = nullptr;

    QToolButton* m_tabFile = nullptr;
    QToolButton* m_tabWeb = nullptr;
    QStackedWidget* m_pages = nullptr;
    QFrame* m_titleBar = nullptr;
    // 可拖区域判定用：这几个容器自身的空白留白允许拖动（其内部的交互控件不允许）
    QFrame* m_card = nullptr;
    QWidget* m_filePage = nullptr;
    QWidget* m_webPage = nullptr;

    QThread* m_thread = nullptr;
    FileSearchWorker* m_worker = nullptr;
    qint64 m_searchToken = 0;
    QString m_currentKeyword;
    QTimer* m_debounce = nullptr;
    QTimer* m_layerKeeper = nullptr;      // 可见期间 500ms 看护：只在被 band 窗口压住时才救回

    bool m_dragging = false;              // 是否正按住标题栏拖动窗口
    QPoint m_dragPos;                     // 按下点相对窗口左上角的偏移（全局坐标）
};

#endif // FILESEARCHWIDGET_H
