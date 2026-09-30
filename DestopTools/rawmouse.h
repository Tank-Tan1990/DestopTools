/*
 * @file rawmouse.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef RAWMOUSE_H
#define RAWMOUSE_H

// 进程级鼠标按键监视（Raw Input 版 / 2026-09-24 · 鼠标卡顿根治）
// 为什么要有本模块：
// Windows 的低级鼠标钩子 WH_MOUSE_LL 是【系统同步回调到安装线程】的 —— 每一次鼠标移动
// （1000Hz 鼠标 = 每秒最多 1000 次）系统都要把控制权交到我们的 GUI 线程并**等它返回**，
// 才继续投递输入。于是只要 GUI 线程有一刻没空闲（重绘 / 布局 / 图标提取 / 跨进程查询…），
// **整个系统的光标都会跟着发涩** —— 这就是"程序一运行，鼠标就不跟手"的根因。
// Raw Input 的机制完全不同：注册之后系统把鼠标事件**异步投递**到指定窗口的消息队列。
// 我们处理得慢只会堆积在自己的队列里，**不会阻塞系统输入链** —— 别人拖鼠标永远顺畅。
// 我们真正需要的只是"按键发生在哪"（按下位置的命中测试），所以本模块只做两件事：
// · 收到 WM_INPUT 先 GetRawInputData 看一眼 usButtonFlags，**纯移动立即返回**
// （最常见路径，一次 ~1µs 的内核拷贝，不做任何其它事）；
// · 只有真有按键时才 GetCursorPos 取位置，并塞进一个 MSLLHOOKSTRUCT 分发出去 ——
// 于是 Dock / 收纳盒既有的消费代码（hotkeyMouseConsumer / gridMouseConsumer）
// **一行都不用改**（它们读的就是 ms->pt）。
// 已知差异（可接受）：低级钩子给的是"按下那一刻"的精确坐标；Raw Input 是异步消息，
// 在处理 WM_INPUT 时用 GetCursorPos() 取到的是"此刻"的光标位置 —— 在"按下后立刻甩开"
// 的极端操作下可能差几个像素。而这条路径只用于"点落在哪个窗口/图标上"的粗判定
// （Dock 全屏、网格窗口有边界余量），实际无感。
// 兜底：若 RegisterRawInputDevices 失败（极罕见，如被安全软件拦截），调用方
// （LowLevelHookManager）会自动降级回安装 WH_MOUSE_LL —— 功能不失效。
// header-only（inline 静态单例 + 自建窗口过程），除 .pro 的 HEADERS 外无需改构建。

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <QtGlobal>
#include <cstring>

class RawMouseWatcher {
public:
    // 与 LowLevelHookManager::MouseProc 相比少了 ctx：这里只有"整进程一条鼠标流"，
    // 由调用方（管理器）在自己的分发里带上各自的 ctx。
    typedef void (*Handler)(WPARAM msg, LPARAM lParam);

    // instance
    static RawMouseWatcher& instance() {
        static RawMouseWatcher s_inst;   // 全进程唯一
        return s_inst;
    }

    // 设置handler
    void setHandler(Handler h) { m_handler = h; }

    // 注册 / 注销 Raw Input。
    // RIDEV_INPUTSINK —— 本窗口**不在前台时也照样收**（Dock 是 WS_EX_NOACTIVATE，永不会前台，
    // 所以必须用 INPUTSINK，这也是本方案能替代钩子的前提）。
    // 返回 false 表示注册失败（调用方应降级到 WH_MOUSE_LL）。
    bool enable(bool on) {
        HWND h = ensureSinkWindow();
        if (!h) return false;

        RAWINPUTDEVICE rid;
        std::memset(&rid, 0, sizeof(rid));
        rid.usUsagePage = 0x01;        // Generic Desktop Controls
        rid.usUsage     = 0x02;        // Mouse
        if (on) {
            rid.dwFlags    = RIDEV_INPUTSINK;
            rid.hwndTarget = h;
        } else {
            rid.dwFlags    = RIDEV_REMOVE;   // 注销：hwndTarget 必须为 nullptr
            rid.hwndTarget = nullptr;
        }
        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
            if (on) m_registered = false;
            return !on;   // 注销失败无所谓；注册失败才是需要降级的信号
        }
        m_registered = on;
        if (on) {
            m_inputSeen = 0;                      // 重新注册 → 重新累计“通道存活证据”
            m_registeredAt = GetTickCount64();
        }
        return true;
    }

    bool registered() const { return m_registered; }
    HWND sinkWindow() const { return m_hwnd; }

    // 自注册以来收到的 WM_INPUT 条数（**含纯移动**）。用于“通道是否真的通”的判据。
    quint64 inputSeen() const { return m_inputSeen; }

    // 通道死亡判据（误注册保护）：注册成功 ≠ 消息一定送得到。
    // 若本机因安全软件 / 沙箱 / 特殊输入栈导致 WM_INPUT 根本到不了我们的窗口，我们会
    // **静默收不到任何鼠标按键** —— 表现为 F2 / Delete / 框选 / 双击全部失效，且没有任何报错。
    // 故：注册满 1.5 秒后若仍一条 WM_INPUT 都没收到，并且系统明显在正常投递鼠标消息
    // （调用方用“Qt 收到了 WM_MOUSEMOVE”做交叉验证，见 desktopimmunityfilter.cpp），
    // 就判定该通道在本机不通，由调用方降级回 WH_MOUSE_LL。
    // 用“交叉验证”而不是“N 秒内没收到就降级”：后者会在“开机自启后用户半天没碰鼠标”时误判。
    bool suspectedDead() const {
        if (!m_registered) return false;
        if (m_inputSeen > 0) return false;
        return (GetTickCount64() - m_registeredAt) > 1500;
    }

private:
    RawMouseWatcher() = default;
    RawMouseWatcher(const RawMouseWatcher&) = delete;
    RawMouseWatcher& operator=(const RawMouseWatcher&) = delete;

    // 建（或复用）接收 WM_INPUT 的隐藏顶层窗口。
    // 刻意用 WS_POPUP + 不 ShowWindow：它永远不可见、不抢焦点、不进任务栏，
    // 也不参与"显示桌面"（外壳只操作有任务栏入口 / 可见的窗口）。
    HWND ensureSinkWindow() {
        if (m_hwnd && IsWindow(m_hwnd)) return m_hwnd;

        static const wchar_t* kClass = L"DestopToolsRawMouseSink";
        HINSTANCE inst = GetModuleHandleW(nullptr);

        WNDCLASSEXW wc;
        std::memset(&wc, 0, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = &RawMouseWatcher::sinkProcThunk;
        wc.hInstance     = inst;
        wc.lpszClassName = kClass;
        if (!RegisterClassExW(&wc)) {
            // 已注册（ERROR_CLASS_ALREADY_EXISTS）也视为成功；其它错误则建不出窗口
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
        }

        m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                 kClass, L"", WS_POPUP,
                                 0, 0, 0, 0,
                                 nullptr, nullptr, inst, this);
        return m_hwnd;
    }

    // WM_INPUT：唯一的输入入口。**必须**先 GetRawInputData 再交给 DefWindowProc
    // （DefWindowProc 会做 raw input 缓冲清理）。
    void handleInput(LPARAM lParam) {
        ++m_inputSeen;   //  通道存活证据：**含纯移动**（所以“用户动一下鼠标”即可证明通道是通的）
        UINT size = 0;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
                            nullptr, &size, sizeof(RAWINPUTHEADER)) != 0)
            return;
        if (size == 0 || size > 512) return;      // RAWINPUT 实际远小于 512
        BYTE buf[512];
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
                            buf, &size, sizeof(RAWINPUTHEADER)) != size)
            return;
        const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf);
        if (ri->header.dwType != RIM_TYPEMOUSE) return;

        const USHORT f = ri->data.mouse.usButtonFlags;
        if (f == 0) return;                       //  纯移动：到此为止（最常见的路径）

        POINT pt = {0, 0};
        GetCursorPos(&pt);                        // 与低级钩子同为"物理像素 + 屏幕坐标"

        // 复用 MSLLHOOKSTRUCT 作为载体：既有消费者（Dock / 收纳盒）读的就是 ms->pt，
        // 因此它们在本次改造中**零改动**。
        MSLLHOOKSTRUCT ms;
        std::memset(&ms, 0, sizeof(ms));
        ms.pt = pt;

        if (!m_handler) return;
        LPARAM lp = reinterpret_cast<LPARAM>(&ms);

        if (f & RI_MOUSE_LEFT_BUTTON_DOWN)   m_handler(WM_LBUTTONDOWN, lp);
        if (f & RI_MOUSE_LEFT_BUTTON_UP)     m_handler(WM_LBUTTONUP,   lp);
        if (f & RI_MOUSE_RIGHT_BUTTON_DOWN)  m_handler(WM_RBUTTONDOWN, lp);
        if (f & RI_MOUSE_RIGHT_BUTTON_UP)    m_handler(WM_RBUTTONUP,   lp);
        if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) m_handler(WM_MBUTTONDOWN, lp);
        if (f & RI_MOUSE_MIDDLE_BUTTON_UP)   m_handler(WM_MBUTTONUP,   lp);
        if (f & RI_MOUSE_BUTTON_4_DOWN)      m_handler(WM_XBUTTONDOWN, lp);
        if (f & RI_MOUSE_BUTTON_4_UP)        m_handler(WM_XBUTTONUP,   lp);
        if (f & RI_MOUSE_BUTTON_5_DOWN)      m_handler(WM_XBUTTONDOWN, lp);
        if (f & RI_MOUSE_BUTTON_5_UP)        m_handler(WM_XBUTTONUP,   lp);
    }

    static LRESULT CALLBACK sinkProcThunk(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_NCCREATE) {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        if (m == WM_INPUT) {
            RawMouseWatcher* self =
                reinterpret_cast<RawMouseWatcher*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            if (self) self->handleInput(l);
            // 交给 DefWindowProc：它会完成 raw input 缓冲清理（MSDN 要求），返回 0
            return DefWindowProcW(h, m, w, l);
        }
        return DefWindowProcW(h, m, w, l);
    }

    HWND    m_hwnd = nullptr;
    Handler m_handler = nullptr;
    bool    m_registered = false;
    quint64 m_inputSeen = 0;        // 自本次注册以来收到的 WM_INPUT 条数（含纯移动）
    ULONGLONG m_registeredAt = 0;   // 本次注册时刻（GetTickCount64，用于 suspectedDead 的宽限期）
};

inline RawMouseWatcher& rawMouse() { return RawMouseWatcher::instance(); }

#endif // Q_OS_WIN
#endif // RAWMOUSE_H
