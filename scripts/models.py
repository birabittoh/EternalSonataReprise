#!/usr/bin/env python3
"""List, dump and repackage Eternal Sonata field models (NOBJ chunks).

A source is any of:
  * a decoded .bmd (AppKeep.bmd): "path/AppKeep.bmd" or "path/AppKeep.bmd:1"
  * an encoded game file, found through the index.vmtoc next to it
    (assets-build/AppKeep.bmd)
  * a bare NOBJ file (.nobj, .p3obj converted by the installer)

  models.py list [SOURCE]
  models.py dump SOURCE[:ENTRY] OUTDIR [--png]
  models.py pack DIR OUT.nobj

`dump` writes model.nobj (the entry verbatim), manifest.txt and one file per
chunk of the model under chunks/. `pack` rebuilds a NOBJ from the manifest,
taking every chunk from its file, so an edited chunk replaces the original.
The result is what assets.toml `[[costume]] model = ...` takes.

Chunks may change size only in multiples of 4096: an NTX2's pixels sit at the
first 4 KB aligned offset after its header, and the guest places a model on a
4 KB boundary, so anything else moves them.
"""
import argparse
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_SOURCE = os.path.join(REPO, 'assets-build', 'AppKeep.bmd')

# AppKeep.bmd entries 0..9 are the field characters, March and Claves swapped.
CHARACTERS = ['Allegretto', 'Polka', 'Beat', 'Frederic', 'Viola', 'Salsa', 'Jazz', 'Falsetto',
              'March', 'Claves']

TILE_FORMATS = {18: 'DXT1', 19: 'DXT3', 20: 'DXT5'}


def be32(d, o):
    return struct.unpack_from('>I', d, o)[0]


# ---------------------------------------------------------------------------
# Containers
# ---------------------------------------------------------------------------

def load_bmd(path):
    """The decoded bytes of a .bmd, decoding it through its index.vmtoc if needed."""
    data = open(path, 'rb').read()
    if data[:4] == b'BMD ':
        return data
    from unpack_e import unpack_file
    root = os.path.dirname(os.path.abspath(path))
    out, _, _ = unpack_file(os.path.basename(path), root)
    return out


def bmd_entries(data):
    count = be32(data, 8)
    starts = [be32(data, 12 + 4 * i) for i in range(count)]
    ends = sorted(set(starts + [len(data)]))
    out = []
    for s in starts:
        if s == 0:
            out.append(b'')
            continue
        e = next(x for x in ends if x > s)
        out.append(data[s:e])
    return out


def chunks(data, start, end):
    """(tag, offset, size) of the sibling chunks in [start, end)."""
    o = start
    while o + 8 <= end:
        size = be32(data, o + 4)
        if size < 8 or o + size > end:
            break
        yield data[o:o + 4], o, size
        o += size


def nmdl_body(data, offset):
    """First child offset of an NMDL: its header is 64 bytes, 96 with flag 2."""
    return offset + (96 if struct.unpack_from('>H', data, offset + 32)[0] & 2 else 64)


def model_chunks(model):
    """Flat list of (path, tag, offset, size) of one NOBJ: its direct children
    and the children of its NMDL, in file order."""
    if model[:4] != b'NOBJ':
        raise ValueError('not a NOBJ')
    out = []
    for tag, off, size in chunks(model, 8, len(model)):
        if tag == b'NMDL':
            out.append(('NMDL', tag, off, size))
            for ctag, coff, csize in chunks(model, nmdl_body(model, off), off + size):
                out.append(('NMDL/' + ctag.decode('latin1'), ctag, coff, csize))
        else:
            out.append((tag.decode('latin1'), tag, off, size))
    return out


def chunk_name(model, tag, off):
    """The name a chunk carries, or ''."""
    if tag == b'NTX2':
        return model[off + 0x2C:off + 0x4C].split(b'\0')[0].decode('latin1')
    if tag == b'NSHP':
        return model[off + 8:off + 40].split(b'\0')[0].decode('latin1')
    if tag in (b'NBN2', b'NMTN'):
        return model[off + 8:off + 40].split(b'\0')[0].decode('latin1')
    return ''


# ---------------------------------------------------------------------------
# Textures
# ---------------------------------------------------------------------------

class Texture:
    """An NTX2 (XPR2 wrapped, tiled, big endian) or NTEX (a PC DDS) chunk."""

    def __init__(self, model, off, size):
        self.off, self.size = off, size
        tag = model[off:off + 4]
        self.ntex = tag == b'NTEX'
        if self.ntex:
            dds = model[off + 8:off + size]
            self.height, self.width = struct.unpack_from('<II', dds, 12)
            self.format = dds[84:88].decode('latin1')
            self.name = ''
            self.pix_off = 8 + 128
            self.pix_size = size - self.pix_off
            self.endian = 0
            self.tiled = False
        else:
            hdr = be32(model, off + 0x0C)
            self.pix_size = be32(model, off + 0x10)
            name_size = be32(model, off + 0x1C)
            fetch = off + 0x18 + name_size + 0x18
            w0, w1, w2 = struct.unpack_from('>III', model, fetch)
            self.format = TILE_FORMATS.get(w1 & 0x3F, 'fmt%d' % (w1 & 0x3F))
            self.width = (w2 & 0x1FFF) + 1
            self.height = ((w2 >> 13) & 0x1FFF) + 1
            self.endian = (w1 >> 6) & 3
            self.tiled = bool(w0 >> 31)
            self.name = chunk_name(model, tag, off)
            self.pix_off = ((off + 8 + hdr + 0xFFF) & ~0xFFF) - off

    @property
    def blocks(self):
        return self.format in ('DXT1', 'DXT3', 'DXT5')


def untile(raw, width, height, bpb):
    """Level 0 of a tiled texture as linear rows of blocks (xenia's addressing)."""
    log2 = bpb.bit_length() - 1
    wb, hb = (width + 3) // 4, (height + 3) // 4
    pitch = (wb + 31) // 32 * 32
    ys, xs = np.mgrid[0:hb, 0:wb].astype(np.int64)
    macro = ((xs >> 5) + (ys >> 5) * (pitch >> 5)) << (log2 + 7)
    micro = ((xs & 7) + ((ys & 0xE) << 2)) << log2
    off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((ys & 1) << 4)
    addr = (((off & ~0x1FF) << 3) + ((ys & 16) << 7) + ((off & 0x1C0) << 2)
            + (((((ys & 8) >> 2) + (xs >> 3)) & 3) << 6) + (off & 0x3F))
    buf = np.frombuffer(raw, dtype=np.uint8)
    idx = (addr[..., None] + np.arange(bpb)).reshape(hb, wb * bpb)
    return buf[idx]


def texture_dds(model, tex):
    """A PC DDS of the texture's top level, for viewing."""
    pix = model[tex.off + tex.pix_off:tex.off + tex.pix_off + tex.pix_size]
    bpb = 8 if tex.format == 'DXT1' else 16
    if tex.ntex:
        level = pix[:((tex.width + 3) // 4) * ((tex.height + 3) // 4) * bpb]
    else:
        raw = pix
        if tex.endian == 1:
            raw = np.frombuffer(pix, dtype='>u2').astype('<u2').tobytes()
        level = untile(raw, tex.width, tex.height, bpb).tobytes() if tex.tiled else raw
    hdr = bytearray(128)
    hdr[0:4] = b'DDS '
    struct.pack_into('<7I', hdr, 4, 124, 0x81007, tex.height, tex.width, len(level), 0, 0)
    struct.pack_into('<II4s', hdr, 76, 32, 4, tex.format.encode())
    struct.pack_into('<I', hdr, 108, 0x1000)
    return bytes(hdr) + level


def recolor_texture(model, tex, fn):
    """Replaces the model's pixels of `tex` in place with `fn(rgb)` applied to every
    DXT color endpoint. `rgb` is a float array (n, 3) in 0..255, `fn` returns the
    same. Alpha, indices and layout are untouched, so tiling does not matter."""
    if not tex.blocks:
        return False
    start = tex.off + tex.pix_off
    raw = bytearray(model[start:start + tex.pix_size])
    be = tex.endian == 1
    w = np.frombuffer(bytes(raw), dtype='>u2' if be else '<u2').astype('<u2').copy()
    stride = 4 if tex.format == 'DXT1' else 8
    colour = w.reshape(-1, stride)[:, -4:] if stride == 8 else w.reshape(-1, 4)
    c0, c1 = colour[:, 0].astype(np.int32), colour[:, 1].astype(np.int32)
    idx = colour[:, 2].astype(np.uint32) | (colour[:, 3].astype(np.uint32) << 16)
    four = c0 > c1 if tex.format == 'DXT1' else np.ones(len(c0), bool)

    def unpack(c):
        return np.stack([(c >> 11) & 31, (c >> 5) & 63, c & 31], 1) * np.array(
            [255 / 31, 255 / 63, 255 / 31])

    def pack(rgb):
        rgb = np.clip(np.rint(rgb / np.array([255 / 31, 255 / 63, 255 / 31])), 0, [31, 63, 31])
        rgb = rgb.astype(np.int32)
        return (rgb[:, 0] << 11) | (rgb[:, 1] << 5) | rgb[:, 2]

    n0, n1 = pack(fn(unpack(c0))), pack(fn(unpack(c1)))
    if tex.format == 'DXT1':
        # c0 > c1 selects four colour mode, so keep that ordering: swap the
        # endpoints and the indices that name them when the tint inverted it.
        flip = np.where(four, n0 < n1, n0 > n1)
        n0, n1 = np.where(flip, n1, n0), np.where(flip, n0, n1)
        swap = np.zeros(len(c0), np.uint32)
        for i in range(16):
            bits = (idx >> (2 * i)) & 3
            new = np.where(four, bits ^ 1, np.where(bits < 2, bits ^ 1, bits))
            swap |= np.where(flip, new, bits).astype(np.uint32) << (2 * i)
        idx = swap
        tie = four & (n0 == n1)
        n0 = np.where(tie, np.where(n1 < 0xFFFF, n1 + 1, n0), n0)
        n1 = np.where(tie & (n0 == n1), n1 - 1, n1)
    colour[:, 0], colour[:, 1] = n0.astype('<u2'), n1.astype('<u2')
    colour[:, 2], colour[:, 3] = (idx & 0xFFFF).astype('<u2'), (idx >> 16).astype('<u2')
    if stride == 8:
        w.reshape(-1, 8)[:, -4:] = colour
    out = w.astype('>u2' if be else '<u2').tobytes()
    model[start:start + len(out)] = out
    return True


# ---------------------------------------------------------------------------
# Source loading
# ---------------------------------------------------------------------------

def load_source(spec):
    """(label, [(index, NOBJ bytes)]) for a SOURCE[:ENTRY] spec."""
    entry = None
    if ':' in spec[2:]:
        spec, entry = spec.rsplit(':', 1)
        entry = int(entry)
    data = open(spec, 'rb').read()
    if data[:4] == b'NOBJ':
        return spec, [(0, data[:be32(data, 4)])]
    bmd = load_bmd(spec)
    models = []
    for i, e in enumerate(bmd_entries(bmd)):
        if entry is not None and i != entry:
            continue
        if e[:4] == b'NOBJ':
            models.append((i, e[:be32(e, 4)]))
    if entry is not None and not models:
        sys.exit('error: entry %d is not a NOBJ' % entry)
    return spec, models


def is_character_model(model):
    kinds = {c[1] for c in model_chunks(model)}
    return b'NMDL' in kinds and b'NSHP' in kinds and b'NBN2' in kinds


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_list(args):
    _, models = load_source(args.source)
    for i, m in models:
        if not args.all and not is_character_model(m):
            continue
        cs = model_chunks(m)
        shapes = [chunk_name(m, c[1], c[2]) for c in cs if c[1] == b'NSHP']
        texs = [Texture(m, c[2], c[3]) for c in cs if c[1] == b'NTX2' or c[1] == b'NTEX']
        label = CHARACTERS[i] if i < len(CHARACTERS) else ''
        print('entry %-3d %-10s %9d bytes  %2d meshes  %2d textures' %
              (i, label, len(m), len(shapes), len(texs)))
        if args.verbose:
            print('  meshes  : ' + ', '.join(shapes))
            for t in texs:
                print('  texture : %-24s %4dx%-4d %s' % (t.name or '-', t.width, t.height, t.format))


def chunk_filename(n, path, name):
    stem = '%03d_%s' % (n, path.replace('/', '_'))
    if name:
        stem += '_' + ''.join(c if c.isalnum() or c in '-.' else '_' for c in name)
    return stem + '.bin'


def cmd_dump(args):
    _, models = load_source(args.source)
    if len(models) != 1:
        sys.exit('error: name one entry, SOURCE:ENTRY')
    _, m = models[0]
    out = args.outdir
    os.makedirs(os.path.join(out, 'chunks'), exist_ok=True)
    open(os.path.join(out, 'model.nobj'), 'wb').write(m)
    lines = ['# path, file, offset, size. Order is file order; pack follows it.']
    for n, (path, tag, off, size) in enumerate(model_chunks(m)):
        name = chunk_name(m, tag, off)
        fname = chunk_filename(n, path, name)
        if path != 'NMDL':
            # The NMDL is its 64 byte header and children; the children have files.
            open(os.path.join(out, 'chunks', fname), 'wb').write(m[off:off + size])
        else:
            open(os.path.join(out, 'chunks', fname), 'wb').write(
                m[off:nmdl_body(m, off)])
        lines.append('%s\t%s\t%d\t%d' % (path, fname, off, size))
        if args.png and tag in (b'NTX2', b'NTEX'):
            t = Texture(m, off, size)
            if t.blocks:
                from PIL import Image
                import io
                try:
                    Image.open(io.BytesIO(texture_dds(m, t))).convert('RGBA').save(
                        os.path.join(out, 'chunks', fname[:-4] + '.png'))
                except Exception as e:  # noqa: BLE001
                    print('png failed for %s: %s' % (fname, e))
    open(os.path.join(out, 'manifest.txt'), 'w').write('\n'.join(lines) + '\n')
    print('dumped %d chunks of a %d byte model to %s' % (len(lines) - 1, len(m), out))


def read_manifest(directory):
    rows = []
    for line in open(os.path.join(directory, 'manifest.txt')):
        if line.startswith('#') or not line.strip():
            continue
        path, fname, off, size = line.rstrip('\n').split('\t')
        rows.append((path, fname, int(off), int(size)))
    return rows


def pack(directory):
    """The NOBJ the directory describes."""
    base = open(os.path.join(directory, 'model.nobj'), 'rb').read()
    rows = read_manifest(directory)
    top = [r for r in rows if '/' not in r[0]]
    kids = [r for r in rows if r[0].startswith('NMDL/')]

    def part(r):
        return open(os.path.join(directory, 'chunks', r[1]), 'rb').read()

    body = bytearray()
    shift_check = 0
    nmdl_head = None
    for r in top:
        if r[0] == 'NMDL':
            head = part(r)
            kid_bytes = bytearray()
            for k in kids:
                d = part(k)
                if len(d) != k[3]:
                    if (len(d) - k[3]) % 4096:
                        sys.exit('error: %s changed size by %d, not a multiple of 4096' %
                                 (k[1], len(d) - k[3]))
                    shift_check += len(d) - k[3]
                if d[4:8] != struct.pack('>I', len(d)):
                    d = d[:4] + struct.pack('>I', len(d)) + d[8:]
                kid_bytes += d
            nmdl = bytearray(head) + kid_bytes
            nmdl[4:8] = struct.pack('>I', len(nmdl))
            body += nmdl
        else:
            d = part(r)
            if len(d) != r[3] and (len(d) - r[3]) % 4096:
                sys.exit('error: %s changed size by %d, not a multiple of 4096' %
                         (r[1], len(d) - r[3]))
            body += d
    out = bytearray(base[:8]) + body
    out[4:8] = struct.pack('>I', len(out))
    return bytes(out)


def cmd_pack(args):
    data = pack(args.dir)
    open(args.out, 'wb').write(data)
    print('wrote %s, %d bytes' % (args.out, len(data)))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('list', help='list the models of a source')
    s.add_argument('source', nargs='?', default=DEFAULT_SOURCE)
    s.add_argument('-v', '--verbose', action='store_true', help='meshes and textures too')
    s.add_argument('--all', action='store_true', help='every NOBJ, not only character models')
    s.set_defaults(fn=cmd_list)
    s = sub.add_parser('dump', help='split one model into chunk files')
    s.add_argument('source', help='SOURCE or SOURCE:ENTRY')
    s.add_argument('outdir')
    s.add_argument('--png', action='store_true', help='also write each texture as a PNG')
    s.set_defaults(fn=cmd_dump)
    s = sub.add_parser('pack', help='rebuild a NOBJ from a dump directory')
    s.add_argument('dir')
    s.add_argument('out')
    s.set_defaults(fn=cmd_pack)
    args = p.parse_args()
    args.fn(args)


if __name__ == '__main__':
    main()
