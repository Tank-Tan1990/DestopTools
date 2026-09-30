# -*- coding: utf-8 -*-
"""生成「喇叭（扬声器）」提示音图标 —— images/icons/speaker_1x.png(24) / speaker_2x.png(48)。

为什么要有生成器：项目 images/icons 下的图标都是**单色** 24/48 两档切图，运行时用
`Theme::recolorPixmap()` 染成需要的颜色。手改 PNG 不可复现，故留脚本。

设计：设计坐标基于 96×96 画布（喇叭体多边形 + 右侧一条声波弧），
按 16 倍超采样绘制再 LANCZOS 降采样 —— Pillow 的 polygon/arc **不做抗锯齿**，
直接在小画布上画边缘会有明显阶梯。

用法：python.exe tools/make_speaker_icon.py [--preview 输出目录]
"""
import argparse
import os
from PIL import Image, ImageDraw

# 设计坐标基准画布 96×96；图形整体居中（x 25..71，y 24..72）
DESIGN = 96.0
SS = 16                      # 超采样倍率
WHITE = (255, 255, 255, 255)
ICON_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "DestopTools", "images", "icons")


def _draw(canvas_px):
    img = Image.new("RGBA", (canvas_px, canvas_px), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    k = canvas_px / DESIGN

    def P(x, y):
        return (x * k, y * k)

    # 喇叭体：左侧箱体（竖矩形）+ 右侧张开的喇叭口
    #   多边形顶点顺序：(25,38)→(37,38)→(53,24)→(53,72)→(37,58)→(25,58)
    body = [P(25, 38), P(37, 38), P(53, 24), P(53, 72), P(37, 58), P(25, 58)]
    d.polygon(body, fill=WHITE)

    # 声波弧：圆心 (53,48)，半径 18，-52°..52°（Pillow 角度：3 点钟为 0、顺时针为正）
    r = 18.0
    cx, cy = 53.0, 48.0
    bbox = [(cx - r) * k, (cy - r) * k, (cx + r) * k, (cy + r) * k]
    d.arc(bbox, start=-52, end=52, fill=WHITE, width=max(1, round(7 * k)))
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", default=None, help="额外输出 4 倍放大预览图的目录（自查用）")
    args = ap.parse_args()

    out_dir = os.path.normpath(ICON_DIR)
    os.makedirs(out_dir, exist_ok=True)
    for size, tag in ((24, "1x"), (48, "2x")):
        big = _draw(size * SS)
        icon = big.resize((size, size), Image.LANCZOS)
        path = os.path.join(out_dir, "speaker_%s.png" % tag)
        icon.save(path)
        print("wrote %s (%dx%d)" % (path, size, size))
        if args.preview:
            os.makedirs(args.preview, exist_ok=True)
            icon.resize((size * 6, size * 6), Image.NEAREST).save(
                os.path.join(args.preview, "preview_speaker_%s.png" % tag))


if __name__ == "__main__":
    main()
