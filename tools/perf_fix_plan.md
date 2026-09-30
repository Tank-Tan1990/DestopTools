# 卡顿二次排查与修复思路（02:57）

> 状态：**仅诊断，未改任何代码**。等你确认要做哪几条。

---

## 一、为什么 ① ② ③ 改完还是严重卡顿

### 1. 节流只降「次数」，没降「单次时长」——这是根本原因

① 把拖动时的高频下发用 120ms 合并窗口压到约 8 次/秒。但**每一次下发的实际成本是 300~600ms**：

| # | 成本项（每次 `themeChanged` 全量重跑） | 单次耗时 |
|---|---|---|
| ① | 全应用 `a.setStyleSheet(15KB QSS)` → Qt 遍历进程内**每一个 widget** 重解析 + repolish + 重算尺寸 | 150–330ms |
| ② | 15 个窗口各自 `applyTheme()` → 再各自 `setStyleSheet`（内部还有几十次 `replace()` 令牌替换） | 80–150ms |
| ③ | 15 次 `setWindowOpacity()`，其中 **2 个是全屏分层窗**（Dock / 网格镜像，`WA_TranslucentBackground` → `WS_EX_LAYERED`） | 50–150ms |
| ④ | `SettingCenterDialog::updateStackMask()` → `setMask()` 大面积区域重算 | ≈10ms |
| | **合计** | **≈300–600ms** |

⇒ 单次就超过低级钩子允许的阻塞时间，GUI 线程被持续占满，**鼠标照样一直冻结**。节流救不了。

### 2. ③ 的 QSS 字符串缓存，对「透明度」这条路径完全无效

`theme.h` 里 `alphaF(base) = base × effectiveOpacity()` —— **透明度是被乘进 QSS 每一个 rgba 的**。所以每改一次透明度，`buildGlobalStyleSheet()` 的产物必然不同 → `main.cpp` 里 `if (qss == s_appliedQss) return;` **永远不成立** → 每次都全量下发。③ 只挡住了「什么都没变」的重复调用，对拖动路径等于没做。

### 3. 透明度被实现了两遍

`alphaF()` 进 QSS（成本①） **＋** 各窗口 `setWindowOpacity()`（成本③）——一次改动同时命中两种最贵的机制，成本叠加而不是二选一。

### 4. 🚨 上一轮 ⑤ 只延后了 Dock 的钩子，**网格窗的钩子仍在启动瞬间就装**

- `mainwindow.cpp:294` → `m_gridWindow->show()` → `IconGridWindow::showEvent`（:2342）→ `registerGridWindow()` → `ensureGridHooks()` **立刻装钩**；
- 而此时 `MainWindow` 构造后续的重活（助手窗、收纳盒、Dock 首刷…）还没跑完。
- trace 日志佐证：每次启动 `[grid] registerGridWindow/ensureGridHooks` 与 `[dock] ensureHotkeyHooks` 只相距约 70ms —— 也就是说**整个启动重活期，网格钩子一直都在位**。
- 结论：**「打开瞬间卡死」这一条，上一轮没根治**；关闭路径同理（网格钩子要到 `~IconGridWindow`/`closeEvent` 才卸，晚于大部分析构重活）。

---

## 二、思路清单（按「效果 / 成本」排序）

### 思路 A —— 把透明度从 QSS 里摘出去，只走窗口不透明度（治本，改动小）
- **做法**：`alphaF()` / `panelBgString()` / `windowBgString()` 不再乘 `effectiveOpacity()`（QSS 只随**主题色**变化）；透明度完全由各窗口既有的 `setWindowOpacity()` 承担。
- **为什么有效**：拖透明度时 QSS 字符串**恒定不变** → `applyGlobalStyleSheet` 的内容比较直接命中 → **全应用 setStyleSheet 一次都不调用**（砍掉成本①②的大头，约 70–80%）。
- **代价**：观感会变——原来「面板背景 alpha × 整窗 opacity」是叠加的，摘掉后玻璃层次会略不同（同一档位看起来更透/更实）。**需要你确认是否接受**。

### 思路 B —— 拖动期间「零下发」，只松手应用一次（推荐，最直接）
- **做法**：`m_transparencySlider->setTracking(false)`（Qt 官方推荐做法；`valueChanged` 只在松手时发一次）；拖动中用 `sliderMoved` 只更新 `xx%` 标签；`sliderReleased` 才真正应用一次。
- **为什么有效**：拖动过程**成本归零**，鼠标绝对不卡；只在松手时卡一次。
- **依据**：Qt 文档 `QAbstractSlider::tracking` —— `false` 时 `valueChanged` 仅在释放时触发；`sliderMoved` 专供「实时显示值」。
- **代价**：拖动时看不到实时效果（数字标签仍实时）。

### 思路 C —— 重活期间临时摘掉低级钩子（推荐，护栏；与 A/B 正交）
- **做法**：滑块 `sliderPressed` → `releaseHotkeyHooks()` + `releaseGridHooks()`；`sliderReleased` → 重装。同理包住「启动重活」「关闭重活」「刷新桌面」等已知长任务。
- **为什么有效**：**没有钩子就没人等我们** —— 即使单次主题应用仍要 300ms，系统鼠标也不会被冻结，最多是这个操作本身掉帧。
- **依据**：MS《LowLevelMouseProc》——超时会被静默摘钩；SO「SetWindowsHookEx with WH_MOUSE_LL slows down the mouse」高票结论：**只在真正需要时持钩，做重活前先摘钩**；MS 另建议把钩子放**专用线程**、或改用 **Raw Input**。
- **代价**：拖动那几秒 F2 / Delete 暂不可用（可接受）；重装走项目已有的「先卸后装」自愈路径。
- **建议升级为通用护栏**：任何已知会长时间占用线程的操作，前后都摘/装钩子。

### 思路 D —— 应用级 QSS 改为「分窗口下发 + 只 polish 不 setStyleSheet」（治本，工程量中等）
- **做法**：取消 `a.setStyleSheet()` 的全局下发；只对真正需要的顶层窗口设 QSS；主题色变化时用 `style()->unpolish(w); style()->polish(w); w->update();` 代替 `setStyleSheet`。
- **依据**：SO 18187376 实测 —— 同一份样式的 `unpolish/polish + update` 是 **0–1ms**，而 `setStyleSheet` 是 **150–200ms**。
- **代价**：要给每个窗口建「主题依赖」清单；`QToolTip`/`QMenu` 等全局兜底仍需保留一次应用级设置。

### 思路 E —— 只对**可见**窗口下发（低风险，立刻见效）
- **做法**：各 `applyTheme()` 首行 `if (!isVisible()) return;`（或在主题扇出循环里跳过隐藏窗）。
- **为什么有效**：15 个窗里通常只有 3~4 个可见（Dock/网格/助手/设置中心），其余（文件搜索/关机定时/壁纸/帮助反馈/备份/更新/消息框…）虽隐藏但照样在付 `setStyleSheet` 的钱。直接省 70% 左右。
- **代价**：几乎为零。需注意「窗口稍后 show()」时补一次 `applyTheme()`。

### 思路 F —— 把低级钩子搬到独立线程 / 改用 Raw Input（最终形态，工程量大）
- **做法**：钩子装到专用线程（高优先级），回调只把事件投递给工作线程后立即返回；或改用 `RegisterRawInputDevices` 做键鼠判定。
- **依据**：MS 官方明确「必须在专用线程上执行这些钩子，把工作交给工作线程后立即返回」，并称 Raw Input 比低级钩子更高效。
- **代价**：最大。当前钩子回调直接依赖 Qt 对象与 `QTimer::singleShot`，需重构成线程安全投递。属「以后重构」级别。

### 思路 G —— 顺手清理常驻开销（补充）
- `DesktopImmunityFilter::nativeEventFilter` 加**消息白名单**（只处理 `WM_WINDOWPOSCHANGING / WM_SHOWWINDOW / WM_SYSCOMMAND / WM_DESTROY`），其余直通 —— 它现在对**每一条**本进程原生消息都要跑两次 `QWidget::find`（`isInputWindow` + `isDesktopWidget`），而这两次可以合并成一次。
- `setWindowOpacity` 只在值真变化时调用。
- `s_pollTimer`(50ms) / `m_bottomTimer`(1s) / `m_recyclePollTimer`(1.5s) 已在上轮优化，维持即可。

---

## 三、推荐组合

**A + B + C（推荐）**：
- **B** 让「拖动」这段路成本归零 → 手感立刻正常；
- **A** 让「松手那一次」也变便宜（砍掉全应用 repolish）；
- **C** 兜底：不管还剩多少成本，都**不会再冻结系统鼠标**。

**再加 E** 则是顺手把 15 窗扇出削到 3~4 窗，几乎零风险。
**D / F** 建议后续单独排期，不要和这次混在一起。

另外 **⑤ 的补漏**（网格钩子延后到重活之后 + 关闭前先摘钩）建议一并做，否则「打开/关闭瞬间卡」还会残留。

---

## 四、验证方式
1. 改完先编译（`python tools/build.py` 或 `--full`）；
2. 拖动透明度滑块：鼠标应**全程跟手**，松手时最多一次轻微停顿；
3. 启动 / 关闭瞬间：鼠标应**全程可动**；
4. 回归：F2 重命名、Delete 删除仍可用（钩子摘/装后必须能恢复）；主题色切换、透明度重启后仍持久化。
