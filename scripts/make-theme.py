#!/usr/bin/env python3
"""Convert your own pictures into a pupui theme (port/pupui/theme.c reads the result).

usage: make-theme.py SRC_DIR OUT_DIR

SRC_DIR holds any of (png, jpg, jpeg or webp):
  pup0 .. pup5     the six pups, in home-tile order (Pup Pop, Match, Paint, Parade use 0-3)
  home             the home screen background (cropped to 640x480)
  names.txt        up to six names, one per line, for Pup Parade

Pictures with transparency (cut-outs) keep it; others are cropped to a circle. Each pup
also becomes its Match card. Missing files keep the built-in art.
"""
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageOps

EXTS = (".png", ".jpg", ".jpeg", ".webp")


def find(src, stem):
    for e in EXTS:
        for name in (stem + e, stem + e.upper()):
            p = os.path.join(src, name)
            if os.path.isfile(p):
                return p
    return None


def write_raw(img, path):
    img = img.convert("RGBA")
    w, h = img.size
    data = bytearray(b"RFI1" + struct.pack("<II", w, h))
    for r, g, b, a in img.getdata():
        data += struct.pack("<I", (a << 24) | (r << 16) | (g << 8) | b)
    with open(path, "wb") as f:
        f.write(data)


def circle(img, size, ring=0):
    """Square-crop to size; transparent pictures stay as they are, others get a round mask."""
    img = ImageOps.exif_transpose(img).convert("RGBA")
    has_alpha = img.getextrema()[3][0] < 255
    img = ImageOps.fit(img, (size, size), Image.LANCZOS)
    if has_alpha:
        return img
    ss = 4
    mask = Image.new("L", (size * ss, size * ss), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, size * ss - 1, size * ss - 1], fill=255)
    mask = mask.resize((size, size), Image.LANCZOS)
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    if ring:
        out.paste((255, 255, 255, 255), (0, 0), mask)
        inner = size - 2 * ring
        img = ImageOps.fit(img, (inner, inner), Image.LANCZOS)
        m2 = mask.resize((inner, inner), Image.LANCZOS)
        out.paste(img, (ring, ring), m2)
    else:
        out.paste(img, (0, 0), mask)
    return out


def main(src, out):
    os.makedirs(out, exist_ok=True)
    made = []
    for i in range(6):
        p = find(src, f"pup{i}")
        if not p:
            continue
        img = Image.open(p)
        write_raw(circle(img, 96), os.path.join(out, f"pupsmall{i}.raw"))
        write_raw(circle(img, 100, ring=5), os.path.join(out, f"badge{i}.raw"))
        made.append(f"pup{i}")
    p = find(src, "home")
    if p:
        img = ImageOps.fit(ImageOps.exif_transpose(Image.open(p)), (640, 480), Image.LANCZOS)
        write_raw(img, os.path.join(out, "home.raw"))
        made.append("home")
    p = os.path.join(src, "names.txt")
    if os.path.isfile(p):
        names = [l.strip()[:14] for l in open(p, encoding="utf-8", errors="replace").read().splitlines()[:6]]
        with open(os.path.join(out, "names.txt"), "w", encoding="ascii", errors="replace") as f:
            f.write("\n".join(names) + "\n")
        made.append("names")
    print("theme:", ", ".join(made) if made else "nothing found")
    return 0 if made else 1


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
