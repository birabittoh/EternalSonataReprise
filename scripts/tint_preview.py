#!/usr/bin/env python3
"""Previews a costume tint rule (assets.toml [[costume.tint]], see
docs/costumes.md) on a model's textures and on portraits, to tune it offline.

    tint_preview.py OUT.png --model assets-build/360-pal/AppKeep.bmd:1
        --images assets-build/360-pal/AppKeep.bmd:196,206
        --from BD6952 --to 517EBD,383638 [--textures skirt.tga,4]
        [--hue-range 20] [--min-saturation 0.4] [--max-saturation 1]
        [--min-value 0] [--max-value 1] [--mask]

--model is any models.py source (a PS3 .p3obj too); --textures takes names
or indices as `models.py list -v` orders them, every texture when left out.
--images are AppKeep.bmd entries (image id minus one). The first row is
untinted; --mask paints what the rule catches red instead. The maths mirrors
src/engine/costume_tint.cpp; keep them in step.
"""
import argparse
import io
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import models as M  # noqa: E402

HUE_FEATHER, SATURATION_FEATHER, SATURATION_SHARE = 0.03, 0.1, 0.65
GREY_SATURATION = 0.05


def to_hsv(c):
    hi, lo = c.max(1), c.min(1)
    d = hi - lo
    s = np.where(hi > 0, d / np.maximum(hi, 1e-6), 0)
    r, g, b = c.T
    dd = np.maximum(d, 1e-6)
    h = np.where(hi == r, (g - b) / dd, np.where(hi == g, 2 + (b - r) / dd, 4 + (r - g) / dd)) / 6 % 1
    return np.where(d > 0, h, 0), s, hi


def to_rgb(h, s, v):
    h6 = (h % 1) * 6
    i = np.floor(h6).astype(int) % 6
    f = h6 - np.floor(h6)
    p, q, t = v * (1 - s), v * (1 - f * s), v * (1 - (1 - f) * s)
    return np.stack([np.choose(i, [v, q, p, p, t, v]), np.choose(i, [t, v, v, q, p, p]),
                     np.choose(i, [p, p, t, v, v, q])], 1)


def hsv_of(text):
    text = text.lstrip('#')
    return [x[0] for x in to_hsv(np.array([[int(text[k:k + 2], 16) / 255 for k in (0, 2, 4)]]))]


def band(x, lo, hi, feather):
    return np.clip((x - lo) / feather + 1, 0, 1) * np.clip((hi - x) / feather + 1, 0, 1)


def tint_fn(args, to, mask):
    fh, fs, fv = hsv_of(args.from_)
    th, ts, tv = hsv_of(to)
    grey = fs < GREY_SATURATION
    lo, hi = fh - args.hue_range / 360, fh + args.hue_range / 360
    smin = args.min_saturation if args.min_saturation is not None else \
        0 if grey else fs * SATURATION_SHARE

    def fn(rgb):
        c = rgb / 255
        h, s, v = to_hsv(c)
        h = lo - HUE_FEATHER + (h - lo + HUE_FEATHER) % 1
        w = (1 if grey else band(h, lo, hi, HUE_FEATHER)) \
            * band(s, smin, args.max_saturation, SATURATION_FEATHER) \
            * band(v, args.min_value, args.max_value, SATURATION_FEATHER)
        w = (w * np.ones_like(h))[:, None]
        if mask:
            return (c * (1 - w) + np.array([1, 0, 0]) * w) * 255
        nv = np.clip(v * tv / fv, 0, 1)
        if grey:
            n = to_rgb(np.full_like(h, th), np.full_like(h, ts), nv)
        else:
            n = to_rgb(h + th - fh, np.clip(s * ts / max(fs, 1e-3), 0, 1), nv)
        return (c + (n - c) * w) * 255
    return fn


def tint_dds(dds, fn):
    """A DXT3 / DXT5 DDS with its colour endpoints passed through fn."""
    out = bytearray(dds)
    n = (len(out) - 128) // 16 * 8
    w = np.frombuffer(bytes(out[128:128 + 2 * n]), '<u2').reshape(-1, 8).copy()
    for k in (4, 5):
        c = w[:, k].astype(int)
        rgb = np.stack([(c >> 11 & 31) * 255 / 31, (c >> 5 & 63) * 255 / 63, (c & 31) * 255 / 31], 1)
        o = np.clip(np.rint(fn(rgb) / np.array([255 / 31, 255 / 63, 255 / 31])), 0, [31, 63, 31])
        o = o.astype(int)
        w[:, k] = (o[:, 0] << 11) | (o[:, 1] << 5) | o[:, 2]
    out[128:128 + w.nbytes] = w.tobytes()
    return bytes(out)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument('out')
    p.add_argument('--model', help='SOURCE[:ENTRY], as models.py takes it')
    p.add_argument('--images', default='', help='BMD:ENTRY,ENTRY,...')
    p.add_argument('--textures', default='')
    p.add_argument('--from', dest='from_', required=True)
    p.add_argument('--to', default='517EBD,4E9A4E,383638,D8D8DC')
    p.add_argument('--hue-range', type=float, default=20)
    p.add_argument('--min-saturation', type=float)
    p.add_argument('--max-saturation', type=float, default=1)
    p.add_argument('--min-value', type=float, default=0)
    p.add_argument('--max-value', type=float, default=1)
    p.add_argument('--mask', action='store_true')
    args = p.parse_args()

    textures, model = [], b''
    if args.model:
        _, models = M.load_source(args.model)
        model = models[0][1]
        wanted = [t for t in args.textures.split(',') if t]
        found = [M.Texture(model, off, size) for _, tag, off, size in M.model_chunks(model)
                 if tag in (b'NTX2', b'NTEX')]
        textures = [t for i, t in enumerate(found)
                    if not wanted or str(i) in wanted or (t.name and t.name in wanted)]
    portraits = []
    if args.images:
        bmd, entries = args.images.rsplit(':', 1)
        all_entries = M.bmd_entries(M.load_bmd(bmd))
        portraits = [all_entries[int(i)][8:] for i in entries.split(',')]
    rows = [(None, False), (args.from_, True)] if args.mask else \
        [(None, False)] + [(t, False) for t in args.to.split(',')]
    size = 192
    columns = len(textures) + len(portraits)
    sheet = Image.new('RGBA', (size * columns, size * len(rows)), (60, 60, 60, 255))
    for y, (to, mask) in enumerate(rows):
        tinted = bytearray(model)
        images = []
        if to:
            fn = tint_fn(args, to, mask)
            for t in textures:
                M.recolor_texture(tinted, t, fn)
            images = [M.texture_dds(tinted, t) for t in textures] + [tint_dds(d, fn) for d in portraits]
        else:
            images = [M.texture_dds(tinted, t) for t in textures] + portraits
        for x, dds in enumerate(images):
            image = Image.open(io.BytesIO(dds)).convert('RGBA')
            image.thumbnail((size, size))
            sheet.alpha_composite(image, (x * size, y * size))
    sheet.save(args.out)


if __name__ == '__main__':
    main()
