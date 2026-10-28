#!/usr/bin/env python3
"""Previews a costume tint rule (assets.toml [[costume.tint]], see
docs/costumes.md) on a model's textures and on portraits, to tune it offline.

    tint_preview.py OUT.png --model assets-build/360-pal/AppKeep.bmd:1
        --images assets-build/360-pal/AppKeep.bmd:196,206
        --from BD6952 --to 517EBD,383638 [--textures skirt.tga,4]
        [--hue-range 20] [--min-saturation 0.4] [--max-saturation 1]
        [--min-value 0] [--max-value 1] [--mask]
        [--rects "[[96, 40, 300, 180]]"] [--exclude "[[150, 90, 230, 170]]"]
        [--region-mask hair.png]

--model is any models.py source (a PS3 .p3obj too); --textures takes names
or indices as `models.py list -v` orders them, every texture when left out.
--images are AppKeep.bmd entries (image id minus one). The first row is
untinted; --mask paints what the rule catches red instead. --rects, --exclude
and --region-mask are a portrait rule's region, applied to the images only.
The maths mirrors src/engine/costume_tint.cpp; keep them in step. tint_dds and
rule_fn take a rule as assets.toml spells it, for other scripts to import.
"""
import argparse
import io
import json
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


def rule_fn(rule, mask=False):
    """fn(rgb 0..255 rows, weight per row) for one rule, as assets.toml spells
    it; with `mask`, what it catches turns red instead."""
    fh, fs, fv = hsv_of(rule['from'])
    th, ts, tv = hsv_of(rule['to'])
    grey = fs < GREY_SATURATION
    hue_range = rule.get('hue_range', 20) / 360
    lo, hi = fh - hue_range, fh + hue_range
    smin = rule.get('min_saturation', 0 if grey else fs * SATURATION_SHARE)
    smax = rule.get('max_saturation', 1)
    vmin, vmax = rule.get('min_value', 0), rule.get('max_value', 1)

    def fn(rgb, region=1):
        c = rgb / 255
        h, s, v = to_hsv(c)
        h = lo - HUE_FEATHER + (h - lo + HUE_FEATHER) % 1
        w = (1 if grey else band(h, lo, hi, HUE_FEATHER)) \
            * band(s, smin, smax, SATURATION_FEATHER) \
            * band(v, vmin, vmax, SATURATION_FEATHER)
        w = (w * np.ones_like(h) * region)[:, None]
        if mask:
            return (c * (1 - w) + np.array([1, 0, 0]) * w) * 255
        if tv > fv > 1e-3:
            # Brightening follows 1 - e^(-k v), which rolls off before white.
            nv = 1 - np.exp(np.log(1 - min(tv, 0.999)) / fv * v)
        else:
            nv = np.clip(v * tv / fv, 0, 1)
        if grey:
            n = to_rgb(np.full_like(h, th), np.full_like(h, ts), nv)
        else:
            n = to_rgb(h + th - fh, np.clip(s * ts / max(fs, 1e-3), 0, 1), nv)
        return (c + (n - c) * w) * 255
    return fn


def rects_of(value):
    """[[x0, y0, x1, y1], ...], or a lone [x0, y0, x1, y1]."""
    if not value:
        return []
    return [value] if not isinstance(value[0], list) else value


def mask_of(path):
    """A mask image as the host reads it: grey, transparent counting as black."""
    a = np.asarray(Image.open(path).convert('RGBA')).astype(int)
    return (a[..., 0] * 77 + a[..., 1] * 150 + a[..., 2] * 29) // 256 * a[..., 3] // 255


REGION_FIELDS = ('rects', 'exclude', 'mask')


def kind_region(layers, kind):
    """The region one portrait kind gets from region key tables applied in
    order, a [[portrait_region]] and then the rule itself, as the host does:
    in each, the keys for every kind and then `<kind>_` ones."""
    out = {}
    for keys in layers:
        for field in REGION_FIELDS:
            if field in keys:
                out[field] = keys[field]
        for field in REGION_FIELDS:
            if f'{kind}_{field}' in keys:
                out[field] = keys[f'{kind}_{field}']
    return out


def region_weights(rule, width, height, blocks, root='.'):
    """Per block weights of a portrait region (rects, exclude, mask) over a
    width by height mip chain, or None when it has none: RegionWeights in
    costume_tint.cpp."""
    rects, exclude = rects_of(rule.get('rects')), rects_of(rule.get('exclude'))
    mask = mask_of(os.path.join(root, rule['mask'])) if rule.get('mask') else None
    if not rects and not exclude and mask is None:
        return None
    offsets = (np.arange(4) + 0.5) / 4
    weights = []
    w, h = width, height
    while len(weights) < blocks:
        sx, sy = width / w, height / h
        bx, by = np.arange((w + 3) // 4), np.arange((h + 3) // 4)
        x0, x1 = bx * 4 * sx, np.minimum((bx + 1) * 4, w) * sx
        y0, y1 = by * 4 * sy, np.minimum((by + 1) * 4, h) * sy
        xs = (x0[:, None] + (x1 - x0)[:, None] * offsets)[None, :, None, :]
        ys = (y0[:, None] + (y1 - y0)[:, None] * offsets)[:, None, :, None]
        xs, ys = np.broadcast_arrays(xs, ys)

        def inside(r):
            return (xs >= r[0]) & (xs < r[2]) & (ys >= r[1]) & (ys < r[3])
        ok = np.ones(xs.shape, bool)
        if rects:
            ok &= np.any([inside(r) for r in rects], 0)
        for r in exclude:
            ok &= ~inside(r)
        value = ok.astype(float)
        if mask is not None:
            mh, mw = mask.shape
            mx = np.minimum((xs / width * mw).astype(int), mw - 1)
            my = np.minimum((ys / height * mh).astype(int), mh - 1)
            value *= mask[my, mx] / 255
        weights.extend(value.mean((2, 3)).ravel())
        if w == 1 and h == 1:
            break
        w, h = max(1, w // 2), max(1, h // 2)
    return np.array((weights + [0.0] * blocks)[:blocks])


QUANT = np.array([31, 63, 31])


def unpack565(c):
    return np.stack([c >> 11 & 31, c >> 5 & 63, c & 31], -1) / QUANT


def pack565(rgb):
    o = np.clip(np.rint(rgb * QUANT), 0, QUANT).astype(np.int64)
    return (o[..., 0] << 11) | (o[..., 1] << 5) | o[..., 2]


FOUR_POS = np.array([0, 1, 1 / 3, 2 / 3])  # by index, from c0 to c1
THREE_POS = np.array([0, 1, 1 / 2, 0])     # 3 is transparent


def fit_line(t, counts, pos):
    """Least squares endpoints for points t (n, 4, 3) weighted by counts at
    positions pos (n, 4) along the line; None where they don't fix one."""
    P = (counts * (1 - pos) ** 2).sum(1)[:, None]
    Q = (counts * pos * (1 - pos)).sum(1)[:, None]
    R = (counts * pos * pos).sum(1)[:, None]
    X = ((counts * (1 - pos))[..., None] * t).sum(1)
    Y = ((counts * pos)[..., None] * t).sum(1)
    det = (P * R - Q * Q)[:, 0]
    solved = det > 1e-6 * np.maximum(counts.sum(1), 1) ** 2
    d = np.where(solved, det, 1)[:, None]
    return (R * X - Q * Y) / d, (P * Y - Q * X) / d, solved


def nearest(t, e0, e1, choices):
    """For every point, the index among `choices` (positions by index, -1 for
    none) whose colour on the e0..e1 line is nearest."""
    pal = e0[:, None] + (e1 - e0)[:, None] * choices[:, :, None]   # n, 4, 3
    dist = ((t[:, :, None] - pal[:, None]) ** 2).sum(-1)              # n, point, index
    dist = np.where(choices[:, None] < 0, np.inf, dist)
    return np.argmin(dist, -1)


def tint_colour_blocks(colour, fn, region=None, dxt1=False):
    """DXT colour blocks, (n, 4) words c0, c1 and the two index words, tinted
    by fn: TintBlocks in costume_tint.cpp. Each palette colour is tinted (so
    every pixel is), then the block is encoded again, endpoints and indices
    both, block by block, so block order and tiling don't matter."""
    colour = colour.astype(np.int64).copy()
    n = len(colour)
    region = np.ones(n) if region is None else region
    c0, c1 = colour[:, 0], colour[:, 1]
    idx = colour[:, 2] | colour[:, 3] << 16
    four = (c0 > c1) if dxt1 else np.ones(n, bool)
    old_pos = np.where(four[:, None], FOUR_POS, THREE_POS)
    pixel = np.stack([idx >> (2 * i) & 3 for i in range(16)], 1)
    counts = np.stack([(pixel == k).sum(1) for k in range(4)], 1).astype(float)
    # Three colour mode's 3 is transparent and stays so.
    transparent = ~four & (counts[:, 3] > 0)
    counts[~four, 3] = 0
    e0, e1 = unpack565(c0), unpack565(c1)
    palette = e0[:, None] + (e1 - e0)[:, None] * old_pos[..., None]
    t = fn(palette.reshape(-1, 3) * 255, np.repeat(region, 4)).reshape(n, 4, 3) / 255
    changed = (region > 0) & (np.abs(t - palette).max((1, 2)) > 0.5 / 255)

    # Start from the principal axis of the tinted colours.
    total = np.maximum(counts.sum(1), 1)[:, None]
    mean = (counts[..., None] * t).sum(1) / total
    d = t - mean[:, None]
    cov = np.einsum('nk,nki,nkj->nij', counts, d, d)
    axis = np.ones((n, 3))
    for _ in range(8):
        axis = np.einsum('nij,nj->ni', cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1), 1e-12)[:, None]
    proj = np.einsum('nki,ni->nk', d, axis)
    lo = np.where(counts > 0, proj, np.inf).min(1)
    hi = np.where(counts > 0, proj, -np.inf).max(1)
    lo, hi = np.where(np.isfinite(lo), lo, 0), np.where(np.isfinite(hi), hi, 0)
    f0, f1 = mean + axis * lo[:, None], mean + axis * hi[:, None]

    # Choose indices against the line, refit it to them, twice.
    choices = np.where(transparent[:, None], [0, 1, 1 / 2, -1], FOUR_POS)
    for _ in range(2):
        pick = nearest(t, f0, f1, choices)
        g0, g1, solved = fit_line(t, counts, np.take_along_axis(choices, pick, 1))
        f0 = np.where(solved[:, None], np.clip(g0, 0, 1), f0)
        f1 = np.where(solved[:, None], np.clip(g1, 0, 1), f1)
    n0, n1 = pack565(f0), pack565(f1)
    if dxt1:
        # c0 > c1 selects four colour mode, c0 <= c1 three with transparency.
        flip = np.where(transparent, n0 > n1, n0 < n1)
        n0, n1 = np.where(flip, n1, n0), np.where(flip, n0, n1)
    q0, q1 = unpack565(n0), unpack565(n1)
    pick = nearest(t, q0, q1, choices)
    if dxt1:
        # Equal endpoints decode as three colours, where 3 is transparent.
        pick = np.where((~transparent & (n0 == n1))[:, None], 0, pick)
    pick[:, 3] = np.where(transparent, 3, pick[:, 3])
    new_idx = np.zeros(n, np.int64)
    for i in range(16):
        new_idx |= np.take_along_axis(pick, pixel[:, i:i + 1], 1)[:, 0] << (2 * i)
    out = np.stack([n0, n1, new_idx & 0xFFFF, new_idx >> 16], 1)
    out[~changed] = colour[~changed]
    return out.astype(np.uint16)


def tint_dds(dds, fn, rule=None, root='.'):
    """A DXT3 / DXT5 DDS tinted by fn, within the region `rule` (rects,
    exclude, mask) has, if any."""
    out = bytearray(dds)
    n = (len(out) - 128) // 16 * 8
    w = np.frombuffer(bytes(out[128:128 + 2 * n]), '<u2').reshape(-1, 8).copy()
    height, width = np.frombuffer(bytes(out[12:20]), '<u4')
    region = region_weights(rule, int(width), int(height), len(w), root) if rule else None
    w[:, 4:] = tint_colour_blocks(w[:, 4:], fn, region)
    out[128:128 + w.nbytes] = w.tobytes()
    return bytes(out)


def tint_texture(model, tex, fn):
    """A model texture's pixels in `model` (a bytearray), tinted by fn."""
    if not tex.blocks:
        return
    start = tex.off + tex.pix_off
    be = tex.endian == 1
    words = np.frombuffer(bytes(model[start:start + tex.pix_size]), '>u2' if be else '<u2')
    stride = 4 if tex.format == 'DXT1' else 8
    w = words.astype(np.uint16).reshape(-1, stride).copy()
    w[:, -4:] = tint_colour_blocks(w[:, -4:], fn, dxt1=tex.format == 'DXT1')
    model[start:start + tex.pix_size] = w.astype('>u2' if be else '<u2').tobytes()


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
    p.add_argument('--rects', help='portrait region, as assets.toml writes it')
    p.add_argument('--exclude', help='portrait pixels to leave, the same way')
    p.add_argument('--region-mask', help='grey image, white tints')
    args = p.parse_args()

    rule = {'from': args.from_, 'hue_range': args.hue_range, 'max_saturation': args.max_saturation,
            'min_value': args.min_value, 'max_value': args.max_value}
    if args.min_saturation is not None:
        rule['min_saturation'] = args.min_saturation
    region = {'rects': json.loads(args.rects) if args.rects else None,
              'exclude': json.loads(args.exclude) if args.exclude else None,
              'mask': os.path.abspath(args.region_mask) if args.region_mask else None}

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
            fn = rule_fn(dict(rule, to=to), mask)
            for t in textures:
                tint_texture(tinted, t, fn)
            images = [M.texture_dds(tinted, t) for t in textures] + \
                [tint_dds(d, fn, region) for d in portraits]
        else:
            images = [M.texture_dds(tinted, t) for t in textures] + portraits
        for x, dds in enumerate(images):
            image = Image.open(io.BytesIO(dds)).convert('RGBA')
            image.thumbnail((size, size))
            sheet.alpha_composite(image, (x * size, y * size))
    sheet.save(args.out)


if __name__ == '__main__':
    main()
