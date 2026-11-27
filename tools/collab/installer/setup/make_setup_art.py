"""Artwork for PloqSetup.exe: the blurred Ploq mark on the right of the window.

The approved design (lander style): the mark at ~42% white, Gaussian-blurred, partly cut off by the
window's right edge. A transparent picture the size of the window, at 2x so it stays smooth on
high-resolution screens. Uses the exact mark geometry from tools/collab/branding/make_icons.py.

    python tools/collab/installer/setup/make_setup_art.py
"""
import os
import sys

from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..", "branding")))
from make_icons import mark_mask  # noqa: E402  (same geometry as every other Ploq icon)

SCALE = 2                  # pixels per DIP
W, H = 680, 380            # the window, in DIPs; the art covers all of it (transparent where empty)
MARK = (354, -34, 140 * 2.8)   # the mark's viewBox drawn at x, y, width (DIPs): fitted to the approved mockup
OPACITY = 0.42
BLUR_DIP = 8


def main():
    pad = 100                              # room above the window, so the mark can start above y = 0
    side = (W + 2 * pad) * SCALE
    x0, y0, w = MARK
    mask = mark_mask(side, ((x0 + pad) * SCALE, (y0 + pad) * SCALE, w * SCALE))
    mask = mask.filter(ImageFilter.GaussianBlur(BLUR_DIP * SCALE))   # blur before cropping: no hard edge at the top
    mask = mask.crop((pad * SCALE, pad * SCALE, (pad + W) * SCALE, (pad + H) * SCALE))
    art = Image.new("RGBA", mask.size, (255, 255, 255, 0))
    art.putalpha(mask.point(lambda a: int(a * OPACITY + 0.5)))
    out = os.path.join(HERE, "setup-mark.png")
    art.save(out, optimize=True)
    print("wrote", out, art.size)


if __name__ == "__main__":
    main()
