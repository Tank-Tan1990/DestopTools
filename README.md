# DestopTools · 桌面整理助手

> 一款基于 **Qt 5.14 (Widgets) + Win32** 的 Windows 桌面整理与效率工具，灵感来自 360 桌面助手，
> 提供图标收纳、分类整理、待办提醒、快速搜索、自动对齐、主题个性化等一体化桌面管理能力。

![License](https://img.shields.io/badge/license-GPL--3.0-blue.svg)
![Platform](https://img.shields.io/badge/platform-Windows%2010%2B-0078D6.svg)
![Qt](https://img.shields.io/badge/Qt-5.14.2-41CD52.svg)
![Build](https://img.shields.io/badge/build-MSVC%202022-5C2D91.svg)

---

## 一、项目简介

桌面文件与快捷方式越积越多，视觉上杂乱、找东西费时。DestopTools 以一组**无边框、半透明的悬浮窗口**
覆盖在桌面之上，把图标、文件、待办、搜索、快捷操作聚合到一层"桌面整理层"中，既能保持桌面清爽，
又不改变你原有的文件存放位置——**只整理视图，不移动文件**。

项目定位于「个人效率桌面环境」，核心价值：

- **整洁**：把散落的图标收进可折叠的"收纳盒"，桌面一键清爽。
- **有序**：按主题分类（工作 / 娱乐 / 临时），分类间切换瞬时完成。
- **安全**：内置桌面备份与一键还原，收纳 / 分类误操作可回退。
- **顺手**：待办提醒、全盘搜索、快捷操作（双击隐藏图标、空白拉框建盒）随手可用。
- **个性**：深色 / 浅色主题、背景透明度、圆角、强调色均可调，并实时联动全部窗口。

---

## 二、核心特性

| 模块 | 能力 |
|---|---|
| **桌面整理层（Dock）** | 无边框分层悬浮窗，玻璃拟态，承载图标网格与分类标签 |
| **收纳盒（Fence）** | 可折叠 / 拖拽 / 重排的图标容器，多实例，局部刷新无卡顿 |
| **分类管理** | 增删分类、跨分类移动、页缓存复用，切换零延迟 |
| **桌面备份与还原** | 双配置文件（主设置 + 分类映射）整体快照与恢复 |
| **待办与提醒** | 待办清单、到期喇叭闪烁提醒、闹铃开关 |
| **全盘搜索** | 桌面 / 常用目录异步搜索，原子令牌防过期结果串扰 |
| **快捷操作** | 双击桌面隐藏图标、空白处拉框就地确认建盒 |
| **自动对齐** | 磁吸 + 网格吸附、顶部留白、Alt 逃生，窗口摆放自对齐 |
| **主题系统** | 深色 / 浅色、透明度、圆角、强调色，位图与 QSS 双通道联动 |
| **崩溃取证** | 统一诊断日志（trace / crash），可由 `traceEnabled` 一键开关 |

---

## 三、系统架构

程序由若干**无边框顶层窗口**构成，全部位于桌面图标层（band）之上，借助 Windows 原生
`SetWindowPos` / `DWM` 实现分层与透明度，并通过**系统级低级钩子（Low-Level Hooks）**
与 **Raw Input** 采集鼠标输入（避免钩子拖累系统光标）。主要子系统：

```
┌──────────────────────────────────────────────────────────┐
│                      main() 启动入口                       │
├──────────────────────────────────────────────────────────┤
│  桌面整理层  IconGridWindow / DockIconButton / DesktopItem  │
│  收纳盒层    FenceBox / DesktopMirrorWindow (band 锚点)     │
│  助手层      AssistantWindow / SidePanelWidget             │
├──────────────────────────────────────────────────────────┤
│  基础服务    ThemeManager / SettingsManager / CategoryStore │
│             TodoStore / RegionData / CrashTrace / DiagTrace│
├──────────────────────────────────────────────────────────┤
│  系统桥接    ShellOps / DesktopScanner / DesktopImmunity    │
│             WindowSnap / LowLevelHookManager / RawMouse    │
├──────────────────────────────────────────────────────────┤
│  Win32 API   shell32 / user32 / dwmapi / comctl32 / gdi32  │
└──────────────────────────────────────────────────────────┘
```

更详细的产品设计与实现说明见 [`docs/DestopTools产品设计与实现文档.html`](docs/DestopTools产品设计与实现文档.html)
与 [`docs/功能设计总览.md`](docs/功能设计总览.md)。

---

## 四、目录结构

```
DestopTools/
├── DestopTools/            # 应用源码（Qt 工程根）
│   ├── DestopTools.pro     # qmake 工程文件（官方构建入口）
│   ├── main.cpp            # 启动入口
│   ├── *.h / *.cpp         # 40+ 模块源码
│   ├── images/icons/       # 图标资源（含生成脚本）
│   ├── resources.qrc       # 资源集合
│   └── mainwindow.ui
├── tools/                  # 构建与开发工具
│   ├── build.py            # 【唯一编译入口】MSVC 命令行构建
│   ├── pack.py             # 打包脚本
│   ├── make_icon.py        # 应用图标生成
│   ├── addr2func.py        # 崩溃符号（偏移→函数名）反查
│   ├── assert_*.py         # 构建产物特征断言
│   ├── rawinput_selftest.py# Raw Input 自检
│   └── <probe>/            # 各专项探针小程序（透明度 / 滑块 / 菜单…）
├── docs/                   # 文档
│   ├── DestopTools产品设计与实现文档.html
│   ├── 功能设计总览.md
│   ├── assets/             # 文档配图
│   └── dev-reports/        # 历史开发 / 修复报告归档
├── .gitignore
├── README.md
├── LICENSE                 # GPL-3.0
├── CONTRIBUTING.md
├── SECURITY.md
├── CHANGELOG.md
└── CODE_OF_CONDUCT.md
```

---

## 五、环境依赖

| 依赖 | 版本 / 说明 |
|---|---|
| 操作系统 | Windows 10 及以上（依赖 DWM / Win32 桌面合成） |
| Qt | **5.14.2**，`msvc2017_64` 套件（Widgets / GUI / Network / Concurrent / WinExtras） |
| 编译器 | Visual Studio 2022（MSVC 14.4x），64 位 |
| Windows SDK | 10.0.22621.0（含 ucrt / um / shared 头与库） |
| 构建工具 | `qmake` + `nmake`（由 Qt / VS 提供） |
| Python | 3.x（仅用于 `tools/` 下的构建与脚本，非运行期依赖） |

---

## 六、构建指南

> ⚠️ **路径说明**：`tools/build.py` 与 `DestopTools/build_msvc.bat` 中写死了本机 Qt / VS / SDK 的
> **绝对路径**（如 `D:\Qt\Qt5.14.2`、`F:\Program Files\Microsoft Visual Studio\2022\Community`）。
> 克隆到你的机器后，请先按本机实际安装位置修改这些常量，再执行构建。

### 方式一：Python 构建脚本（推荐）

```bat
REM 在「VS 2022 x64 Native Tools」命令提示符下，于仓库根目录执行：
python tools/build.py
```

脚本会自动完成：`qmake -spec win32-msvc CONFIG+=release` → 补丁 Makefile → `nmake release`，
产物位于 `DestopTools/release/DestopTools.exe`。

### 方式二：手动 qmake + nmake

```bat
set QTDIR=D:\Qt\Qt5.14.2\5.14.2\msvc2017_64
set PATH=%QTDIR%\bin;%PATH%
cd DestopTools
qmake DestopTools.pro -spec win32-msvc "CONFIG+=release" "CONFIG-=debug"
nmake release
```

> 注：仓库根另有 `DestopTools/CMakeLists.txt` 作为**实验性备用**方案（仅覆盖部分源文件），
> 官方构建请以 `DestopTools.pro` 为准。

---

## 七、运行与配置

直接运行 `DestopTools/release/DestopTools.exe` 即可。首次启动会在
`%APPDATA%\DestopTools\DestopTools.ini` 生成配置。

### 诊断日志开关

所有诊断日志（含崩溃日志）统一由 INI 开关控制，**默认关闭**：

```ini
[Diagnostics]
traceEnabled=false   ; true = 生成 trace.log 与 crash.log；false = 完全不写盘
```

设为 `true` 后，日志落在 exe 同目录（`DestopTools/release/`）。

### 运行时依赖的数据文件
- **`DestopTools/region.json`**：省 / 市 / 区三级行政区数据，`regiondata.cpp` 在程序启动时从 exe 所在目录（及上级目录）加载，**必须与 exe 一同分发**，否则地点匹配功能不可用。
- **`DestopTools/images/icons/`**：应用图标资源（`.png`）。其矢量源文件位于 **`images/icons/src/*.svg`**，可用 `images/icons/rasterize_icons.cpp` / `make_feedback_png.py` 重新栅格化生成。

---

## 八、文档索引

- **产品设计与实现文档**（HTML，含产品介绍 / 功能设计 / 功能实现）：[`docs/DestopTools产品设计与实现文档.html`](docs/DestopTools产品设计与实现文档.html)
- **功能设计总览**（工程索引 + 60 条功能清单 + 报告索引）：[`docs/功能设计总览.md`](docs/功能设计总览.md)
- **历史开发 / 修复报告归档**：[`docs/dev-reports/`](docs/dev-reports/)

---

## 九、许可证

本项目以 **GNU General Public License v3.0 (GPL-3.0)** 发布。
详见 [`LICENSE`](LICENSE)。

版权所有 © 2026 谭征（Tan Zheng）。基于 Qt 5.14.2 开源版构建，遵循其开源许可条款。

---

## 十、贡献与联系

欢迎通过 Issue / Pull Request 参与贡献，详见 [`CONTRIBUTING.md`](CONTRIBUTING.md)。
安全漏洞请按 [`SECURITY.md`](SECURITY.md) 流程私报，勿公开 Issue。

---

## 十一、免责声明

- 本项目为个人开源作品，**与 360 公司无任何隶属或关联关系**，名称"桌面整理助手"仅为功能描述。
- 软件按"现状"提供，使用本工具整理 / 备份桌面数据前，请自行确认重要文件已另行备份。
- Qt 及相关库版权归 The Qt Company 所有，遵循其开源许可；本仓库仅包含本项目自有源码与资源。
