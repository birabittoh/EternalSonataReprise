#!/usr/bin/env python3
"""Renders the Xbox and PlayStation face buttons, D-pads, shoulder and menu buttons into res/generated/, which is gitignored and built by gen-images.py.

The face buttons mirror DrawFaceButton and DrawPlayStationButton in
src/platform/touch_controls_overlay.cpp, unpressed and fully opaque.
"""
import os

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.join(ROOT, "res", "generated")
FONT = os.path.join(ROOT, "scripts", "fonts", "Arimo-Variable.ttf")

SIZE = 128
SS = 4  # supersampling
MARGIN = 0.04

XBOX = {
    "a": ((0.369, 0.682, 0.227), "A"),
    "b": ((0.761, 0.231, 0.231), "B"),
    "x": ((0.243, 0.471, 0.761), "X"),
    "y": ((0.847, 0.635, 0.118), "Y"),
}
PLAYSTATION = {
    "cross": ((0.490, 0.651, 0.949), "cross"),
    "circle": ((0.957, 0.392, 0.408), "circle"),
    "square": ((0.902, 0.553, 0.792), "square"),
    "triangle": ((0.282, 0.788, 0.643), "triangle"),
}


def bold_font(size):
    font = ImageFont.truetype(FONT, size)
    font.set_variation_by_name("Bold")
    return font


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def lerp(a, b, t):
    return a + (b - a) * t[..., None]


def solve_rings(px, py, center, radius, focus):
    """For each pixel, the ring t and direction of FillShadedEllipse that covers it."""
    qx, qy = px - focus[0], py - focus[1]
    dx, dy = center[0] - focus[0], center[1] - focus[1]
    a = dx * dx + dy * dy - radius * radius
    b = qx * dx + qy * dy
    c = qx * qx + qy * qy
    if abs(a) < 1e-9:
        t = c / (2.0 * np.maximum(b, 1e-9))
    else:
        t = (b - np.sqrt(np.maximum(b * b - a * c, 0.0))) / a
    t = np.maximum(t, 1e-6)
    ex, ey = (qx - t * dx) / (radius * t), (qy - t * dy) / (radius * t)
    return t, ex, ey


def over(dst, rgb, alpha):
    """Composites rgb at alpha (array) onto the premultiplied-free RGBA dst."""
    a = alpha[..., None]
    out_a = a + dst[..., 3:4] * (1.0 - a)
    out_rgb = (rgb * a + dst[..., :3] * dst[..., 3:4] * (1.0 - a)) / np.maximum(out_a, 1e-9)
    return np.concatenate([out_rgb, out_a], axis=-1)


def disc_mask(px, py, center, radius):
    d = np.hypot(px - center[0], py - center[1])
    return np.clip(radius - d + 0.5 * SS, 0.0, SS) / SS


def ring_mask(px, py, center, radius, width):
    d = np.hypot(px - center[0], py - center[1])
    return np.clip(width / 2 + 0.5 * SS - np.abs(d - radius), 0.0, SS) / SS


def grid():
    n = SIZE * SS
    ys, xs = np.mgrid[0:n, 0:n].astype(np.float64) + 0.5
    return xs, ys, n


def xbox(color, label):
    px, py, n = grid()
    center = (n / 2, n / 2)
    radius = n / 2 * (1 - MARGIN) - SS
    color = np.array(color)
    white, black = np.ones(3), np.zeros(3)
    highlight = color + (white - color) * 0.40
    shadow = color + (black - color) * 0.40
    glow = color + (white - color) * 0.25
    focus = (center[0], center[1] - radius * 0.35)

    img = np.zeros((n, n, 4))
    t, ex, ey = solve_rings(px, py, center, radius, focus)
    inside = (t <= 1.0)
    col = lerp(np.broadcast_to(highlight, (n, n, 3)), np.broadcast_to(color, (n, n, 3)),
               smooth(0.0, 0.6, t))
    col = lerp(col, np.broadcast_to(shadow, (n, n, 3)), smooth(0.55, 1.0, t))
    lower = np.maximum(ey, 0.0)
    col = lerp(col, np.broadcast_to(glow, (n, n, 3)),
               0.7 * lower * lower * smooth(0.7, 0.95, t))
    img = over(img, col, inside * disc_mask(px, py, center, radius))

    font = bold_font(int(round(radius * 1.20)))
    text = label or ""
    box = ImageDraw.Draw(Image.new("L", (1, 1))).textbbox((0, 0), text, font=font)
    tx = center[0] - (box[0] + box[2]) / 2
    ty = center[1] - (box[1] + box[3]) / 2
    glyph = Image.new("L", (n, n), 0)
    ImageDraw.Draw(glyph).text((tx, ty), text, font=font, fill=255,
                               stroke_width=int(2.5 * SS), stroke_fill=255)
    # Counters are what the glyph encloses. A highlight over one reads as a hole
    # in the button, so the gloss leaves them at the body colour.
    outside = Image.eval(glyph, lambda v: 255 - v)
    ImageDraw.floodfill(outside, (0, 0), 128, thresh=0)
    counters = outside.point(lambda v: 255 if v == 255 else 0).filter(
        ImageFilter.GaussianBlur(0.6 * SS))
    keep = 1.0 - 0.9 * np.asarray(counters, dtype=np.float64) / 255.0

    gc = (center[0], center[1] - radius * 0.42)
    gr = (radius * 0.62, radius * 0.40)
    gx, gy = (px - gc[0]) / gr[0], (py - gc[1]) / gr[1]
    gt = np.hypot(gx, gy)
    ydir = np.where(gt > 0, gy / np.maximum(gt, 1e-9), 0.0)
    y = 0.5 - 0.5 * ydir * gt
    gloss = 0.75 * (0.15 + 0.85 * y * y) * (1.0 - smooth(0.55, 1.0, gt))
    gloss = np.where(gt <= 1.0, gloss, 0.0)
    img = over(img, np.broadcast_to(white, (n, n, 3)), gloss * keep)
    img = over(img, np.broadcast_to(white, (n, n, 3)),
               0.45 * ring_mask(px, py, center, radius - 3.5 * SS, 1.5 * SS))
    img = over(img, np.zeros((n, n, 3)), ring_mask(px, py, center, radius - 1.2 * SS, 5.0 * SS))

    pil = Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8), "RGBA")
    if label is None:
        return pil
    shadow_layer = Image.new("RGBA", pil.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow_layer).text((tx, ty + SS), label, font=font, fill=(0, 0, 0, 107))
    pil = Image.alpha_composite(pil, shadow_layer)
    text_layer = Image.new("RGBA", pil.size, (0, 0, 0, 0))
    # A black outline puts three unrelated colours in the blocks of a counter, which
    # DXT cannot hold; the button's own shade keeps them near one line.
    outline = tuple(int(v * 255 * 0.45) for v in color) + (255,)
    ImageDraw.Draw(text_layer).text((tx, ty), label, font=font, fill=(250, 244, 226, 255),
                                    stroke_width=int(2.5 * SS), stroke_fill=outline)
    pil = Image.alpha_composite(pil, text_layer)
    # Counters take the outline's colour: a block then holds the letter and one dark
    # colour, which DXT keeps exact, where plain body colour washes out.
    return paste_masked(pil, solid(n, outline[:3]), counters)


def draw_symbol(draw, glyph, c, size, fill, thickness):
    cx, cy = c
    if glyph == "cross":
        d = size * 0.62
        draw.line([(cx - d, cy - d), (cx + d, cy + d)], fill=fill, width=int(thickness))
        draw.line([(cx - d, cy + d), (cx + d, cy - d)], fill=fill, width=int(thickness))
    elif glyph == "circle":
        r = size * 0.72
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline=fill, width=int(thickness))
    elif glyph == "square":
        h = size * 0.60
        draw.rectangle([cx - h, cy - h, cx + h, cy + h], outline=fill, width=int(thickness))
    else:
        r = size * 0.82
        y = cy + size * 0.06
        pts = [(cx, y - r), (cx + r * 0.866, y + r * 0.5), (cx - r * 0.866, y + r * 0.5)]
        draw.line(pts + [pts[0], pts[1]], fill=fill, width=int(thickness), joint="curve")


def playstation(color, glyph):
    px, py, n = grid()
    center = (n / 2, n / 2)
    radius = n / 2 * (1 - MARGIN) - SS
    bezel_lit = np.array([0.30, 0.30, 0.33])
    bezel_dark = np.array([0.11, 0.11, 0.12])
    well_top = np.array([0.04, 0.04, 0.05])
    well_bottom = np.array([0.13, 0.13, 0.15])

    t, ex, ey = solve_rings(px, py, center, radius, center)
    y = 0.5 + 0.5 * ey * t
    shape = (n, n, 3)
    well = lerp(np.broadcast_to(well_top, shape), np.broadcast_to(well_bottom, shape), y)
    bezel = lerp(np.broadcast_to(bezel_lit, shape), np.broadcast_to(bezel_dark, shape),
                 0.5 + 0.5 * ey)
    col = lerp(well, bezel, smooth(0.74, 0.9, t))
    img = over(np.zeros((n, n, 4)), col, (t <= 1.0) * disc_mask(px, py, center, radius))
    img = over(img, np.zeros(shape), 0.8 * ring_mask(px, py, center, radius, 2.0 * SS))
    img = over(img, np.ones(shape), 0.30 * ring_mask(px, py, center, radius + 1.5 * SS, SS))
    pil = Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8), "RGBA")

    size = radius * 0.66
    thickness = max(2.0 * SS, radius * 0.17)
    shadow = Image.new("RGBA", pil.size, (0, 0, 0, 0))
    draw_symbol(ImageDraw.Draw(shadow), glyph, (center[0], center[1] + 1.5 * SS), size,
                (0, 0, 0, 153), thickness)
    pil = Image.alpha_composite(pil, shadow)
    symbol = Image.new("RGBA", pil.size, (0, 0, 0, 0))
    rgb = tuple(int(round(v * 255)) for v in color)
    draw_symbol(ImageDraw.Draw(symbol), glyph, center, size, rgb + (255,), thickness)
    return Image.alpha_composite(pil, symbol)

PC = {
    "left_click": (XBOX["a"][0], "left"),
    "right_click": (XBOX["b"][0], "right"),
    "middle_click": (XBOX["x"][0], "middle"),
    "space": (XBOX["y"][0], "space"),
}
# The same four on the colours of the PS3 buttons they sit in the place of.
PC_PS_COLORS = {
    "left_click": PLAYSTATION["cross"][0],
    "right_click": PLAYSTATION["circle"][0],
    "middle_click": PLAYSTATION["square"][0],
    "space": PLAYSTATION["triangle"][0],
}
INK = (30, 30, 34)


def rrect_mask(n, box, radius):
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=radius, fill=255)
    return mask


def keycap(img, box, radius, edge=INK):
    """A keycap: dark outline, a darker plate and a lighter top face set toward the upper edge.

    Returns the top face box so the legend can be centred on it."""
    n = img.size[0]
    x0, y0, x1, y1 = box
    inset = (x1 - x0) * 0.06
    face = (x0 + inset, y0 + inset, x1 - inset, y1 - inset * 2.2)
    plate = rrect_mask(n, box, radius)
    img = outlined(img, plate, (0.74, 0.75, 0.78), (0.52, 0.53, 0.57), edge=edge, width=2.5,
                   shine=0.0)
    top = rrect_mask(n, face, radius * 0.7)
    img = paste_masked(img, gradient(n, (0.99, 0.99, 1.0), (0.84, 0.85, 0.88)), top)
    return img, face


def legend(img, text, face, height, fill=INK):
    """Bold text centred on a keycap's top face."""
    n = img.size[0]
    font = bold_font(int(height))
    draw = ImageDraw.Draw(img)
    box = draw.textbbox((0, 0), text, font=font)
    cx = (face[0] + face[2]) / 2
    cy = (face[1] + face[3]) / 2
    draw.text((cx - (box[0] + box[2]) / 2, cy - (box[1] + box[3]) / 2), text, font=font,
              fill=fill + (255,))
    return img


def mouse_icon(img, color, active):
    """A mouse seen from above with one of its inputs filled in the button's own shade."""
    n = img.size[0]
    c = n / 2
    dark = tuple(int(v * 255 * 0.45) for v in color)
    fill = tuple(int(v * 255 * 0.62) for v in color)
    w, top, bottom = 0.19 * n, c - 0.29 * n, c + 0.29 * n
    body = rrect_mask(n, [c - w, top, c + w, bottom], w)
    img = outlined(img, body, (1.0, 0.99, 0.97), (0.80, 0.80, 0.82),
                   edge=dark, width=3.0, shine=0.0)
    split = c - 0.03 * n
    regions = {"left": [0, 0, c, split], "right": [c, 0, n, split]}
    if active in regions:
        part = Image.new("L", (n, n), 0)
        ImageDraw.Draw(part).rectangle(regions[active], fill=255)
        img = paste_masked(img, solid(n, fill), ImageChops.multiply(part, body))
    line = int(0.016 * n)
    draw = ImageDraw.Draw(img)
    draw.line([(c - w, split), (c + w, split)], fill=dark + (255,), width=line)
    draw.line([(c, top), (c, split)], fill=dark + (255,), width=line)
    ww, wt, wb = 0.036 * n, c - 0.215 * n, c - 0.085 * n
    wheel = rrect_mask(n, [c - ww, wt, c + ww, wb], ww)
    grow = wheel.filter(ImageFilter.MaxFilter(int(0.012 * n) * 2 + 1))
    img = paste_masked(img, solid(n, dark), grow)
    img = paste_masked(img, solid(n, fill if active == "middle" else (240, 240, 244)), wheel)
    return img


def pc_button(color, kind):
    img = xbox(color, None)
    n = img.size[0]
    c = n / 2
    if kind == "space":
        box = [c - 0.36 * n, c - 0.15 * n, c + 0.36 * n, c + 0.18 * n]
        img, face = keycap(img, box, 0.07 * n)
        # The space bar's own mark: a bracket open at the top.
        draw = ImageDraw.Draw(img)
        fx = (face[0] + face[2]) / 2
        fy = (face[1] + face[3]) / 2
        half, drop, line = 0.20 * n, 0.045 * n, int(0.028 * n)
        pts = [(fx - half, fy - drop), (fx - half, fy + drop), (fx + half, fy + drop),
               (fx + half, fy - drop)]
        draw.line(pts, fill=INK + (255,), width=line, joint="curve")
        for p in (pts[0], pts[-1]):
            draw.ellipse([p[0] - line / 2, p[1] - line / 2, p[0] + line / 2, p[1] + line / 2],
                         fill=INK + (255,))
        return img
    return mouse_icon(img, color, kind)


def esc_key():
    n = SIZE * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    c = n / 2
    box = [c - 0.42 * n, c - 0.31 * n, c + 0.42 * n, c + 0.33 * n]
    img, face = keycap(img, box, 0.10 * n)
    return legend(img, "Esc", face, 0.30 * n)


def wasd():
    n = SIZE * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    c = n / 2
    key, gap = 0.30 * n, 0.025 * n
    pitch = key + gap
    keys = {"W": (c, c - pitch / 2), "A": (c - pitch, c + pitch / 2),
            "S": (c, c + pitch / 2), "D": (c + pitch, c + pitch / 2)}
    for letter, (kx, ky) in keys.items():
        box = [kx - key / 2, ky - key / 2, kx + key / 2, ky + key / 2]
        img, face = keycap(img, box, 0.06 * n)
        img = legend(img, letter, face, 0.19 * n)
    return img


DIRECTIONS = {"up": (0, -1), "down": (0, 1), "left": (-1, 0), "right": (1, 0)}


def gradient(n, top, bottom):
    y = np.linspace(0.0, 1.0, n)[:, None, None]
    row = np.array(top) * (1 - y) + np.array(bottom) * y
    rgb = np.broadcast_to(row, (n, n, 3))
    return Image.fromarray((rgb * 255).astype(np.uint8), "RGB").convert("RGBA")


def paste_masked(base, layer, mask):
    layer = layer.copy()
    layer.putalpha(mask)
    return Image.alpha_composite(base, layer)


def arrow(n, d, tip, base, half):
    """Arrowhead polygon pointing along d, in supersampled pixels."""
    c = n / 2
    dx, dy = d
    px, py = -dy, dx
    return [(c + dx * tip, c + dy * tip),
            (c + dx * base + px * half, c + dy * base + py * half),
            (c + dx * base - px * half, c + dy * base - py * half)]


def along(n, d, a, w):
    """Point a pixels out along d and w pixels to its side, from the centre."""
    c = n / 2
    return (c + d[0] * a - d[1] * w, c + d[1] * a + d[0] * w)


def mask_of(n, pts):
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).polygon(pts, fill=255)
    return mask


def solid(n, rgb, alpha=255):
    return Image.new("RGBA", (n, n), tuple(rgb) + (alpha,))


def chrome(n, bounds):
    """The 360 cross's polished metal: bright top, a dark bronze band, a bright lower edge."""
    x0, y0, x1, y1 = bounds
    ys, xs = np.mgrid[0:n, 0:n].astype(np.float64)
    u = np.clip(((ys - y0) / (y1 - y0)) * 0.85 + ((xs - x0) / (x1 - x0)) * 0.15, 0.0, 1.0)
    stops = [0.0, 0.25, 0.5, 0.75, 1.0]
    colors = np.array([(0.96, 0.96, 0.98), (0.80, 0.80, 0.84), (0.46, 0.45, 0.47),
                       (0.70, 0.69, 0.71), (0.52, 0.52, 0.55)])
    rgb = np.stack([np.interp(u, stops, colors[:, i]) for i in range(3)], axis=-1)
    return Image.fromarray((rgb * 255).astype(np.uint8), "RGB").convert("RGBA")


def xbox_dpad(pressed):
    """A disc whose plus shaped plate fills the middle, with a patch in each corner."""
    n = SIZE * SS
    c = n / 2
    r = n / 2 * (1 - MARGIN) - SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))

    def disc(radius):
        mask = Image.new("L", (n, n), 0)
        ImageDraw.Draw(mask).ellipse([c - radius, c - radius, c + radius, c + radius], fill=255)
        return mask

    img = paste_masked(img, gradient(n, (0.62, 0.63, 0.66), (0.34, 0.35, 0.38)), disc(r))
    img = paste_masked(img, gradient(n, (0.02, 0.02, 0.03), (0.20, 0.20, 0.22)), disc(r * 0.88))
    face = disc(r * 0.80)

    arm = r * 0.30
    patches = Image.new("L", (n, n), 0)
    draw = ImageDraw.Draw(patches)
    for sx in (-1, 1):
        for sy in (-1, 1):
            x0, x1 = sorted((c + sx * arm, c + sx * r))
            y0, y1 = sorted((c + sy * arm, c + sy * r))
            draw.rounded_rectangle([x0, y0, x1, y1], radius=r * 0.16, fill=255)
    patches = ImageChops.multiply(patches, face)
    plate = ImageChops.subtract(face, patches)

    img = paste_masked(img, gradient(n, (0.50, 0.51, 0.55), (0.28, 0.29, 0.32)), patches)
    lit = ImageChops.subtract(patches, ImageChops.offset(patches, 0, 2 * SS))
    img = paste_masked(img, solid(n, (255, 255, 255)), lit.point(lambda v: v * 0.5))
    shadow = ImageChops.offset(plate, 0, 2 * SS).filter(ImageFilter.GaussianBlur(2 * SS))
    img = paste_masked(img, solid(n, (0, 0, 0)), shadow.point(lambda v: v * 0.6))
    img = paste_masked(img, chrome(n, (c - r, c - r, c + r, c + r)), plate)
    edge = ImageChops.subtract(plate, ImageChops.offset(plate, 2 * SS, 2 * SS))
    img = paste_masked(img, solid(n, (255, 255, 255)), edge.point(lambda v: v * 0.8))
    if pressed:
        d = DIRECTIONS[pressed]
        strip = mask_of(n, [along(n, d, 0, -arm), along(n, d, r, -arm),
                            along(n, d, r, arm), along(n, d, 0, arm)])
        strip = ImageChops.multiply(strip.filter(ImageFilter.GaussianBlur(6 * SS)), plate)
        img = paste_masked(img, solid(n, (255, 255, 255)), strip.point(lambda v: v * 0.55))
    return img


def label_text(img, text, cx, cy, height):
    """Bold white text in a thick black outline, the way the game letters every glyph."""
    font = bold_font(int(height))
    draw = ImageDraw.Draw(img)
    box = draw.textbbox((0, 0), text, font=font)
    tx = cx - (box[0] + box[2]) / 2
    ty = cy - (box[1] + box[3]) / 2
    draw.text((tx, ty), text, font=font, fill=(255, 255, 255, 255),
              stroke_width=max(2, int(height * 0.14)), stroke_fill=(0, 0, 0, 255))


def outlined(img, mask, top, bottom, edge=(0, 0, 0), width=2.5, shine=0.5):
    """Fills mask with a vertical gradient inside a dark outline and a lit upper edge."""
    n = img.size[0]
    grow = mask.filter(ImageFilter.MaxFilter(int(width * SS) * 2 + 1))
    img = paste_masked(img, solid(n, edge), grow)
    img = paste_masked(img, gradient(n, top, bottom), mask)
    hi = ImageChops.subtract(mask, ImageChops.offset(mask, 0, 3 * SS))
    return paste_masked(img, solid(n, (255, 255, 255)), hi.point(lambda v: v * shine))


def round_mask(mask, blur):
    return mask.filter(ImageFilter.GaussianBlur(blur)).point(lambda v: 255 if v > 128 else 0)


def ps_dpad(pressed):
    """Four separate grey keys, each a rounded kite pointing at the centre."""
    n = SIZE * SS
    r = n / 2 * (1 - MARGIN) - 1 * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    for name, d in DIRECTIONS.items():
        half = r * 0.34
        key = mask_of(n, [along(n, d, r * 0.20, -r * 0.06), along(n, d, r * 0.52, -half),
                          along(n, d, r * 0.98, -half * 0.86), along(n, d, r * 0.98, half * 0.86),
                          along(n, d, r * 0.52, half), along(n, d, r * 0.20, r * 0.06)])
        key = round_mask(key, 2.2 * SS)
        top, bottom = (((0.62, 0.63, 0.68), (0.40, 0.41, 0.46)) if name == pressed else
                       ((0.50, 0.51, 0.54), (0.26, 0.27, 0.30)))
        img = outlined(img, key, top, bottom, edge=(5, 5, 7), width=3.0, shine=0.35)
    return img

XBOX_SHOULDERS = {"lb": ("LB", False, False), "rb": ("RB", True, False),
                  "lt": ("LT", False, True), "rt": ("RT", True, True)}
PS_SHOULDERS = {"l1": ("L1", False, False), "r1": ("R1", True, False),
                "l2": ("L2", False, True), "r2": ("R2", True, True)}


def wheel_symbol(img, cx, cy, w, h, direction, fill, edge, ink):
    """A mouse and an arrow for the wheel's direction, as lettering: filled shapes in an outline."""
    n = img.size[0]
    x0, y0 = cx - w / 2, cy - h / 2
    x1, y1 = x0 + w, y0 + h
    mw = 0.50 * w
    shape = Image.new("L", (n, n), 0)
    draw = ImageDraw.Draw(shape)
    draw.rounded_rectangle([x0, y0, x0 + mw, y1], radius=mw / 2.2, fill=255)
    ax, head, shaft = x0 + 0.76 * w, 0.26 * w, 0.10 * w
    up = direction == "up"
    base = y0 + 0.45 * h if up else y1 - 0.45 * h
    draw.polygon([(ax - head / 2, base), (ax + head / 2, base), (ax, y0 if up else y1)], fill=255)
    draw.rectangle([ax - shaft / 2, base if up else y0, ax + shaft / 2, y1 if up else base], fill=255)
    grow = shape.filter(ImageFilter.MaxFilter(int(0.045 * min(w, h)) * 2 + 1))
    img = paste_masked(img, solid(n, edge), grow)
    img = paste_masked(img, solid(n, fill), shape)
    line = max(2, int(0.045 * w))
    draw = ImageDraw.Draw(img)
    split = y0 + 0.45 * (y1 - y0)
    mx = x0 + mw / 2
    draw.line([(x0, split), (x0 + mw, split)], fill=ink + (255,), width=line)
    draw.line([(mx, y0), (mx, split)], fill=ink + (255,), width=line)
    draw.rounded_rectangle([mx - line, y0 + 0.12 * h, mx + line, y0 + 0.34 * h], radius=line,
                           fill=ink + (255,))
    return img


def battle_wheel(direction):
    """The wheel glyph in the battle HUD's lettering: gold with a dark outline, no plate."""
    n = SIZE * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    w = 0.80 * n
    img = wheel_symbol(img, n / 2, n / 2, w, w * 22 / 34, direction, (178, 174, 70), (58, 46, 8),
                       (58, 46, 8))
    box = img.getbbox()
    img = img.crop(box)
    return img.resize((SIZE, int(SIZE * img.height / img.width)), Image.LANCZOS)


def shoulder(label, right, trigger, ps):
    """Shoulder buttons fill the tile like the game's own art: big plates, big lettering.

    Drawn as the left hand button; a right hand one mirrors the plate and keeps the
    lettering the right way round."""
    n = SIZE * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    mask = Image.new("L", (n, n), 0)
    draw = ImageDraw.Draw(mask)
    text_x, text_y, text_h = 0.5, 0.5, 0.30
    if ps:
        if trigger:
            draw.rounded_rectangle([0.06 * n, 0.08 * n, 0.94 * n, 0.78 * n], radius=0.13 * n, fill=255)
            draw.pieslice([0.06 * n, 0.48 * n, 0.94 * n, 0.92 * n], 0, 180, fill=255)
            text_y = 0.45
            text_h = 0.36
        else:
            draw.rounded_rectangle([0.04 * n, 0.16 * n, 0.96 * n, 0.84 * n], radius=0.12 * n, fill=255)
            text_y = 0.50
            text_h = 0.38
    elif trigger:
        draw.polygon([(0.30 * n, 0.04 * n), (0.76 * n, 0.04 * n), (0.80 * n, 0.96 * n),
                      (0.22 * n, 0.96 * n), (0.22 * n, 0.22 * n)], fill=255)
        mask = round_mask(mask, 0.03 * n)
        text_x, text_y, text_h = 0.52, 0.56, 0.30
    else:
        # Flat top and bottom, a blunt tapered outer end, a flat inner edge.
        draw.polygon([(1.3 * n, 0.22 * n), (1.3 * n, 0.78 * n), (0.30 * n, 0.78 * n),
                      (0.04 * n, 0.62 * n), (0.04 * n, 0.38 * n), (0.30 * n, 0.22 * n)], fill=255)
        mask = round_mask(mask, 0.07 * n)
        ImageDraw.Draw(mask).rectangle([0.97 * n, 0, n, n], fill=0)
        text_x, text_y, text_h = 0.57, 0.52, 0.36
    if right:
        mask = mask.transpose(Image.FLIP_LEFT_RIGHT)
        text_x = 1.0 - text_x

    if ps:
        inner = mask.filter(ImageFilter.MinFilter(int(0.05 * n) | 1))
        img = outlined(img, mask, (0.48, 0.48, 0.52), (0.20, 0.20, 0.22), width=2.0, shine=0.0)
        img = paste_masked(img, gradient(n, (0.34, 0.34, 0.37), (0.06, 0.06, 0.07)), inner)
    else:
        if trigger:
            img = outlined(img, mask, (0.88, 0.89, 0.92), (0.40, 0.41, 0.45), width=2.5, shine=0.7)
        else:
            img = outlined(img, mask, (0.97, 0.97, 0.98), (0.55, 0.55, 0.57), width=2.5, shine=0.9)
        if trigger:
            slot = Image.new("L", (n, n), 0)
            x0, x1 = (0.34, 0.70) if not right else (0.30, 0.66)
            ImageDraw.Draw(slot).rounded_rectangle([x0 * n, 0.10 * n, x1 * n, 0.20 * n],
                                                   radius=0.04 * n, fill=255)
            img = paste_masked(img, solid(n, (30, 30, 34)), ImageChops.multiply(slot, mask))
    if label.startswith("wheel_"):
        w = 0.44 * n if not trigger else 0.40 * n
        img = wheel_symbol(img, text_x * n, text_y * n, w, w * 0.78, label[6:],
                           (255, 255, 255), (0, 0, 0), (0, 0, 0))
    else:
        if len(label) > 2:
            # Centre on the plate at the lettering's row and shrink until it fits.
            row = mask.crop((0, int(text_y * n), n, int(text_y * n) + 1)).getbbox()
            span = row[2] - row[0]
            text_x = (row[0] + row[2]) / 2 / n
            text_h *= 0.85
            draw = ImageDraw.Draw(img)
            while text_h > 0.1:
                box = draw.textbbox((0, 0), label, font=bold_font(int(text_h * n)))
                if box[2] - box[0] + 0.3 * text_h * n <= 0.86 * span:
                    break
                text_h *= 0.95
        label_text(img, label, text_x * n, text_y * n, text_h * n)
    return img


MENU_BUTTONS = {"xbox": {"start": ("START", "right"), "back": ("BACK", "left")},
                "ps": {"start": ("START", "right"), "select": ("SELECT", "none")}}


def menu_button(kind, label, arrow_dir):
    """Start and Back/Select: a small badge on top and its name across the bottom."""
    n = SIZE * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    ps = kind == "ps"
    cx = n / 2
    shape = Image.new("L", (n, n), 0)
    draw = ImageDraw.Draw(shape)
    if ps and arrow_dir == "right":
        a = 0.30 * n
        cy = 0.33 * n
        pts = [(cx + a, cy), (cx - a * 0.8, cy - a * 0.95), (cx - a * 0.8, cy + a * 0.95)]
        draw.polygon(pts, fill=255)
        shape = round_mask(shape, 0.012 * n)
        img = outlined(img, shape, (0.46, 0.46, 0.50), (0.18, 0.18, 0.21), edge=(5, 5, 7),
                       width=3.0, shine=0.3)
    elif ps and arrow_dir == "none":
        draw.rounded_rectangle([0.08 * n, 0.06 * n, 0.92 * n, 0.54 * n], radius=0.10 * n, fill=255)
        img = outlined(img, shape, (0.36, 0.36, 0.40), (0.12, 0.12, 0.14), width=2.5, shine=0.35)
        inset = Image.new("L", (n, n), 0)
        ImageDraw.Draw(inset).rounded_rectangle([0.15 * n, 0.12 * n, 0.85 * n, 0.48 * n],
                                                radius=0.07 * n, fill=255)
        img = paste_masked(img, gradient(n, (0.20, 0.20, 0.23), (0.07, 0.07, 0.08)), inset)
    else:
        cy = 0.33 * n
        draw.ellipse([0.08 * n, 0.02 * n, 0.92 * n, 0.64 * n], fill=255)
        if ps:
            img = outlined(img, shape, (0.30, 0.30, 0.34), (0.03, 0.03, 0.04), width=2.5, shine=0.3)
            ink = (140, 140, 148)
        else:
            img = outlined(img, shape, (0.94, 0.95, 0.97), (0.52, 0.53, 0.58), width=2.5, shine=0.8)
            ink = (92, 94, 100)
        a = 0.17 * n
        sign = 1 if arrow_dir == "right" else -1
        pts = [(cx + sign * a * 1.3, cy), (cx - sign * a * 0.9, cy - a * 1.25),
               (cx - sign * a * 0.9, cy + a * 1.25)]
        tri = Image.new("L", (n, n), 0)
        ImageDraw.Draw(tri).polygon(pts, fill=255)
        edge = tri.filter(ImageFilter.MaxFilter(int(2 * SS) * 2 + 1))
        img = paste_masked(img, solid(n, (30, 30, 34)), edge)
        img = paste_masked(img, solid(n, ink), tri)
    label_text(img, label, cx, 0.83 * n, (0.27 if len(label) < 6 else 0.22) * n)
    return img

def solid_size(size, rgb):
    return Image.new("RGBA", size, tuple(rgb) + (255,))


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, (color, label) in XBOX.items():
        xbox(color, label).resize((SIZE, SIZE), Image.LANCZOS).save(
            os.path.join(OUT, f"button_xbox_{name}.png"))
    for name, (color, glyph) in PLAYSTATION.items():
        playstation(color, glyph).resize((SIZE, SIZE), Image.LANCZOS).save(
            os.path.join(OUT, f"button_ps_{name}.png"))
    for name, color in PC_PS_COLORS.items():
        pc_button(color, PC[name][1]).resize((SIZE, SIZE), Image.LANCZOS).save(
            os.path.join(OUT, f"button_pc_ps3_{name}.png"))
    for name, (color, kind) in PC.items():
        pc_button(color, kind).resize((SIZE, SIZE), Image.LANCZOS).save(
            os.path.join(OUT, f"button_pc_360_{name}.png"))
    esc_key().resize((SIZE, SIZE), Image.LANCZOS).save(os.path.join(OUT, "button_pc_esc.png"))
    wasd().resize((SIZE, SIZE), Image.LANCZOS).save(os.path.join(OUT, "dpad_pc_neutral.png"))
    for kind, render in (("xbox", xbox_dpad), ("ps", ps_dpad)):
        render(None).resize((SIZE, SIZE), Image.LANCZOS).save(
            os.path.join(OUT, f"dpad_{kind}_neutral.png"))
    for kind, table in (("xbox", XBOX_SHOULDERS), ("ps", PS_SHOULDERS)):
        for name, (label, right, trigger) in table.items():
            shoulder(label, right, trigger, kind == "ps").resize((SIZE, SIZE), Image.LANCZOS).save(
                os.path.join(OUT, f"button_{kind}_{name}.png"))
    for kind, ps in (("360", False), ("ps3", True)):
        for name, label, right, trigger in (("lb", "wheel_up", False, False),
                                            ("rb", "wheel_down", True, False),
                                            ("lt", "ALT", False, True),
                                            ("rt", "CTRL", True, True)):
            shoulder(label, right, trigger, ps).resize((SIZE, SIZE), Image.LANCZOS).save(
                os.path.join(OUT, f"button_pc_{kind}_{name}.png"))
    for direction in ("up", "down"):
        battle_wheel(direction).save(os.path.join(OUT, f"button_pc_battle_wheel_{direction}.png"))
    for kind, table in MENU_BUTTONS.items():
        for name, (label, arrow_dir) in table.items():
            menu_button(kind, label, arrow_dir).resize((SIZE, SIZE), Image.LANCZOS).save(
                os.path.join(OUT, f"button_{kind}_{name}.png"))


if __name__ == "__main__":
    main()
