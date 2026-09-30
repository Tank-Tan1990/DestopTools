# 贡献指南（CONTRIBUTING）

感谢你考虑为 **DestopTools** 贡献代码或文档！本文件说明参与方式与本项目的工程约定。
参与本仓库即表示你同意遵守 [行为准则](CODE_OF_CONDUCT.md)。

---

## 一、如何开始

1. **Fork** 本仓库到你的 GitHub 账号。
2. 克隆你的 Fork，并**基于 `main` 分支**创建特性 / 修复分支：
   - 新功能：`feature/<简短描述>`
   - 缺陷修复：`fix/<简短描述>`
3. 在本机按 [README 构建指南](README.md#六构建指南) 完成编译与运行，确保基线可跑。
4. 提交改动，推送分支，发起 **Pull Request (PR)**。

---

## 二、工程约定（务必遵守）

这些是本项目在多次迭代中确立、且与构建正确性直接相关的硬规则，请勿"顺手优化"：

### 1. 构建方法唯一且固定
- **唯一编译入口**是 `tools/build.py`（等价于 `qmake -spec win32-msvc + nmake release`）。
- 禁止改用 MinGW / Qt Creator / `mingw32-make`。
- 禁止添加"跳过 qmake 只跑 nmake"的快捷分支——会漏编引用了新增头文件的翻译单元（TU）。
- 每次改动后必须重新 `qmake`（哪怕只加了头文件），否则新文件不进依赖表、静默不编译。

### 2. 新增源文件必须登记
- 任何新增 `.h` / `.cpp` 都必须在 `DestopTools/DestopTools.pro` 的 `SOURCES` / `HEADERS`
  中登记；否则不会参与构建。

### 3. 代码风格与注释
- 语言标准：**C++17**，保留 `QT_DEPRECATED_WARNINGS`。
- 注释使用**中文**；头文件方法声明与 `.cpp` 方法实现均应有注释。
- 注释**去 AI 味**：不使用 emoji、不使用装饰性长横线（`═══` / `────`）、不使用带日期的修复标记前缀。
- 方法实现上方署名 `// 作者：谭征`（仓库既有约定，新文件沿用）。

### 4. 主题与透明度联动
- 颜色 / 透明度改动一律走 `Theme` 通道（`Theme::applyTokens` 等），不得回退到 `setWindowOpacity`。
- **位图类图形**（用 `QPainter` 把颜色画进 `QPixmap` 的，如关于页 Logo）**不参与主题联动**，
  必须在 `applyTheme()` 中重建，否则改主题时该图形会停在旧色。
- 圆角一律走 `Theme::radiusPx()`，禁止在 C++ 侧写死数字（0 时 `clearMask`）。

### 5. 提交信息
- 使用中文，以动词开头，必要时关联 Issue（如 `修复：分类切换卡顿（关联 #12）`）。

---

## 三、Pull Request 要求

- PR 描述清楚：**改了什么 / 为什么 / 如何自测**。
- 确认 **`tools/build.py` 能在你的机器上编译通过**（先按本机路径修改脚本中的 Qt / VS / SDK 常量）。
- 不夹带个人配置、日志、构建产物（已被 `.gitignore` 忽略，提交前请 `git status` 自检）。
- 大型重构请先在 Issue 中讨论方案。

---

## 四、报告问题（Issue）

提交 Bug 时请尽量包含：

- 操作系统版本、Qt 版本、编译器版本；
- 问题现象与**复现步骤**；
- 是否可在 `%APPDATA%\DestopTools\DestopTools.ini` 开启 `[Diagnostics] traceEnabled=true` 后复现，并附 `release/DestopTools_trace.log` / `DestopTools_crash.log`（如有）；
- 期望行为与实际行为。

> 安全相关的问题**请勿公开 Issue**，请按 [SECURITY.md](SECURITY.md) 私报。
