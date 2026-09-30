# -*- coding: utf-8 -*-
"""扫描 DestopTools 全部 UI 源码里的“文字颜色”定义，汇总哪些不是白色。
用法: python scan_text_colors.py > out.txt
"""
import os
import re
import sys

ROOT = r'F:/Desktop/DestopTools/DestopTools'

# 只匹配独立的 `color:`（排除 background-color / border-color / selection-color 等）
CSS_COLOR = re.compile(
    r'(?<![-\w])color\s*:\s*(rgba?\([^)]*\)|#[0-9A-Fa-f]{3,8}|\bwhite\b|\bgray\b|\bgrey\b|\bsilver\b|\blightgray\b)',
    re.I)
# 代码里直接给文本/前景设色
CODE_COLOR = re.compile(
    r'(setForeground|setPen|setTextColor|QPalette::\w+|\bsetColor\s*\(\s*QPalette)'
    r'[^\n]{0,140}?(QColor\s*\(\s*(?:"[^"]*"|\'[^\']*\')|#[0-9A-Fa-f]{6})')

WHITEISH = {'#ffffff', '#fff', 'white', 'rgba(255,255,255,1)', 'rgba(255, 255, 255, 1)',
            'rgb(255,255,255)', 'rgb(255, 255, 255)'}


def norm(tok):
    return re.sub(r'\s+', '', tok).lower()


def is_white(tok):
    n = norm(tok)
    if n in WHITEISH:
        return True
    m = re.fullmatch(r'rgba\(255,255,255,(\d*\.?\d+)\)', n)
    if m:
        return float(m.group(1)) >= 1.0
    return False


def gray_level(tok):
    """返回 0~255 的等效灰度，用于判断“偏灰”程度；无法判定返回 None。"""
    n = norm(tok)
    m = re.fullmatch(r'#([0-9a-f]{6})', n)
    if m:
        r, g, b = (int(n[1:3], 16), int(n[3:5], 16), int(n[5:7], 16))
        return (r + g + b) // 3
    m = re.fullmatch(r'rgba?\((\d+),(\d+),(\d+)(?:,(\d*\.?\d+))?\)', n)
    if m:
        r, g, b = int(m.group(1)), int(m.group(2)), int(m.group(3))
        a = float(m.group(4)) if m.group(4) else 1.0
        # 半透明白叠在深色底上 → 等效亮度 = 255*a + 底*a 的反向；这里按深底(约 20)估算
        return int(((r + g + b) / 3) * a + 20 * (1 - a))
    return None


files = []
for dirpath, dirnames, filenames in os.walk(ROOT):
    low = dirpath.lower()
    if 'release' in low or 'debug' in low or '.git' in low:
        continue
    for fn in filenames:
        if fn.lower().endswith(('.cpp', '.h', '.qss')):
            files.append(os.path.join(dirpath, fn))
files.sort()

out = []
total = 0
nonwhite = 0
summary = {}

for p in files:
    rel = os.path.relpath(p, ROOT).replace('\\', '/')
    try:
        src = open(p, encoding='utf-8', errors='replace').read().splitlines()
    except Exception as e:
        out.append('!! 读取失败 %s: %s' % (rel, e))
        continue
    hits = []
    for i, line in enumerate(src, 1):
        stripped = line.strip()
        if stripped.startswith('//') or stripped.startswith('*'):
            continue
        for m in CSS_COLOR.finditer(line):
            tok = m.group(1)
            total += 1
            if is_white(tok):
                continue
            nonwhite += 1
            hits.append((i, 'CSS ', tok, stripped[:120]))
            summary.setdefault(norm(tok), []).append('%s:%d' % (rel, i))
        for m in CODE_COLOR.finditer(line):
            tok = m.group(2).strip('"\'')
            total += 1
            if is_white(tok):
                continue
            nonwhite += 1
            hits.append((i, 'CODE', tok, stripped[:120]))
            summary.setdefault(norm(tok), []).append('%s:%d' % (rel, i))
    if hits:
        out.append('')
        out.append('=' * 78)
        out.append('FILE %s   (%d 处非白)' % (rel, len(hits)))
        out.append('=' * 78)
        for i, kind, tok, ctx in hits:
            g = gray_level(tok)
            gs = ('灰度≈%d' % g) if g is not None else '?'
            out.append('  L%-5d [%s] %-22s %-10s | %s' % (i, kind, tok, gs, ctx))

out.append('')
out.append('#' * 78)
out.append('# 按色值汇总（出现次数 / 位置）')
out.append('#' * 78)
for tok, locs in sorted(summary.items(), key=lambda kv: -len(kv[1])):
    g = gray_level(tok)
    out.append('%-26s %-10s x%-3d %s' % (tok, ('灰度≈%d' % g) if g is not None else '?',
                                         len(locs), ', '.join(locs[:12]) + (' ...' if len(locs) > 12 else '')))

out.insert(0, '总 color 声明 %d 处，其中非白 %d 处（扫描 %d 个文件）' % (total, nonwhite, len(files)))

sys.stdout.reconfigure(encoding='utf-8')
print('\n'.join(out))
