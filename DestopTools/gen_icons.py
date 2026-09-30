#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generate the dark-tech SVG icon set for DestopTools and emit:
  - images/icons/src/<name>.svg          (vector sources)
  - images/icons/icons.qrc               (Qt resource listing the PNG slices)
  - images/icons/rasterize_icons.cpp     (Qt helper that rasterizes SVGs -> PNG @1x/@2x)
Icons share one visual language: 24x24 viewBox, rounded line style.
Placeholders {c}=cyan {g}=green {r}=red {b}=blue are filled from COLORS.
"""
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__)))
SRC = os.path.join(ROOT, "images", "icons", "src")
PNG_DIR = os.path.join(ROOT, "images", "icons")
os.makedirs(SRC, exist_ok=True)
os.makedirs(PNG_DIR, exist_ok=True)

COLORS = {
    "c": "#22D3EE",   # cyan accent
    "g": "#34D399",   # green
    "r": "#F87171",   # red
    "b": "#3B82F6",   # blue
    "l": "#FFFFFF",   # white stroke (matches all-white font direction)
}

TEMPLATE = (
    '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" '
    'viewBox="0 0 24 24" fill="{fill}" stroke="{stroke}" stroke-width="{sw}" '
    'stroke-linecap="round" stroke-linejoin="round">{inner}</svg>'
)


def icon(inner, stroke="#FFFFFF", fill="none", sw=1.8):
    inner = inner.format(**COLORS)
    return TEMPLATE.format(inner=inner, stroke=stroke, fill=fill, sw=sw)


# ---------------------------------------------------------------- icon library
ICONS = {}

# --- brand / accent ---
ICONS["logo"] = icon(
    '<path d="M12 2.5 20 7v10l-8 4.5L4 17V7z" stroke="{c}"/>'
    '<path d="M12 2.5V22M4 7l8 4.5L20 7M4 17l8-4.5L20 17" stroke="{c}" stroke-width="1.2"/>'
    '<circle cx="12" cy="12" r="2.2" fill="{c}" stroke="none"/>'
)
ICONS["about_logo"] = ICONS["logo"]

# --- chrome ---
ICONS["menu"] = icon(
    '<line x1="4" y1="7" x2="20" y2="7"/><line x1="4" y1="12" x2="20" y2="12"/>'
    '<line x1="4" y1="17" x2="20" y2="17"/>'
)
ICONS["search"] = icon('<circle cx="11" cy="11" r="6.5"/><line x1="16" y1="16" x2="21" y2="21"/>')
ICONS["view_grid"] = icon(
    '<rect x="3.5" y="3.5" width="7" height="7" rx="1.5"/>'
    '<rect x="13.5" y="3.5" width="7" height="7" rx="1.5"/>'
    '<rect x="3.5" y="13.5" width="7" height="7" rx="1.5"/>'
    '<rect x="13.5" y="13.5" width="7" height="7" rx="1.5"/>'
)
ICONS["settings"] = icon(
    '<circle cx="12" cy="12" r="3.2"/>'
    '<path d="M12 2.5v2.4M12 19.1v2.4M2.5 12h2.4M19.1 12h2.4'
    'M5 5l1.7 1.7M17.3 17.3 19 19M19 5l-1.7 1.7M6.7 17.3 5 19"/>'
)
ICONS["appearance"] = icon(
    '<circle cx="12" cy="12" r="8.5"/>'
    '<path d="M12 3.5a8.5 8.5 0 0 1 0 17z" fill="{c}" stroke="none"/>'
)
ICONS["refresh"] = icon('<path d="M20 11a8 8 0 1 0-1.6 4.8"/><path d="M20 4v5h-5"/>')
ICONS["more"] = icon(
    '<circle cx="5.5" cy="12" r="1.4" fill="{l}" stroke="none"/>'
    '<circle cx="12" cy="12" r="1.4" fill="{l}" stroke="none"/>'
    '<circle cx="18.5" cy="12" r="1.4" fill="{l}" stroke="none"/>'
)
ICONS["close"] = icon('<line x1="6" y1="6" x2="18" y2="18"/><line x1="18" y1="6" x2="6" y2="18"/>')
ICONS["close_hover"] = icon('<line x1="6" y1="6" x2="18" y2="18"/><line x1="18" y1="6" x2="6" y2="18"/>', stroke="{r}")
ICONS["collapse"] = icon('<path d="M6 9l6 6 6-6"/>')
ICONS["add"] = icon('<line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/>')
ICONS["unlock"] = icon(
    '<rect x="5" y="10.5" width="14" height="10" rx="2"/>'
    '<path d="M8 10.5V8a4 4 0 0 1 7.5-1.5"/>'
    '<circle cx="12" cy="15" r="1.4" fill="{c}" stroke="none"/>'
)

# --- actions ---
ICONS["rename"] = icon('<path d="M4 20h4L19 9l-4-4L4 16z"/><path d="M14.5 6.5l3 3"/>')
ICONS["delete"] = icon(
    '<path d="M4 7h16M9 7V4.5h6V7M6 7l1 13h10l1-13"/>'
    '<path d="M10 11v6M14 11v6"/>'
)
ICONS["new_category"] = icon(
    '<path d="M3 7.5A1.5 1.5 0 0 1 4.5 6H9l2 2.5h8.5A1.5 1.5 0 0 1 21 10v8a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18z"/>'
    '<line x1="12" y1="13" x2="12" y2="18"/><line x1="9.5" y1="15.5" x2="14.5" y2="15.5"/>'
)
ICONS["new_box"] = icon(
    '<rect x="3" y="4.5" width="18" height="13" rx="1.8"/>'
    '<path d="M3 9h18M8.5 2.5v3M15.5 2.5v3"/>'
    '<line x1="12" y1="12" x2="12" y2="16"/><line x1="10" y1="14" x2="14" y2="14"/>'
)
ICONS["home"] = icon(
    '<path d="M4 11l8-7 8 7"/><path d="M6 10v9h12v-9"/>'
    '<path d="M10 19v-5h4v5"/>'
)
ICONS["market"] = icon(
    '<path d="M12 3 21 8l-9 5-9-5z" stroke="{g}"/>'
    '<path d="M3 12l9 5 9-5M3 16l9 5 9-5" stroke="{g}"/>'
)
ICONS["check"] = icon(
    '<circle cx="12" cy="12" r="8.5" stroke="{c}"/><path d="M8 12.5l2.5 2.5L16 9" stroke="{c}"/>'
)
ICONS["feedback"] = icon(
    '<path d="M4 5.5h16a1 1 0 0 1 1 1V15a1 1 0 0 1-1 1H9l-4 3.5V16H4a1 1 0 0 1-1-1V6.5a1 1 0 0 1 1-1z" stroke="{c}"/>'
    '<circle cx="9" cy="10.5" r="1.1" fill="{c}" stroke="none"/>'
    '<circle cx="12" cy="10.5" r="1.1" fill="{c}" stroke="none"/>'
    '<circle cx="15" cy="10.5" r="1.1" fill="{c}" stroke="none"/>'
)

# --- quick tools (14) ---
ICONS["tool_organize"] = icon('<rect x="3.5" y="4" width="17" height="13" rx="1.8"/><path d="M3.5 8.5h17M8.5 8.5V17M15.5 8.5V17"/>')
ICONS["tool_search"] = ICONS["search"]
ICONS["tool_files"] = icon('<path d="M3 7.5A1.5 1.5 0 0 1 4.5 6H9l2 2.5h8.5A1.5 1.5 0 0 1 21 10v8a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18z"/>')
ICONS["tool_shutdown"] = icon('<path d="M12 3.5v8"/><path d="M7 6.5a7 7 0 1 0 10 0"/>')
ICONS["tool_screenshot"] = icon(
    '<rect x="3.5" y="5.5" width="17" height="13" rx="2" stroke-dasharray="3 2.4"/>'
    '<circle cx="9" cy="11" r="1.3" fill="{c}" stroke="none"/>'
    '<path d="M13 10l4 4M17 10l-4 4"/>'
)
ICONS["tool_notes"] = icon(
    '<path d="M6 3.5h8l4 4V20a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V4.5a1 1 0 0 1 1-1z"/>'
    '<path d="M14 3.5V8h4.5"/><path d="M8 12h8M8 15.5h8M8 8.5h3"/>'
)
ICONS["tool_wallpaper"] = icon(
    '<rect x="3.5" y="4.5" width="17" height="15" rx="2"/>'
    '<circle cx="9" cy="9.5" r="1.6" stroke="{c}"/>'
    '<path d="M4 17l5-5 4 4 3-3 4 4"/>'
)
ICONS["tool_lock"] = icon(
    '<rect x="5" y="10.5" width="14" height="10" rx="2"/>'
    '<path d="M8 10.5V8a4 4 0 0 1 8 0v2.5"/>'
    '<circle cx="12" cy="15" r="1.4" fill="{c}" stroke="none"/>'
)
ICONS["tool_browser"] = icon('<circle cx="12" cy="12" r="8.5"/><path d="M3.5 12h17M12 3.5c2.5 2.4 2.5 14.6 0 17M12 3.5c-2.5 2.4-2.5 14.6 0 17"/>')
ICONS["tool_calc"] = icon(
    '<rect x="5" y="3.5" width="14" height="17" rx="2"/>'
    '<path d="M5 8h14M9 12v1.5M12 12v1.5M15 12v1.5M9 15.5v1.5M12 15.5v1.5M15 15.5v1.5"/>'
)
ICONS["tool_registry"] = icon(
    '<ellipse cx="12" cy="6" rx="7" ry="3"/>'
    '<path d="M5 6v12c0 1.7 3.1 3 7 3s7-1.3 7-3V6"/>'
    '<path d="M5 12c0 1.7 3.1 3 7 3s7-1.3 7-3"/>'
)
ICONS["tool_cmd"] = icon('<rect x="3.5" y="4.5" width="17" height="15" rx="2"/><path d="M7 9l3 3-3 3M13 15h4"/>')
ICONS["tool_clean"] = icon(
    '<path d="M19 5l-7 7"/><path d="M14 4l1 2 2 1-2 1-1 2-1-2-2-1 2-1z" stroke="{c}"/>'
    '<path d="M5 19l9-9 3 3-9 9z"/>'
)
ICONS["tool_manage"] = icon(
    '<rect x="3.5" y="3.5" width="6.5" height="6.5" rx="1.4"/>'
    '<rect x="14" y="3.5" width="6.5" height="6.5" rx="1.4"/>'
    '<rect x="3.5" y="14" width="6.5" height="6.5" rx="1.4"/>'
    '<path d="M14 17.5h6.5M17.5 14v6.5" stroke="{c}"/>'
)

# ---------------------------------------------------------------- write SVGs
NAMES = list(ICONS.keys())
for name in NAMES:
    with open(os.path.join(SRC, name + ".svg"), "w", encoding="utf-8") as f:
        f.write(ICONS[name])

# ---------------------------------------------------------------- resource qrc
# Entries are relative to THIS .qrc file's directory (images/icons/), so use
# basenames. Qt's rcc resolves <file> paths against the .qrc location, not the
# project root. (Matches the convention in resources.qrc, which sits at the
# project root and references images/... from there.)
lines = ['<RCC>', '    <qresource prefix="/icons">']
for name in NAMES:
    for scale in (1, 2):
        lines.append('        <file>%s_%dx.png</file>' % (name, scale))
lines.append('    </qresource>')
lines.append('</RCC>')
with open(os.path.join(PNG_DIR, "icons.qrc"), "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")

# ---------------------------------------------------------------- rasterizer cpp
names_cpp = ",\n".join('        QStringLiteral("%s")' % n for n in NAMES)
cpp = '''#include <QApplication>
#include <QSvgRenderer>
#include <QPainter>
#include <QPixmap>
#include <QStringList>
#include <QDir>
#include <QByteArray>

int main(int argc, char** argv) {{
    QApplication a(argc, argv);
    QString src = QDir::toNativeSeparators(QDir::currentPath() + "/images/icons/src/");
    QString out = QDir::toNativeSeparators(QDir::currentPath() + "/images/icons/");
    QStringList names = {{
{names}
    }};
    QList<int> scales = {{1, 2}};
    int count = 0;
    for (const QString& n : names) {{
        QSvgRenderer r(src + n + ".svg");
        if (!r.isValid()) {{ qWarning("invalid svg: %s", qPrintable(n)); continue; }}
        for (int s : scales) {{
            int size = 24 * s;
            QPixmap pm(size, size);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            r.render(&p, QRect(0, 0, size, size));
            QString path = out + n + QString("_%1x.png").arg(s);
            if (pm.save(path))
                ++count;
        }}
    }}
    printf("rasterized %d png files\\n", count);
    return 0;
}}
'''.format(names=names_cpp)
with open(os.path.join(PNG_DIR, "rasterize_icons.cpp"), "w", encoding="utf-8") as f:
    f.write(cpp)

print("generated %d svg sources + icons.qrc + rasterize_icons.cpp" % len(NAMES))
print("names:", ", ".join(NAMES))
