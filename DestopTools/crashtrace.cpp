/*
 * @file crashtrace.cpp
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#include "crashtrace.h"
#include "diagtrace.h"   // 复用 DiagTrace::enabled() 作为统一开关（trace.log 与 crash.log 同一控制）

#include <QByteArray>
#include <QDateTime>
#include <QFileInfo>
#include <QString>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <exception>

namespace {

// “最后动作”缓冲：只在 GUI 线程写入、崩溃时由同一线程读取，无需同步。
// 固定长度 + strncpy，零堆分配 —— 可在低级钩子回调等临界上下文中安全调用。
char g_lastAction[512] = "app-start";

wchar_t g_logPathW[MAX_PATH] = {0};
bool g_installed = false;

// —— 崩溃态可用的日志写入：CreateFileW + WriteFile（UTF-16LE，追加） ——
// 刻意不用 Qt/QString/QFile：崩溃时堆可能已损坏，任何分配都可能二次崩溃导致日志为空。
void rawAppend(const wchar_t* text, int lenChars) {
    if (!g_logPathW[0] || !text || lenChars <= 0) return;
    HANDLE h = CreateFileW(g_logPathW, FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    sz.QuadPart = 0;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart == 0) {
        const wchar_t bom = 0xFEFF;   // UTF-16LE BOM：让记事本能正确识别编码
        DWORD w = 0;
        WriteFile(h, &bom, sizeof(bom), &w, nullptr);
    }
    DWORD written = 0;
    WriteFile(h, text, (DWORD)(lenChars * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
}

// 追加一行：ASCII/UTF-8 前缀 + 正文（正文按 UTF-8 转宽字符后写入）
void appendLine(const wchar_t* prefix, const char* utf8Body) {
    wchar_t body[768] = {0};
    if (utf8Body && *utf8Body)
        MultiByteToWideChar(CP_UTF8, 0, utf8Body, -1, body, 767);
    wchar_t line[1024] = {0};
    _snwprintf_s(line, _TRUNCATE, L"%s%s\r\n", prefix ? prefix : L"", body);
    rawAppend(line, (int)wcslen(line));
}

// 取地址所属模块的文件名与偏移（用 FROM_ADDRESS 反查模块，无需 dbghelp 依赖）
void describeAddress(void* addr, wchar_t* modName, int modChars,
                     unsigned long long* offset) {
    if (modName && modChars > 0) modName[0] = 0;
    if (offset) *offset = 0;
    if (!addr) return;
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &mod) && mod) {
        wchar_t full[MAX_PATH] = {0};
        if (GetModuleFileNameW(mod, full, MAX_PATH)) {
            const wchar_t* base = wcsrchr(full, L'\\');
            if (modName && modChars > 0)
                _snwprintf_s(modName, modChars, _TRUNCATE, L"%s", base ? base + 1 : full);
        }
        if (offset) *offset = (unsigned long long)addr - (unsigned long long)mod;
    }
}

void writeHeaderLine(const wchar_t* kind) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[256] = {0};
    _snwprintf_s(line, _TRUNCATE,
                 L"\r\n===== %04d-%02d-%02d %02d:%02d:%02d  %s =====\r\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, kind);
    rawAppend(line, (int)wcslen(line));
}

void writeLastAction() {
    wchar_t wide[640] = {0};
    MultiByteToWideChar(CP_UTF8, 0, g_lastAction, -1, wide, 639);
    wchar_t line[768] = {0};
    _snwprintf_s(line, _TRUNCATE, L"最后动作: %s\r\n", wide);
    rawAppend(line, (int)wcslen(line));
}

// 统一崩溃记录入口（异常码 / 地址 / 所属模块 / 偏移 / 最后动作）
void writeCrashRecord(const wchar_t* kind, DWORD exCode, void* exAddr, const char* note) {
    writeHeaderLine(kind);
    wchar_t line[256] = {0};
    // 线程 ID：区分"崩在 GUI 线程"与"崩在某个后台线程"，二者排查方向完全不同。
    _snwprintf_s(line, _TRUNCATE, L"线程: 0x%04lX\r\n", (unsigned long)GetCurrentThreadId());
    rawAppend(line, (int)wcslen(line));
    if (exCode)
        _snwprintf_s(line, _TRUNCATE, L"异常代码: 0x%08lX\r\n", (unsigned long)exCode);
    else
        _snwprintf_s(line, _TRUNCATE, L"异常代码: (无 SEH 异常码)\r\n");
    rawAppend(line, (int)wcslen(line));

    if (exAddr) {
        wchar_t modName[MAX_PATH] = {0};
        unsigned long long off = 0;
        describeAddress(exAddr, modName, MAX_PATH, &off);
        _snwprintf_s(line, _TRUNCATE, L"异常地址: 0x%p  模块: %s + 0x%llX\r\n",
                     exAddr, modName[0] ? modName : L"(未知)", off);
        rawAppend(line, (int)wcslen(line));
    }
    if (note && *note) appendLine(L"说明: ", note);
    writeLastAction();
}

// —— 调用栈捕获（决定性证据）——
// 为什么需要：VEH 只给出"异常地址"，而异常常常发生在**第三方 DLL**（COMCTL32 / shell 扩展 …）
// 内部 —— 那时光看地址无法知道"是我们的哪一行代码把它调进来的"。
// x64 + MSVC 会为每个函数生成 .pdata unwind 信息，因此 RtlCaptureStackBackTrace 即使在
// release /O2 下也能准确回溯整条调用链（这一点与 x86 依赖 EBP 链完全不同）。
// 再配合链接期 /MAP 生成的 DestopTools.map，本程序自己的每一帧都能反查成"函数名 + 偏移"。
// 限次：良性异常（被 SEH 消化掉的探测性 AV）可能反复出现，只记录前若干次，避免日志被淹没。
const int kMaxStackReports = 6;
int g_stackReports = 0;

void writeStackFrames() {
    if (g_stackReports >= kMaxStackReports) return;
    ++g_stackReports;
    void* frames[32] = {0};
    // FramesToSkip = 1：跳过 RtlCaptureStackBackTrace 自身的帧。
    const USHORT n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
    if (n == 0) return;
    static const wchar_t kStackHead[] = L"调用栈:\r\n";
    rawAppend(kStackHead, (int)wcslen(kStackHead));
    for (USHORT i = 0; i < n; ++i) {
        wchar_t modName[MAX_PATH] = {0};
        unsigned long long off = 0;
        describeAddress(frames[i], modName, MAX_PATH, &off);
        wchar_t line[512] = {0};
        _snwprintf_s(line, _TRUNCATE, L"  #%02u %s + 0x%llX\r\n",
                     (unsigned)i, modName[0] ? modName : L"(未知)", off);
        rawAppend(line, (int)wcslen(line));
    }
}

// —— 向量化异常处理（VEH）：抢在 CRT 的 SIGSEGV 转换与 SetUnhandledExceptionFilter **之前** ——
// 为什么必须有它（2026-09-24 实测）：MSVC CRT 对访问违例会先把它转成 SIGSEGV 信号、并调用
// signal handler，于是 onFatalSignal 写出的"异常代码"只是**信号号 11 = 0x0000000B**、
// 并且**完全没有异常地址**（日志里那几行 "异常代码: 0x0000000B" 就是这么来的）——
// 排查时几乎无从下手。VEH 在所有 SEH / signal 之前被调用，能拿到真正的
// ExceptionCode（0xC0000005）与 ExceptionAddress。
// 只对"真故障码"记录：C++ 异常（0xE06D7363）、调试断点（0x80000003）等正常控制流一律不记，避免污染日志。
// 记录后一律 EXCEPTION_CONTINUE_SEARCH —— 不改变原有崩溃行为与 Windows 错误报告流程。
bool isFatalExceptionCode(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:          // 0xC0000005 访问违例（最常见）
    case EXCEPTION_IN_PAGE_ERROR:             // 0xC0000006
    case EXCEPTION_ILLEGAL_INSTRUCTION:       // 0xC000001D
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:  // 0xC0000025
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:     // 0xC000008C
    case EXCEPTION_INT_DIVIDE_BY_ZERO:        // 0xC0000094
    case EXCEPTION_PRIV_INSTRUCTION:          // 0xC0000096
    case EXCEPTION_STACK_OVERFLOW:            // 0xC00000FD（典型成因：无限递归）
    case EXCEPTION_DATATYPE_MISALIGNMENT:     // 0x80000002
        return true;
    default:
        return false;
    }
}

LONG WINAPI vehFilter(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord && isFatalExceptionCode(ep->ExceptionRecord->ExceptionCode)) {
        EXCEPTION_RECORD* er = ep->ExceptionRecord;
        writeCrashRecord(L"VEH-故障异常", er->ExceptionCode, er->ExceptionAddress, nullptr);
        // 访存细节：区分"读野指针"与"写野指针"，并给出**被访问的那个地址** ——
        // 这常常一眼就能看出是空指针、还是已被释放/尚未分配的堆块。
        if (er->NumberParameters >= 2) {
            const ULONG_PTR kind = er->ExceptionInformation[0];
            const wchar_t* act = (kind == 0) ? L"读取"
                              : (kind == 1) ? L"写入"
                              : (kind == 8) ? L"执行(DEP)" : L"未知";
            wchar_t line[256] = {0};
            _snwprintf_s(line, _TRUNCATE, L"访存: %s @ 0x%p\r\n",
                         act, (void*)er->ExceptionInformation[1]);
            rawAppend(line, (int)wcslen(line));
        }
        // 调用栈：回答"是谁把它调进来的"——特别是当异常落在第三方 DLL 里时，这是唯一线索。
        writeStackFrames();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI sehFilter(EXCEPTION_POINTERS* ep) {
    DWORD code = 0;
    void* addr = nullptr;
    if (ep && ep->ExceptionRecord) {
        code = ep->ExceptionRecord->ExceptionCode;
        addr = ep->ExceptionRecord->ExceptionAddress;
    }
    writeCrashRecord(L"未处理异常(SEH)", code, addr, nullptr);
    // 交回系统默认处理：保留 Windows 错误报告/调试器附加机会，不改变原有崩溃行为。
    return EXCEPTION_CONTINUE_SEARCH;
}

void onTerminate() {
    writeCrashRecord(L"std::terminate", 0, nullptr,
                     "未捕获异常或 noexcept 违约（常由 Qt 断言/qFatal 触发）");
    _exit(3);
}

void onFatalSignal(int sig) {
    const wchar_t* kind = L"fatal-signal";
    if (sig == SIGABRT) kind = L"SIGABRT";
    else if (sig == SIGSEGV) kind = L"SIGSEGV";
    else if (sig == SIGILL) kind = L"SIGILL";
    else if (sig == SIGFPE) kind = L"SIGFPE";
    writeCrashRecord(kind, (DWORD)sig, nullptr,
                     sig == SIGABRT ? "abort()（多为 Qt 断言失败 / qFatal）" : nullptr);
    _exit(3);
}

void qtMsgHandler(QtMsgType type, const QMessageLogContext&, const QString& msg) {
    const QByteArray utf8 = msg.toUtf8();
    // 落盘只在致命/严重级别：普通警告量大，写进去反而淹没崩溃现场。
    if (type == QtFatalMsg) {
        appendLine(L"[QtFatal] ", utf8.constData());
    } else if (type == QtCriticalMsg) {
        appendLine(L"[QtCritical] ", utf8.constData());
    }
    // 保留 stderr 输出，等价 Qt 默认处理器行为（调试构建/调试器可见）
    fprintf(stderr, "%s\n", utf8.constData());
    if (type == QtFatalMsg) abort();
}

} // namespace

namespace CrashTrace {

void mark(const char* action) {
    if (!action) return;
    strncpy_s(g_lastAction, sizeof(g_lastAction), action, _TRUNCATE);
}

// 直接**落盘**一行里程碑事件（与 mark 的本质区别：mark 只写内存缓冲，**仅在崩溃时**才被输出，
// 所以 "normal-quit" 这类"正常收尾"标记用它根本看不到）。
// 唯一调用点：main() 的 aboutToQuit → event("normal-quit")。
// 有了它，"进程为何消失"就能二分：
// · 日志里有 normal-quit  ⇒ 走的是 Qt 正常退出流程（含「应用备份」的退出→重启）；
// · 日志里没有 normal-quit ⇒ 进程是被外部**强杀**的（TerminateProcess / 安全软件 / 任务管理器）。
void event(const char* tag) {
    if (!tag || !*tag) return;
    writeHeaderLine(L"事件");
    appendLine(L"事件名: ", tag);
}

void markPath(const char* action, const QString& path) {
    const QByteArray b = path.toUtf8();
    char buf[sizeof(g_lastAction)] = {0};
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s %s",
                action ? action : "", b.constData());
    strncpy_s(g_lastAction, sizeof(g_lastAction), buf, _TRUNCATE);
}

QString logFilePath() {
    if (g_logPathW[0]) return QString::fromWCharArray(g_logPathW);
    return QString();
}

void install() {
    if (g_installed) return;
    g_installed = true;

    // 统一诊断开关：仅当 INI [Diagnostics] traceEnabled=true
    // 时才安装崩溃捕获并生成 DestopTools_crash.log；关闭时直接返回，完全不写盘、
    // 也不注册任何异常处理器（与 DiagTrace 共用同一开关，保证“关即全无日志”）。
    if (!DiagTrace::enabled()) return;

    // 用 GetModuleFileNameW 自己定位（可在 QApplication 之前调用），日志与 exe 同目录。
    wchar_t exePath[MAX_PATH] = {0};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return;
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash) *(slash + 1) = 0;
    _snwprintf_s(g_logPathW, MAX_PATH, _TRUNCATE, L"%sDestopTools_crash.log", exePath);

    // 日志超过 512KB 先清空，避免无限增长。
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(g_logPathW, GetFileExInfoStandard, &fad)) {
        const unsigned long long sz =
            ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (sz > 512ULL * 1024ULL) DeleteFileW(g_logPathW);
    }

    writeHeaderLine(L"启动 / 崩溃捕获已安装");
    wchar_t line[MAX_PATH + 64] = {0};
    _snwprintf_s(line, _TRUNCATE, L"日志文件: %s\r\n", g_logPathW);
    rawAppend(line, (int)wcslen(line));

    // VEH 必须最先注册（first = 1）：它比 CRT 的 SIGSEGV 转换与 SetUnhandledExceptionFilter 都早，
    // 是本程序里**唯一**能在日志里留下"真实异常地址"的钩子（原因见 vehFilter 上方注释）。
    AddVectoredExceptionHandler(1, vehFilter);
    SetUnhandledExceptionFilter(sehFilter);
    std::set_terminate(onTerminate);
    signal(SIGABRT, onFatalSignal);
    signal(SIGSEGV, onFatalSignal);
    signal(SIGILL, onFatalSignal);
    signal(SIGFPE, onFatalSignal);
    qInstallMessageHandler(qtMsgHandler);
}

} // namespace CrashTrace

#else   // 非 Windows：空实现（本项目仅在 Windows 构建，保持调用点无分支）

namespace CrashTrace {
void mark(const char*) {}
void markPath(const char*, const QString&) {}
void event(const char*) {}
QString logFilePath() { return QString(); }
void install() {}
} // namespace CrashTrace

#endif
