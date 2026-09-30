# 发布到 GitHub —— 操作指引（PUBLISH）

> 本目录（`githubcode/`）已是一个 **初始化完成、含 8 个提交、310 个文件、工作区干净的 Git 仓库**。
> 源码、文档、许可证（GPL-3.0）、社区模板均已就位，本地 `origin` 远端也已指向：
> `https://github.com/Tank-Tan1990/DestopTools.git`
> 只差最后一步「推送到 GitHub 远端」。

---

## ⚠️ 最重要：GitHub 已停用「账号密码」推送，必须用 Personal Access Token (PAT)

你提供的 GitHub **登录密码无法直接用于 `git push`**（GitHub 自 2021-08 起禁用密码认证，改用令牌）。
推送时「密码」那一栏要填的是 **PAT**，不是登录密码。

此外，**明文密码已出现在对话记录中，建议你在 GitHub 改一次密码**（Settings → Password）以策安全。

---

## 第一步：生成 PAT（约 1 分钟）

1. 打开 https://github.com/settings/tokens/new
2. **Note**：`DestopTools-publish`（随便起）
3. **Expiration**：选 `7 days`（用完可删，最安全）
4. **Scopes**：只勾 **`repo`**（完整控制私有/公开仓库）即可
5. 拉到底点 **Generate token**
6. **立刻复制**那串 `ghp_xxxxxxxx...` —— 只显示一次，关掉就没了

---

## 第二步：在 GitHub 上建空仓库（仓库目前还不存在）

1. 打开 https://github.com/new
2. 填写：
   - **Repository name**：`DestopTools`
   - **Description**：`Windows 桌面整理工具（Qt 5.14.2 / MSVC）—— 磁吸 Dock、收纳盒、桌面助手、待办、文件搜索等`
   - **Visibility**：**Public**
   - ⚠️ **不要**勾选 "Add a README file" / "Add .gitignore" / "Choose a license" —— 这些本仓库已包含，避免首次 push 冲突
3. 点 **Create repository**（建完**不用**在里面做别的，直接关掉）

---

## 第三步：推送（二选一）

### 方案 A：把 PAT 给我，我直接在沙箱里推（推荐，网络已验证可通）
把第一步复制的 `ghp_...` 令牌发给我，我会在这里执行：
```bash
# 用 PAT 作为密码推送（remote 已设好）
git -C "F:/Desktop/DestopTools/githubcode" push -u origin main
```
（推送时用户名填 `Tank-Tan1990`，密码/令牌填你给的 PAT。）

### 方案 B：你自己在本机终端推
```bash
cd F:\Desktop\DestopTools\githubcode
git push -u origin main
# 提示输入用户名 → 填 Tank-Tan1990
# 提示输入密码 → 填【你的 PAT】，不是登录密码
```

---

## 第四步：验证（网页端可见）

```bash
git ls-remote origin   # 确认远端已收到 main
```
浏览器打开 **https://github.com/Tank-Tan1990/DestopTools** ，应能看到：
- `README.md`（徽章 + 项目介绍）
- `DestopTools/`（应用源码）、`tools/`（构建脚本）、`docs/`（文档）
- `LICENSE`（GPL-3.0）、`CONTRIBUTING.md`、`SECURITY.md`、`CHANGELOG.md`、`CODE_OF_CONDUCT.md`
- `.github/`（Issue / PR 模板）

---

## 第五步：桌面端查看（GitHub Desktop）

装好 [GitHub Desktop](https://desktop.github.com/) 后任选一种：

- **方式 1（仓库已推上去后克隆）**：
  GitHub Desktop → **File → Clone repository** → URL 标签页 → 粘贴
  `https://github.com/Tank-Tan1990/DestopTools.git` → Clone。即可在桌面端浏览/管理。
- **方式 2（直接接管本地文件夹）**：
  GitHub Desktop → **File → Add Local Repository** → 选 `F:\Desktop\DestopTools\githubcode`
  → 若提示未发布，点 **Publish to GitHub**（在 GitHub Desktop 里登录你的账号即可）。

---

## 发布 Release（可选）

1. 仓库页点 **Releases → Draft a new release**
2. **Tag**：`v1.0.0`，**Target**：`main`
3. **Title**：`DestopTools 1.0.0`，正文从 `CHANGELOG.md` 的 `[1.0.0]` 段落粘贴
4. 可把本地 `tools/pack.py` 打好的 `release/DestopTools.zip` 作为附件上传
5. 点 **Publish release**

---

## ⚠️ 克隆者须知（README 已写明）

- 构建唯一入口 `tools/build.py`（`qmake -spec win32-msvc` + `nmake release`）。
- 该脚本与部分源码**写死了作者本机绝对路径**（Qt / VS2022 / Windows SDK），克隆后需按自己机器改路径才能编译。
- 本仓库**不含**任何用户配置（`%APPDATA%\DestopTools\DestopTools.ini`）与运行期日志（`.log`），运行时由程序在本机自动生成，已被 `.gitignore` 排除。

---

## 遇 `failed to push` / `non-fast-forward`

说明远端仓库创建时带了初始提交（误勾了 README）。处理：
```bash
git -C "F:/Desktop/DestopTools/githubcode" pull origin main --allow-unrelated-histories
git -C "F:/Desktop/DestopTools/githubcode" push -u origin main
```
或直接删掉远端仓库重建为空仓库再 push（最干净）。
