#!/usr/bin/env python3
"""Generate original "Pup Rescue HQ" artwork and fonts as C for pupui.

All art is drawn here from simple shapes (no external images). Output: OUT.c with
ARGB8888 images and 8-bit alpha font glyphs, OUT.h declaring them.

usage: gen_assets.py OUT   (writes OUT.c and OUT.h)
"""
import math
import sys

from PIL import Image, ImageDraw, ImageFont

SS = 4  # supersampling factor for smooth edges
FONTS = "/usr/share/fonts/truetype/dejavu/"
FONT = FONTS + "DejaVuSans-Bold.ttf"

# name, team colour, fur, ear style, badge symbol
PUPS = [
    ("Blaze", (226, 38, 38), (250, 250, 250), "up", "flame"),
    ("Bolt", (38, 84, 200), (176, 116, 64), "up", "star"),
    ("Breeze", (236, 92, 168), (238, 214, 168), "flop", "wings"),
    ("Sprout", (52, 168, 72), (150, 150, 158), "flop", "leaf"),
    ("Digger", (247, 148, 29), (196, 140, 80), "flop", "cone"),
    ("Splash", (0, 166, 170), (120, 80, 50), "up", "drop"),
]


def shade(c, f):
    return tuple(max(0, min(255, int(v * f))) for v in c)


def star_points(cx, cy, r_out, r_in, n=5, rot=-90):
    pts = []
    for i in range(n * 2):
        r = r_out if i % 2 == 0 else r_in
        a = math.radians(rot + i * 180 / n)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def draw_symbol(d, sym, cx, cy, r, col=(255, 255, 255)):
    if sym == "flame":
        d.polygon([(cx, cy - r), (cx + r * 0.7, cy + r * 0.2), (cx + r * 0.45, cy + r * 0.8),
                   (cx, cy + r * 0.55), (cx - r * 0.45, cy + r * 0.8), (cx - r * 0.7, cy + r * 0.2)], fill=col)
        d.ellipse([cx - r * 0.3, cy, cx + r * 0.3, cy + r * 0.7], fill=(255, 210, 60))
    elif sym == "star":
        d.polygon(star_points(cx, cy, r, r * 0.45), fill=col)
    elif sym == "wings":
        d.ellipse([cx - r, cy - r * 0.35, cx - r * 0.05, cy + r * 0.35], fill=col)
        d.ellipse([cx + r * 0.05, cy - r * 0.35, cx + r, cy + r * 0.35], fill=col)
        d.ellipse([cx - r * 0.25, cy - r * 0.25, cx + r * 0.25, cy + r * 0.25], fill=(255, 220, 80))
    elif sym == "leaf":
        d.ellipse([cx - r * 0.55, cy - r, cx + r * 0.55, cy + r * 0.8], fill=col)
        d.line([cx, cy - r * 0.6, cx, cy + r], fill=shade(col, 0.6) if col != (255, 255, 255) else (52, 120, 60), width=max(1, int(r * 0.15)))
    elif sym == "cone":
        d.polygon([(cx, cy - r), (cx + r * 0.65, cy + r * 0.75), (cx - r * 0.65, cy + r * 0.75)], fill=col)
        d.rectangle([cx - r * 0.85, cy + r * 0.7, cx + r * 0.85, cy + r * 0.95], fill=col)
        d.rectangle([cx - r * 0.42, cy - r * 0.05, cx + r * 0.42, cy + r * 0.2], fill=(247, 148, 29))
    elif sym == "drop":
        d.ellipse([cx - r * 0.6, cy - r * 0.15, cx + r * 0.6, cy + r * 0.95], fill=col)
        d.polygon([(cx, cy - r), (cx + r * 0.55, cy + r * 0.25), (cx - r * 0.55, cy + r * 0.25)], fill=col)


def badge(d, cx, cy, r, team, sym):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(255, 214, 0))
    d.ellipse([cx - r * 0.82, cy - r * 0.82, cx + r * 0.82, cy + r * 0.82], fill=team)
    draw_symbol(d, sym, cx, cy, r * 0.55)


def pup_image(size, team, fur, ears, sym, happy=True):
    S = size * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = S / 2
    hr = S * 0.30  # head radius
    hy = S * 0.48
    ear_col = shade(fur, 0.72)
    if ears == "up":
        for sx in (-1, 1):
            d.polygon([(c + sx * hr * 0.35, hy - hr * 0.7), (c + sx * hr * 1.05, hy - hr * 1.35),
                       (c + sx * hr * 1.0, hy - hr * 0.25)], fill=ear_col)
            d.polygon([(c + sx * hr * 0.5, hy - hr * 0.65), (c + sx * hr * 0.95, hy - hr * 1.1),
                       (c + sx * hr * 0.9, hy - hr * 0.4)], fill=(240, 170, 170))
    else:
        for sx in (-1, 1):
            d.ellipse([c + sx * hr * 0.75 - hr * 0.35, hy - hr * 0.75, c + sx * hr * 0.75 + hr * 0.35,
                       hy + hr * 0.45], fill=ear_col)
    # body peeking below the head, collar, badge
    d.ellipse([c - hr * 0.95, hy + hr * 0.55, c + hr * 0.95, hy + hr * 2.2], fill=fur)
    d.ellipse([c - hr, hy - hr, c + hr, hy + hr], fill=fur)
    if team == (226, 38, 38):  # Blaze: spots
        for (sx, sy, sr) in ((-0.7, -0.55, 0.12), (0.72, 0.05, 0.1), (-0.62, 0.35, 0.09), (0.3, -0.75, 0.08)):
            d.ellipse([c + sx * hr - sr * hr, hy + sy * hr - sr * hr, c + sx * hr + sr * hr, hy + sy * hr + sr * hr], fill=(40, 40, 40))
    # muzzle
    d.ellipse([c - hr * 0.55, hy + hr * 0.05, c + hr * 0.55, hy + hr * 0.8], fill=shade(fur, 1.12) if sum(fur) < 700 else (255, 255, 255))
    # eyes
    for sx in (-1, 1):
        ex, ey, er = c + sx * hr * 0.38, hy - hr * 0.18, hr * 0.2
        d.ellipse([ex - er, ey - er, ex + er, ey + er], fill=(255, 255, 255))
        d.ellipse([ex - er * 0.7, ey - er * 0.55, ex + er * 0.7, ey + er * 0.85], fill=(60, 40, 30))
        d.ellipse([ex - er * 0.35, ey - er * 0.45, ex + er * 0.05, ey - er * 0.05], fill=(255, 255, 255))
    # nose and smile
    d.ellipse([c - hr * 0.16, hy + hr * 0.15, c + hr * 0.16, hy + hr * 0.36], fill=(30, 30, 30))
    w = max(2, int(hr * 0.06))
    if happy:
        d.arc([c - hr * 0.3, hy + hr * 0.2, c, hy + hr * 0.6], 20, 160, fill=(30, 30, 30), width=w)
        d.arc([c, hy + hr * 0.2, c + hr * 0.3, hy + hr * 0.6], 20, 160, fill=(30, 30, 30), width=w)
        d.ellipse([c - hr * 0.12, hy + hr * 0.45, c + hr * 0.12, hy + hr * 0.7], fill=(240, 100, 110))
    # collar and badge
    d.rounded_rectangle([c - hr * 0.8, hy + hr * 0.85, c + hr * 0.8, hy + hr * 1.08], radius=hr * 0.1, fill=team)
    badge(d, c, hy + hr * 1.3, hr * 0.38, team, sym)
    return img.resize((size, size), Image.LANCZOS)


def badge_image(size, team, sym):
    S = size * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    badge(ImageDraw.Draw(img), S / 2, S / 2, S * 0.46, team, sym)
    return img.resize((size, size), Image.LANCZOS)


def home_scene(w, h):
    img = Image.new("RGBA", (w * 2, h * 2))
    d = ImageDraw.Draw(img)
    W, H = w * 2, h * 2
    for y in range(H):  # sky gradient
        t = y / H
        d.line([(0, y), (W, y)], fill=(int(90 + 120 * t), int(170 + 60 * t), 255))
    d.ellipse([W * 0.78, H * 0.04, W * 0.95, H * 0.27], fill=(255, 222, 70))  # sun
    for (x, y, s) in ((0.12, 0.12, 1.0), (0.42, 0.07, 0.8)):  # clouds
        for (dx, dy, r) in ((0, 0, 0.07), (0.06, -0.03, 0.08), (0.13, 0, 0.065)):
            d.ellipse([W * (x + dx) - W * r * s, H * (y + dy) - W * r * s * 0.7, W * (x + dx) + W * r * s,
                       H * (y + dy) + W * r * s * 0.7], fill=(255, 255, 255))
    d.ellipse([-W * 0.2, H * 0.62, W * 0.7, H * 1.5], fill=(108, 196, 80))  # hills
    d.ellipse([W * 0.35, H * 0.68, W * 1.3, H * 1.6], fill=(88, 176, 66))
    # lookout tower: pole, pod, roof
    tx = W * 0.82
    d.rectangle([tx - W * 0.025, H * 0.38, tx + W * 0.025, H * 0.86], fill=(200, 205, 215))
    d.rounded_rectangle([tx - W * 0.09, H * 0.28, tx + W * 0.09, H * 0.42], radius=W * 0.02, fill=(230, 235, 245))
    d.rectangle([tx - W * 0.075, H * 0.31, tx + W * 0.075, H * 0.37], fill=(110, 190, 240))
    d.polygon([(tx - W * 0.1, H * 0.29), (tx + W * 0.1, H * 0.29), (tx, H * 0.2)], fill=(226, 38, 38))
    d.ellipse([tx - W * 0.018, H * 0.16, tx + W * 0.018, H * 0.2], fill=(255, 214, 0))
    return img.resize((w, h), Image.LANCZOS)


# RufusOS app icons (AsteroidOS-style: a white pictogram on a coloured circle).
ICONS = [
    ("timer", (232, 93, 60)), ("stopwatch", (240, 160, 40)), ("alarm", (210, 64, 120)),
    ("flashlight", (230, 190, 30)), ("calculator", (90, 110, 140)), ("game", (150, 90, 210)),
    ("kids", (226, 38, 38)), ("settings", (80, 90, 105)), ("terminal", (40, 44, 52)),
    ("desktop", (40, 130, 200)), ("music", (230, 70, 90)),
]
ICON_SIZE = 84


def icon_image(size, kind, colour):
    S = size * SS
    img = Image.new("RGBA", (S, S))
    d = ImageDraw.Draw(img)
    d.ellipse([0, 0, S - 1, S - 1], fill=colour)
    c, r, wl = S / 2, S * 0.28, max(2, int(S * 0.05))
    white = (255, 255, 255)
    if kind in ("timer", "stopwatch", "alarm"):
        d.ellipse([c - r, c - r * 0.9, c + r, c + r * 1.1], outline=white, width=wl)
        d.line([c, c + r * 0.1, c, c - r * 0.55], fill=white, width=wl)
        d.line([c, c + r * 0.1, c + r * 0.45, c + r * 0.3], fill=white, width=wl)
        if kind == "stopwatch":
            d.rectangle([c - r * 0.15, c - r * 1.35, c + r * 0.15, c - r * 0.9], fill=white)
        elif kind == "alarm":
            for sx in (-1, 1):
                d.line([c + sx * r * 0.9, c - r * 1.05, c + sx * r * 0.45, c - r * 1.3], fill=white, width=wl)
        else:
            d.pieslice([c - r * 0.7, c - r * 0.6, c + r * 0.7, c + r * 0.8], -90, 30, fill=white)
    elif kind == "flashlight":
        d.polygon([(c - r * 0.7, c - r), (c + r * 0.7, c - r), (c + r * 0.3, c - r * 0.2),
                   (c - r * 0.3, c - r * 0.2)], fill=white)
        d.rectangle([c - r * 0.3, c - r * 0.2, c + r * 0.3, c + r * 1.1], fill=white)
    elif kind == "calculator":
        d.rounded_rectangle([c - r * 0.8, c - r, c + r * 0.8, c + r], radius=r * 0.15, outline=white, width=wl)
        d.rectangle([c - r * 0.55, c - r * 0.75, c + r * 0.55, c - r * 0.35], fill=white)
        for i in range(3):
            for j in range(2):
                x, y = c - r * 0.45 + i * r * 0.45, c + j * r * 0.45
                d.ellipse([x - r * 0.12, y - r * 0.12, x + r * 0.12, y + r * 0.12], fill=white)
    elif kind == "game":
        for i in range(2):
            for j in range(2):
                x0, y0 = c - r + i * r * 1.05, c - r + j * r * 1.05
                d.rounded_rectangle([x0, y0, x0 + r * 0.95, y0 + r * 0.95], radius=r * 0.15,
                                    fill=white if (i + j) % 2 == 0 else (255, 255, 255, 140))
    elif kind == "kids":  # a paw
        d.ellipse([c - r * 0.6, c - r * 0.1, c + r * 0.6, c + r * 0.9], fill=white)
        for (dx, dy) in ((-0.75, -0.35), (-0.3, -0.85), (0.3, -0.85), (0.75, -0.35)):
            x, y = c + dx * r, c + dy * r
            d.ellipse([x - r * 0.25, y - r * 0.3, x + r * 0.25, y + r * 0.3], fill=white)
    elif kind == "settings":  # a gear
        for i in range(8):
            a = math.radians(i * 45)
            x, y = c + math.cos(a) * r * 0.85, c + math.sin(a) * r * 0.85
            d.ellipse([x - r * 0.25, y - r * 0.25, x + r * 0.25, y + r * 0.25], fill=white)
        d.ellipse([c - r * 0.85, c - r * 0.85, c + r * 0.85, c + r * 0.85], fill=white)
        d.ellipse([c - r * 0.35, c - r * 0.35, c + r * 0.35, c + r * 0.35], fill=colour)
    elif kind == "terminal":
        d.line([c - r * 0.8, c - r * 0.5, c - r * 0.2, c, c - r * 0.8, c + r * 0.5], fill=white, width=wl, joint="curve")
        d.line([c, c + r * 0.6, c + r * 0.8, c + r * 0.6], fill=white, width=wl)
    elif kind == "music":  # two joined notes
        for x in (c - r * 0.75, c + r * 0.45):
            d.ellipse([x - r * 0.32, c + r * 0.45, x + r * 0.32, c + r * 0.95], fill=white)
            d.rectangle([x + r * 0.2, c - r * 0.75, x + r * 0.32, c + r * 0.7], fill=white)
        d.polygon([(c - r * 0.55 + r * 0.0, c - r * 0.75), (c + r * 0.77, c - r * 1.0),
                   (c + r * 0.77, c - r * 0.7), (c - r * 0.55, c - r * 0.45)], fill=white)
    elif kind == "desktop":
        d.rounded_rectangle([c - r, c - r * 0.75, c + r, c + r * 0.55], radius=r * 0.1, outline=white, width=wl)
        d.rectangle([c - r * 0.15, c + r * 0.55, c + r * 0.15, c + r * 0.9], fill=white)
        d.rectangle([c - r * 0.5, c + r * 0.85, c + r * 0.5, c + r], fill=white)
    return img.resize((size, size), Image.LANCZOS)


def c_img(name, img):
    px = img.convert("RGBA").load()
    w, h = img.size
    vals = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            vals.append("0x%08x" % ((a << 24) | (r << 16) | (g << 8) | b))
    body = ",".join(vals)
    return f"static const uint32_t {name}_px[] = {{{body}}};\nconst struct image {name} = {{ {w}, {h}, {name}_px }};\n"


def c_font(name, size, path=FONT):
    font = ImageFont.truetype(path, size)
    asc, desc = font.getmetrics()
    out, glyphs = [], []
    for ch in range(32, 127):
        bbox = font.getbbox(chr(ch))
        adv = int(round(font.getlength(chr(ch))))
        x0, y0, x1, y1 = bbox
        gw, gh = max(0, x1 - x0), max(0, y1 - y0)
        data = []
        if gw and gh:
            im = Image.new("L", (gw, gh), 0)
            ImageDraw.Draw(im).text((-x0, -y0), chr(ch), font=font, fill=255)
            data = list(im.getdata())
        out.append(f"static const uint8_t {name}_{ch}[] = {{{','.join(map(str, data)) or '0'}}};")
        glyphs.append(f"{{{gw},{gh},{x0},{y0},{adv},{name}_{ch}}}")
    out.append(f"const struct font {name} = {{ {asc + desc}, {asc}, {{{','.join(glyphs)}}} }};")
    return "\n".join(out) + "\n"


def main(out):
    header = ["/* Generated by gen_assets.py: original artwork, DejaVu glyphs. */\n",
              "#include <stdint.h>\n",
              "struct image { int w, h; const uint32_t *px; };\n",
              "struct glyph { int w, h, x, y, adv; const uint8_t *a; };\n",
              "struct font { int height, ascent; struct glyph g[95]; };\n"]
    parts = [f'#include "{out.rsplit("/", 1)[-1]}.h"\n']
    for i, (name, team, fur, ears, sym) in enumerate(PUPS):
        parts.append(c_img(f"pup{i}", pup_image(150, team, fur, ears, sym)))
        parts.append(c_img(f"pupsmall{i}", pup_image(96, team, fur, ears, sym)))
        parts.append(c_img(f"badge{i}", badge_image(100, team, sym)))
    parts.append(c_img("home_bg", home_scene(640, 480)))
    # font_s/m/l: the kid UI; font_t: grown-up text; font_mono: the terminal.
    parts.append(c_font("font_s", 26))
    parts.append(c_font("font_m", 40))
    parts.append(c_font("font_l", 72))
    parts.append(c_font("font_t", 22, FONTS + "DejaVuSans.ttf"))
    parts.append(c_font("font_mono", 20, FONTS + "DejaVuSansMono.ttf"))
    parts.append(c_font("font_xl", 150, FONTS + "DejaVuSans.ttf"))  # RufusOS clock
    for kind, colour in ICONS:
        parts.append(c_img(f"icon_{kind}", icon_image(ICON_SIZE, kind, colour)))
    header.append("extern const struct image " + ", ".join(f"icon_{k}" for k, _ in ICONS) + ";\n")
    header.append("extern const struct font font_s, font_m, font_l, font_t, font_mono, font_xl;\n"
                  "/* Pointers, so a theme (theme.c) can replace them. */\n"
                  "extern const struct image *home_img, *pups[6], *pups_small[6], *badges[6];\n"
                  "extern const char *pup_names[6];\nextern const uint32_t pup_team[6];\n")
    names = ",".join(f'"{p[0]}"' for p in PUPS)
    teams = ",".join("0x%06x" % ((p[1][0] << 16) | (p[1][1] << 8) | p[1][2]) for p in PUPS)
    parts.append(f"const char *pup_names[6] = {{{names}}};\nconst uint32_t pup_team[6] = {{{teams}}};\n")
    parts.append("const struct image *home_img = &home_bg;\n")
    parts.append("const struct image *pups[6] = {&pup0,&pup1,&pup2,&pup3,&pup4,&pup5};\n")
    parts.append("const struct image *pups_small[6] = {&pupsmall0,&pupsmall1,&pupsmall2,&pupsmall3,&pupsmall4,&pupsmall5};\n")
    parts.append("const struct image *badges[6] = {&badge0,&badge1,&badge2,&badge3,&badge4,&badge5};\n")
    open(out + ".h", "w").write("".join(header))
    open(out + ".c", "w").write("".join(parts))
    # Preview sheet for checking the art without the watch.
    sheet = Image.new("RGBA", (6 * 160, 380), (255, 255, 255, 255))
    for i, (name, team, fur, ears, sym) in enumerate(PUPS):
        sheet.alpha_composite(pup_image(150, team, fur, ears, sym), (i * 160 + 5, 5))
        sheet.alpha_composite(badge_image(100, team, sym), (i * 160 + 30, 170))
        ImageDraw.Draw(sheet).text((i * 160 + 40, 290), name, font=ImageFont.truetype(FONT, 26), fill=(0, 0, 0))
    sheet.save(out.rsplit("/", 1)[0] + "/preview-pups.png")
    home_scene(640, 480).save(out.rsplit("/", 1)[0] + "/preview-home.png")
    icons = Image.new("RGBA", (len(ICONS) * (ICON_SIZE + 8), ICON_SIZE + 8), (20, 22, 28, 255))
    for i, (kind, colour) in enumerate(ICONS):
        icons.alpha_composite(icon_image(ICON_SIZE, kind, colour), (i * (ICON_SIZE + 8) + 4, 4))
    icons.save(out.rsplit("/", 1)[0] + "/preview-icons.png")


if __name__ == "__main__":
    main(sys.argv[1])
