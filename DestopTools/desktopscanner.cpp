/*
 * @file desktopscanner.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "desktopscanner.h"
#include "crashtrace.h"
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QSet>
#include <QHash>
#include <QPair>
#include <QFile>
#include <cstring>
#include <QFileIconProvider>
#include <QPainter>
#include <QPainterPath>
#include <QMimeDatabase>
#include <QMimeType>
#include <QStandardPaths>
#include <QUrl>
#include <QRegularExpression>
#include <QtWin>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <commoncontrols.h>
#endif

DesktopScanner::DesktopScanner(QObject* parent) : QObject(parent) {
}

// 桌面文件夹路径
// 作者：谭征
QString DesktopScanner::desktopFolderPath() {
    return QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
}

// 扫描指定目录，返回分类好的项目
// 作者：谭征
QVector<DesktopItem> DesktopScanner::scan(const QString& folder) const {
    QString path = folder.isEmpty() ? desktopFolderPath() : folder;
    QVector<DesktopItem> result;
    QSet<QString> seen;   // 按绝对路径去重，避免个人/公共桌面出现同名项时重复计数

    QMimeDatabase mimeDb;

#ifdef Q_OS_WIN
    // COM 仅在每次 scan 初始化一次（而非逐个 .lnk 文件开关），显著减少“桌面整理”时的开销。
    const HRESULT cohr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = SUCCEEDED(cohr);
#endif

    // Windows 桌面 = 个人桌面 ∪ 公共桌面。逐个目录枚举并去重合并，
    // 保证整理结果包含公共桌面项，与 dock（1:1 镜像真实桌面）图标数量一致。
    auto addFrom = [&](const QString& dirPath) {
        if (dirPath.isEmpty()) return;
        QDir dir(dirPath);
        if (!dir.exists()) return;
        const QFileInfoList entries = dir.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
            QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo& info : entries) {
            if (info.fileName().startsWith(QLatin1String("DestopTools"))) continue; // 跳过自己
            const QString abs = QDir::fromNativeSeparators(info.absoluteFilePath());
            const QString key = abs.toLower();
            if (seen.contains(key)) continue;
            seen.insert(key);
            result.append(makeItem(abs));
        }
    };

    addFrom(path);                  // 个人桌面（F:\Desktop 或重定向位置）
    addFrom(publicDesktopPath());   // 公共桌面（C:\Users\Public\Desktop）

#ifdef Q_OS_WIN
    if (needUninit) CoUninitialize();
#endif
    return result;
}

// 只扫个人桌面会漏掉公共桌面项，导致整理结果比 dock（1:1 镜像真实桌面）少图标。
// 作者：谭征
QString DesktopScanner::publicDesktopPath() const {
#ifdef Q_OS_WIN
    // CSIDL_COMMON_DESKTOPDIRECTORY = 公共（所有用户）桌面目录。
    WCHAR buf[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_DESKTOPDIRECTORY,
                                   nullptr, 0, buf)) && buf[0] != L'\0') {
        return QDir::fromNativeSeparators(QString::fromWCharArray(buf));
    }
#endif
    return QString();
}

// 图标提取较重（走 Shell），单独由 loadIcon() 提供，并由 DesktopIconButton 异步加载，避免主线程卡死。
// 作者：谭征
DesktopItem DesktopScanner::makeItem(const QString& filePath) {
    // 注意：此处只构造元数据（路径/名称/分类/可执行标记），不提取图标——图标提取走 Shell，
    // 在主线程同步做会成为卡死根因。真实图标由 DesktopIconButton 构造后异步（定时器分批）加载。
    QFileInfo info(filePath);

    DesktopItem item;
    item.sourcePath = info.absoluteFilePath();
    item.isShortcut = info.suffix().compare(QLatin1String("lnk"), Qt::CaseInsensitive) == 0;

    if (item.isShortcut) {
        item.targetPath = resolveShortcut(item.sourcePath);
        item.displayName = info.completeBaseName();
        QFileInfo targetInfo(item.targetPath);
        item.isExecutable = targetInfo.suffix().compare(QLatin1String("exe"), Qt::CaseInsensitive) == 0
                        || targetInfo.isExecutable();
    } else {
        item.targetPath = item.sourcePath;
        item.displayName = info.fileName();
        item.isExecutable = info.isExecutable();
    }

    item.category = classify(item);
    return item;
}

// 内部辅助：返回系统图像列表索引对应的 QPixmap（不含覆盖层），供 loadIcon 手动合成箭头。
static QPixmap iconFromSystemImageListPixmap(int iImage, int sizeHint)
{
    if (iImage < 0) return QPixmap();
    int pref;
    if (sizeHint >= 256)      pref = SHIL_JUMBO;
    else if (sizeHint >= 48)  pref = SHIL_EXTRALARGE;
    else if (sizeHint >= 32)  pref = SHIL_LARGE;
    else                      pref = SHIL_SMALL;
    const int order[] = { pref, SHIL_EXTRALARGE, SHIL_LARGE, SHIL_JUMBO, SHIL_SMALL };
    for (int i = 0; i < 5; ++i) {
        IImageList* piml = nullptr;
        if (FAILED(SHGetImageList(order[i], IID_IImageList, reinterpret_cast<void**>(&piml))) || !piml)
            continue;
        HICON hico = nullptr;
        if (SUCCEEDED(piml->GetIcon(iImage, ILD_TRANSPARENT, &hico)) && hico) {
            QPixmap pm = QtWin::fromHICON(hico);
            DestroyIcon(hico);
            piml->Release();
            if (!pm.isNull()) return pm;
        }
        piml->Release();
    }
    return QPixmap();
}

// 仅应在主线程调用（COM/Shell 操作）。取图失败返回空图标。
// 作者：谭征
QIcon DesktopScanner::iconFromSystemImageList(int iImage, int sizeHint, int overlay) {
    // 用系统镜像列表索引取图标：该索引就是 Windows 桌面绘制同一图标所用的索引。
    // 关键：选择与桌面实际渲染尺寸(sizeHint)最接近的 SHIL 层级优先返回，而非固定 JUMBO(256)。
    // 特殊命名空间项(我的电脑/控制面板/回收站/网络)在 JUMBO 层级常是旧版/占位图标，
    // 而桌面实际使用的是 LARGE(32)/EXTRALARGE(48) 层级——贴近桌面尺寸取图，内容与桌面完全一致。
    // 注意：不再建议对快捷方式使用 overlay>0，因为某些系统上覆盖层索引 1 会被第三方 Shell
    // 扩展占用，导致箭头显示成其它图标（如“两个人头”）。快捷方式箭头改由 composeShortcutOverlay
    // 手动叠加 SIID_LINK Stock Icon。
    if (iImage < 0) return QIcon();
    int pref;
    if (sizeHint >= 256)      pref = SHIL_JUMBO;
    else if (sizeHint >= 48)  pref = SHIL_EXTRALARGE;
    else if (sizeHint >= 32)  pref = SHIL_LARGE;
    else                      pref = SHIL_SMALL;
    const int order[] = { pref, SHIL_EXTRALARGE, SHIL_LARGE, SHIL_JUMBO, SHIL_SMALL };
    const DWORD overlayMask = (overlay > 0) ? (static_cast<DWORD>(overlay) << 8) : 0;
    for (int i = 0; i < 5; ++i) {
        IImageList* piml = nullptr;
        if (FAILED(SHGetImageList(order[i], IID_IImageList, reinterpret_cast<void**>(&piml))) || !piml)
            continue;
        HICON hico = nullptr;
        if (SUCCEEDED(piml->GetIcon(iImage, ILD_TRANSPARENT | overlayMask, &hico)) && hico) {
            QPixmap pm = QtWin::fromHICON(hico);
            DestroyIcon(hico);
            piml->Release();
            if (!pm.isNull()) return QIcon(pm);
        }
        piml->Release();
    }
    return QIcon();
}

// 用 shell 解析名(文件绝对路径 / 特殊项 "::{CLSID}")直接取该项的真实图标。
// 走 IExtractIcon(经 PIDL)，返回 Windows 桌面绘制该图标所用的同源权威图标，sizePx 为请求像素尺寸(≤256)。
// 这是修复“Dock 与桌面特殊项(我的电脑/控制面板/回收站/网络)图标不一致”的关键路径。
// 作者：谭征
QPixmap DesktopScanner::extractIconByParsingName(const QString& parsingName, int sizePx) {
    if (parsingName.isEmpty() || sizePx <= 0) return QPixmap();
    const HRESULT cohr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = (cohr == S_OK);
    QPixmap result;
    do {
        LPITEMIDLIST pidl = nullptr;
        if (FAILED(SHParseDisplayName(parsingName.toStdWString().c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
            break;
        IShellFolder* desk = nullptr;
        if (FAILED(SHGetDesktopFolder(&desk))) { CoTaskMemFree(pidl); break; }
        IExtractIconW* pei = nullptr;
        LPCITEMIDLIST pidls[1] = { pidl };
        if (FAILED(desk->GetUIObjectOf(nullptr, 1, pidls, IID_IExtractIconW, nullptr,
                                       reinterpret_cast<void**>(&pei))) || !pei) {
            desk->Release(); CoTaskMemFree(pidl); break;
        }
        HICON hLarge = nullptr, hSmall = nullptr;
        // 请求 sizePx×sizePx；实现不支持该尺寸时会回退到可用的最大尺寸，内容仍为该项真实图标。
        const HRESULT hr = pei->Extract(nullptr, 0, &hLarge, &hSmall, MAKELONG(sizePx, sizePx));
        if (SUCCEEDED(hr) && (hLarge || hSmall)) {
            HICON h = hLarge ? hLarge : hSmall;
            QPixmap pm = QtWin::fromHICON(h);
            if (!pm.isNull()) result = pm;
            if (hLarge) DestroyIcon(hLarge);
            if (hSmall) DestroyIcon(hSmall);
        }
        pei->Release();
        desk->Release();
        CoTaskMemFree(pidl);
    } while (false);
    if (needUninit) CoUninitialize();
    return result;
}

// 仅应在主线程调用（COM/Shell 操作）。取图失败返回空位图。
// 作者：谭征
QPixmap DesktopScanner::highResIconFromFile(const QString& filePath) {
    // 用 SHDefExtractIcon 直接从真实文件（含 .lnk 目标）按 256px 提取高清图标。
    // 这是 Windows 桌面绘制图标所用的同源高分辨率来源；即便 Dock 在小尺寸（如 50px）绘制，
    // 也是从 256px 源平滑下采样，不会发虚。失败返回空位图。
    const HRESULT cohr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = (cohr == S_OK);

    QPixmap result;
    HICON hLarge = nullptr;
    HICON hSmall = nullptr;
    const std::wstring path = filePath.toStdWString();
    // 目标 256px；SHDefExtractIcon 会按文件实际可用的最大分辨率返回（不高于请求值）。
    // 关键（崩溃修复）：nIconSize 必须是 MAKELONG(large, small)，高 16 位是“小图标尺寸”。
    // 旧实现直接传 256，高 16 位为 0 —— 等于向 Shell 请求“0×0 的小图标”。系统自带处理器
    // 通常容忍，但图片/媒体等类型会转交 WIC / 第三方缩略图提供者，它们对 0 尺寸不做校验，
    // 一除零/越界就越界访问，直接带走本进程。（“改后缀”后必然走冷缓存重取图标，正是触发点。）
    const HRESULT hr = SHDefExtractIconW(path.c_str(), 0, 0, &hLarge, &hSmall,
                                         MAKELONG(256, 16));
    if (SUCCEEDED(hr) && (hLarge || hSmall)) {
        HICON h = hLarge ? hLarge : hSmall;
        QPixmap pm = QtWin::fromHICON(h);
        if (!pm.isNull()) result = pm;
        if (hLarge) DestroyIcon(hLarge);
        if (hSmall) DestroyIcon(hSmall);
    }

    if (needUninit) CoUninitialize();
    return result;
}

// 在图标左下角叠加 Windows 快捷方式小箭头，生成“与桌面一致”的最终图标（仅系统覆盖层路径
// 未命中时的兜底）。箭头源优先用系统 Stock Icon(SIID_LINK)——这正是 Windows 桌面快捷方式箭头
// 的同源字形；取不到再回退到注册表 Shell Icons\29 / shell32.dll,29。箭头尺寸随图标大小成比例
// 缩小为小角标（约 iconSize 的 0.30 倍，上限 56），与 Windows 桌面快捷方式角标观感一致（大图标档只放大程序本体，箭头不被放成巨无霸）。
// 作者：谭征
QPixmap DesktopScanner::composeShortcutOverlay(const QPixmap& base, int iconSize, double arrowScale) {
    // arrowScale <= 0 ⇒ 调用方明确表示「不要箭头」（「在快捷方式图标上显示箭头」复选关闭时，
    // 调用方经 Theme::shortcutArrowOverlayScale() 传 0 进来）。必须在这里早退，
    // 否则下面的尺寸换算会算出 0×0 的角标并被 scaled() 成空图 —— 底图会被抹掉。
    if (base.isNull() || iconSize <= 0 || arrowScale <= 0.0) return base;

    QPixmap arrow;

    // 1) 优先：系统真实的“链接”角标（SHGetStockIconInfo，与桌面箭头同源同样式）
    SHSTOCKICONINFO sii = { sizeof(sii) };
    if (SUCCEEDED(SHGetStockIconInfo(SIID_LINK,
                                     SHGSI_ICON | SHGSI_LARGEICON, &sii)) && sii.hIcon) {
        arrow = QtWin::fromHICON(sii.hIcon);
        DestroyIcon(sii.hIcon);
    }

    // 2) 兜底：注册表 Shell Icons\29 或 shell32.dll,29
    if (arrow.isNull()) {
        QString path;
        int index = 29;
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Icons",
                          0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            WCHAR buf[512] = {0};
            DWORD len = sizeof(buf);
            DWORD type = 0;
            if (RegQueryValueExW(hKey, L"29", nullptr, &type,
                                 reinterpret_cast<LPBYTE>(buf), &len) == ERROR_SUCCESS) {
                QString val;
                if (type == REG_EXPAND_SZ) {
                    WCHAR expanded[1024] = {0};
                    if (ExpandEnvironmentStringsW(buf, expanded, 1024))
                        val = QString::fromWCharArray(expanded);
                }
                if (val.isEmpty()) val = QString::fromWCharArray(buf);
                const int comma = val.lastIndexOf(QLatin1Char(','));
                if (comma > 0) {
                    path = val.left(comma).trimmed();
                    bool ok = false;
                    const int n = val.mid(comma + 1).trimmed().toInt(&ok);
                    if (ok) index = n;
                } else if (!val.isEmpty()) {
                    path = val;
                }
            }
            RegCloseKey(hKey);
        }
        if (path.isEmpty()) {
            path = qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"))
                   + QStringLiteral("\\System32\\shell32.dll");
            index = 29;
        }
        HICON hIcon = nullptr;
        if (ExtractIconExW(path.toStdWString().c_str(), index, nullptr, &hIcon, 1) > 0 && hIcon) {
            arrow = QtWin::fromHICON(hIcon);
            DestroyIcon(hIcon);
        }
    }

    // 3) 终极兜底：如果系统箭头都取不到（或像用户系统一样被占用成别的图标），
    // 自绘一个简洁的 Windows 风格链接箭头，确保快捷方式角标永远可识别。
    if (arrow.isNull()) {
        const int aw = qBound(8, qRound(iconSize * 0.28), 72);
        arrow = QPixmap(aw, aw);
        arrow.fill(Qt::transparent);
        QPainter pa(&arrow);
        pa.setRenderHint(QPainter::Antialiasing, true);
        pa.setPen(Qt::NoPen);
        // 经典 Windows 链接箭头：左下指向右上，蓝色填充 + 白色描边，任何背景都可见。
        const QPointF pts[7] = {
            QPointF(aw * 0.18, aw * 0.82),
            QPointF(aw * 0.55, aw * 0.45),
            QPointF(aw * 0.42, aw * 0.32),
            QPointF(aw * 0.88, aw * 0.28),
            QPointF(aw * 0.84, aw * 0.74),
            QPointF(aw * 0.71, aw * 0.61),
            QPointF(aw * 0.34, aw * 0.98)
        };
        QPolygonF poly;
        for (const auto& pt : pts) poly << pt;
        QPainterPath path;
        path.addPolygon(poly);
        // 白色外描边
        pa.setPen(QPen(Qt::white, qMax(1.0, aw * 0.08)));
        pa.setBrush(QColor(0x2d, 0x7d, 0xd2)); // Windows 10/11 链接箭头蓝
        pa.drawPath(path);
        pa.end();
    }

    // Windows 在快捷方式图标左下角绘制一个“小”链接箭头（约占图标 0.28 倍，与桌面角标观感一致）。
    // “大图标”档只放大程序本体图标（2 倍），箭头保持同比例的小角标，不要放大成巨无霸。
    // arrowScale：收纳盒 / Dock 等场景需要把箭头放大到当前的两倍（=2.0），默认 1.0 保持原样；
    // 上限随比例放宽，保证 2 倍时在大图标档也能真正翻倍而不是被钉死在 72。
    const double frac = 0.28 * arrowScale;
    const int awCap = qRound(72.0 * arrowScale);
    const int aw = qBound(8, qRound(iconSize * frac), awCap);
    arrow = arrow.scaled(aw, aw, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (arrow.isNull()) return base;

    QPixmap result = base;
    QPainter p(&result);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // 左下角对齐（留出 1px 边距），与 Windows 桌面角标位置一致。
    p.drawPixmap(1, result.height() - arrow.height() - 1, arrow);
    p.end();
    return result;
}

namespace {
// 按“扩展名”取通用图标，完全不接触真实文件内容。
// 用不存在的假文件名 + SHGFI_USEFILEATTRIBUTES：Shell 只按后缀查文件类型关联，
// 不会打开/解析真实文件，因此不会触发任何第三方 in-proc 图标/缩略图处理器 ——
// 这是“后缀与内容不符”（改后缀造成）时的安全取图方式。
QPixmap genericIconByExtension(const QString& suffix, int sizePx) {
    if (suffix.isEmpty()) return QPixmap();
    const QString fake = QStringLiteral("destoptools_type_probe.") + suffix;
    SHFILEINFOW sfi = {0};
    const UINT flags = SHGFI_ICON | SHGFI_USEFILEATTRIBUTES
                     | (sizePx >= 48 ? SHGFI_LARGEICON : SHGFI_SMALLICON);
    if (SHGetFileInfoW(fake.toStdWString().c_str(), FILE_ATTRIBUTE_NORMAL,
                       &sfi, sizeof(sfi), flags) && sfi.hIcon) {
        QPixmap pm = QtWin::fromHICON(sfi.hIcon);
        DestroyIcon(sfi.hIcon);
        return pm;
    }
    return QPixmap();
}
} // namespace

// loadIcon 的进程内图标缓存。
// 2026-09-22 由「loadIcon 内的函数级 static」提为文件级 static，**唯一**目的是让
// clearIconCache() 能清空它：「在快捷方式图标上显示箭头」开关切换后，同一项目的带箭头 /
// 不带箭头是两套位图，旧条目必须丢弃重取（缓存键含 arrowScale，两态各占一份条目，不会串味）。
// 其余行为与原先完全一致（仍然只在主线程访问）。
static QHash<QString, QIcon> g_iconCache;

// 仅应在主线程调用。
// 作者：谭征
void DesktopScanner::clearIconCache() {
    g_iconCache.clear();
}

// 仅应在主线程调用（STA 安全），不要在后台线程取图标以免触发 Shell 死锁。
// 作者：谭征
QIcon DesktopScanner::loadIcon(const DesktopItem& item, int sizeHint, double arrowScale) {
    // 图标提取走 Shell；以 parsename 为键做进程内缓存，避免每次刷新重复取图标。
    // 仅由主线程（DesktopIconButton 构造时同步调用）调用，确保 STA 安全、不触发 Shell 死锁。
    // 目标：Dock 图标大小、清晰度与 Windows 桌面逐项一致。
    // 关键：必须按 effSize 取到对应分辨率的真实图标，绝不能拿 32px 小图标再放大——
    // 放大后既模糊又显得比桌面小。

    const int effSize = sizeHint > 0 ? sizeHint : 48;

    // 权威解析名：特殊项用 CLSID；其它统一用 sourcePath（含 .lnk 本身）。
    QString parsename;
    if (item.isSpecial) {
        parsename = item.shellPath.startsWith(QLatin1String("::"))
                        ? item.shellPath : item.sourcePath;
    } else {
        parsename = item.sourcePath;
    }

    // 回收站是“动态图标”：shell 按满/空状态实时切换位图。loadIcon 的进程内静态缓存会把
    // 首次取到的状态永久钉死 —— 清空/放入文件后 Dock 图标不再跟随桌面变化。对回收站一律
    // 走 recycleBinIcon（每次实时提取、不经缓存），根除“状态钉死”。
    if (item.isSpecial && parsename.contains(QStringLiteral("645ff040"), Qt::CaseInsensitive)) {
        return recycleBinIcon(effSize);
    }

    QHash<QString, QIcon>& cache = g_iconCache;   // 见文件级定义：clearIconCache() 需能清空它
    // A4：图标缓存此前**无上限** —— 桌面/收纳盒里出现过的每个
    // (路径 | 尺寸档 | 角标档) 组合都永久驻留（QIcon 里是 effSize×effSize 的 QPixmap）。
    // 增删桌面项、改后缀、图标落进收纳盒又拖回桌面……都会新增条目，长时间运行后单调增长。
    // 与同文件 isRealShortcut() 的「超限即清」口径保持一致（阈值按最大并发条目数放大到 2048，
    // 避免正常规模下刚清完又满、反复重取图标反而变慢）。
    if (cache.size() > 2048) cache.clear();
    // 缓存键含 arrowScale：不同角标缩放因子（收纳盒/Dock=2.0，桌面=1.0）必须各自缓存，
    // 否则会互相命中对方系数下的旧图（盒拿到 1.0 角标、或桌面误用 2.0 角标）。
    // 也让「显示快捷方式箭头」开关的两态（scale=2.0 / scale=0）各占一份条目。
    const QString key = parsename + QLatin1Char('|') + QString::number(effSize)
                        + QLatin1Char('|') + QString::number(arrowScale);
    auto it = cache.find(key);
    if (it != cache.end()) return *it;

    // —— 类型错配防护（“改后缀”专属）：后缀与文件内容明显不符时，绝不把该文件交给 Shell 的
    // 图标/缩略图处理器。这类处理器（图片编解码器、视频缩略图、压缩包预览、PE 资源…）多为
    // 第三方 in-proc Shell 扩展，解析“类型撒谎”的文件时崩溃会直接带走本进程，正是
    // “只有改后缀才崩”的最合理机制。此处改按扩展名取通用图标（完全不打开真实文件）。
    if (!item.isSpecial && !parsename.isEmpty()
        && !parsename.startsWith(QLatin1String("::"))) {
        const QString suf = QFileInfo(parsename).suffix();
        if (!suf.isEmpty() && suffixMismatchesContent(parsename, suf)) {
            CrashTrace::markPath("loadIcon:type-mismatch(generic)", parsename);
            QPixmap pm = genericIconByExtension(suf, effSize);
            QIcon gi(pm);
            cache.insert(key, gi);
            return gi;
        }
    }

    const HRESULT cohr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = (cohr == S_OK);
    QIcon result;

    // 统一的收尾：① 把底图规整到 effSize×effSize；② 叠加快捷方式角标；③ 包成 QIcon。
    // 为什么必须规整（2026-09-16 实测）：QToolButton（含 QSS/QStyleSheetStyle 接管时）
    // **只会把位图缩小到 iconSize，不会放大不足的位图** —— 画出尺寸 = min(位图实际像素, iconSize)。
    // 实验（.workbuddy/tmp/icontest，QSS+TextUnderIcon+144×168+iconSize=96）：
    // 位图 96/256px → 画 96px；位图 48px → 画 48px。
    // 于是任何一条兜底路径只要返回 48px（系统镜像列表 EXTRALARGE 就只有 48px），
    // “大图标”档就会变成“只有格子放大、图标没放大”。这里统一兜住：不足 effSize 就平滑放大，
    // 超过 effSize 就高质量缩小（大源缩下来比小源放大清晰）。
    // 次序很重要：必须先规整再叠箭头 —— 箭头尺寸按 0.28×effSize 算，底图不是 effSize 时
    // 箭头位置/比例都会错位。
    auto finalize = [&](QPixmap pm) -> QIcon {
        if (pm.isNull()) return QIcon();
        if (pm.width() != effSize || pm.height() != effSize)
            pm = pm.scaled(effSize, effSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (item.isShortcut) pm = composeShortcutOverlay(pm, effSize, arrowScale);
        return QIcon(pm);
    };

    // 快捷方式一律用**解析后的目标**去取图：SHDefExtractIconW 对 .lnk 本体取不到任何图标
    // （96/256 全为空，实测），只有目标文件才有对应尺寸的真图。
    const bool useTarget = item.isShortcut && !item.isSpecial && !item.targetPath.isEmpty()
                           && item.targetPath.compare(item.sourcePath, Qt::CaseInsensitive) != 0;
    const QString extractName = useTarget ? item.targetPath : parsename;

    // —— 路径1：按目标尺寸 effSize 直接提取该项的真实高清图标（特殊项/普通文件/快捷方式目标）。
    // 对快捷方式，取到目标图标后再手动叠加系统链接箭头（composeShortcutOverlay）。
    // 注意：不再使用系统图像列表的“覆盖层索引 1”来合成箭头——在某些系统上该索引会被
    // 第三方 Shell 扩展（网盘/共享软件）占用，导致 Dock 快捷方式角标显示成“两个人头”等
    // 错误图标。Windows Explorer 自己不用这个索引合成箭头，我们改为手动取 SIID_LINK
    // Stock Icon 再叠加，保证与桌面箭头逐项一致。
    if (result.isNull() && !extractName.isEmpty()) {
        CrashTrace::markPath("loadIcon:p1-IExtractIcon", extractName);
        const QPixmap pm = extractIconByParsingName(extractName, effSize);
        if (!pm.isNull()) result = finalize(pm);
    }

    // —— 路径2：普通文件/快捷方式目标按 effSize 用 SHDefExtractIcon 直接取。
    if (result.isNull() && !item.isSpecial && !extractName.isEmpty()) {
        CrashTrace::markPath("loadIcon:p2-SHDefExtractIcon", extractName);
        const QString target = extractName;
        HICON hLarge = nullptr, hSmall = nullptr;
        const UINT want = (UINT)qMax(effSize, 32);
        // 关键（崩溃修复）：nIconSize 必须形如 MAKELONG(large, small)。
        // 旧实现直接传 want（如 48），高 16 位恒为 0 —— 相当于向 Shell 请求“0×0 的小图标”。
        // shell32 自带处理器一般容忍，但图片/视频/压缩等后缀会把请求转给 WIC / 第三方缩略图
        // 提供者（in-proc Shell 扩展），它们对 0 尺寸普遍不做校验，一除零就越界访问并把整个
        // 进程带走 —— 这正是“只有改了后缀才崩”的机制：换后缀 = 换图标处理器 = 换到不健壮的那个。
        const HRESULT hr = SHDefExtractIconW(target.toStdWString().c_str(), 0, 0,
                                             &hLarge, &hSmall, MAKELONG(want, 16));
        if (SUCCEEDED(hr) && (hLarge || hSmall)) {
            // 取回大图标优先；两个句柄都必须销毁，避免“只取小图标”时句柄泄漏
            QPixmap pm = QtWin::fromHICON(hLarge ? hLarge : hSmall);
            if (hLarge) DestroyIcon(hLarge);
            if (hSmall) DestroyIcon(hSmall);
            if (!pm.isNull()) result = finalize(pm);
        }
    }

    // —— 路径3（兜底）：用本进程系统镜像列表索引，按 effSize 选最接近的 SHIL 层级。
    // 对快捷方式，取到目标图标后再手动叠加链接箭头。
    if (result.isNull() && !parsename.isEmpty()) {
        CrashTrace::markPath("loadIcon:p3-imageListIndex", parsename);
        LPITEMIDLIST pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(parsename.toStdWString().c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
            SHFILEINFOW fi = {0};
            if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &fi, sizeof(fi),
                               SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_LARGEICON)) {
                const QPixmap pm = iconFromSystemImageListPixmap(fi.iIcon, effSize);
                if (!pm.isNull()) result = finalize(pm);
            }
            CoTaskMemFree(pidl);
        }
    }

    // —— 路径4（兜底）：旧的跨进程系统镜像列表索引，仅作为“有图比没图强”末位选择。
    if (result.isNull() && item.systemImageIndex >= 0) {
        const QPixmap pm = iconFromSystemImageListPixmap(item.systemImageIndex, effSize);
        if (!pm.isNull()) result = finalize(pm);
    }

    // —— 路径5（终极兜底）：QFileIconProvider（不依赖 COM，分辨率低）。
    CrashTrace::markPath("loadIcon:p5-iconProvider", parsename);
    if (result.isNull()) {
        QFileIconProvider provider;
        QIcon prov;
        if (item.isShortcut && !item.targetPath.isEmpty())
            prov = provider.icon(QFileInfo(item.targetPath));
        if (prov.isNull())
            prov = provider.icon(QFileInfo(item.sourcePath));
        // QFileIconProvider 常只给 16/32/48px，同样要规整（否则大图标档又会“格子大、图标小”）。
        if (!prov.isNull()) result = finalize(prov.pixmap(QSize(effSize, effSize)));
    }

    if (result.isNull()) result = QIcon();
    CrashTrace::mark("loadIcon:done");
    cache.insert(key, result);
    if (needUninit) CoUninitialize();
    return result;
}

// 统一的「快捷方式真实目标」解析入口（详见 desktopscanner.h 的注释）。
// 解析结果按路径缓存：scan/refresh 会对同一批 .lnk 反复建项，每次走一遍 IShellLink 是纯浪费。
// 作者：谭征
QString DesktopScanner::effectiveTarget(const QString& sourcePath, bool isShortcut) {
    if (!isShortcut || sourcePath.isEmpty()) return sourcePath;
    static QHash<QString, QString> cache;
    // 键含修改时间：.lnk 被改过目标后必须重解析（与 isRealShortcut 的缓存策略一致）。
    const QFileInfo fi(sourcePath);
    const QString key = sourcePath + QLatin1Char('|') + QString::number(fi.lastModified().toMSecsSinceEpoch());
    auto it = cache.find(key);
    if (it != cache.end()) return it.value();
    // resolveShortcut 假定调用方已初始化 COM（scan 内部只在整轮扫描时开一次）。本函数会被
    // 窗口侧建项/改名同步直接调用，那些路径没有 COM 初始化 ⇒ 这里自己保证一次，否则
    // CoCreateInstance 失败会让 targetPath 静默退回 .lnk、图标又只剩 48px 兜底。
    const HRESULT cohr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = (cohr == S_OK);
    const QString t = resolveShortcut(sourcePath);
    if (needUninit) CoUninitialize();
    const QString out = t.isEmpty() ? sourcePath : t;
    cache.insert(key, out);
    return out;
}

// 仅应在主线程调用（COM/Shell 操作）。取图失败返回空图标。
// 作者：谭征
QIcon DesktopScanner::recycleBinIcon(int sizeHint) {
    const int effSize = sizeHint > 0 ? sizeHint : 48;
#ifdef Q_OS_WIN
    // 首选：经回收站 PIDL 取系统镜像列表索引，再按 effSize 选对应 SHIL 层级位图。
    // 该索引是稳定的，但 shell 会在清空/放入文件时调用 SHUpdateRecycleBinIcon 就地刷新
    // 索引处的位图（满/空两套）—— 这正是 Windows 桌面显示满/空图标所用的同一条路径，
    // 因此取到的一定是“当前状态”的图标，且与桌面同源、尺寸清晰（48px→SHIL_EXTRALARGE）。
    LPITEMIDLIST pidl = nullptr;
    if (SUCCEEDED(SHGetSpecialFolderLocation(nullptr, CSIDL_BITBUCKET, &pidl)) && pidl) {
        SHFILEINFOW fi = {0};
        if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &fi, sizeof(fi),
                           SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_LARGEICON)) {
            QPixmap pm = iconFromSystemImageListPixmap(fi.iIcon, effSize);
            CoTaskMemFree(pidl);
            if (!pm.isNull()) return QIcon(pm);
        } else {
            CoTaskMemFree(pidl);
        }
    }
    // 兜底：IExtractIcon 按解析名取（同样态敏感，走回收站 shell 文件夹的动态图标提取）。
    const QString rbn = QStringLiteral("::{645FF040-5081-101B-9F08-00AA002F954E}");
    QPixmap pm = extractIconByParsingName(rbn, effSize);
    if (!pm.isNull()) return QIcon(pm);
#endif
    Q_UNUSED(effSize)
    return QIcon();
}

// 给项目重新分类
// 作者：谭征
QString DesktopScanner::classify(const DesktopItem& item) {
    QFileInfo info(item.launchPath());
    const QString ext = info.suffix().toLower();
    const QString name = item.displayName.toLower();

    static const QRegularExpression reOffice(QStringLiteral("^(docx?|xlsx?|pptx?|pdf|txt|md|csv)$"));
    static const QRegularExpression reMedia(QStringLiteral("^(mp3|mp4|avi|mkv|flac|aac|wav|jpg|jpeg|png|gif|bmp|webp|svg)$"));
    static const QRegularExpression reDev(QStringLiteral("^(cpp|c|h|hpp|py|js|ts|java|go|rs|html|css|json|xml|sql|cmake|pro|sln)$"));

    if (item.isExecutable || ext == QLatin1String("exe") || ext == QLatin1String("msi")) {
        return QStringLiteral("已安装软件");
    }
    if (reOffice.match(ext).hasMatch()) return QStringLiteral("办公文档");
    if (reMedia.match(ext).hasMatch()) return QStringLiteral("图片影音");
    if (reDev.match(ext).hasMatch()) return QStringLiteral("开发工具");
    if (item.isShortcut) return QStringLiteral("快捷方式");
    if (info.isDir()) return QStringLiteral("文件夹");
    return QStringLiteral("其它");
}

// 默认整理规则，与设置中心“整理规则”表默认内容一致。
// 作者：谭征
QVector<OrganizeRule> DesktopScanner::defaultRules() {
    static const QVector<OrganizeRule> rules = {
        { QStringLiteral("目录"),    {},                                                                                                                                  QStringLiteral("目录"),    true },
        { QStringLiteral("文档"),    { QStringLiteral("doc"), QStringLiteral("docx"), QStringLiteral("dot"), QStringLiteral("dotm"), QStringLiteral("dotx"),
                                      QStringLiteral("xls"), QStringLiteral("xlsx"), QStringLiteral("xlsm"), QStringLiteral("xlsb"),
                                      QStringLiteral("ppt"), QStringLiteral("pptx"), QStringLiteral("pps"), QStringLiteral("ppsx"),
                                      QStringLiteral("pdf"), QStringLiteral("txt"), QStringLiteral("md"), QStringLiteral("csv"), QStringLiteral("rtf"),
                                      QStringLiteral("wps"), QStringLiteral("et"), QStringLiteral("dps"),
                                      QStringLiteral("odt"), QStringLiteral("ods"), QStringLiteral("odp"),
                                      QStringLiteral("pages"), QStringLiteral("numbers"), QStringLiteral("key") },                              QStringLiteral("文档"),    true },
        { QStringLiteral("压缩"),    { QStringLiteral("001"), QStringLiteral("7z"), QStringLiteral("zip"), QStringLiteral("rar"), QStringLiteral("tar"),
                                      QStringLiteral("gz"), QStringLiteral("bz2"), QStringLiteral("xz"), QStringLiteral("lz"), QStringLiteral("lha"),
                                      QStringLiteral("lzh"), QStringLiteral("arj"), QStringLiteral("cab"), QStringLiteral("iso"), QStringLiteral("dmg"),
                                      QStringLiteral("apk"), QStringLiteral("apm"), QStringLiteral("a"), QStringLiteral("ar"), QStringLiteral("z"),
                                      QStringLiteral("zoo"), QStringLiteral("ace"), QStringLiteral("uha"), QStringLiteral("uhx") },                   QStringLiteral("压缩"),    true },
        { QStringLiteral("图片"),    { QStringLiteral("bmp"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"), QStringLiteral("gif"),
                                      QStringLiteral("tiff"), QStringLiteral("tif"), QStringLiteral("webp"), QStringLiteral("svg"), QStringLiteral("ico"),
                                      QStringLiteral("raw"), QStringLiteral("cr2"), QStringLiteral("nef"), QStringLiteral("dng"), QStringLiteral("heic"),
                                      QStringLiteral("heif"), QStringLiteral("psd") },                                                                  QStringLiteral("图片"),    true },
        { QStringLiteral("快捷方式"), { QStringLiteral("lnk") },                                                                                          QStringLiteral("快捷方式"), true },
        { QStringLiteral("网址"),    { QStringLiteral("url") },                                                                                          QStringLiteral("网址"),    false },
        { QStringLiteral("视频"),    { QStringLiteral("mp4"), QStringLiteral("3gp"), QStringLiteral("avi"), QStringLiteral("wmv"), QStringLiteral("mkv"),
                                      QStringLiteral("mov"), QStringLiteral("flv"), QStringLiteral("f4v"), QStringLiteral("rm"), QStringLiteral("rmvb"),
                                      QStringLiteral("mpg"), QStringLiteral("mpeg"), QStringLiteral("m2v"), QStringLiteral("ts"), QStringLiteral("mts"),
                                      QStringLiteral("m2ts"), QStringLiteral("vob"), QStringLiteral("webm") },                                         QStringLiteral("视频"),    false },
        { QStringLiteral("音频"),    { QStringLiteral("mp3"), QStringLiteral("mp2"), QStringLiteral("mp1"), QStringLiteral("wav"), QStringLiteral("aiff"),
                                      QStringLiteral("aif"), QStringLiteral("flac"), QStringLiteral("ape"), QStringLiteral("m4a"), QStringLiteral("ogg"),
                                      QStringLiteral("wma"), QStringLiteral("mid"), QStringLiteral("midi"), QStringLiteral("aac"), QStringLiteral("ac3") }, QStringLiteral("音频"),    false },
        { QStringLiteral("其它"),    {},                                                                                                                                  QStringLiteral("其它"),    true }
    };
    return rules;
}

// 从 SettingsManager 读取用户自定义的整理规则（目标分区、启用状态）。
// 作者：谭征
QVector<OrganizeRule> DesktopScanner::loadRules(SettingsManager& sm) {
    QVector<OrganizeRule> rules = defaultRules();

    static const QStringList partitions = {
        QStringLiteral("目录"), QStringLiteral("文档"), QStringLiteral("压缩"),
        QStringLiteral("图片"), QStringLiteral("快捷方式"), QStringLiteral("网址"),
        QStringLiteral("视频"), QStringLiteral("音频"), QStringLiteral("其它")
    };

    const QVariantList partitionsIdx = sm.loadValue(QStringLiteral("Organize/rulePartitions"), QVariantList()).toList();
    const QVariantList enabled = sm.loadValue(QStringLiteral("Organize/ruleEnabled"), QVariantList()).toList();

    for (int i = 0; i < rules.size(); ++i) {
        if (i < partitionsIdx.size()) {
            const int idx = qBound(0, partitionsIdx.at(i).toInt(), partitions.size() - 1);
            rules[i].targetCategory = partitions.at(idx);
        }
        if (i < enabled.size()) {
            rules[i].enabled = enabled.at(i).toInt() != 0;
        }
    }
    return rules;
}

// 使用整理规则对项目进行分类（设置中心“桌面整理”规则）。
// 作者：谭征
QString DesktopScanner::classifyWithRules(const DesktopItem& item, const QVector<OrganizeRule>& rules) {
    if (rules.isEmpty()) return classify(item);

    const QFileInfo sourceInfo(item.sourcePath);
    const QString sourceExt = sourceInfo.suffix().toLower();
    const bool isDir = sourceInfo.isDir();
    const bool isShortcut = item.isShortcut || sourceExt == QLatin1String("lnk");
    const bool isUrl = sourceExt == QLatin1String("url");

    // 第一轮：特殊类型（目录、快捷方式、网址），按 typeName 匹配，不使用扩展名列表。
    for (const auto& rule : rules) {
        if (!rule.enabled) continue;
        if (rule.typeName == QStringLiteral("目录") && isDir) return rule.targetCategory;
        if (rule.typeName == QStringLiteral("快捷方式") && isShortcut) return rule.targetCategory;
        if (rule.typeName == QStringLiteral("网址") && isUrl) return rule.targetCategory;
    }

    // 第二轮：普通扩展名规则。对快捷方式使用目标扩展名，其余使用自身扩展名。
    QString ext;
    if (isShortcut && !item.targetPath.isEmpty()) {
        ext = QFileInfo(item.targetPath).suffix().toLower();
    }
    if (ext.isEmpty()) {
        ext = sourceExt;
    }

    for (const auto& rule : rules) {
        if (!rule.enabled) continue;
        if (rule.typeName == QStringLiteral("目录") || rule.typeName == QStringLiteral("快捷方式") ||
            rule.typeName == QStringLiteral("网址") || rule.typeName == QStringLiteral("其它")) {
            continue;
        }
        if (rule.extensions.contains(ext)) return rule.targetCategory;
    }

    // 最后启用“其它”兜底
    for (const auto& rule : rules) {
        if (rule.enabled && rule.typeName == QStringLiteral("其它")) return rule.targetCategory;
    }

    return QStringLiteral("其它");
}

// fixedCategory 非空时，所有项目归入该固定分区；否则使用 rules 进行规则匹配。
// 作者：谭征
QVector<DesktopItem> DesktopScanner::scanWithRules(const QVector<OrganizeRule>& rules,
                                                  const QString& fixedCategory) const {
    QVector<DesktopItem> items = scan();
    if (!fixedCategory.isEmpty()) {
        for (auto& item : items) item.category = fixedCategory;
    } else if (!rules.isEmpty()) {
        for (auto& item : items) item.category = classifyWithRules(item, rules);
    }
    return items;
}

// 作为 dock 未就绪时的回退；dock 就绪时“系统”分类改由 dock 实际启用的系统图标决定。
// 作者：谭征
QVector<DesktopItem> DesktopScanner::scanSpecialItems() {
    // 桌面特殊命名空间项；整屏隐藏原生桌面后，这些图标会随桌面一起消失，
    // 必须在程序窗口内单独枚举并提供访问。launchCommand 用 explorer 打开对应命名空间。
    struct Spec { const wchar_t* clsid; const wchar_t* name; };
    static const Spec specs[] = {
        { L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", L"我的电脑" },
        { L"::{645FF040-5081-101B-9F08-00AA002F954E}", L"回收站" },
        { L"::{208D2C60-3AEA-1069-A2D7-08002B30309D}", L"网络" },
        { L"::{26EE0668-A00A-44D7-9371-BEB064C98683}", L"控制面板" },
        { L"::{59031A47-3F72-44A7-89C5-5595FE6B30EE}", L"个人文件夹" },
    };
    QVector<DesktopItem> items;
    for (const auto& s : specs) {
        DesktopItem item;
        item.isSpecial = true;
        item.displayName = QString::fromWCharArray(s.name);
        item.sourcePath = QString::fromWCharArray(s.clsid);   // 形如 "::{CLSID}"，作为唯一标识
        item.launchCommand = QStringLiteral("explorer.exe ") + item.sourcePath;
        item.category = QStringLiteral("系统");
        items.append(item);
    }
    return items;
}

// 解析快捷方式
// 作者：谭征
QString DesktopScanner::resolveShortcut(const QString& linkPath) {
#ifdef Q_OS_WIN
    // 卡死治理·核心（2026-09-24）：本函数此前**完全没有缓存** —— 而 scan() 会对桌面上
    // 【每一个】.lnk 调用它一次，每次都要 CoCreateInstance(CLSID_ShellLink)
    // + IPersistFile::Load(读盘) + IShellLink::GetPath(求解目标路径，可能再触达目标所在卷)。
    // 桌面 .lnk 有几十个 ⇒ 每次 refresh()（**GUI 线程**）就有几十次 COM 往返；机械盘 + 360
    // 抢盘时单次可达数十~数百 ms，几十项叠加正是「鼠标卡死 2~3 秒」的量级，而常驻的
    // WH_KEYBOARD_LL 会把这段阻塞**同步放大**到全系统输入链（GUI 线程不空闲，键鼠一起等）。
    // 解：按「路径 | 文件修改时间」缓存。.lnk 未被改写 ⇒ 其目标恒不变 ⇒ 结果与实时解析**完全等价**；
    // mtime 一变（改名 / 改指向 / 重建快捷方式）缓存键即失效，自动重解析。
    // 键含 mtime 的口径与同文件 isRealShortcut() 完全一致（那里早就是这么做的）。
    // 仅主线程（scan → GUI 线程）访问，static 无需加锁。
    static QHash<QString, QString> s_resolveCache;
    const QString cacheKey = linkPath + QLatin1Char('|')
                           + QString::number(QFileInfo(linkPath).lastModified().toMSecsSinceEpoch());
    auto cached = s_resolveCache.constFind(cacheKey);
    if (cached != s_resolveCache.constEnd()) return *cached;

    // 调用方（scan）负责 COM 的初始化/反初始化（每次 scan 仅一次），此处不再逐文件开关 COM。
    IShellLinkW* shellLink = nullptr;
    IPersistFile* persistFile = nullptr;
    QString target;

    do {
        HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IShellLinkW, reinterpret_cast<void**>(&shellLink));
        if (FAILED(hr)) break;

        hr = shellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persistFile));
        if (FAILED(hr)) break;

        hr = persistFile->Load(linkPath.toStdWString().c_str(), STGM_READ);
        if (FAILED(hr)) break;

        wchar_t path[MAX_PATH] = {0};
        WIN32_FIND_DATAW findData;
        hr = shellLink->GetPath(path, MAX_PATH, &findData, SLGP_UNCPRIORITY);
        if (SUCCEEDED(hr) && path[0] != L'\0') {
            target = QString::fromWCharArray(path);
        }
    } while (false);

    if (persistFile) persistFile->Release();
    if (shellLink) shellLink->Release();

    // 统一收尾：把「解析成功的目标 / 解析失败」一并写入缓存 —— 失败也要缓存（存 linkPath，
    // 正是本函数的失败返回值），否则每次 refresh 都会重试同一次注定失败的 COM 解析。
    const QString resolved = target.isEmpty() ? linkPath : target;
    if (s_resolveCache.size() > 512) s_resolveCache.clear();
    s_resolveCache.insert(cacheKey, resolved);
    return resolved;
#endif
    return linkPath;
}

// 重命名“改后缀”引发的类型错配防护
// ----------------------------------------------------------------------------
// 用户把文件重命名成另一个后缀后，文件内容与新后缀就不再匹配：Shell 会按**新后缀**
// 去调用对应的图标/缩略图处理器（图片编解码器、视频缩略图、压缩包预览、PE 图标资源…），
// 而这些处理器多为第三方 in-proc Shell 扩展。它们解析一个内容根本不匹配的文件时若崩溃，
// 会直接带走本进程 —— 这是“只有改后缀才崩”的最合理机制。
// 下面两个函数分别处理两种错配：
// 1) 后缀是 .lnk，内容却不是快捷方式  → 按普通文件处理，绝不交给 IShellLink/链接处理器；
// 2) 后缀是图片/媒体/压缩等“需要解析内容”的类型，内容魔数明显不符 → 改用不读内容的通用图标。

namespace {
// 读文件头 n 字节（失败返回 false，且 out 已被清零）
bool readFileHead(const QString& path, unsigned char* out, int n) {
    if (!out || n <= 0) return false;
    memset(out, 0, size_t(n));
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(n);
    if (head.isEmpty()) return false;
    memcpy(out, head.constData(), size_t(qMin(n, head.size())));
    return true;
}

bool headMatches(const unsigned char* head, int headLen, int offset, const QByteArray& magic) {
    if (!head || offset < 0 || magic.isEmpty()) return false;
    if (offset + magic.size() > headLen) return false;
    return memcmp(head + offset, magic.constData(), size_t(magic.size())) == 0;
}
} // namespace

// 返回 true 表示“后缀与内容不符”，图标提取应改用不读取文件内容的通用后缀图标。
// 作者：谭征
bool DesktopScanner::suffixMismatchesContent(const QString& filePath, const QString& suffix) {
    const QString ext = suffix.toLower();
    if (ext.isEmpty() || filePath.isEmpty()) return false;

    // 只为“Shell 会据后缀去解析文件内容”的类型做嗅探；其余类型保持原有行为，不冒险改动图标观感。
    // 值 = 允许的魔数列表（偏移, hex 魔数）；任一命中即视为“内容与后缀相符”。
    static const QHash<QString, QVector<QPair<int, QByteArray>>> table = []() {
        QHash<QString, QVector<QPair<int, QByteArray>>> m;
        auto add = [&m](const char* e, int off, const char* hex) {
            m[QString::fromLatin1(e)].append(qMakePair(off, QByteArray::fromHex(hex)));
        };
        // 图片（第三方编解码器最多，改后缀最容易在这里踩到崩溃）
        add("jpg", 0, "ffd8ff");   add("jpeg", 0, "ffd8ff");  add("jpe", 0, "ffd8ff");
        add("png", 0, "89504e47");
        add("gif", 0, "47494638");
        add("bmp", 0, "424d");
        add("webp", 0, "52494646");  add("webp", 8, "57454250");
        add("tif", 0, "49492a00");   add("tif", 0, "4d4d002a");
        add("tiff", 0, "49492a00");  add("tiff", 0, "4d4d002a");
        add("ico", 0, "00000100");
        add("psd", 0, "38425053");
        add("heic", 4, "66747970");  add("heif", 4, "66747970");
        const QStringList vExts = { QStringLiteral("mp4"), QStringLiteral("m4v"),
                                    QStringLiteral("mov"), QStringLiteral("3gp") };
        for (const QString& e : vExts)
            m[e].append(qMakePair(4, QByteArray::fromHex("66747970")));
        add("avi", 0, "52494646");
        add("mkv", 0, "1a45dfa3");   add("webm", 0, "1a45dfa3");
        add("wmv", 0, "3026b275");   add("asf", 0, "3026b275");
        add("flv", 0, "464c5601");
        add("mpg", 0, "000001ba");   add("mpeg", 0, "000001ba");  add("mpg", 0, "000001b3");
        // 音频
        add("mp3", 0, "494433");     add("mp3", 0, "fffb");  add("mp3", 0, "fff3");  add("mp3", 0, "fff2");
        add("wav", 0, "52494646");
        add("flac", 0, "664c6143");
        add("ogg", 0, "4f676753");   add("oga", 0, "4f676753");
        add("m4a", 4, "66747970");   add("aac", 0, "fff1");  add("aac", 0, "fff9");
        add("wma", 0, "3026b275");
        // 压缩/包（docx/xlsx/pptx 同为 zip）
        const QStringList zipExts = { QStringLiteral("zip"), QStringLiteral("docx"),
                                      QStringLiteral("xlsx"), QStringLiteral("pptx"),
                                      QStringLiteral("apk"), QStringLiteral("jar"),
                                      QStringLiteral("epub"), QStringLiteral("odt") };
        for (const QString& e : zipExts) {
            m[e].append(qMakePair(0, QByteArray::fromHex("504b0304")));
            m[e].append(qMakePair(0, QByteArray::fromHex("504b0506")));
            m[e].append(qMakePair(0, QByteArray::fromHex("504b0708")));
        }
        add("rar", 0, "52617221");
        add("7z", 0, "377abcaf271c");
        add("gz", 0, "1f8b");        add("tgz", 0, "1f8b");
        add("xz", 0, "fd377a585a00");
        add("bz2", 0, "425a68");
        add("cab", 0, "4d534346");
        // 文档 / 可执行
        add("pdf", 0, "25504446");
        add("rtf", 0, "7b5c727466");
        const QStringList peExts = { QStringLiteral("exe"), QStringLiteral("dll"),
                                     QStringLiteral("sys"), QStringLiteral("com"),
                                     QStringLiteral("scr"), QStringLiteral("ocx") };
        for (const QString& e : peExts)
            m[e].append(qMakePair(0, QByteArray::fromHex("4d5a")));
        add("lnk", 0, "4c000000");
        return m;
    }();

    auto it = table.find(ext);
    if (it == table.end()) return false;          // 该后缀不参与嗅探：保持原有取图行为

    unsigned char head[16] = {0};
    if (!readFileHead(filePath, head, 16)) return false;   // 读不到（无权限/被占用）：不做判断
    for (const auto& rule : it.value()) {
        if (headMatches(head, 16, rule.first, rule.second)) return false;   // 命中任一魔数即相符
    }
    return true;   // 后缀在表内但所有魔数都不匹配 → 内容与后缀明显不符
}

// 结果按“路径 + 修改时间”缓存，改后缀/文件被改动后自动失效。
// 作者：谭征
bool DesktopScanner::isRealShortcut(const QString& linkPath, QString* targetOut) {
    if (targetOut) targetOut->clear();
    if (linkPath.isEmpty()) return false;
    if (!linkPath.endsWith(QLatin1String(".lnk"), Qt::CaseInsensitive)) return false;

    const QFileInfo fi(linkPath);
    // 缓存键含修改时间：改名/改写文件后自动失效（避免改后缀后仍沿用旧的“是快捷方式”结论）。
    static QHash<QString, bool> cache;
    static QHash<QString, QString> targetCache;
    const QString key = linkPath + QLatin1Char('|')
                      + QString::number(fi.lastModified().toMSecsSinceEpoch());
    auto it = cache.find(key);
    if (it != cache.end()) {
        if (targetOut && *it) *targetOut = targetCache.value(key);
        return *it;
    }

    const HRESULT cohr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = (cohr == S_OK);
    const QString target = resolveShortcut(linkPath);
    if (needUninit) CoUninitialize();

    // resolveShortcut 解析失败时返回入参自身 → 目标与自身同路径即“不是真实快捷方式”
    const bool real = !target.isEmpty()
        && QFileInfo(target).absoluteFilePath().compare(fi.absoluteFilePath(),
                                                        Qt::CaseInsensitive) != 0;
    if (cache.size() > 512) { cache.clear(); targetCache.clear(); }
    cache.insert(key, real);
    if (real) targetCache.insert(key, target);
    if (targetOut && real) *targetOut = target;
    return real;
}
