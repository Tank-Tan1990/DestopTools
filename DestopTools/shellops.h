/*
 * @file shellops.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef SHELLOPS_H
#define SHELLOPS_H

#include <QString>
#include <QStringList>

// 桌面图标（Dock）与收纳盒/整理窗口图标**共用**的 Shell 文件操作。
// 铁律：重命名 / 删除的落盘通道只能有一条。
// 两个窗口若各写一套 SHFileOperation / SetNameOf，很快就会出现口径分裂
// （一边进回收站、一边永久删除；一边能删、一边删不掉；一边判了受保护项、一边漏判），
// 而这类分歧直接对应“文件没了 / 文件删不掉”这种不可逆后果。
// 本模块即那条唯一通道：任何一端要动真实文件，都必须经它。
namespace ShellOps {

// 把 absPath 原地重命名为同目录下的 newName（纯文件名，不含路径分隔符）。
// 走 Windows Shell（IShellFolder::SetNameOf），与资源管理器重命名同一条路径：
// 重名 / 非法字符 / 权限不足 / 文件被占用一律由 Shell 判定并返回失败，
// 绝不出现“界面已经改了名、磁盘上其实没改”的假成功。
// ownerHwnd 为可能的系统 UI 归属窗口（可为 nullptr）。失败原因写入 errorOut（可为 nullptr）。
bool renameInPlace(const QString& absPath, const QString& newName,
                   void* ownerHwnd, QString* errorOut = nullptr);

// 删除到回收站（可还原），与资源管理器“删除”同一条通道：
// FOF_ALLOWUNDO（进回收站）+ FOF_WANTNUKEWARNING（仅“回收站放不下”时警告），
// 不加 FOF_NOCONFIRMATION —— 是否二次确认交给系统“显示删除确认”的用户设置。
// absPaths 为绝对路径（正/反斜杠皆可，内部转原生分隔符）。重复项会自动去掉。
// 返回 false 表示一项都没删成（全部判定为受保护项 / 失败 / 用户在系统确认框里取消）。
bool deleteToRecycleBin(const QStringList& absPaths, void* ownerHwnd);

// 该路径是否“可以被重命名”。系统命名空间项（::{CLSID}）没有真实文件名，一律 false。
bool isSafeToRename(const QString& absPath);

// 回收站当前是否为空（i64NumItems == 0）。用 SHQueryRecycleBinW 汇总所有驱动器，
// 是 Windows 桌面切换回收站“满/空”两套图标的唯一权威依据。查询失败时按“空”返回
// （避免把无法确定的状态误显示成“满”）。仅 Windows；非 Windows 平台恒返回 true。
bool recycleBinIsEmpty();

// 该路径是否“可以被删除”——受保护项一律 false：
// · 系统命名空间项（::{...}）
// · 不存在、无文件名（盘根等）
// · 当前用户主目录**及其所有上级**
// · 桌面目录本身
// 后两条是灾难级兜底：把整个用户目录或整个桌面拖进回收站是不可逆的。
bool isSafeToDelete(const QString& absPath);

// 原生右键菜单
// 用户在菜单里选中的“动词”归类。菜单项本身完全由 Windows Shell 生成，此处只把
// 两个需要**调用方自己接管**的动词单独挑出来（其余命令已由本模块直接执行完）。
enum class MenuAction {
    None,      // 关掉菜单 / 什么都没选
    Rename,    // 命中了「重命名」——调用方需自己做内联编辑（见下方说明）
    Delete,    // 命中了「删除」——调用方需自己走 deleteToRecycleBin 并同步本地数据
    Invoked    // 其它命令（打开/属性/复制/剪切/发送到/压缩…）已由本模块执行完毕
};

// 弹出 **Windows 桌面同款的系统原生右键菜单**（IShellFolder::GetUIObjectOf +
// IContextMenu::QueryContextMenu + TrackPopupMenu）。
// 为什么必须共用这一条通道：菜单项、分组、图标、启用状态、界面语言全部由 Shell 自己生成，
// 与资源管理器/桌面完全一致，且自动包含系统扩展（压缩、发送到、打开方式、属性、
// Git/杀软等第三方 Shell 扩展）。自己用 QMenu 拼一份“打开/重命名/删除”，
// 观感与功能都会与用户熟知的桌面菜单明显不同。
// 参数：
// ownerHwnd    菜单归属窗口（通常传顶层窗口句柄，可为 nullptr）
// shellPath    目标项的 Shell 路径（绝对路径或 ::{CLSID}）；为空/解析失败时按 displayName 兜底
// displayName  显示名（兜底解析用）
// screenX/Y    弹出位置（**物理屏幕像素**，即 QCursor::pos() 或 GetMessagePos 的量纲）
// interceptDelete  true = 命中「删除」时不执行，直接返回 MenuAction::Delete，
// 由调用方走 deleteToRecycleBin 以便同步本地持久化数据；
// false = 交给 Shell 自己删（与 Dock 现有行为一致）。
// 「重命名」**永远**拦截并返回 MenuAction::Rename，从不调用 Shell 的 InvokeCommand：
// 本控件不是 Shell 视图，Shell 发出的 "rename" 动词没有行内编辑接收方，点了会静默哑火。
// 用法：返回 Rename → 调用方 startInlineRename()；Delete → 调用方走删除通道；
// Invoked → 文件系统可能已变，调用方自行决定是否重新扫描。
MenuAction showNativeContextMenu(void* ownerHwnd,
                                 const QString& shellPath,
                                 const QString& displayName,
                                 int screenX, int screenY,
                                 bool interceptDelete);

// 剪贴板当前是否含有**可粘贴的文件数据**（CF_HDROP / 文件拖放格式）。
// 用于「粘贴 / 粘贴快捷方式」菜单项在弹出时按有无数据置灰。
bool clipboardHasFileData();

// 读取剪贴板里的源文件列表（复制/剪切自资源管理器或 Dock 原生菜单均写入 CF_HDROP）。
// 返回绝对路径（原生分隔符）；无可读文件时返回空列表。
// 用途：粘贴前抓取源路径，便于把“源文件”标记为已归档（从 Dock 隐藏）。
QStringList clipboardFileList();

// 在**桌面命名空间**直接调用原生「粘贴 / 粘贴快捷方式」动词（IShellFolder + IContextMenu）。
// 与资源管理器的桌面右键「粘贴」完全同一条通道：同名冲突弹「替换/跳过」、可撤销、
// 语义与系统一致。verb 传 "paste"（复制/移动文件到桌面）或 "pastelink"（仅生成指向原文件的 .lnk）。
// ownerHwnd 为可能的系统 UI（冲突对话框）归属窗口，可为 nullptr。
// 命令为异步执行（CMIC_MASK_ASYNCOK），文件落盘通常稍后才完成，调用方需自行轮询等待。
// 返回 true 表示命令已成功下发（不代表文件已落盘）。剪贴板无可粘贴数据时返回 false。
bool pasteToDesktop(const QString& verb, void* ownerHwnd);

}   // namespace ShellOps

#endif // SHELLOPS_H
