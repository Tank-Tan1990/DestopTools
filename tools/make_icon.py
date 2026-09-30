#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""DestopTools 应用图标生成器 —— 复刻「关于我们」页里的显示器 Logo（createAboutLogo），
把原来的 W / P 两个字母换成中文「谭」「征」。

几何与配色逐项对齐 settingcenterdialog.cpp 的 createAboutLogo()（坐标取相对比例，
所以画在正方形画布上也能保持同一套版式）：
    monitor  x 0.15w .. 0.85w , y 0.08h .. 0.63h , r=0.05w
    screen   monitor 内缩 3px + 青蓝竖向渐变 #1B3A6B -> #111A2E
    tile L   sW*0.26 x sH*0.48 @ (sX+0.16sW, sY+0.20sH)  底色 #111A2E , 字 谭 = 强调色
    tile R   sW*0.28 x sH*0.42 @ (sX+0.52sW, sY+0.18sH)  底色 #13406B , 字 征 = 强调色.lighter(115)
    stand    x 0.44w , y 0.66h , 0.12w x 0.10h   强调色 alpha 60
    base     x 0.28w , y 0.76h , 0.44w x 0.08h   强调色 alpha 60
    border   强调色 alpha 120 描边

配色取自用户提供的截图逐像素实测：强调色 = #10B981（翡翠绿）。

用法（托管 venv 的 python，需 Pillow）：
    <venv>/python.exe tools/make_icon.py
产物：
    DestopTools/appicon.ico   多尺寸（16/20/24/32/40/48/64/96/128/256）
    tools/icon_preview.png    256px 预览（供肉眼比对）
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

# ── 配色（来自截图实测）────────────────────────────────────────────────
ACCENT = (16, 185, 129)          # #10B981 主题强调色
ACCENT_LIGHT = (18, 213, 148)    # accent.lighter(115) —— 右块「征」的填充色
WINDOW_BG = (17, 26, 46)         # #111A2E
SCREEN_TOP = (27, 58, 107)       # #1B3A6B
TILE_RIGHT_BG = (19, 64, 107)    # #13406B
BORDER_ALPHA = 120
STAND_ALPHA = 60

# ── 字号（占所在小块短边的比例）────────────────────────────────────────
# 原图实测：W 高 ≈ 小块高 0.36、宽 ≈ 小块宽 0.60。中文字面近似正方，按「宽」对齐取 0.62。
GLYPH_RATIO = 0.62

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.join(os.path.dirname(HERE), "DestopTools")
ICO_PATH = os.path.join(PROJ, "appicon.ico")
PREVIEW_PATH = os.path.join(HERE, "icon_preview.png")

FONT_CANDIDATES = [
    r"C:\Windows\Fonts\msyhbd.ttc",   # 微软雅黑 Bold
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\simhei.ttf",   # 黑体
]


def load_font(size):
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size)
            except Exception:
                continue
    raise SystemExit("找不到可用的中文字体（msyhbd/msyh/simhei）")


def lerp_color(c0, c1, t):
    return tuple(int(round(a + (b - a) * t)) for a, b in zip(c0, c1))


def vertical_gradient(w, h, top, bottom):
    """生成 w x h 的竖向渐变（RGB）。"""
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        d.line([(0, y), (w, y)], fill=lerp_color(top, bottom, y / max(1, h - 1)))
    return img


def rounded_mask(size, box, radius):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle(box, radius=radius, fill=255)
    return m


def draw_logo(S, canvas_scale=None):
    """在 S x S 画布上绘制 Logo，返回 RGBA Image。"""
    w = h = S
    px = lambda v: int(round(v))          # noqa: E731  短别名

    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    sx, sy = px(w * 0.15), px(h * 0.08)
    sw, sh = px(w * 0.70), px(h * 0.55)
    mon_r = max(2, px(w * 0.05))
    stroke = max(1, px(w * 0.00625))      # 原为 160px 画布上的 1px 描边

    # ① 显示器外框：深色底 + 强调色弱描边
    d.rounded_rectangle([sx, sy, sx + sw, sy + sh], radius=mon_r,
                        fill=WINDOW_BG + (255,),
                        outline=ACCENT + (BORDER_ALPHA,), width=stroke)

    # ② 屏幕：内缩 3/160 后铺竖向渐变（渐变图与遮罩都按「屏幕自身尺寸」建，再整体贴到屏幕位置，
    #    避免「全画布尺寸的渐变 + 带偏移的遮罩」两层偏移叠加导致的错位）
    inset = max(1, px(w * 0.01875))       # 原 3px @160
    ix, iy = sx + inset, sy + inset
    iw, ih = max(1, sw - 2 * inset), max(1, sh - 2 * inset)
    screen_r = max(2, px(w * 0.0375))
    screen = vertical_gradient(iw, ih, SCREEN_TOP, WINDOW_BG)
    screen_mask = Image.new("L", (iw, ih), 0)
    ImageDraw.Draw(screen_mask).rounded_rectangle([0, 0, iw - 1, ih - 1], radius=screen_r, fill=255)
    img.paste(screen, (ix, iy), screen_mask)

    # ③ 左小块：暗底 + 「谭」（强调色）
    tile_r = max(2, px(w * 0.025))        # 原 4px @160
    dw, dh = px(sw * 0.26), px(sh * 0.48)
    dx, dy = sx + px(sw * 0.16), sy + px(sh * 0.20)
    d.rounded_rectangle([dx, dy, dx + dw, dy + dh], radius=tile_r, fill=WINDOW_BG + (255,))
    f = load_font(max(8, px(min(dw, dh) * GLYPH_RATIO)))
    d.text((dx + dw / 2, dy + dh / 2), "谭", font=f, fill=ACCENT + (255,), anchor="mm")

    # ④ 右小块：蓝底 + 「征」（强调色提亮）
    pw, ph = px(sw * 0.28), px(sh * 0.42)
    pxx, pyy = sx + px(sw * 0.52), sy + px(sh * 0.18)
    d.rounded_rectangle([pxx, pyy, pxx + pw, pyy + ph], radius=tile_r, fill=TILE_RIGHT_BG + (255,))
    f2 = load_font(max(8, px(min(pw, ph) * GLYPH_RATIO)))
    d.text((pxx + pw / 2, pyy + ph / 2), "征", font=f2, fill=ACCENT_LIGHT + (255,), anchor="mm")

    # ⑤ 支架 + 底座（强调色 alpha 60）
    st = ACCENT + (STAND_ALPHA,)
    d.rounded_rectangle([px(w * 0.44), px(h * 0.66), px(w * 0.44) + px(w * 0.12), px(h * 0.66) + px(h * 0.10)],
                        radius=max(1, px(w * 0.0125)), fill=st)
    d.rounded_rectangle([px(w * 0.28), px(h * 0.76), px(w * 0.28) + px(w * 0.44), px(h * 0.76) + px(h * 0.08)],
                        radius=tile_r, fill=st)
    return img


def render_size(target):
    """按 target 尺寸渲染：先画在 4 倍（且不小于 512）画布上，再 LANCZOS 降采样。"""
    ss = max(target * 4, 512)
    master = draw_logo(ss)
    return master.resize((target, target), Image.LANCZOS)


def main():
    frames = {s: render_size(s) for s in SIZES}
    master = frames[256]
    master.save(ICO_PATH, format="ICO", sizes=[(s, s) for s in SIZES])
    master.save(PREVIEW_PATH, format="PNG")
    print("ICO   ->", ICO_PATH, os.path.getsize(ICO_PATH), "B")
    print("PREVIEW ->", PREVIEW_PATH, os.path.getsize(PREVIEW_PATH), "B")
    with Image.open(ICO_PATH) as check:
        print("ICO sizes:", sorted(check.ico.sizes()))


if __name__ == "__main__":
    main()
