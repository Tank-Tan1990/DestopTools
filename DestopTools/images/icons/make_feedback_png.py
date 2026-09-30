#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Fallback rasterizer for the 'feedback' icon.

The full Qt toolchain (qmake / nmake / qsvg plugin) was used in the main
pipeline (gen_icons.py -> rasterize_icons.cpp -> rasterize_icons.exe) to slice
all vector icons into @1x/@2x PNGs. This script exists only as a lightweight,
dependency-free fallback to (re)generate the two 'feedback' PNGs when the Qt
SDK is not on PATH, so the resource file (icons.qrc) stays valid and the
project still compiles. The vector source of truth remains
images/icons/src/feedback.svg.

Output matches the icon set's 24x24 viewBox, rounded-line, cyan (#22D3EE) style.
"""
import os
import math

# Pure-Python canvas (no Pillow / Cairo / Qt required).
class Canvas:
    def __init__(self, size):
        self.size = size
        self.scale = size / 24.0
        # RGBA pixels, transparent background
        self.px = [[(0, 0, 0, 0) for _ in range(size)] for _ in range(size)]

    def _t(self, x, y):
        return int(round(x * self.scale)), int(round(y * self.scale))

    def _blend(self, x, y, rgba):
        if 0 <= x < self.size and 0 <= y < self.size:
            r, g, b, a = rgba
            if a <= 0:
                return
            br, bg, bb, ba = self.px[y][x]
            af = a / 255.0
            nr = int(r * af + br * (1 - af))
            ng = int(g * af + bg * (1 - af))
            nb = int(b * af + bb * (1 - af))
            na = max(a, ba)
            self.px[y][x] = (nr, ng, nb, na)

    def line(self, x0, y0, x1, y1, width, color):
        dx = x1 - x0
        dy = y1 - y0
        dist = math.hypot(dx, dy)
        if dist == 0:
            return
        steps = int(dist * self.scale) + 1
        nx = -dy / dist
        ny = dx / dist
        half = (width * self.scale) / 2.0
        for i in range(steps + 1):
            t = i / steps
            cx = x0 + dx * t
            cy = y0 + dy * t
            pcx, pcy = self._t(cx, cy)
            for sx in range(int(-half - 1), int(half + 2)):
                for sy in range(int(-half - 1), int(half + 2)):
                    d = math.hypot(sx + 0.5, sy + 0.5)
                    if d <= half + 0.6:
                        self._blend(pcx + sx, pcy + sy, color)

    def circle(self, cx, cy, r, color, filled=True):
        pcx, pcy = self._t(cx, cy)
        pr = r * self.scale
        rad = int(math.ceil(pr)) + 1
        for sx in range(-rad, rad + 1):
            for sy in range(-rad, rad + 1):
                d = math.hypot(sx, sy)
                if filled:
                    if d <= pr + 0.5:
                        self._blend(pcx + sx, pcy + sy, color)
                else:
                    if pr - 0.8 <= d <= pr + 0.8:
                        self._blend(pcx + sx, pcy + sy, color)

    def round_rect(self, x, y, w, h, radius, width, color):
        self.line(x + radius, y, x + w - radius, y, width, color)
        self.line(x + radius, y + h, x + w - radius, y + h, width, color)
        self.line(x, y + radius, x, y + h - radius, width, color)
        self.line(x + w, y + radius, x + w, y + h - radius, width, color)
        # corners
        for (ccx, ccy, a0) in [
            (x + radius, y + radius, 180), (x + w - radius, y + radius, 270),
            (x + w - radius, y + h - radius, 0), (x + radius, y + h - radius, 90),
        ]:
            self._arc(ccx, ccy, radius, a0, a0 + 90, width, color)

    def _arc(self, cx, cy, r, a0, a1, width, color):
        pcx, pcy = self._t(cx, cy)
        pr = r * self.scale
        half = (width * self.scale) / 2.0
        steps = int((a1 - a0) * pr / 3) + 4
        for i in range(steps + 1):
            ang = math.radians(a0 + (a1 - a0) * i / steps)
            for sx in range(int(-half - 1), int(half + 2)):
                for sy in range(int(-half - 1), int(half + 2)):
                    d = math.hypot(sx + 0.5, sy + 0.5)
                    if d <= half + 0.6:
                        x = int(round(pr * math.cos(ang))) + sx
                        y = int(round(pr * math.sin(ang))) + sy
                        self._blend(pcx + x, pcy + y, color)

    def save(self, path):
        # Write a minimal PNG (truecolor + alpha) without external deps.
        size = self.size
        raw = bytearray()
        for y in range(size):
            raw.append(0)  # filter type 0
            for x in range(size):
                r, g, b, a = self.px[y][x]
                raw += bytes((r, g, b, a))
        self._write_png(path, size, size, bytes(raw))

    @staticmethod
    def _write_png(path, w, h, raw):
        import zlib
        import struct

        def chunk(tag, data):
            c = struct.pack(">I", len(data)) + tag + data
            crc = zlib.crc32(tag + data) & 0xFFFFFFFF
            return c + struct.pack(">I", crc)

        sig = b"\x89PNG\r\n\x1a\n"
        ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
        idat = zlib.compress(raw, 9)
        with open(path, "wb") as f:
            f.write(sig)
            f.write(chunk(b"IHDR", ihdr))
            f.write(chunk(b"IDAT", idat))
            f.write(chunk(b"IEND", b""))


CYAN = (34, 211, 238, 255)


def draw_feedback(size):
    c = Canvas(size)
    # chat bubble outline (matches SVG)
    c.round_rect(4, 5.5, 16, 10, 2.4, 1.8, CYAN)
    # tail
    c.line(9, 15.5, 5, 19, 1.8, CYAN)
    c.line(5, 19, 5, 16, 1.8, CYAN)
    # three dots
    dot_r = 1.1 if size < 48 else 1.1
    for dx in (9, 12, 15):
        c.circle(dx, 10.5, dot_r, CYAN, filled=True)
    return c


def main():
    here = os.path.abspath(os.path.dirname(__file__))
    for scale in (1, 2):
        size = 24 * scale
        canvas = draw_feedback(size)
        out = os.path.join(here, "feedback_%dx.png" % scale)
        canvas.save(out)
        print("wrote", out)


if __name__ == "__main__":
    main()
