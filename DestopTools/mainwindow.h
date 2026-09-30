/*
 * @file mainwindow.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QMap>
#include <QVector>
#include <QSet>
#include <QStringList>
#include <QFutureWatcher>
#include "desktopitem.h"

QT_BEGIN_NAMESPACE
class IconGridWindow;
class AssistantWindow;
class DesktopMirrorWindow;
class FileSearchWidget;
class ShutdownTimerWindow;
class WallpaperWindow;
class QCloseEvent;
class QTimer;
QT_END_NAMESPACE

#include "settingsmanager.h"   // createBoxFromPersist 参数用到 SettingsManager::UserBoxPersist

// 协调两个无边框窗口：左侧图标网格 + 右侧桌面助手面板
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

private slots:
    // 响应搜索变化信号
    void onSearchChanged(const QString& text);
    // 响应工具触发信号
    void onToolTriggered(const QString& name);
    // 响应分类变化信号
    void onCategoryChanged(const QString& category);
    // 显示分类菜单
    void showCategoryMenu();
    // 响应request新建分类
    void onRequestNewCategory();
    // 删除分类
    void deleteCategory(const QString& name);
    // 重命名分类by名称
    void renameCategoryByName(const QString& name);
    // 保存布局
    void saveLayout();
    // 加载布局
    void loadLayout();
    // 删除盒子分类
    void deleteBoxCategory(const QString& name);
    // 重命名盒子分类
    void renameBoxCategory(const QString& oldName);
    // request保存布局
    void requestSaveLayout();
    // Dock 内联重命名落盘后的收纳盒/网格侧同步（见 mainwindow.cpp 实现）
    void onDockItemRenamed(const QString& oldPath, const QString& newPath);
    // Dock 内图标被 Delete 快捷键删除后的收纳盒/网格侧同步（清理孤儿记录）
    void onDockItemsDeleted(const QStringList& paths);
    // 跨窗口拖拽（Dock ⇄ 收纳盒）的悬停提示：拖动 Dock 图标时实时点亮命中的收纳盒窗口。
    void onDockIconDragHover(const QPoint& globalPos);
    // 跨窗口拖拽结束：清除所有收纳盒窗口的悬停提示。
    void onDockIconDragFinished();
    // 粘贴 / 粘贴快捷方式：轮询等待桌面命名空间原生粘贴落盘后归类到当前分类。
    void onPasteWatchTick();
    // 「显示主界面/隐藏主界面」：勾选=显示助手面板，取消=隐藏（注销 cloak 守卫防止被强制复活）
    void applyAssistantVisibility(bool show);

protected:
    // 关闭事件
    void closeEvent(QCloseEvent* event) override;

private:
    // 位置windows
    void positionWindows();
    // 同步分类到网格
    void syncCategoryToGrid(const QString& category);
    // 添加新建分类
    void addNewCategory();
    // 重命名当前分类
    void renameCurrentCategory();
    // 打开appearance设置
    void openAppearanceSettings();
    // 打开桌面organize设置
    void openDesktopOrganizeSettings();
    // 创建新建盒子
    void createNewBox(QWidget* parent = nullptr);
    // 快捷操作②（设置中心 →「在桌面空白处绘制创建收纳盒」）：Dock 空白处拉框松手后，
    // 按该框新建收纳盒并把框内图标收进它的默认分类（见 DesktopMirrorWindow::endRubber）。
    void createBoxFromDockRegion(const QRect& globalRect, const QStringList& shellPaths,
                                 const QStringList& displayNames);
    // disband末尾盒子
    void disbandLastBox();
    // disband盒子窗口
    void disbandBoxWindow(IconGridWindow* target);

    // 收纳盒（含主整理窗口）的图标集合都以绝对路径为键。某个文件被删除或被改名后，
    // 不剔除/不迁移就会在盒子里留下“点不动、重启也不消失”的幽灵记录。
    // removedNorm 非空 = 删除；oldNorm/newPath 非空 = 改名。返回是否有收纳盒被改动。
    // 调用方负责在事件循环下一拍执行（重建网格不能在删除/改名的调用栈上做）。
    bool syncBoxItemPaths(const QSet<QString>& removedNorm,
                          const QString& oldNorm, const QString& newPath);

    // 已解散分类（持久隐藏）：扫描会按规则重新生成分类，这里把用户主动解散的分类
    // 从扫描结果中移除，并将其内图标并入用户指定的兜底分类，使“解散”在重启/刷新后持续生效。
    // resetDisbanded=true 时（桌面整理按钮触发）：先清空已解散集合，按规则完全重新分类。
    // autoClassifyEmpty=false 时（程序启动）：不自动对“尚未归类”的文件按规则实时整理，
    // 仅加载已持久化的分类，避免用户解散的分类在重启后被扫描规则重新带出。
    // 只有用户点击“桌面整理”按钮时才执行完整整理。
    void refreshDesktop(bool resetDisbanded = false, bool autoClassifyEmpty = true);
    void organizeAndClassifyDesktop();                 // 桌面整理按钮（异步）：主线程收集 dock 图标，后台线程只写分类库（文件不搬动）
    // 应用disbanded分类
    void applyDisbandedCategories();
    // mark分类disbanded
    void markCategoryDisbanded(const QString& name, const QString& fallback);
    // unmark分类disbanded
    void unmarkCategoryDisbanded(const QString& name);
    QSet<QString> disbandedNameSet() const;   // 解析 IconGridWindow/disbandedCategories 得到解散的分类名集合

    // —— 跨窗口拖拽：Dock ⇄ 收纳盒 ——
    // 直接操作归属语义（不经过 refreshDesktop 的全量扫描/自动归类），理由：
    // refreshDesktop() 是“按规则重建全部归属”的完整流程，其中包含“若收纳盒为空则把 dock
    // 全部图标收进收纳盒”的首次启动兜底 —— 用它来响应一次拖拽，会把用户刚移出去的图标
    // 重新收回盒子里，语义完全相反。因此这里只做精确的增量更新，与 onDockItemsDeleted /
    // onDockItemRenamed 的做法保持一致。归属真值仍以 CategoryStore（path → 分类）为准。
    struct BoxDropTarget {
        IconGridWindow* window = nullptr;   // 命中的收纳盒窗口
        QString category;                   // 命中的分类
        bool valid() const { return window != nullptr && !category.isEmpty(); }
    };
    // 屏幕逻辑坐标 → 命中的收纳盒窗口与其分类（含主整理窗口与所有独立收纳盒窗口）。
    BoxDropTarget resolveBoxAt(const QPoint& globalPos) const;
    // 全部收纳盒窗口（主整理窗口 + 各独立收纳盒窗口），供悬停高亮的统一开关。
    // C4：改为返回内部缓存的常量引用 —— 调用方只读遍历，无需每次拷贝。
    const QVector<IconGridWindow*>& allBoxWindows() const;
    // 落点是否落在**真实可放置目标**上（任一收纳盒窗口里真正接得住图标的位置）。
    // 不能用「窗口矩形」判：全屏接管模式下收纳盒窗口铺满整个桌面且背景完全透明，
    // 任何坐标都落在矩形里 → 判定恒为真，会把“丢到壁纸上的图标”也当成丢进了盒子，
    // 既让盒 → Dock 永远不生效，也把 Dock 图标拖到壁纸上的常规重排一并挡掉。
    // 真正的目标由各窗口的 IconGridWindow::hitRealDropTarget 决定
    // （全屏/盒子视图＝命中某个 FenceBox；窗口化网格视图＝落在窗口内）。
    // 用途：「拖拽交换绝不删除文件」的结构性保险 —— 只要松手点真的落在某个盒子上，
    // 无论能否解析出分类，都不允许继续落到 Dock 侧的回收站删除分支。
    bool anyRealDropTargetAt(const QPoint& globalPos) const;
    // 统一开关“松手即收进此处”提示框：命中的那一个亮起，其余一律熄灭。
    void setExternalDropHighlightWindow(IconGridWindow* target);
    // 给一个收纳盒窗口装上“拖出到 Dock”的落点解析器（主整理窗口与每个独立收纳盒窗口都要装）。
    // 判定：松手点若仍落在某个**真实的盒子**上 → 不处理（属窗口内重排或跨盒移动）；
    // 松手点落在 Dock 镜像面上 → 取消该项的分类归属，使其重新出现在 Dock 上，且就落在松手点。
    void installBoxDropResolver(IconGridWindow* w);

    // 给一个收纳盒窗口接上全部主窗口侧信号处理（关闭/菜单/新建分类/解散/改名/刷新/跨窗拖放等），
    // 与默认收纳盒（主整理窗口）保持一致；新建与重启重载两条创建路径共用。
    void connectBoxWindow(IconGridWindow* win);
    // 关闭按钮 / 解散后从 m_boxes 移除并注销守卫（不恢复 Dock 隐藏，沿用既有行为）。
    void removeBoxByWindow(IconGridWindow* target);
    // 启动阶段从本地持久化数据重建用户收纳盒（仅一次）。
    void restoreUserBoxes();
    // 由持久化结构重建单个收纳盒窗口（不弹对话框）。
    void createBoxFromPersist(const UserBoxPersist& p,
                             const QMap<QString, QVector<DesktopItem>>& items);
    // 把给定 shellPath 集合从主窗口 m_allItems 中剔除（收纳盒与主窗口互斥持有同一文件）。
    void stripBoxPathsFromAllItems(const QSet<QString>& paths);
    // 把一组路径从主窗口 m_allItems、各收纳盒 box.items、CategoryStore 中彻底移除，
    // 并让它们在 Dock（桌面镜像）重新显示：用于“解散分类 / 解散收纳盒”释放图标回桌面。
    void releasePathsToDock(const QSet<QString>& normPaths);

    // Dock → 收纳盒：把一批 Dock 图标归入某分类（延迟一拍执行，见实现说明）。
    // insertIndex = 松手点在目标分类网格上的插入下标（0 起；-1 或越界 = 追加末尾）。
    // 它由 IconGridWindow::externalDropIndexAt() 按“松手光标位置”算出，
    // 于是图标会**插到光标所在的那一格**，而不是一律堆到末尾（用户 2026-09-22 要求）。
    void scheduleDockItemsIntoBox(const QStringList& paths, const QStringList& displayNames,
                                  const QString& category, IconGridWindow* targetWindow,
                                  int insertIndex = -1);
    // 收纳盒 → Dock：取消某图标在收纳盒中的归属，使其重新出现在 Dock 上（延迟一拍执行）。
    // globalDropPos = 松手时的逻辑屏幕坐标；图标会“原地出现”在该点上（落点即显示位置）。
    void scheduleBoxItemBackToDock(const QString& sourcePath, const QString& displayName,
                                   const QPoint& globalDropPos);
    // 按 m_allItems 重算 Dock 的隐藏集合并下发（被收纳盒收走的图标不在 Dock 显示）。
    // 这是 Dock 隐藏集合的**唯一**写入出口：任何刷新/改名/删除路径都必须走它，
    // 否则会覆盖掉 CategoryStore 里「已归档（仅 Dock 不显示）」的记录。
    void resetDockHiddenFromAllItems();
    // 标记"用户已手动管理过归属"：此后 refreshDesktop 不再执行"收纳盒为空则全收"的首次兜底。
    void markBoxAssignmentUserHandled();

    // 粘贴 / 粘贴快捷方式：把 Dock/资源管理器复制的图标按桌面命名空间原生动词落到桌面，
    // 再归入 target 当前分类（itemsMap 即 showCategoryMenu 解析出的 m_allItems 或某收纳盒 items，
    // 但归类真值统一走 m_allItems —— 与 scheduleDockItemsIntoBox 同口径）。
    void pasteIntoCategory(IconGridWindow* target, QMap<QString, QVector<DesktopItem>>* itemsMap,
                           int boxIndex, const QString& category, bool shortcut);

    IconGridWindow* m_gridWindow = nullptr;
    AssistantWindow* m_assistantWindow = nullptr;
    DesktopMirrorWindow* m_dock = nullptr;   // 全屏 Dock 镜像层（始终最底层，作为 Windows 桌面的存在）
    FileSearchWidget* m_fileSearch = nullptr;   // 「快速搜索」文件搜索窗口（独立顶层，常驻）
    ShutdownTimerWindow* m_shutdownTimer = nullptr;   // 「定时关机」窗口（独立顶层，常驻；调度器随程序运行）
    WallpaperWindow* m_wallpaper = nullptr;           // 「壁纸」窗口（独立顶层，常驻；更换桌面壁纸）

    bool m_hasGridGeometry = false;
    bool m_hasAssistantGeometry = false;

    QTimer* m_geometrySaveTimer = nullptr;

    // 粘贴 / 粘贴快捷方式：轮询等待原生粘贴落盘的临时状态。
    QTimer* m_pasteWatcher = nullptr;
    QSet<QString> m_pasteBefore;       // 粘贴前桌面文件集合快照
    QString m_pasteCategory;           // 目标归类分类
    // 粘贴意图兜底：归一化路径(小写) → 目标分类。onPasteWatchTick 检测到粘贴产生的新文件即登记，
    // 供 refreshDesktop（SHChangeNotify 触发，autoClassifyEmpty=false）抢跑时把尚未写入映射的副本
    // 重新归回目标分类，避免“副本变 Dock 散件(B 不隐藏) + 不落盘(C 重启丢失)”。
    QHash<QString, QString> m_pasteIntents;
    IconGridWindow* m_pasteTarget = nullptr;
    int m_pasteTicks = 0;              // 轮询计数（上限约 5s）

    // 《桌面整理》后台运行的监视器：写分类库在后台线程执行，避免主线程被阻塞（此前表现为卡死）
    QFutureWatcher<void>* m_organizeWatcher = nullptr;

    QMap<QString, QVector<DesktopItem>> m_allItems;
    QString m_currentCategory;

    // 用户通过“新建收纳盒”创建的独立整理窗口
    struct OrganizerBox {
        QString id;     // 稳定唯一 id（UUID），持久化与重启重载据此关联
        QString name;
        IconGridWindow* window = nullptr;
        QMap<QString, QVector<DesktopItem>> items;
        bool collapsed = false;
    };
    QList<OrganizerBox> m_boxes;
    bool m_boxesRestored = false;   // 启动时收纳盒只重建一次，避免与兜底定时器重复
    // C4：allBoxWindows() 的结果缓存。拖动悬停（40ms 节流）与落点判定都会反复
    // 调用它，而原实现每次都 new 一个 QVector 并拷贝全部指针。缓存每次调用时与
    // “m_boxes + m_gridWindow”逐项比对（盒数量通常个位数），不一致才重建 ——
    // 因此不需要任何“盒增删时置脏”的通知点，天然不会漏更新。
    mutable QVector<IconGridWindow*> m_boxWindowsCache;

#ifdef Q_OS_WIN
    // 整屏接管原生桌面：隐藏/还原 SysListView32，不触碰任何单项 LVIS_HIDDEN，
    // 因此不会触发 LVS_AUTOARRANGE 重排，也就不会刷 Explorer、不会关掉已打开的文件夹窗口。
    void hideNativeDesktop();      // 隐藏原生桌面图标层（找不到句柄时会先定位）
    void restoreNativeDesktop();   // 还原原生桌面图标层
    void toggleNativeDesktop();    // 手动开关，作为安全兜底
    // m_desktopListView 存 SysListView32 句柄（用 void* 避免头文件引入 windows.h）
    void* m_desktopListView = nullptr;
    // 原生事件
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;
#endif
};

#endif // MAINWINDOW_H
