#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
清理 DestopTools 代码注释：
  1) 去掉 AI 味道：emoji、装饰性方框/长横线、带日期的修复标记前缀
  2) 确保每个方法都有注释（.cpp 实现缺注释则补；已有则确保署名）
  3) 每个文件加上 @author 谭征 文件头

只改注释，绝不改代码逻辑。
"""
import re
import os
import glob

ROOT = r"F:\Desktop\DestopTools"
SRC_DIR = os.path.join(ROOT, "DestopTools")
SKIP_DIRS = {".workbuddy", "release", "debug"}
GEN_PREFIXES = ("moc_",)
AUTHOR = "谭征"

# 只保留符号/绘文字区块，避免误删中文破折号等正常标点
EMOJI_RE = re.compile(
    r"[\U0001F000-\U0001FAFF\U00002600-\U000027BF\U0001F1E6-\U0001F1FF"
    r"\U0000FE00-\U0000FE0F\U00002B00-\U00002BFF]"
)
# 修复标记：A7（2026-09-24）： -> A7：
TAG_RE = re.compile(r"([A-Z]\d*)\s*[（(][^（）()]*[）)]\s*：")
# 行首的破折号项目符号
LEAD_DASH_RE = re.compile(r"^(──|——|─)\s*")
# 仅用于判定“整行都是装饰”的字符
DECOR_CHARS = set("=─═║╔╗╚╝╠╣╦╩│")
EDGE_TRIM = "=─═║╔╗╚╝╠╣╦╩│ "

KEYWORDS = {
    "if", "for", "while", "switch", "return", "catch", "throw", "delete",
    "new", "sizeof", "alignof", "using", "typedef", "struct", "class",
    "enum", "namespace", "do", "else", "goto", "case", "default", "try",
}

MEMBER_DEF_RE = re.compile(
    r"^\s*~?([\w]+(?:\s*[\*&]\s*|\s+|<[^>]*>)*)\s+(\w+)::(\w+)\s*\("
)
FREE_DEF_RE = re.compile(
    r"^\s*(?:static\s+|inline\s+|const\s+|virtual\s+|explicit\s+|friend\s+)*"
    r"((?:[\w:]+\s*[*\*&]\s*)+?)(\w+)\s*\("
)


def clean_comment_text(content):
    """content 为去掉 // 之后的文本（不含斜杠本身）。返回清理后的文本。"""
    t = EMOJI_RE.sub("", content)
    t = TAG_RE.sub(r"\1：", t)
    t = LEAD_DASH_RE.sub("", t)
    t = t.strip()
    t = t.lstrip(EDGE_TRIM)
    t = t.rstrip(EDGE_TRIM)
    return t


def is_decorative_only(line):
    s = line.lstrip()
    if not s.startswith("//"):
        return False
    inner = s[2:]
    stripped = inner
    for ch in DECOR_CHARS:
        stripped = stripped.replace(ch, "")
    stripped = stripped.replace("/", "").replace(" ", "").replace("\t", "")
    return stripped == ""


def process_comment_line(line):
    """处理单行 // 注释，返回新行；如果清理后为空则返回 None（删除该行）。"""
    stripped = line.lstrip()
    if stripped.startswith("//"):
        content = stripped[2:]
        newc = clean_comment_text(content)
        if newc == "":
            return None
        indent = line[: len(line) - len(stripped)]
        return indent + "// " + newc
    return line


def has_author(lines):
    for ln in lines[:15]:
        if "谭征" in ln or "@author" in ln:
            return True
    return False


def make_file_header(fname):
    return (
        "/*\n"
        " * @file %s\n" % fname +
        " * @brief 本文件属于 DestopTools 桌面整理工具。\n"
        " * @author " + AUTHOR + "\n"
        " * @date 2026-09-26\n"
        " */\n"
    )


# ---------- 头文件方法注释采集（用于 .cpp 补注释时复用） ----------
def build_header_map(header_files):
    """返回 {方法名: 清理后的首句说明}"""
    mmap = {}
    for hf in header_files:
        try:
            with open(hf, "r", encoding="utf-8") as f:
                lines = f.read().split("\n")
        except Exception:
            continue
        n = len(lines)
        for i, ln in enumerate(lines):
            s = ln.strip()
            if s.startswith("//"):
                # 下一行（跳过空行）若为方法声明，则记录
                j = i + 1
                while j < n and lines[j].strip() == "":
                    j += 1
                if j < n:
                    decl = lines[j].strip()
                    m = re.match(r"^\s*(?:virtual\s+|static\s+|inline\s+|const\s+)*"
                                 r"[\w:<>*&,\s]+\s+(\w+)\s*\(", decl)
                    if m:
                        name = m.group(1)
                        first = clean_comment_text(s[2:])
                        if first and name not in mmap:
                            mmap[name] = first
    return mmap


def gen_summary_from_name(name):
    """根据方法名生成一句自然的中文说明（驼峰拆分 + 词典翻译）。"""
    if name.startswith("~"):
        return "析构函数：释放资源"
    if name and name[0].isupper():
        return "构造函数：初始化对象"

    WORD = {
        "panel": "面板", "size": "尺寸", "event": "事件", "text": "文本", "menu": "菜单",
        "window": "窗口", "category": "分类", "categories": "分类", "item": "条目",
        "items": "条目", "icon": "图标", "icons": "图标", "button": "按钮", "grid": "网格",
        "box": "盒子", "boxes": "盒子", "dock": "Dock", "selection": "选中", "mode": "模式",
        "index": "索引", "path": "路径", "paths": "路径", "state": "状态", "view": "视图",
        "search": "搜索", "rename": "重命名", "name": "名称", "names": "名称", "list": "列表",
        "tab": "标签", "tabs": "标签", "page": "页", "pages": "页", "color": "颜色",
        "colour": "颜色", "font": "字体", "theme": "主题", "setting": "设置",
        "settings": "设置", "config": "配置", "position": "位置", "pos": "位置",
        "rect": "矩形", "geometry": "几何", "geom": "几何", "flag": "标志", "flags": "标志",
        "timer": "定时器", "dialog": "对话框", "widget": "控件", "label": "标签",
        "frame": "边框", "image": "图像", "data": "数据", "store": "存储",
        "manager": "管理器", "mirror": "镜像", "desktop": "桌面", "screen": "屏幕",
        "clip": "裁剪", "ghost": "浮层", "hook": "钩子", "drag": "拖拽", "drop": "投放",
        "resize": "缩放", "collapsed": "折叠", "lock": "锁定", "unlock": "解锁",
        "visible": "可见", "hidden": "隐藏", "current": "当前", "first": "首个",
        "last": "末尾", "next": "下一个", "prev": "上一个", "order": "顺序",
        "count": "数量", "width": "宽度", "height": "高度", "cell": "单元", "row": "行",
        "column": "列", "columns": "列", "margin": "边距", "radius": "圆角",
        "opacity": "透明度", "alpha": "透明度", "accent": "主题色", "badge": "徽章",
        "cursor": "光标", "hover": "悬停", "press": "按下", "release": "松开",
        "move": "移动", "enter": "进入", "leave": "离开", "close": "关闭", "open": "打开",
        "create": "创建", "build": "构建", "apply": "应用", "load": "加载", "save": "保存",
        "update": "更新", "refresh": "刷新", "clear": "清除", "reset": "重置",
        "remove": "移除", "add": "添加", "delete": "删除", "set": "设置", "get": "获取",
        "is": "是否", "has": "是否", "should": "是否", "can": "能否", "show": "显示",
        "hide": "隐藏", "start": "开始", "stop": "停止", "init": "初始化", "enable": "启用",
        "disable": "禁用", "draw": "绘制", "compute": "计算", "find": "查找", "select": "选择",
        "commit": "提交", "cancel": "取消", "activate": "激活", "deactivate": "停用",
        "toggle": "切换", "on": "响应", "emit": "发出", "ensure": "确保", "check": "检查",
        "validate": "校验", "parse": "解析", "resolve": "解析", "query": "查询",
        "register": "注册", "unregister": "注销", "restore": "还原", "backup": "备份",
        "export": "导出", "import": "导入", "sync": "同步", "launch": "启动", "quit": "退出",
        "exit": "退出", "edit": "编辑", "begin": "开始", "end": "结束", "finish": "完成",
        "suspend": "暂停", "resume": "恢复", "park": "压底", "raise": "提层", "clamp": "钳制",
        "detect": "检测", "hit": "命中", "layout": "布局", "relayout": "重排",
        "prepare": "准备", "setup": "初始化", "insert": "插入", "requested": "请求",
        "changed": "变化", "moving": "移动", "pressed": "按下", "released": "松开",
        "double": "双击", "single": "单击", "blank": "空白", "region": "区域",
        "dragged": "拖拽", "dragging": "拖拽", "dropped": "投放", "outside": "外部",
        "internal": "内部", "external": "外部", "auto": "自动", "custom": "自定义",
        "default": "默认", "global": "全局", "local": "本地", "native": "原生",
        "pending": "待处理", "active": "活动", "inactive": "非活动", "valid": "有效",
        "invalid": "无效", "ready": "就绪", "empty": "空", "full": "满", "new": "新建",
        "old": "旧", "source": "源", "target": "目标", "from": "从", "to": "到",
        "categoryorder": "分类顺序", "categorystore": "分类库", "fencebox": "收纳盒",
        "fences": "全屏收纳", "icongrid": "图标网格", "dockicon": "Dock图标",
        "desktopicon": "桌面图标", "snapshot": "快照", "shortcut": "快捷方式",
        "tabswitch": "标签切换", "tag": "标签", "reveal": "显示", "rename": "重命名",
        "timestamp": "时间戳", "throttle": "节流", "debounce": "防抖", "marker": "指示线",
        "band": "条带", "card": "卡片", "shadow": "阴影", "overlay": "浮层",
        "rubber": "框选", "rubberband": "框选", "anchor": "锚点", "range": "范围",
        "exclusive": "独占", "additive": "追加", "tree": "树", "node": "节点",
        "mouse": "鼠标", "expanded": "展开", "expand": "展开", "collapse": "折叠",
        "collapsing": "折叠", "pointer": "指针", "wheel": "滚轮", "key": "按键",
        "keyboard": "键盘", "input": "输入", "output": "输出", "buffer": "缓冲",
        "cache": "缓存", "caching": "缓存", "queue": "队列", "thread": "线程",
        "system": "系统", "shell": "外壳", "application": "应用", "context": "上下文",
        "filter": "过滤", "sort": "排序", "group": "分组", "file": "文件",
        "folder": "文件夹", "drive": "驱动器", "recycle": "回收站", "bin": "回收站",
        "trash": "回收站", "tool": "工具", "virtual": "虚拟", "notify": "通知",
        "rebuild": "重建", "at": "在", "point": "点", "fence": "收纳盒",
        "transparency": "透明度", "transparent": "透明", "opacity": "不透明度",
    }
    VERB = {
        "set", "get", "is", "has", "should", "can", "update", "refresh", "load",
        "save", "store", "persist", "create", "make", "build", "apply", "handle",
        "process", "clear", "remove", "reset", "show", "hide", "start", "stop",
        "init", "enable", "disable", "draw", "compute", "find", "select", "commit",
        "cancel", "activate", "deactivate", "toggle", "on", "emit", "ensure",
        "check", "validate", "parse", "resolve", "query", "register", "unregister",
        "restore", "backup", "export", "import", "sync", "launch", "quit", "exit",
        "edit", "begin", "end", "finish", "suspend", "resume", "park", "raise",
        "clamp", "detect", "hit", "layout", "relayout", "prepare", "setup", "insert",
    }

    s = name
    # 拆分驼峰 + 数字边界
    raw = re.sub(r"(?<=[a-z])(?=[A-Z])", " ", s)
    raw = re.sub(r"(?<=[A-Za-z])(?=\d)", " ", raw)
    raw = re.sub(r"(?<=\d)(?=[A-Za-z])", " ", raw)
    tokens = [t for t in raw.split(" ") if t]
    low = [t.lower() for t in tokens]
    # 翻译每个 token（未知则保留原英文）
    zh = [WORD.get(t, t) for t in low]

    # 信号/事件后缀
    if low and low[-1] in ("changed", "requested", "triggered"):
        suffix = {"changed": "变化信号", "requested": "请求信号", "triggered": "触发信号"}[low[-1]]
        head = zh[:-1]
        # 去掉可能重复的“变化/请求/触发”
        if head and head[-1] in ("变化", "请求", "触发"):
            head = head[:-1]
        return "".join(head) + suffix
    if low and low[-1] == "event":
        return "".join(zh[:-1]) + "事件"

    first = low[0] if low else ""
    if first in VERB:
        verb = WORD.get(first, first)
        rest = zh[1:]
        phrase = "".join(rest)
        if first in ("is", "has", "should", "can"):
            return "判断" + ("".join(rest) if rest else "状态")
        if first == "on":
            return "响应" + phrase
        if first in ("set", "get", "apply", "update", "refresh", "load", "save",
                     "clear", "reset", "remove", "create", "build", "show", "hide",
                     "start", "stop", "enable", "disable", "draw", "compute", "find",
                     "select", "commit", "cancel", "toggle", "register", "unregister",
                     "restore", "backup", "sync", "launch", "quit", "exit", "edit",
                     "suspend", "resume", "park", "raise", "clamp", "detect", "hit",
                     "layout", "relayout", "prepare", "setup", "insert", "resolve",
                     "query", "parse", "handle", "process", "activate", "deactivate",
                     "begin", "end", "finish", "ensure", "check", "validate",
                     "emit", "add", "delete", "open", "close", "move", "enter",
                     "leave", "draw"):
            return verb + phrase
        return verb + phrase

    # 普通名词短语（方法名本身即名词，如 panel / expandedSize）
    return "".join(zh)


def preprocess_for_defs(lines):
    """标记每一行是否为注释行（仅 // 与 /* 起始），返回 list of bool。"""
    is_comment = []
    in_block = False
    for ln in lines:
        s = ln.lstrip()
        if in_block:
            is_comment.append(True)
            if "*/" in ln:
                in_block = False
            continue
        if s.startswith("//"):
            is_comment.append(True)
        elif s.startswith("/*"):
            is_comment.append(True)
            if "*/" not in ln:
                in_block = True
        else:
            is_comment.append(False)
    return is_comment


def attached_comment(lines, i, is_comment):
    """返回方法 i 行上方紧邻的连续注释行索引列表（从近到远）。"""
    idxs = []
    j = i - 1
    while j >= 0:
        if is_comment[j]:
            idxs.append(j)
            j -= 1
            continue
        if lines[j].strip() == "":
            # 允许最多一个空行仍视为“紧邻”
            if not idxs:
                j -= 1
                continue
            else:
                break
        break
    return idxs


def ensure_method_author_and_comments(lines, is_comment, mmap):
    """就地处理 .cpp：为缺少注释的方法补注释，并为已有注释补署名。返回 (out, changed)。"""
    out = []
    changed = False
    n = len(lines)
    i = 0
    while i < n:
        ln = lines[i]
        s = ln.lstrip()
        # 跳过注释行与空行（注释行已在之前迭代原样输出）
        if is_comment[i] or s == "":
            out.append(ln)
            i += 1
            continue

        # 顶层定义才处理：方法定义都在第 0 列；函数体内的调用/语句都带缩进
        if ln[0] in (" ", "\t"):
            out.append(ln)
            i += 1
            continue

        mdef = MEMBER_DEF_RE.search(ln)
        fdef = None
        if not mdef:
            fdef = FREE_DEF_RE.search(ln)
        if mdef:
            name = mdef.group(3)
            is_def = True
        elif fdef:
            name = fdef.group(2)
            is_def = True
        else:
            is_def = False

        if is_def:
            # 行尾分号 -> 声明/调用，不是定义
            if ln.rstrip().endswith(";"):
                is_def = False
            if name in KEYWORDS:
                is_def = False
            # 排除出现在表达式中的调用（前面有 = ( , . -> 等）
            pre = ln[: ln.find(name)]
            if re.search(r"[=(),.>]\s*$", pre.rstrip()):
                is_def = False

        if not is_def:
            out.append(ln)
            i += 1
            continue

        # 判定上方是否已有紧邻注释
        idxs = attached_comment(lines, i, is_comment)
        if idxs:
            has_a = any(("谭征" in lines[k]) or ("@author" in lines[k]) for k in idxs)
            if not has_a:
                out.append("// 作者：" + AUTHOR)
                changed = True
            out.append(ln)
            i += 1
            continue
        else:
            summary = mmap.get(name)
            if not summary:
                summary = gen_summary_from_name(name)
            indent = ln[: len(ln) - len(s)]
            out.append(indent + "// " + summary)
            out.append(indent + "// 作者：" + AUTHOR)
            out.append(ln)
            changed = True
            i += 1
            continue

    return out, changed


def process_cpp(path, mmap):
    with open(path, "r", encoding="utf-8") as f:
        raw = f.read()
    lines = raw.split("\n")
    is_comment = preprocess_for_defs(lines)
    new_lines, changed = ensure_method_author_and_comments(lines, is_comment, mmap)
    # 清理注释行
    final = []
    removed = 0
    for ln in new_lines:
        # 仅对 // 行做清理（块注释行已在 is_comment 标记，但块注释内部 emoji 也清）
        s = ln.lstrip()
        if s.startswith("//"):
            nl = process_comment_line(ln)
            if nl is None:
                removed += 1
                continue
            final.append(nl)
        else:
            # 去掉代码行中可能残留的 emoji（几乎不会有，但保险）
            final.append(EMOJI_RE.sub("", ln))
    if not has_author(final):
        final.insert(0, make_file_header(os.path.basename(path)))
        changed = True
    return "\n".join(final), changed, removed


def is_method_decl_line(ln):
    s = ln.strip()
    if not s or s.startswith("//") or s.startswith("#"):
        return False
    if re.search(r"\b(using|typedef|enum|struct|class|namespace|return|if|for|while|"
                 r"switch|catch|throw|delete|new|sizeof|template|typedef)\b", s):
        return False
    # 等号出现在首个左括号之前，多半是变量/赋值，不是方法声明
    head = s.split("(", 1)[0]
    if "=" in head:
        return False
    m = re.match(
        r"^(?:virtual\s+|static\s+|inline\s+|const\s+|explicit\s+|friend\s+|"
        r"signals\s+|slots\s+|Q_SIGNALS\s+|public\s+|private\s+|protected\s+)*"
        r"([\w:]+(?:<[^>]*>)?[<>*&,\s]*?)\s+(\w+)\s*\(",
        s,
    )
    if not m:
        return False
    name = m.group(2)
    if name in KEYWORDS:
        return False
    return True


def add_header_method_comments(lines):
    """为头文件中缺少注释的方法声明（类作用域内）补一句简要说明。"""
    out = []
    depth = 0
    for i, ln in enumerate(lines):
        s = ln.strip()
        if is_method_decl_line(ln) and depth == 1:
            commented = ("//" in ln)
            j = i - 1
            while j >= 0 and lines[j].strip() == "":
                j -= 1
            if j >= 0 and lines[j].strip().startswith("//"):
                commented = True
            if not commented:
                mname = re.search(r"(\w+)\s*\(", s)
                name = mname.group(1) if mname else s
                summ = gen_summary_from_name(name)
                indent = ln[: len(ln) - len(s)]
                out.append(indent + "// " + summ)
        out.append(ln)
        depth += ln.count("{") - ln.count("}")
    return out


def process_h(path):
    with open(path, "r", encoding="utf-8") as f:
        raw = f.read()
    lines = raw.split("\n")
    lines = add_header_method_comments(lines)
    final = []
    removed = 0
    for ln in lines:
        s = ln.lstrip()
        if s.startswith("//"):
            nl = process_comment_line(ln)
            if nl is None:
                removed += 1
                continue
            final.append(nl)
        else:
            final.append(EMOJI_RE.sub("", ln))
    if not has_author(final):
        # 头文件：把文件头插到最前（在 #ifndef 之前）
        final.insert(0, make_file_header(os.path.basename(path)))
    return "\n".join(final), removed


def collect_files():
    files = []
    for dirpath, dirnames, filenames in os.walk(SRC_DIR):
        # 也允许 tools 目录（属于“所有代码”）
        rel = os.path.relpath(dirpath, ROOT)
        # 跳过备份与构建目录
        parts = set(rel.split(os.sep))
        if parts & SKIP_DIRS:
            continue
        for fn in filenames:
            if fn.startswith(GEN_PREFIXES):
                continue
            if fn.endswith((".h", ".cpp")):
                files.append(os.path.join(dirpath, fn))
    # 额外包含 tools 下的源文件（在 ROOT/tools 下）
    for dirpath, dirnames, filenames in os.walk(os.path.join(ROOT, "tools")):
        parts = set(os.path.relpath(dirpath, ROOT).split(os.sep))
        if parts & SKIP_DIRS:
            continue
        for fn in filenames:
            if fn.startswith(GEN_PREFIXES):
                continue
            if fn.endswith((".h", ".cpp")):
                files.append(os.path.join(dirpath, fn))
    return sorted(set(files))


def main():
    files = collect_files()
    header_files = [f for f in files if f.endswith(".h")]
    mmap = build_header_map(header_files)
    print("头文件方法说明采集：%d 条" % len(mmap))
    total_changed = 0
    total_removed = 0
    for f in files:
        if f.endswith(".h"):
            new_text, removed = process_h(f)
        else:
            new_text, changed, removed = process_cpp(f, mmap)
        with open(f, "r", encoding="utf-8") as fh:
            old = fh.read()
        if new_text != old:
            with open(f, "w", encoding="utf-8") as fh:
                fh.write(new_text)
            total_changed += 1
        total_removed += removed
    print("处理文件：%d，改动：%d，删除纯装饰行：%d" % (len(files), total_changed, total_removed))


if __name__ == "__main__":
    main()
