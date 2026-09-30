/*
 * @file lowlevelhookmanager.h
 * @brief 本文件属于 DestopTools 桌面整理工具。
 * @author 谭征
 * @date 2026-09-26
 */

#ifndef LOWLEVELHOOKMANAGER_H
#define LOWLEVELHOOKMANAGER_H

// 进程级低级键鼠钩子管理器（A1 / 2026-09-21）
// 背景（鼠标移动卡顿排查 A1）：
// 低级钩子 WH_KEYBOARD_LL / WH_MOUSE_LL 是【系统同步回调到安装线程】的 —— 谁安装，
// 系统就在他返回之前把每一次原始键鼠输入回调过去（1000Hz 鼠标 = 1000 次/秒的同步往返）。
// 此前 Dock 与收纳盒（网格）各自安装了一整套（key + mouse），全进程最多同时存在
// 4 个低级钩子：每次鼠标移动都要走两遍本进程回调，且 GUI 线程任何一次卡顿都会
// 直接变成全系统光标迟滞（鼠标输入在等我们返回）。
// 本类把全进程的低级钩子收敛为 **2 个**（键盘 1 + 鼠标 1），按注册顺序分发给各消费者。
// 2026-09-24（鼠标卡顿根治）：**鼠标侧已不再使用 WH_MOUSE_LL**，改走 Raw Input
// （见 `rawmouse.h`）—— Raw Input 是**异步投递**，我们处理慢只会堆积在自己的消息队列，
// 绝不会阻塞系统输入链。键盘侧仍保留 WH_KEYBOARD_LL（F2/Delete 要"吞键"，Raw Input 做不到）。
// 注册失败时自动降级回 WH_MOUSE_LL，功能不失效。
// 消费代码（Dock / 收纳盒）**完全没改**：rawmouse 把按键塞进 MSLLHOOKSTRUCT 后
// 仍然走本类的 dispatchMouse 分发，签名与语义与旧实现一致：
// · 键盘：消费者返回 true ⇒ 该键已被消费 ⇒ 立即返回 1（吞掉），后续消费者与前台程序都收不到；
// · 鼠标：消费者无返回值（从不吞事件），全部消费者都会按注册顺序被通知；
// · 自愈式安装：reinstall() 一律“先卸后装”（句柄非空 ≠ 钩子有效：系统会静默摘除超时的低级钩子）；
// · suspend()/resume() 为嵌套计数：重活期间全进程摘钩，免得线程被钉住时拖垮全系统鼠标。
// header-only（单例用 inline 函数的局部 static，跨 TU 唯一），因此无需改动 .pro。
// rawmouse.h 需列进 .pro 的 HEADERS（保持工程文件与源码一致，便于 Qt Creator 索引）。

#ifdef Q_OS_WIN

#include <QtGlobal>
#include <QVector>
#include <windows.h>
#include "rawmouse.h"   // 鼠标侧：Raw Input 接收器（异步，不阻塞系统输入链）

class LowLevelHookManager {
public:
    // 键盘消费者：返回 true = 已消费（吞掉该键）。
    typedef bool (*KeyProc)(int vk, void* ctx);
    // 鼠标消费者：不吞事件，只做“记录 / 投递 0ms 请求”。
    typedef void (*MouseProc)(WPARAM msg, LPARAM lParam, void* ctx);

    // instance
    static LowLevelHookManager& instance() {
        static LowLevelHookManager s_inst;   // 全进程唯一
        return s_inst;
    }

    // 注册消费者（幂等：同一 token 只保留一份，重复注册等价于“刷新 + 重装钩子”）。
    void subscribe(void* token, KeyProc kp, MouseProc mp, void* ctx) {
        if (!token) return;
        for (int i = 0; i < m_items.size(); ++i) {
            if (m_items[i].token == token) {
                m_items[i].key = kp;
                m_items[i].mouse = mp;
                m_items[i].ctx = ctx;
                reinstall();          // 自愈：每次交互都重装，规避钩子被系统静默摘除
                return;
            }
        }
        Item it;
        it.token = token; it.key = kp; it.mouse = mp; it.ctx = ctx;
        m_items.append(it);
        reinstall();
    }

    // 注销消费者。最后一个消费者注销后自动摘钩。
    void unsubscribe(void* token) {
        bool changed = false;
        for (int i = m_items.size() - 1; i >= 0; --i) {
            if (m_items[i].token == token) { m_items.removeAt(i); changed = true; }
        }
        if (!changed) return;
        if (m_items.isEmpty()) uninstall();
        else reinstall();
    }

    // 重活期间全进程摘钩（嵌套安全）。
    void suspend() {
        if (m_suspendDepth++ > 0) return;
        uninstall();
    }
    // 与 suspend() 配对；计数归零且仍有消费者时重装。
    void resume() {
        if (m_suspendDepth == 0) return;
        if (--m_suspendDepth > 0) return;
        if (m_items.isEmpty()) return;
        reinstall();
    }

    bool  active() const { return m_keyHook != nullptr || m_mouseHook != nullptr || m_mouseRaw; }
    bool  suspended() const { return m_suspendDepth > 0; }
    HHOOK keyHook() const { return m_keyHook; }
    // 降级模式下才会非空（正常情况下鼠标走 Raw Input，不再有 WH_MOUSE_LL 句柄）。
    HHOOK mouseHook() const { return m_mouseHook; }
    bool  mouseViaRawInput() const { return m_mouseRaw; }   // 诊断用：鼠标是否走的是 Raw Input
    int   consumerCount() const { return m_items.size(); }

    // token 是否仍在本管理器注册。
    // A1 回归修复（2026-09-21）：消费者侧的“我是否持钩”闸门（Dock 的 s_hkActive、
    // 收纳盒的 s_gridHooksActive）**必须**用它，绝不能用 active()。
    // 原因：重活期间的摘钩是【全进程一起摘】的（m_suspendDepth 是全进程嵌套计数），
    // 而两侧的 resume 是分别调用的。设调用序为 suspend(Dock) → suspend(Grid) →
    // resume(Grid) → resume(Dock)（mainwindow.cpp 的启动重扫 / 析构 / refreshDesktop 都是这个序）：
    // · resume(Grid) 时 m_suspendDepth 由 2 减到 1（非 0）→ reinstall() 被跳过
    // → 此刻 active() == false（Dock 还没 resume）→ 收纳盒把自己的
    // s_gridHooksActive 误清成 false → 从此 F2/Delete/重命名 静默全废
    // （直到用户再点一次收纳盒触发 armGridHotkey 才恢复，表现为“时灵时不灵”）。
    // registered() 只回答“我还在名单里吗”，与暂态摘钩无关，因此不受另一侧 suspend 影响；
    // 而“钩子此刻是否真的在位”对闸门没有意义 —— 摘钩期间根本收不到任何按键。
    bool  registered(const void* token) const {
        if (!token) return false;
        for (int i = 0; i < m_items.size(); ++i) {
            if (m_items[i].token == token) return true;
        }
        return false;
    }

    // 2026-09-24：Raw Input 通道“注册成功但消息送不到”时的降级入口。
    // 由 desktopimmunityfilter 的交叉验证触发（Qt 收到了 WM_MOUSEMOVE 却没有一条 WM_INPUT）。
    // 降级后行为与改造前完全一致（WH_MOUSE_LL），功能不受影响，只是少了“不阻塞系统输入链”的收益。
    void fallbackToLegacyMouseHook() {
        if (!m_mouseRaw) return;         // 已经是钩子 / 本来就没用 Raw Input
        if (m_suspendDepth > 0) return;  // 暂态摘钩中：等 resume 时按新状态重新决定
        m_mouseRaw = false;
        RawMouseWatcher::instance().enable(false);
        if (hasMouseConsumer() && !m_mouseHook) {
            HMODULE mod = GetModuleHandleW(nullptr);
            m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &LowLevelHookManager::mouseHookProc, mod, 0);
        }
    }

    // 自愈式安装：先卸后装。（suspend 期间为空操作。）
    void reinstall() {
        if (m_suspendDepth > 0) return;
        if (m_items.isEmpty()) { uninstall(); return; }
        uninstall();
        HMODULE mod = GetModuleHandleW(nullptr);
        // 键盘：仍用低级钩子（F2 / Delete 需要"吞键"，Raw Input 只能观察、不能拦）。
        m_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, &LowLevelHookManager::keyHookProc, mod, 0);

        // 鼠标：优先 Raw Input（异步，不阻塞系统输入链）。失败则降级回 WH_MOUSE_LL。
        if (hasMouseConsumer()) {
            RawMouseWatcher::instance().setHandler(&LowLevelHookManager::mouseThunk);
            if (RawMouseWatcher::instance().enable(true)) {
                m_mouseRaw = true;
            } else {
                m_mouseRaw = false;
                m_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, &LowLevelHookManager::mouseHookProc, mod, 0);
            }
        } else {
            m_mouseRaw = false;
            RawMouseWatcher::instance().enable(false);
        }
    }

    // —— 以下两个仅供钩子回调使用 ——
    bool dispatchKey(int vk) {
        for (int i = 0; i < m_items.size(); ++i) {
            const Item& it = m_items[i];
            if (it.key && it.key(vk, it.ctx)) return true;   // 已消费 → 吞掉
        }
        return false;
    }
    // dispatch鼠标
    void dispatchMouse(WPARAM msg, LPARAM lParam) {
        for (int i = 0; i < m_items.size(); ++i) {
            const Item& it = m_items[i];
            if (it.mouse) it.mouse(msg, lParam, it.ctx);
        }
    }

private:
    struct Item {
        void*     token = nullptr;
        KeyProc   key   = nullptr;
        MouseProc mouse = nullptr;
        void*     ctx   = nullptr;
    };

    LowLevelHookManager() = default;
    LowLevelHookManager(const LowLevelHookManager&) = delete;
    LowLevelHookManager& operator=(const LowLevelHookManager&) = delete;

    // uninstall
    void uninstall() {
        if (m_keyHook)   { UnhookWindowsHookEx(m_keyHook);   m_keyHook = nullptr; }
        if (m_mouseHook) { UnhookWindowsHookEx(m_mouseHook); m_mouseHook = nullptr; }
        // 鼠标侧同时注销 Raw Input（RIDEV_REMOVE）—— 重活期间也不再收鼠标消息。
        RawMouseWatcher::instance().enable(false);
        m_mouseRaw = false;
    }

    // 是否还有消费者关心鼠标（都只订键盘时就不必注册 Raw Input）。
    bool hasMouseConsumer() const {
        for (int i = 0; i < m_items.size(); ++i) {
            if (m_items[i].mouse) return true;
        }
        return false;
    }

    // Raw Input 回调 → 复用自己的分发（消费者签名/语义与 WH_MOUSE_LL 时完全一致）。
    static void mouseThunk(WPARAM msg, LPARAM lParam) {
        instance().dispatchMouse(msg, lParam);
    }

    static LRESULT CALLBACK keyHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
            const KBDLLHOOKSTRUCT* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
            if (k && instance().dispatchKey(static_cast<int>(k->vkCode))) return 1;   // 吞掉：不下传
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    static LRESULT CALLBACK mouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode == HC_ACTION) instance().dispatchMouse(wParam, lParam);
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    QVector<Item> m_items;
    HHOOK m_keyHook   = nullptr;
    HHOOK m_mouseHook = nullptr;   // 仅降级模式（Raw Input 不可用）下非空
    bool  m_mouseRaw  = false;     // 鼠标当前是否由 Raw Input 承载
    int   m_suspendDepth = 0;
};

inline LowLevelHookManager& lowLevelHooks() { return LowLevelHookManager::instance(); }

#endif // Q_OS_WIN
#endif // LOWLEVELHOOKMANAGER_H
