/*
 * @file shellops.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "shellops.h"
#include "diagtrace.h"   // 临时：右键菜单决议诊断日志

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <string>

#ifdef Q_OS_WIN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#endif

namespace {

#ifdef Q_OS_WIN

// 绝对路径 → 父文件夹 IShellFolder + 单级子 pidl。
// 为什么不用“桌面文件夹 + ILFindLastID”：那要求目标必须是桌面命名空间的直接子项；
// 而本机的桌面被重定向到 F:\Desktop，桌面命名空间只是它的一个视图，走“真实父目录”
// 对任意位置都成立，也顺带修掉“收纳盒里重命名 F:\Desktop 下的文件”这类场景。
bool parseParentAndChild(const QString& absPath, IShellFolder** parentOut, LPITEMIDLIST* childOut)
{
    if (!parentOut || !childOut) return false;
    LPITEMIDLIST pidlAbs = nullptr;
    SFGAOF attr = 0;
    if (FAILED(SHParseDisplayName(QDir::toNativeSeparators(absPath).toStdWString().c_str(),
                                  nullptr, &pidlAbs, 0, &attr)) || !pidlAbs) {
        return false;
    }
    IShellFolder* parent = nullptr;
    LPCITEMIDLIST child = nullptr;
    const HRESULT hr = SHBindToParent(pidlAbs, IID_IShellFolder,
                                      reinterpret_cast<void**>(&parent), &child);
    if (FAILED(hr) || !parent || !child) {
        CoTaskMemFree(pidlAbs);
        if (parent) parent->Release();
        return false;
    }
    // child 指向 pidlAbs 内部，必须深拷贝后再释放 pidlAbs。
    LPITEMIDLIST rel = ILClone(child);
    CoTaskMemFree(pidlAbs);
    if (!rel) { parent->Release(); return false; }
    *parentOut = parent;
    *childOut = rel;
    return true;
}

// 兜底策略：桌面文件夹 + 末级 pidl。覆盖“路径不在真实文件系统里、只能经桌面命名空间定位”
// 的历史情况（与 Dock 早期实现一致），保证行为不比改动前更弱。
bool parseViaDesktopFolder(const QString& absPath, IShellFolder** parentOut, LPITEMIDLIST* childOut)
{
    if (!parentOut || !childOut) return false;
    IShellFolder* desk = nullptr;
    if (FAILED(SHGetDesktopFolder(&desk)) || !desk) return false;
    LPITEMIDLIST pidlAbs = nullptr;
    SFGAOF attr = 0;
    if (FAILED(SHParseDisplayName(QDir::toNativeSeparators(absPath).toStdWString().c_str(),
                                  nullptr, &pidlAbs, 0, &attr)) || !pidlAbs) {
        desk->Release();
        return false;
    }
    LPITEMIDLIST rel = ILClone(ILFindLastID(pidlAbs));
    CoTaskMemFree(pidlAbs);
    if (!rel) { desk->Release(); return false; }
    *parentOut = desk;          // 调用方负责 Release
    *childOut = rel;
    return true;
}

// 由 shellPath 解析 pidl；失败则退而按显示名相对桌面文件夹解析。
// 兜底很有必要：新建项的 shellPath 存在竞态窗口期（尚未落盘/尚未解析完），
// 此时若直接放弃，会表现成“右键菜单弹不出来 / 重命名点了没反应”。
LPITEMIDLIST resolvePidl(IShellFolder* desk, const QString& shellPath, const QString& displayName)
{
    LPITEMIDLIST pidl = nullptr;
    if (!shellPath.isEmpty()) {
        const std::wstring w = shellPath.toStdWString();
        if (SUCCEEDED(SHParseDisplayName(w.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
            return pidl;
        }
    }
    if (desk && !displayName.isEmpty()) {
        ULONG eaten = 0;
        // 必须用非 const 的 wstring：Shell 的 ParseDisplayName 允许原地改写传入字符串，
        // 传 const 缓冲区的指针（哪怕 const_cast 过）属于未定义行为。
        std::wstring w = displayName.toStdWString();
        if (SUCCEEDED(desk->ParseDisplayName(nullptr, nullptr, w.data(),
                                             &eaten, &pidl, nullptr)) && pidl) {
            return pidl;
        }
    }
    return nullptr;
}

// 弹菜单前把 owner 所属的**本程序顶层窗口**置为前台 —— Win32 上下文菜单的必备前置动作。
// SetForegroundWindow 对子窗口无效，必须取顶层；权限侧由调用方的 AllowSetForegroundWindow 授权。
// 缺了这一步，带 WS_EX_NOACTIVATE 的窗口（收纳盒/网格/Dock）弹菜单时进程不在前台，
// **第一次点击会被前台激活锁吃掉** —— 表现是“右键菜单里点重命名没反应 / 要点两次”。
// 必须用 GA_ROOT（顶层祖先 = 本程序自己的窗口），**绝不能用 GA_ROOTOWNER**。
// 本程序为免疫 Win+D 把三个挂件窗口的 owner（GWLP_HWNDPARENT）设成了桌面图标层
// SHELLDLL_DefView（见 desktopimmunityfilter.cpp / attachToDesktop）。而 GA_ROOTOWNER
// 的语义是“沿父窗口与 **owner** 链走到根，再返回根窗口的 owner”，于是它返回的是
// **Explorer 的 SHELLDLL_DefView**：每次右键都把前台让给资源管理器，后果是连锁的 ——
// · 本进程随即不再是前台进程 → 随后 SetForegroundWindow(编辑框)/SetFocus(编辑框)
// 全部失效 → 重命名框“出来了但打不进字、回车也没用”（F2 与右键重命名一起失效）；
// · 还会干扰 Dock 的“显示桌面 / 前台是否在桌面”状态机（isForegroundDesktop 轮询）。
void activateOwnerForMenu(HWND owner)
{
    if (!owner) return;
    HWND top = ::GetAncestor(owner, GA_ROOT);   // 本程序顶层窗口（不是它的 owner）
    if (!top) top = owner;
    ::SetForegroundWindow(top);
}

#endif // Q_OS_WIN

}   // namespace

// 该路径是否“可以被重命名”。系统命名空间项（::{CLSID}）没有真实文件名，一律 false。
// 作者：谭征
bool ShellOps::isSafeToRename(const QString& absPath)
{
    if (absPath.isEmpty()) return false;
    if (absPath.startsWith(QLatin1String("::"))) return false;   // 系统命名空间项：没有文件名可改
    return true;
}

// （避免把无法确定的状态误显示成“满”）。仅 Windows；非 Windows 平台恒返回 true。
// 作者：谭征
bool ShellOps::recycleBinIsEmpty()
{
#ifdef Q_OS_WIN
    SHQUERYRBINFO rb = {0};
    rb.cbSize = sizeof(rb);
    if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &rb)))
        return rb.i64NumItems == 0;
#endif
    return true;   // 查询失败 / 非 Windows：按空处理
}

// 后两条是灾难级兜底：把整个用户目录或整个桌面拖进回收站是不可逆的。
// 作者：谭征
bool ShellOps::isSafeToDelete(const QString& absPath)
{
    if (absPath.isEmpty()) return false;
    if (absPath.startsWith(QLatin1String("::"))) return false;   // ::{CLSID} 系统项

    const QFileInfo fi(absPath);
    if (!fi.exists() && !fi.isSymLink()) return false;
    if (fi.fileName().isEmpty()) return false;                   // 盘根等非法目标

    // 个人文件夹兜底：正常它会被解析成 ::{59031a47-...} 并被上面拦下；若某条路径把它解析成了
    // 真实目录（历史缺陷），这里再按“是否等于当前用户主目录”拦一次 —— 把整个用户目录拖进
    // 回收站是不可逆的灾难，必须双保险。
    const QString abs  = QDir::fromNativeSeparators(fi.absoluteFilePath()).toLower();
    const QString home = QDir::fromNativeSeparators(QDir::homePath()).toLower();
    if (!home.isEmpty() && (abs == home || home.startsWith(abs + QLatin1Char('/')))) return false;
    // 桌面目录本身同理：删掉等于清空整个桌面。
    const QString desk = QDir::fromNativeSeparators(
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)).toLower();
    if (!desk.isEmpty() && abs == desk) return false;
    return true;
}

// ownerHwnd 为可能的系统 UI 归属窗口（可为 nullptr）。失败原因写入 errorOut（可为 nullptr）。
// 作者：谭征
bool ShellOps::renameInPlace(const QString& absPath, const QString& newName,
                             void* ownerHwnd, QString* errorOut)
{
    const QString trimmed = newName.trimmed();
    if (!isSafeToRename(absPath) || trimmed.isEmpty()) return false;
    // 名字逐字未变 → 视作成功，不做无谓的 Shell 调用。
    // 注意必须逐字比较：Windows 下大小写改名（foo.txt → Foo.txt）是合法且用户真的要做的操作。
    if (QFileInfo(absPath).fileName() == trimmed) return true;

#ifdef Q_OS_WIN
    const HRESULT cohr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninit = (cohr == S_OK);
    const HWND owner = reinterpret_cast<HWND>(ownerHwnd);

    bool ok = false;
    HRESULT hr = E_FAIL;
    // 先走“真实父目录”，失败再退到“桌面文件夹末级 pidl”。两条都试过仍失败才算失败。
    for (int strategy = 0; strategy < 2 && !ok; ++strategy) {
        IShellFolder* parent = nullptr;
        LPITEMIDLIST child = nullptr;
        const bool parsed = (strategy == 0) ? parseParentAndChild(absPath, &parent, &child)
                                            : parseViaDesktopFolder(absPath, &parent, &child);
        if (!parsed) continue;
        hr = parent->SetNameOf(owner, child, trimmed.toStdWString().c_str(),
                               SHGDN_FOREDITING, nullptr);
        ok = SUCCEEDED(hr);
        CoTaskMemFree(child);
        parent->Release();
    }
    if (!ok && errorOut) {
        *errorOut = QStringLiteral("无法重命名为“%1”，文件名可能已存在或包含非法字符。").arg(trimmed);
    }
    if (uninit) CoUninitialize();
    return ok;
#else
    Q_UNUSED(ownerHwnd)
    if (errorOut) *errorOut = QStringLiteral("当前平台不支持重命名。");
    return false;
#endif
}

// 返回 false 表示一项都没删成（全部判定为受保护项 / 失败 / 用户在系统确认框里取消）。
// 作者：谭征
bool ShellOps::deleteToRecycleBin(const QStringList& absPaths, void* ownerHwnd)
{
    // 去重 + 受保护项过滤，全部集中在这里，调用方不必各自实现一遍。
    QStringList nativePaths;
    nativePaths.reserve(absPaths.size());
    QSet<QString> seen;
    for (const QString& p : absPaths) {
        if (!isSafeToDelete(p)) continue;
        const QString abs = QFileInfo(p).absoluteFilePath();
        const QString key = QDir::fromNativeSeparators(abs).toLower();
        if (key.isEmpty() || seen.contains(key)) continue;
        seen.insert(key);
        nativePaths << QDir::toNativeSeparators(abs);
    }
    if (nativePaths.isEmpty()) return false;

#ifdef Q_OS_WIN
    // SHFileOperation 要求 pFrom 是“双 null 结尾”的字符串列表。
    QString joined = nativePaths.join(QChar(0));
    joined.append(QChar(0));
    joined.append(QChar(0));
    const std::wstring wfrom = joined.toStdWString();

    SHFILEOPSTRUCTW op;
    ZeroMemory(&op, sizeof(op));
    op.hwnd   = reinterpret_cast<HWND>(ownerHwnd);
    op.wFunc  = FO_DELETE;
    op.pFrom  = wfrom.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_WANTNUKEWARNING;

    const HRESULT cohr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninit = (cohr == S_OK);
    const int rc = SHFileOperationW(&op);
    const bool aborted = (op.fAnyOperationsAborted != FALSE);
    if (uninit) CoUninitialize();

    // rc != 0 或用户取消 → 一个都不算删成，调用方不得修改任何本地状态。
    return (rc == 0 && !aborted);
#else
    Q_UNUSED(ownerHwnd)
    return false;
#endif
}

ShellOps::MenuAction ShellOps::showNativeContextMenu(void* ownerHwnd,
                                                     const QString& shellPath,
                                                     const QString& displayName,
                                                     int screenX, int screenY,
                                                     bool interceptDelete)
{
#ifdef Q_OS_WIN
    const HWND owner = reinterpret_cast<HWND>(ownerHwnd);
    const HRESULT cohr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninit = (cohr == S_OK);

    MenuAction result = MenuAction::None;

    // 定位目标项：优先「真实父目录 + 子项」，失败再退「桌面文件夹 + 末级 pidl」。
    // 为什么不能用桌面文件夹打头：本机桌面被重定向到 F:\Desktop，桌面命名空间只是它的
    // 一个视图；项一旦不在桌面根下（或路径与命名空间不同步），
    // desk->GetUIObjectOf 就会失败 —— 用户看到的就是“右键弹不出菜单”。
    IShellFolder* parent = nullptr;
    LPITEMIDLIST child = nullptr;
    bool parsed = false;
    if (!shellPath.isEmpty()) {
        parsed = parseParentAndChild(shellPath, &parent, &child);
    }
    if (!parsed) {
        IShellFolder* desk = nullptr;
        if (SUCCEEDED(SHGetDesktopFolder(&desk)) && desk) {
            LPITEMIDLIST abs = resolvePidl(desk, shellPath, displayName);
            if (abs) {
                LPITEMIDLIST rel = ILClone(ILFindLastID(abs));
                CoTaskMemFree(abs);
                if (rel) { parent = desk; child = rel; parsed = true; }
                else desk->Release();
            } else {
                desk->Release();
            }
        }
    }

    HMENU hMenu = CreatePopupMenu();
    IContextMenu* pcm = nullptr;
    bool shellMenuReady = false;
    DiagTrace::log(QStringLiteral("[menu] IN owner=%1 shellPath='%2' name='%3' parsed=%4")
                       .arg(reinterpret_cast<quintptr>(owner))
                       .arg(shellPath, displayName, parsed ? QStringLiteral("Y") : QStringLiteral("N")));
    if (parsed && hMenu) {
        LPCITEMIDLIST childRef = child;
        if (SUCCEEDED(parent->GetUIObjectOf(owner, 1, &childRef, IID_IContextMenu,
                                            nullptr, reinterpret_cast<void**>(&pcm)))) {
            // CMF_EXPLORE 与资源管理器同款（含“打开/打开方式/发送到/属性”等），
            // CMF_CANRENAME 才会让 Shell 生成「重命名」项。
            shellMenuReady = SUCCEEDED(pcm->QueryContextMenu(hMenu, 0, 1, 0x7FFF,
                                                             CMF_EXPLORE | CMF_CANRENAME));
        }
    }

    // 兜底：连目标项都没解析出来（路径已过期、竞态窗口期、命名空间特殊项）——
    // 宁可给一个功能较少的菜单，也不要让用户“点了右键什么都不发生”。
    // 这是“右键菜单时好时坏 / 改名之后右键就失灵”的最后一层保险。
    if (!shellMenuReady) {
        DiagTrace::log(QStringLiteral("[menu] shell menu NOT ready -> fallback menu (parsed=%1)")
                           .arg(parsed ? QStringLiteral("Y") : QStringLiteral("N")));
        if (hMenu) DestroyMenu(hMenu);
        hMenu = CreatePopupMenu();
        if (!hMenu) {
            if (pcm) pcm->Release();
            if (child) CoTaskMemFree(child);
            if (parent) parent->Release();
            if (uninit) CoUninitialize();
            return MenuAction::None;
        }
        const UINT kOpen = 1, kRename = 2, kDelete = 3;
        AppendMenuW(hMenu, MF_STRING, kOpen, L"打开(&O)");
        if (!shellPath.startsWith(QLatin1String("::")))
            AppendMenuW(hMenu, MF_STRING, kRename, L"重命名(&R)");
        if (interceptDelete) {
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, kDelete, L"删除(&D)");
        }
        if (owner) activateOwnerForMenu(owner);
        const UINT id = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                       screenX, screenY, 0, owner, nullptr);
        if (owner) ::PostMessageW(owner, WM_NULL, 0, 0);
        DestroyMenu(hMenu);
        if (id == kRename)      result = MenuAction::Rename;
        else if (id == kDelete) result = MenuAction::Delete;
        else if (id == kOpen) {
            // 用系统默认方式打开（ShellExecute 同样认识 ::{CLSID} 形式的命名空间路径）。
            const std::wstring w = QDir::toNativeSeparators(shellPath).toStdWString();
            ::ShellExecuteW(owner, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            result = MenuAction::Invoked;
        }
        DiagTrace::log(QStringLiteral("[menu] FALLBACK picked id=%1 result=%2")
                           .arg(id).arg(static_cast<int>(result)));
        if (pcm) pcm->Release();
        if (child) CoTaskMemFree(child);
        if (parent) parent->Release();
        if (uninit) CoUninitialize();
        return result;
    }

    // 用**规范动词**（GCS_VERBW）识别命令，而不是匹配菜单文字：
    // 中文系统显示“重命名/删除”，英文系统显示 "Rename/Delete"，
    // 规范动词恒为 "rename"/"delete"（与界面语言无关），据此拦截才不会因换语言而失效。
    // 命令 id 从 1 开始分配，故 id 对应的动词偏移是 id-1。
    UINT renameId = 0, deleteId = 0;
    // 扫描范围必须用**实际菜单项数**作为上界，绝不能用 0x7FFF：
    // GetCommandString 是 COM 调用，对越界偏移返回 E_FAIL 但要真正跑一遍扩展；
    // 某些第三方 Shell 扩展（压缩/杀软/Git 等）在每次调用上都会做磁盘或注册表探测，
    // 32767 次扫描会让“右键菜单”卡住数秒甚至更久 —— 用户观感就是“点了没反应”。
    const int nMenuItems = qMax(0, GetMenuItemCount(hMenu));
    for (UINT i = 1; i <= static_cast<UINT>(nMenuItems); ++i) {
        WCHAR verb[64] = {0};
        if (FAILED(pcm->GetCommandString(i - 1, GCS_VERBW, nullptr,
                                         reinterpret_cast<LPSTR>(verb), 64))) {
            continue;   // 分隔符/子菜单等非命令项：没有动词，跳过
        }
        if (renameId == 0 && _wcsicmp(verb, L"rename") == 0) renameId = i;
        else if (deleteId == 0 && _wcsicmp(verb, L"delete") == 0) deleteId = i;
        if (renameId && deleteId) break;
    }
    DiagTrace::log(QStringLiteral("[menu] items=%1 renameId=%2 deleteId=%3")
                       .arg(nMenuItems).arg(renameId).arg(deleteId));

    // 兜底追加：Shell 没给「重命名」项（部分特殊项/第三方处理器会这样）时自己补一项。
    // 不补的话，用户在这类图标上“右键 → 重命名”就是空的 —— 而重命名恰恰是我们必须
    // 自己接管、也最常被用到的动词。
    UINT fallbackRenameId = 0;
    if (renameId == 0 && !shellPath.startsWith(QLatin1String("::"))) {
        int n = GetMenuItemCount(hMenu);
        if (n < 0) n = 0;
        if (n > 0) AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        fallbackRenameId = static_cast<UINT>(n) + 1;
        AppendMenuW(hMenu, MF_STRING, fallbackRenameId, L"重命名(&R)");
    }

    // 弹出前把 owner 所属顶层窗口设为前台（见 activateOwnerForMenu 的说明：
    // 无焦点窗口不这么做的话，菜单的第一次点击会被前台激活锁吃掉）。
    activateOwnerForMenu(owner);

    const UINT id = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                   screenX, screenY, 0, owner, nullptr);
    // 经典收尾：给 owner 投一条空消息。前台窗口在菜单生命期内反复切换时，
    // 菜单可能残留一个不可见的消息循环（后续鼠标事件被它吞掉）。
    if (owner) ::PostMessageW(owner, WM_NULL, 0, 0);

    if (id > 0) {
        if ((renameId != 0 && id == renameId) || (fallbackRenameId != 0 && id == fallbackRenameId)) {
            // 交回调用方做内联编辑：本控件不属于 Shell 视图，
            // Shell 发出的 "rename" 没有行内编辑接收方，InvokeCommand 会静默哑火。
            result = MenuAction::Rename;
        } else if (deleteId != 0 && id == deleteId && interceptDelete) {
            // 交回调用方：让它走 deleteToRecycleBin 并同步本地持久化数据
            // （直接让 Shell 删的话，分类库里的 path→分类 记录会变成孤儿）。
            result = MenuAction::Delete;
        } else {
            CMINVOKECOMMANDINFOEX cmi;
            ZeroMemory(&cmi, sizeof(cmi));
            cmi.cbSize = sizeof(cmi);
            cmi.fMask = CMIC_MASK_UNICODE;
            cmi.hwnd = owner;
            cmi.lpVerbW = MAKEINTRESOURCEW(id - 1);
            cmi.lpVerb  = MAKEINTRESOURCEA(id - 1);
            cmi.nShow = SW_SHOWNORMAL;
            pcm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&cmi));
            result = MenuAction::Invoked;
        }
    }

    if (pcm) pcm->Release();
    DestroyMenu(hMenu);
    if (child) CoTaskMemFree(child);
    if (parent) parent->Release();
    if (uninit) CoUninitialize();
    DiagTrace::log(QStringLiteral("[menu] OUT picked id=%1 result=%2 (0=None 1=Rename 2=Delete 3=Invoked) renameId=%3 fallbackRenameId=%4")
                       .arg(id).arg(static_cast<int>(result)).arg(renameId).arg(fallbackRenameId));
    return result;
#else
    Q_UNUSED(ownerHwnd)
    Q_UNUSED(shellPath)
    Q_UNUSED(displayName)
    Q_UNUSED(screenX)
    Q_UNUSED(screenY)
    Q_UNUSED(interceptDelete)
    return MenuAction::None;
#endif
}

// 用于「粘贴 / 粘贴快捷方式」菜单项在弹出时按有无数据置灰。
// 作者：谭征
bool ShellOps::clipboardHasFileData() {
#ifdef Q_OS_WIN
    // 不调用 OpenClipboard（可能被其它进程占用而失败）；IsClipboardFormatAvailable
    // 直接查询当前剪贴板可用格式，无需打开，最稳。
    if (IsClipboardFormatAvailable(CF_HDROP)) return true;
    const UINT fmtIdList = ::RegisterClipboardFormatW(L"Shell IDList Array");
    if (fmtIdList && IsClipboardFormatAvailable(fmtIdList)) return true;
    const UINT fmtContents = ::RegisterClipboardFormatW(L"FileContents");
    if (fmtContents && IsClipboardFormatAvailable(fmtContents)) return true;
    return false;
#else
    return false;
#endif
}

// 前向声明：实现体在下方，clipboardFileList() 直接复用同一份 CF_HDROP 读取逻辑。
static QStringList readClipboardFiles();

// 用途：粘贴前抓取源路径，便于把“源文件”标记为已归档（从 Dock 隐藏）。
// 作者：谭征
QStringList ShellOps::clipboardFileList() {
    return readClipboardFiles();
}

// 读剪贴板里的文件列表（CF_HDROP）。复制/剪切自资源管理器或 Dock 原生菜单均写入此格式。
static QStringList readClipboardFiles() {
#ifdef Q_OS_WIN
    QStringList out;
    if (!OpenClipboard(nullptr)) return out;
    HANDLE h = GetClipboardData(CF_HDROP);
    if (h) {
        const DROPFILES* df = reinterpret_cast<const DROPFILES*>(GlobalLock(h));
        if (df) {
            if (df->fWide) {
                const wchar_t* p = reinterpret_cast<const wchar_t*>(
                    reinterpret_cast<const BYTE*>(df) + df->pFiles);
                while (*p) { out.append(QString::fromWCharArray(p)); p += wcslen(p) + 1; }
            } else {
                const char* p = reinterpret_cast<const char*>(
                    reinterpret_cast<const BYTE*>(df) + df->pFiles);
                while (*p) { out.append(QString::fromLocal8Bit(p)); p += strlen(p) + 1; }
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
#else
    return QStringList();
#endif
}

// 粘贴快捷方式：桌面根背景菜单没有 pastelink 动词（隔离实验已枚举确认，只有 paste），
// 故直接为每个剪贴板来源在桌面目录创建 .lnk（IShellLink + IPersistFile），与原生
// 「粘贴快捷方式」产物完全一致，命名沿用资源管理器「xxx - 快捷方式.lnk」约定。
static QString uniqueLinkPath(const QString& dir, const QString& baseName) {
    auto candidate = [&](const QString& name) -> QString {
        return QDir::toNativeSeparators(dir) + QDir::separator() + name + QStringLiteral(".lnk");
    };
    QString p = candidate(baseName);
    if (!QFile::exists(p)) return p;
    QString p2 = candidate(baseName + QStringLiteral(" - 快捷方式"));
    if (!QFile::exists(p2)) return p2;
    for (int i = 2; ; ++i) {
        QString p3 = candidate(baseName + QStringLiteral(" - 快捷方式 (") + QString::number(i) + QChar(')'));
        if (!QFile::exists(p3)) return p3;
    }
}

static bool pasteShortcutsToDesktop() {
#ifdef Q_OS_WIN
    const QStringList srcs = readClipboardFiles();
    if (srcs.isEmpty()) return false;
    const QString desk = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (desk.isEmpty()) return false;

    const HRESULT cohr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninit = (cohr == S_OK);

    bool any = false;
    for (const QString& src : srcs) {
        IShellLinkW* sl = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IShellLinkW, reinterpret_cast<void**>(&sl))) || !sl)
            continue;
        const QFileInfo fi(src);
        sl->SetPath(src.toStdWString().c_str());
        sl->SetDescription(fi.fileName().toStdWString().c_str());
        const QString work = fi.absolutePath();
        if (!work.isEmpty()) sl->SetWorkingDirectory(work.toStdWString().c_str());

        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf))) && pf) {
            const QString linkPath = uniqueLinkPath(desk, fi.completeBaseName());
            if (SUCCEEDED(pf->Save(linkPath.toStdWString().c_str(), TRUE)))
                any = true;
            pf->Release();
        }
        sl->Release();
    }
    if (uninit) CoUninitialize();
    return any;
#else
    return false;
#endif
}

// 返回 true 表示命令已成功下发（不代表文件已落盘）。剪贴板无可粘贴数据时返回 false。
// 作者：谭征
bool ShellOps::pasteToDesktop(const QString& verb, void* ownerHwnd) {
#ifdef Q_OS_WIN
    // 粘贴快捷方式：桌面根背景菜单没有该动词，改为手动建 .lnk（见上）。
    if (verb == QStringLiteral("pastelink"))
        return pasteShortcutsToDesktop();

    const HRESULT cohr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninit = (cohr == S_OK);
    const HWND owner = reinterpret_cast<HWND>(ownerHwnd);
    const HWND hw = (owner ? owner : GetForegroundWindow());

    bool ok = false;
    IShellFolder* desk = nullptr;
    if (SUCCEEDED(SHGetDesktopFolder(&desk)) && desk) {
        // 取「桌面命名空间根」的背景右键菜单（cidl=0 = 文件夹自身）。隔离实验证明：
        // 对桌面根 IShellFolder，GetUIObjectOf(hwnd,0,NULL) 返回 S_OK，且其「粘贴」动词
        // 会把文件落到用户桌面目录，与资源管理器桌面右键「粘贴」完全一致。
        IContextMenu* pcm = nullptr;
        HRESULT gh = desk->GetUIObjectOf(hw, 0, nullptr, IID_IContextMenu, nullptr,
                                         reinterpret_cast<void**>(&pcm));
        if (FAILED(gh))
            gh = desk->CreateViewObject(hw, IID_IContextMenu, reinterpret_cast<void**>(&pcm));
        if (SUCCEEDED(gh) && pcm) {
            HMENU hmenu = CreatePopupMenu();
            if (hmenu) {
                const UINT idCmdFirst = 1, idCmdLast = 0x7FFF;
                if (SUCCEEDED(pcm->QueryContextMenu(hmenu, 0, idCmdFirst, idCmdLast, 0))) {
                    // 枚举菜单项、按规范动词 "paste" 定位偏移；再按偏移调用
                    // （实验证明：按偏移调用在桌面根背景菜单上稳定成功，字符串动词不可靠）。
                    int foundOff = -1;
                    for (UINT id = idCmdFirst; id < idCmdLast; ++id) {
                        wchar_t vbuf[64] = {0};
                        const HRESULT gs = pcm->GetCommandString(
                            id - idCmdFirst, GCS_VERBW, nullptr,
                            reinterpret_cast<LPSTR>(vbuf), sizeof(vbuf));
                        if (SUCCEEDED(gs) && vbuf[0] && _wcsicmp(vbuf, L"paste") == 0) {
                            foundOff = static_cast<int>(id - idCmdFirst);
                            break;
                        }
                    }
                    if (foundOff >= 0) {
                        CMINVOKECOMMANDINFOEX cmi;
                        ZeroMemory(&cmi, sizeof(cmi));
                        cmi.cbSize = sizeof(cmi);
                        cmi.fMask = CMIC_MASK_UNICODE;
                        cmi.hwnd = hw;
                        cmi.lpVerb = MAKEINTRESOURCEA(foundOff);  // 按偏移调用（实验验证可用）
                        cmi.lpVerbW = nullptr;
                        cmi.nShow = SW_SHOWNORMAL;
                        ok = SUCCEEDED(pcm->InvokeCommand(
                            reinterpret_cast<CMINVOKECOMMANDINFO*>(&cmi)));
                    }
                }
                DestroyMenu(hmenu);
            }
            pcm->Release();
        }
        desk->Release();
    }
    if (uninit) CoUninitialize();
    return ok;
#else
    Q_UNUSED(verb)
    Q_UNUSED(ownerHwnd)
    return false;
#endif
}
