"""Make Ploq's app icons from the Ploq mark (ploq-mark-*.svg in this folder).

The mark is two mirrored note-heads (rotated ellipses with an elliptical hole)
with stems, so it is drawn exactly here with numpy + Pillow (no SVG renderer
needed). Output goes to share/icons/Ploq/.

    python tools/collab/branding/make_icons.py
"""
import math
import os

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "..", "..", "share", "icons", "Ploq"))

INK = (20, 22, 26)          # #14161A, the mark's colour in ploq-mark-black.svg
WHITE = (255, 255, 255)

# Shapes in the SVG's 140x120 viewBox (see ploq-mark-black.svg).
# Each note-head: outer ellipse rx20 ry14 rotated -20deg, hole rx13 ry6.5 rotated -38deg.
HEADS = [(46.0, 46.0), (94.0, 46.0)]
STEMS = [(26.6, 50.0, 6.0, 58.0), (107.4, 50.0, 6.0, 58.0)]   # x, y, w, h


def _inside_ellipse(x, y, cx, cy, rx, ry, deg, mirror):
    t = math.radians(-deg if mirror else deg)
    dx, dy = x - cx, y - cy
    u = dx * math.cos(t) + dy * math.sin(t)
    v = -dx * math.sin(t) + dy * math.cos(t)
    return (u / rx) ** 2 + (v / ry) ** 2 <= 1.0


def mark_mask(size, box):
    """Coverage mask (0..1) of the mark drawn into box=(x0, y0, w) of a size x size image."""
    ss = 4
    n = size * ss
    x0, y0, w = (v * ss for v in box)
    scale = w / 140.0
    ys, xs = np.mgrid[0:n, 0:n].astype(np.float64)
    vx = (xs + 0.5 - x0) / scale          # image pixel -> viewBox coords
    vy = (ys + 0.5 - y0) / scale
    m = np.zeros((n, n), dtype=bool)
    for i, (cx, cy) in enumerate(HEADS):
        mirror = i == 1
        outer = _inside_ellipse(vx, vy, cx, cy, 20, 14, -20, mirror)
        hole = _inside_ellipse(vx, vy, cx, cy, 13, 6.5, -38, mirror)
        m |= outer & ~hole
    for (sx, sy, sw, sh) in STEMS:
        m |= (vx >= sx) & (vx <= sx + sw) & (vy >= sy) & (vy <= sy + sh)
    img = Image.fromarray((m * 255).astype(np.uint8), "L")
    return img.resize((size, size), Image.LANCZOS)


def rounded_square_mask(size, radius_frac=0.22):
    ss = 4
    n = size * ss
    r = radius_frac * n
    ys, xs = np.mgrid[0:n, 0:n].astype(np.float64) + 0.5
    cx = np.clip(xs, r, n - r)
    cy = np.clip(ys, r, n - r)
    m = (xs - cx) ** 2 + (ys - cy) ** 2 <= r * r
    img = Image.fromarray((m * 255).astype(np.uint8), "L")
    return img.resize((size, size), Image.LANCZOS)


def tile(size):
    """White mark on a dark rounded square: readable on light and dark taskbars."""
    bg = Image.new("RGBA", (size, size), INK + (0,))
    bg.putalpha(rounded_square_mask(size))
    w = size * 0.84                        # mark (140 wide) fills ~84% of the tile
    # The ink sits at y=32..108 of the 120-high viewBox: centre it vertically.
    s = w / 140.0
    y0 = size / 2 - 70 * s
    fg = Image.new("RGBA", (size, size), WHITE + (0,))
    fg.putalpha(mark_mask(size, ((size - w) / 2, y0, w)))
    return Image.alpha_composite(bg, fg)


def mac_icon(size=1024):
    """macOS app icon on Apple's grid: an 824/1024 rounded square, inset 100/1024, white mark.

    The mark is centred optically: its two note-heads carry most of the weight at the top, so
    the point 35% of the way from the ink's bounding-box centre (y 69.6) to its centroid (y 58.1)
    goes on the tile's centre (y 65.57 in viewBox units).
    """
    inset = size * 100 / 1024
    side = size - 2 * inset
    bg = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    plate = Image.new("RGBA", (round(side), round(side)), INK + (255,))
    plate.putalpha(rounded_square_mask(round(side), radius_frac=185 / 824))
    bg.alpha_composite(plate, (round(inset), round(inset)))
    s = side * 0.58 / 86.78                # the ink (86.78 wide) fills 58% of the tile
    w = 140 * s
    x0 = size / 2 - 70 * s
    y0 = size / 2 - 65.57 * s
    fg = Image.new("RGBA", (size, size), WHITE + (0,))
    fg.putalpha(mark_mask(size, (x0, y0, w)))
    return Image.alpha_composite(bg, fg)


def main():
    os.makedirs(OUT, exist_ok=True)
    mac = mac_icon(1024)
    mac.save(os.path.join(OUT, "Ploq.icns"))   # Pillow writes all the sizes macOS wants
    mac.save(os.path.join(OUT, "Ploq_mac_1024x1024.png"))
    big = tile(256)
    sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    imgs = {s: (big if s == 256 else tile(s)) for s in sizes}
    imgs[256].save(os.path.join(OUT, "Ploq.ico"), sizes=[(s, s) for s in sizes],
                   append_images=[imgs[s] for s in sizes if s != 256])
    imgs[96].save(os.path.join(OUT, "Ploq_96x96.png"))
    imgs[256].save(os.path.join(OUT, "Ploq_256x256.png"))
    tile(512).save(os.path.join(OUT, "Ploq_512x512.png"))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
