#!/usr/bin/env python3
"""PsiBoost's icon: the face of a 9-pin serial plug (D-sub), with a bold
arrow under it pointing right and speed lines behind, in the Psion's own way
(mostly white, black outlines, a grey shadow, and a mask black wherever it
is drawn), at the three sizes the 5mx uses, written to PsiBoost.aif with
psion-core's AIF writer (core/tools/icon/aif.py). The Extras bar shows only
the top 37 rows of the 48-pixel icon, so all of it is above row 37."""
import os, sys
from PIL import Image, ImageDraw
_here = os.path.dirname(os.path.abspath(__file__))
# (psion-core: core/ beside this file, or one folder up)
sys.path.insert(0, next((p for p in (os.path.join(_here, "core/tools/icon"), os.path.join(_here, "../core/tools/icon"))
                         if os.path.isdir(p)), os.path.join(_here, "core/tools/icon")))
from aif import write_aif
K, D, L, W = 0, 85, 170, 255


def icon(n):
    im = Image.new("L", (n, n), W)
    mk = Image.new("1", (n, n), 1)
    d, m = ImageDraw.Draw(im), ImageDraw.Draw(mk)
    s = n / 48.0
    R = lambda v: int(round(v * s))
    sh = max(1, R(2))
    lw = max(1, R(1.5))
    # the plug's shell: a trapezoid, wide at the top, with a grey shadow
    shell = [(R(7), R(4)), (R(41), R(4)), (R(36), R(23)), (R(12), R(23))]
    d.polygon([(x + sh, y + sh) for x, y in shell], fill=L)
    m.polygon([(x + sh, y + sh) for x, y in shell], fill=0)
    d.polygon(shell, fill=W, outline=K)
    m.polygon(shell, fill=0)
    if n >= 32:
        d.line(shell + [shell[0]], fill=K, width=lw)
    # the face inside it, pale grey, and its nine pins: five over four
    face = [(R(11), R(7.5)), (R(37), R(7.5)), (R(33.5), R(19.5)), (R(14.5), R(19.5))]
    d.polygon(face, fill=L if n >= 48 else W, outline=K)
    pr = 1.1 * s
    for i in range(5):
        x, y = (14.6 + i * 4.7) * s, 11.4 * s
        if n < 48:
            d.point((int(round(x)), int(round(y))), fill=K)
        else:
            d.ellipse([x - pr, y - pr, x + pr, y + pr], fill=K)
    for i in range(4):
        x, y = (17.0 + i * 4.7) * s, 16.0 * s
        if n < 48:
            d.point((int(round(x)), int(round(y))), fill=K)
        else:
            d.ellipse([x - pr, y - pr, x + pr, y + pr], fill=K)
    # the arrow under it, pointing right: faster
    ay, ah, aw = R(31), max(1, R(2.5)), R(6)
    shaft = [(R(14), ay - ah), (R(36), ay - ah), (R(36), ay + ah), (R(14), ay + ah)]
    head = [(R(35), ay - aw), (R(45), ay), (R(35), ay + aw)]
    for poly in (shaft, head):
        d.polygon(poly, fill=K)
        m.polygon(poly, fill=0)
    # speed lines behind it
    for k, (y, x0) in enumerate(((ay - R(4), R(5)), (ay, R(2)), (ay + R(4), R(5)))):
        x1 = R(11)
        d.line([(x0, y), (x1, y)], fill=K, width=lw)
        m.line([(x0, y), (x1, y)], fill=0, width=lw)
    return im, mk


if __name__ == "__main__":
    icons = [icon(n) for n in (24, 32, 48)]
    out = sys.argv[1] if len(sys.argv) > 1 else "psiboost.aif"
    print(out, write_aif(out, 0x01000A94, [(1, "PsiBoost"), (10, "PsiBoost")], icons), "bytes")
    if len(sys.argv) <= 1:
        prev = Image.new("L", (24 + 32 + 48 + 40, 58), 200)
        x = 10
        for im, mk in icons:
            bg = Image.new("L", im.size, 200)
            prev.paste(Image.composite(bg, im, mk.convert("L")), (x, 5)); x += im.size[0] + 10
        prev = prev.resize((prev.width * 4, prev.height * 4), Image.NEAREST)
        prev.save("preview.png")
