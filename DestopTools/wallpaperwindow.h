/*
 * @file wallpaperwindow.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef WALLPAPERWINDOW_H
#define WALLPAPERWINDOW_H

#include <QDialog>
#include <QPoint>
#include <QString>

QT_BEGIN_NAMESPACE
class QMouseEvent;
class QShowEvent;
class QHideEvent;
class QLabel;
class QFrame;
class QToolButton;
class QPushButton;
class QComboBox;
class QScrollArea;
class QWidget;
class QGridLayout;
class QTimer;
QT_END_NAMESPACE

// 快捷工具「壁纸」对应的窗口：更换桌面壁纸。
// 页面结构（单页）：
// · 标题栏（壁纸 + 关闭）—— 可拖动；
// · 左：壁纸缩略图画廊（自动扫描 Windows 自带壁纸目录 + 顶部一张「选择本地图片」磁贴）；
// · 右：预览 + 填充方式（拉伸/平铺/居中/填充/适应）+ 「应用」按钮。
// 选中缩略图或本地图片 → 右侧预览；点「应用」或双击缩略图即写入系统壁纸
// （注册表 WallpaperStyle/TileWallpaper + SystemParametersInfoW SPI_SETDESKWALLPAPER）。
// 窗口基础设施（与「快速搜索」「定时关机」窗完全同源，新增顶层窗口一律照此三件套）：
// · Qt::Window 而非 Qt::Dialog —— 避免被 band 窗口 owner 化而沉底；
// · clearOwner + raiseAboveBandWindows（错峰多轮）+ nativeEvent 拦 WM_WINDOWPOSCHANGING；
// · 长期存活窗额外用 500ms 看护定时器：只在被「可见的」band 窗口压住时才救回；
// · 无边框窗口没有系统标题栏，拖动自己实现（按住标题栏）。
class WallpaperWindow : public QDialog {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit WallpaperWindow(QWidget* parent = nullptr);
    ~WallpaperWindow();

    // 打开窗口（每次唤起都重新扫描画廊并停在默认位置）。
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
    // 响应thumbclicked
    void onThumbClicked(const QString& path);
    // 响应应用
    void onApply();
    // 响应pick本地
    void onPickLocal();
    // 响应fill变化信号
    void onFillChanged(int index);

private:
    enum FillMode { Stretch, Tile, Center, Fit, Fill };
    // 应用wallpaper
    static bool applyWallpaper(const QString& path, FillMode mode);

    // 初始化ui
    void setupUi();
    // 构建gallery
    void buildGallery();
    // 选择路径
    void selectPath(const QString& path);
    // 清除选中
    void clearSelection();
    void refreshTheme();             // 主题色/透明度变化 → 全量重刷子控件样式
    void refreshGalleryStyles();     // 只刷画廊磁贴（按 tileRole 区分）

    // 判断拖拽area在
    bool isDragAreaAt(const QPoint& pos) const;
    // assertlayerabove条带windows
    void assertLayerAboveBandWindows();
    // schedulelayerasserts
    void scheduleLayerAsserts();

    QFrame* m_card = nullptr;
    QFrame* m_titleBar = nullptr;
    QLabel* m_preview = nullptr;
    QComboBox* m_fillCombo = nullptr;
    QPushButton* m_applyBtn = nullptr;
    QScrollArea* m_galleryScroll = nullptr;
    QWidget* m_galleryHost = nullptr;
    QGridLayout* m_galleryGrid = nullptr;
    QLabel* m_hint = nullptr;

    QString m_currentPath;        // 当前选中（待应用）的壁纸绝对路径
    QStringList m_scanned;        // 已扫描到的系统壁纸路径（用于双击应用回查）

    QTimer* m_layerKeeper = nullptr;
    bool m_dragging = false;
    QPoint m_dragPos;
};

#endif // WALLPAPERWINDOW_H
