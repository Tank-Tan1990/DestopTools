/*
 * @file desktopscanner.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef DESKTOPSCANNER_H
#define DESKTOPSCANNER_H

#include "desktopitem.h"
#include "settingsmanager.h"
#include <QVector>
#include <QString>
#include <QStringList>
#include <QObject>

    // 桌面整理规则：每行对应设置中心“桌面整理 > 整理规则”表中的一条规则。
    // typeName   : 规则类型名称（如“文档”“图片”），用于识别特殊规则（目录/快捷方式/网址/其它）。
    // extensions : 该规则匹配的文件扩展名（不含点，小写）。目录/其它/快捷方式/网址可空。
    // targetCategory : 该规则命中后项目应归入的分类名称（对应“选择桌面分区”）。
    // enabled    : 是否启用该规则。
    struct OrganizeRule {
        QString typeName;
        QStringList extensions;
        QString targetCategory;
        bool enabled = true;
    };

class DesktopScanner : public QObject {
    Q_OBJECT
public:
    // 构造函数：初始化对象
    explicit DesktopScanner(QObject* parent = nullptr);

    // 扫描指定目录，返回分类好的项目
    QVector<DesktopItem> scan(const QString& folder = QString()) const;

    // 为单个文件/目录构造 DesktopItem（解析快捷方式目标、扩展名、分类等，但不提取图标）。
    // 供 CategoryStore 复用，保证分类记录与桌面扫描一致。
    // 图标提取较重（走 Shell），单独由 loadIcon() 提供，并由 DesktopIconButton 异步加载，避免主线程卡死。
    static DesktopItem makeItem(const QString& filePath);

    // 提取项目图标。sizeHint 为桌面实际渲染像素（如 48），用于选择系统镜像列表层级，
    // 避免 JUMBO 层级对特殊命名空间项返回旧版图标。以 parsename 为键做进程内缓存避免重复取图标。
    // 仅应在主线程调用（STA 安全），不要在后台线程取图标以免触发 Shell 死锁。
    static QIcon loadIcon(const DesktopItem& item, int sizeHint = 0, double arrowScale = 1.0);

    // 清空 loadIcon 的进程内图标缓存。
    // 用途（2026-09-22）：「设置中心 → 外观设置 → 在快捷方式图标上显示箭头」切换后，
    // 同一项目的带箭头 / 不带箭头是两套位图 —— 不清缓存的话，已取过的项目仍会拿到旧态。
    // 调用方随后需要让当前显示的图标**重新取图**（清空 item.icon 再重取，或直接重建网格）。
    // 仅应在主线程调用。
    static void clearIconCache();

    // 统一的「快捷方式真实目标」解析入口：非快捷方式直接返回自身路径；快捷方式用
    // IShellLink 解析出目标文件，解析失败退回自身路径。
    // 凡是要给 DesktopItem::targetPath 赋值的地方都必须用它（构造项、改名同步、加入分类…）。
    // 原因（2026-09-16 实测）：SHDefExtractIconW 对 **.lnk 本体**取不到任何图标（96/256 全为空），
    // 只有对目标文件才能按请求尺寸取到真图。若 targetPath 填成 .lnk 自身，loadIcon 的 p2 就会
    // 拿 .lnk 去取图 → 空 → 掉到 p3 系统镜像列表 → 只拿得到 EXTRALARGE 的 48px；
    // 而 QToolButton/QStyleSheetStyle 只会把位图**缩小**到 iconSize、不会放大不足的位图，
    // 于是“大图标”档只有格子变成 2 倍、图标仍画 48px。
    static QString effectiveTarget(const QString& sourcePath, bool isShortcut);

    // 用系统镜像列表索引取图标（与 Windows 桌面绘制同一图标像素级一致）。
    // sizeHint 为桌面实际渲染像素，选择与桌面同级的 SHIL 层级（LARGE/EXTRALARGE），
    // 而非固定 JUMBO，保证特殊命名空间项(我的电脑/控制面板/回收站/网络)图标与桌面完全一致。
    // 注意：不再建议对快捷方式使用 overlay>0，因为某些系统上覆盖层索引 1 会被第三方 Shell
    // 扩展占用，导致箭头显示成其它图标。快捷方式箭头改由 composeShortcutOverlay 手动叠加。
    // 仅应在主线程调用（COM/Shell 操作）。取图失败返回空图标。
    static QIcon iconFromSystemImageList(int iImage, int sizeHint = 0, int overlay = 0);

    // 用 shell 解析名（文件绝对路径 / 特殊项 "::{CLSID}"）直接取该项的真实图标。
    // 走 IExtractIcon（经 PIDL），返回 Windows 桌面绘制该图标所用的同源权威图标，
    // sizePx 为请求像素尺寸(≤256)。这是修复“Dock 与桌面特殊项图标不一致”的关键路径。
    // 仅应在主线程调用（COM/Shell 操作）。取图失败返回空位图。
    static QPixmap extractIconByParsingName(const QString& parsingName, int sizePx);

    // 取回收站**当前状态**（满/空）对应的真实图标，**不走 loadIcon 的静态缓存**。
    // 回收站是动态图标：清空/放入文件后 shell 会切换满/空两套位图，而 loadIcon 的进程内
    // 缓存会把首次取到的状态永久钉死。此函数每次实时提取，供 Dock 跟随桌面实时切换。
    // 仅应在主线程调用（COM/Shell 操作）。取图失败返回空图标。
    static QIcon recycleBinIcon(int sizeHint = 0);

    // 按指定像素尺寸（256）直接提取真实文件的高清图标（SHDefExtractIcon）。
    // 这是与 Windows 桌面图标同源、分辨率最高的取图方式，避免小图标放大发虚。
    // 仅应在主线程调用（COM/Shell 操作）。取图失败返回空位图。
    static QPixmap highResIconFromFile(const QString& filePath);

    // 在图标左下角叠加 Windows 快捷方式小箭头，生成“与桌面一致”的最终图标。
    // 箭头源取自注册表 Shell Icons\29（与 Explorer 当前角标一致）或 shell32.dll,29，
    // 以高分辨率取出后平滑缩放到较大比例（iconSize 的 50%），保证 Dock 大图标下清晰且不显小。
    // 仅作为 SHGetFileInfo 取图失败时的兜底；正常路径由 SHGetFileInfo(SHGFI_LINKOVERLAY) 直接给出系统箭头。
    // 仅应在主线程调用（COM/Shell 操作）。取图失败返回原图标。
    static QPixmap composeShortcutOverlay(const QPixmap& base, int iconSize, double arrowScale = 1.0);

    // 给项目重新分类
    static QString classify(const DesktopItem& item);

    // 使用整理规则对项目进行分类（设置中心“桌面整理”规则）。
    static QString classifyWithRules(const DesktopItem& item, const QVector<OrganizeRule>& rules);

    // 扫描桌面并按规则/固定分区重新分类后返回。
    // fixedCategory 非空时，所有项目归入该固定分区；否则使用 rules 进行规则匹配。
    QVector<DesktopItem> scanWithRules(const QVector<OrganizeRule>& rules,
                                       const QString& fixedCategory = QString()) const;

    // 桌面特殊命名空间项（我的电脑/回收站/网络/控制面板/个人文件夹）。它们不是磁盘文件，
    // 不在 scan() 的文件枚举中，需单独枚举，整屏隐藏原生桌面后仍能从程序窗口访问。
    // 作为 dock 未就绪时的回退；dock 就绪时“系统”分类改由 dock 实际启用的系统图标决定。
    static QVector<DesktopItem> scanSpecialItems();

    // 默认整理规则，与设置中心“整理规则”表默认内容一致。
    static QVector<OrganizeRule> defaultRules();

    // 从 SettingsManager 读取用户自定义的整理规则（目标分区、启用状态）。
    static QVector<OrganizeRule> loadRules(SettingsManager& sm);

    // —— 以下两项专治“重命名改了后缀”后的类型错配（改后缀后文件内容与新后缀不符） ——

    // 是否为“真实可解析的 .lnk 快捷方式”。
    // 场景：用户把普通文件/文件夹重命名成 .lnk（改后缀）后，后缀虽是 lnk，内容却是普通文件。
    // 这类“假快捷方式”若仍交给 Shell 的链接处理器（图标提取 / 上下文菜单 / IShellLink），
    // 轻则显示出错误图标，重则被 in-proc 加载的 Shell 扩展处理非法数据而崩溃。
    // 返回 false 时应按普通文件处理（图标走通用路径、菜单走普通文件动词）。
    // targetOut 非空时同时回填解析到的目标路径（仅在返回 true 时有效）。
    // 结果按“路径 + 修改时间”缓存，改后缀/文件被改动后自动失效。
    static bool isRealShortcut(const QString& linkPath, QString* targetOut = nullptr);

    // 判断文件“内容是否与其扩展名明显不符”（如 .jpg 开头不是 JPEG 魔数）。
    // 场景：改后缀会把文件变成“类型撒谎”的文件，Shell 会据此调用对应的解码器/图标处理器
    // （图片编解码器、视频缩略图、压缩包预览等，常是第三方 in-proc 扩展）去解析一个内容
    // 根本不匹配的文件。这类处理器在非法数据上崩溃会直接带走本进程。
    // 返回 true 表示“后缀与内容不符”，图标提取应改用不读取文件内容的通用后缀图标。
    static bool suffixMismatchesContent(const QString& filePath, const QString& suffix);

private:
    // 解析快捷方式
    static QString resolveShortcut(const QString& linkPath);

    // 桌面文件夹路径
    static QString desktopFolderPath();

    // 公共桌面（C:\Users\Public\Desktop）：Windows 桌面是个人桌面与公共桌面的并集，
    // 只扫个人桌面会漏掉公共桌面项，导致整理结果比 dock（1:1 镜像真实桌面）少图标。
    QString publicDesktopPath() const;
};

#endif // DESKTOPSCANNER_H