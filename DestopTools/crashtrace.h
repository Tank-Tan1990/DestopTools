/*
 * @file crashtrace.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef CRASHTRACE_H
#define CRASHTRACE_H

#include <QString>

// —— 轻量级崩溃定位工具（仅 Windows 生效，其它平台为空实现） ——
// 背景：Dock 内联重命名在“改后缀”等路径上会崩溃，但崩溃现场没有任何线索可用：
// · dumpDebug() 因曾在 WINEVENTPROC 钩子回调里做 QFile 文件 I/O 会二次崩溃而整体停用；
// · Qt 默认崩溃路径（访问违例 / qFatal / abort）不留业务上下文。
// 本模块提供两件事：
// 1) mark() / markPath()：在关键路径写入“最后动作”。固定缓冲 + strncpy，无堆分配、
// 无锁、可在钩子回调等临界上下文安全高频调用；崩溃日志据此还原出事前最后一步。
// 2) install()：安装进程级异常捕获（SEH 未处理异常 + std::terminate + SIGABRT/SIGSEGV +
// Qt 消息处理器），崩溃时把【异常代码 / 异常地址 / 归属模块 / 最后动作】追加写入
// exe 同目录的 DestopTools_crash.log。
// 崩溃处理器内部只使用 Win32 API 直接写文件，绝不使用 Qt/QString 分配内存，
// 避免在已损坏的进程状态下二次崩溃导致日志为空。
namespace CrashTrace {
// 安装异常捕获（main() 最早处调用一次）
void install();
// 记录“最后动作”（常量字符串，零分配）
void mark(const char* action);
// 记录“最后动作 + 关联路径”（会做一次 UTF-8 转换，仅用于低频关键点）
void markPath(const char* action, const QString& path);
// 记录“里程碑事件”并【立即落盘】（带时间戳的独立“事件”段）。
// 与 mark() 的区别：mark() 只写内存缓冲、仅在崩溃时随日志输出，正常收尾的标记永远看不到；
// event() 直接追加写日志，用于区分「正常退出（有 normal-quit）」vs「被外部强杀（无）」。仅低频调用。
void event(const char* tag);
// 崩溃日志完整路径（exe 同目录；供界面或用户查看）
QString logFilePath();
}

#endif // CRASHTRACE_H
