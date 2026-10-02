#!/usr/bin/env python3
"""Build an Xbox 360 game directory that runs the PS3 release's data.

Input is a tree extracted with unpack_ps3.py, which is already decoded (the
PS3 ships no codec layer), plus the 360 assets directory. The output mirrors
the 360 one (hard linked where possible), with every PS3 file the 360
executable can load converted and written over it, and an index.vmtoc whose
records describe the converted files as stored. Point game_data_root at it.
The formats and the evidence for every rule are in docs/ps3-assets.md.

Converted:
  NMDL  version byte 0x83 -> 0x82
  NTX3  GTF header -> NTEX (DDS); DXT payloads are byte identical
  NSHP  RSX vertex packing -> Xenos (normals, colours, skin weights)
  NMTR, NLIT, NFOG, NCLC, NOL2  RGBA colours -> ARGB
  NLOB  offsets of the placed objects it holds
  .e    list B bulk offsets, header size, reloc offset, task priorities,
        state global ids
  .bmd / CAMP  entry table; .bop  entry directory
  .p3tex  -> .x3tex (a bare texture chain)
  .csf / .cps  audio, see ps3_audio.py; needs ffmpeg on PATH

usage: ps3_convert.py PS3_ROOT OUT --base ASSETS_360 [--verify DECODED_360]
"""
import argparse
import bisect
import collections
import math
import os
import shutil
import struct
import subprocess
import tempfile

import ps3_audio

u32 = struct.Struct('>I')
u16 = struct.Struct('>H')


def rd32(d, o):
    return u32.unpack_from(d, o)[0]


def rd16(d, o):
    return u16.unpack_from(d, o)[0]


def chunk(tag, payload):
    return tag + u32.pack(8 + len(payload)) + payload


def npad(size):
    return chunk(b'NPAD', bytes(size - 8))


class Report:
    def __init__(self):
        self.counts = collections.Counter()
        self.warnings = []

    def warn(self, msg):
        self.warnings.append(msg)


# ---------------------------------------------------------------------------
# Colours: RSX vertex and material colours are RGBA, Xenos D3DCOLOR is ARGB.
# ---------------------------------------------------------------------------
def argb(word):
    return word[3:] + word[:3]


def rotate_words(buf, offsets):
    for o in offsets:
        if o + 4 <= len(buf):
            buf[o:o + 4] = argb(bytes(buf[o:o + 4]))


# ---------------------------------------------------------------------------
# NTX3 (a one-texture GTF) -> NTEX (a PC DDS)
# ---------------------------------------------------------------------------
GTF_DXT = {0x86: b'DXT1', 0x87: b'DXT3', 0x88: b'DXT5'}
GTF_A8R8G8B8 = 0x85
GTF_LINEAR = 0x20  # CELL_GCM_TEXTURE_LN
GTF_NORMALIZED = 0x40  # CELL_GCM_TEXTURE_UN


def dds_header(width, height, mips, fourcc, top_size):
    flags = 0x81007 | (0x20000 if mips > 1 else 0)
    caps = 0x401008 if mips > 1 else 0x1000
    if fourcc:
        pf = struct.pack('<II4s5I', 32, 4, fourcc, 0, 0, 0, 0, 0)
    else:
        pf = struct.pack('<II4s5I', 32, 0x41, bytes(4), 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000)
    head = struct.pack('<4s7I', b'DDS ', 124, flags, height, width, top_size, 0,
                       mips if mips > 1 else 0)
    return head + bytes(44) + pf + struct.pack('<4I', caps, 0, 0, 0) + bytes(4)


def unpitch(pixels, width, height, mips, pitch, block, block_bytes):
    """Linear GTF levels keep the base level's row pitch all the way down the
    chain; DDS packs every level tight."""
    out = bytearray()
    at = 0
    for level in range(max(1, mips)):
        w, h = max(1, width >> level), max(1, height >> level)
        cols = (w + block - 1) // block
        rows = (h + block - 1) // block
        row = cols * block_bytes
        for r in range(rows):
            out += pixels[at + r * pitch:at + r * pitch + row]
        at += rows * pitch
    return bytes(out)


def convert_ntx3(d, o, report):
    size, count, _, off, tsz = struct.unpack_from('>5I', d, o + 4)
    fmt, mips, dim, cube = d[o + 24:o + 28]
    width, height, depth = struct.unpack_from('>3H', d, o + 32)
    pitch = rd32(d, o + 40)
    base = fmt & ~(GTF_LINEAR | GTF_NORMALIZED)
    # The texture offset counts from the chunk tag, not from the GTF header.
    pixels = d[o + off:o + off + tsz]
    if count != 1 or dim != 2 or cube or depth != 1:
        report.warn(f'NTX3 at {o:#x}: unsupported layout count={count} dim={dim} cube={cube}')
        return None
    linear = fmt & GTF_LINEAR and pitch
    if base in GTF_DXT:
        block = 8 if base == 0x86 else 16
        if linear:
            pixels = unpitch(pixels, width, height, mips, pitch, 4, block)
        top = max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * block
        out = dds_header(width, height, mips, GTF_DXT[base], top) + pixels
    elif base == GTF_A8R8G8B8:
        if linear:
            pixels = unpitch(pixels, width, height, mips, pitch, 1, 4)
        # ARGB big endian to the DDS's little endian BGRA.
        swapped = bytearray(len(pixels))
        swapped[0::4] = pixels[3::4]
        swapped[1::4] = pixels[2::4]
        swapped[2::4] = pixels[1::4]
        swapped[3::4] = pixels[0::4]
        out = dds_header(width, height, mips, None, width * height * 4) + bytes(swapped)
    else:
        report.warn(f'NTX3 at {o:#x}: unsupported GTF format {fmt:#x}')
        return None
    report.counts['NTX3->NTEX'] += 1
    return chunk(b'NTEX', out)


# ---------------------------------------------------------------------------
# NSHP vertices. The element order and Xenos types are the ones sub_82131148
# builds from the format word at +0x1C; RSX stores the same elements in the
# same order, only packed differently.
# ---------------------------------------------------------------------------
def vertex_layout(fmt):
    els = []

    def add(name, size, ps3_size=None):
        els.append((name, size, size if ps3_size is None else ps3_size))
    if fmt & 0x1:
        add('copy', 12)                  # position FLOAT3
    if fmt & 0x400:
        add('weights', 12, 4)            # FLOAT3 here, one CMP dword on RSX
        add('copy', 4)                   # blend indices UBYTE4
    if fmt & 0x2:
        add('normal', 4)                 # DEC3N here, CMP on RSX
    if fmt & 0x4:
        add('normal', 4)                 # tangent, same packing
    if fmt & 0x8:
        add('colour', 4)
    for _ in range((fmt & 0xFF) >> 4):
        add('copy', 4)                   # FLOAT16_2 texcoords
    for _ in range((fmt >> 16) & 3):
        add('copy', 8)                   # FLOAT2 texcoords
    if fmt & 0x4000:
        add('copy', 4)
    if fmt & 0x8000:
        add('copy', 24)
    if fmt & 0x41000:
        add('copy', 12)
    if fmt & 0x40000:
        return None                      # morph layout, never seen shipped
    return els


def sext(value, bits):
    return value - (1 << bits) if value >> (bits - 1) & 1 else value


def cmp_unpack(word):
    """RSX CELL_GCM_VERTEX_CMP: signed 11:11:10, x in the low bits."""
    return (sext(word & 0x7FF, 11) / 1023.0,
            sext((word >> 11) & 0x7FF, 11) / 1023.0,
            sext(word >> 22, 10) / 511.0)


def dec3n(x, y, z):
    def q(v):
        return max(-511, min(511, int(math.floor(v * 511.0 + 0.5)))) & 0x3FF
    return q(x) | q(y) << 10 | q(z) << 20


def f32(v):
    return struct.unpack('>f', struct.pack('>f', v))[0]


def skin_weights(word):
    # The 360 ships the authored weights (0.9, 0.1) with the last nonzero one
    # computed as 1 minus the rest in float32; the RSX copy is quantised to
    # 1/1023, so snap back to the nearest thousandth and redo that sum.
    ws = []
    for w in cmp_unpack(word):
        r = round(w, 3)
        ws.append(f32(r if abs(r - w) < 0.5 / 1023 else w))
    last = max((i for i, w in enumerate(ws) if w), default=0)
    if last:
        rest = f32(1.0)
        for w in ws[:last]:
            rest = f32(rest - w)
        if abs(rest - ws[last]) < 1.0 / 1023:
            ws[last] = rest
    return ws


def convert_nshp(d, o, report):
    size = rd32(d, o + 4)
    flags, nv, fmt = struct.unpack_from('>HHH', d, o + 0x18)
    bones = d[o + 0x20]
    els = vertex_layout(fmt)
    if els is None:
        report.warn(f'NSHP at {o:#x}: unsupported vertex format {fmt:#x}')
        return None
    v0 = 0x38 + (32 if flags & 0x80 else 0)
    if bones:
        v0 += (bones * 2 + 3) & ~3
    ps3_stride = sum(e[2] for e in els)
    x_stride = sum(e[1] for e in els)
    if v0 + nv * ps3_stride > size:
        report.warn(f'NSHP at {o:#x}: {nv} vertices of {ps3_stride} bytes overrun the chunk')
        return None
    out = bytearray(d[o:o + v0])
    src = o + v0
    for _ in range(nv):
        for kind, _, ps in els:
            v = d[src:src + ps]
            if kind == 'copy':
                out += v
            elif kind == 'colour':
                out += argb(v)
            elif kind == 'normal':
                out += u32.pack(dec3n(*cmp_unpack(rd32(v, 0))))
            elif kind == 'weights':
                out += struct.pack('>3f', *skin_weights(rd32(v, 0)))
            src += ps
    out += d[src:o + size]
    struct.pack_into('>I', out, 4, len(out))
    report.counts['NSHP'] += 1
    if x_stride != ps3_stride:
        report.counts['NSHP resized'] += 1
    return bytes(out)


# ---------------------------------------------------------------------------
# Small chunks whose only difference is colour byte order.
# ---------------------------------------------------------------------------
def convert_colours(d, o, tag, report):
    size = rd32(d, o + 4)
    buf = bytearray(d[o:o + size])
    if tag == b'NMTR':
        # 96 byte materials; +8 is a flags word. With its bit 0 set, +4 is the
        # texture index rather than a colour.
        rotate_words(buf, (m + k for m in range(8, size - 95, 96) for k in (4, 36, 40, 44)
                           if k != 4 or not buf[m + 11] & 1))
    elif tag == b'NLIT':
        rotate_words(buf, (r + k for r in range(8, size - 47, 48) for k in (20, 28)))
    elif tag == b'NFOG':
        rotate_words(buf, (12,))
    elif tag == b'NCLC':
        rotate_words(buf, (20, 36))
    elif tag == b'NOL2':
        # Type 0 records keep their colour at +8, the typed ones at +24.
        rotate_words(buf, (r + (24 if buf[r] & 3 else 8) for r in range(8, size - 31, 32)))
    report.counts[tag.decode()] += 1
    return bytes(buf)


COLOUR_CHUNKS = {b'NMTR', b'NLIT', b'NFOG', b'NCLC', b'NOL2'}
PASSTHROUGH = {b'NPAD', b'NCAM', b'NLC2', b'NBN2', b'NMTN', b'NMTB', b'NCLS', b'NTXA', b'NDYN',
               b'NMRP', b'NSIG', b'NRTE', b'NAIR', b'NATR'}


# ---------------------------------------------------------------------------
# Chunk trees
# ---------------------------------------------------------------------------
class Rebuild:
    """Accumulates converted bytes and an old -> new offset map."""

    def __init__(self):
        self.out = bytearray()
        self.anchors = []  # (old, new), old ascending

    def mark(self, old):
        self.anchors.append((old, len(self.out)))


def children(d, start, end):
    o = start
    while o + 8 <= end:
        size = rd32(d, o + 4)
        if size < 8 or o + size > end:
            return
        yield o, d[o:o + 4], size
        o += size


def chain_ok(d, start, end):
    o = start
    while o + 8 <= end:
        size = rd32(d, o + 4)
        if size < 8 or size & 3 or o + size > end or d[o:o + 1] != b'N':
            return False
        o += size
    return o == end


def nmdl_header(d, o):
    return 96 if rd16(d, o + 32) & 2 else 64


def is_nobj(d, o):
    if o + 16 > len(d) or d[o:o + 4] != b'NOBJ':
        return False
    size = rd32(d, o + 4)
    return 16 <= size <= len(d) - o and size % 4 == 0 and chain_ok(d, o + 8, o + size)


def is_nmdl(d, o, end):
    if o + 64 > end or d[o:o + 4] != b'NMDL' or d[o + 8] != 0x83:
        return False
    size = rd32(d, o + 4)
    hdr = nmdl_header(d, o)
    return hdr < size <= end - o and size % 4 == 0 and chain_ok(d, o + hdr, o + size)


def is_ntx3(d, o, end):
    return (d[o:o + 4] == b'NTX3' and o + 0x30 <= end and rd32(d, o + 8) == 1
            and rd32(d, o + 16) == 0x80 and o + rd32(d, o + 4) <= end)


def textures_in_place(d, o, size, report):
    """Converts every NTX3 nested in an opaque chunk without moving anything.

    Mefc effects (inside NLEF) address their textures through their own
    directory, so each NTEX has to land exactly where its NTX3 was."""
    buf = None
    at, end = o + 8, o + size
    while at < end:
        at = d.find(b'NTX3', at, end)
        if at < 0:
            break
        if at % 4 == 0 and is_ntx3(d, at, end):
            csize = rd32(d, at + 4)
            converted = convert_ntx3(d, at, report)
            if converted is not None and csize - len(converted) >= 8:
                if buf is None:
                    buf = bytearray(d[o:end])
                rel = at - o
                buf[rel:rel + csize] = converted + npad(csize - len(converted))
                at += csize
                continue
            if converted is not None:
                report.warn(f'NTX3 at {at:#x}: no room to convert in place')
        at += 4
    return None if buf is None else bytes(buf)


def convert_leaf(d, o, tag, size, report):
    if tag == b'NTX3':
        return convert_ntx3(d, o, report)
    if tag == b'NSHP':
        return convert_nshp(d, o, report)
    if tag in COLOUR_CHUNKS:
        return convert_colours(d, o, tag, report)
    if tag not in PASSTHROUGH:
        report.counts['unconverted ' + tag.decode('latin1')] += 1
    return None


def emit_leaf(rb, d, o, tag, size, report):
    rb.mark(o)
    converted = convert_leaf(d, o, tag, size, report)
    if converted is None:
        rb.out += textures_in_place(d, o, size, report) or d[o:o + size]
        return
    rb.out += converted
    # An NTX3 is larger than its NTEX: keep the remainder as padding so
    # nothing after it moves. The texture list loop skips NPAD.
    if tag == b'NTX3' and len(converted) < size:
        gap = size - len(converted)
        if gap >= 8:
            rb.out += npad(gap)
        else:
            report.warn(f'NTX3 at {o:#x}: {gap} byte gap cannot hold an NPAD')


def emit_nlob(rb, d, o, size, report):
    """A map's placed objects: u32 count, u32 offsets from +8, then NOBJs."""
    rb.mark(o)
    start = len(rb.out)
    count = rd32(d, o + 8)
    offsets = struct.unpack_from('>%dI' % count, d, o + 12)
    body = o + 12 + 4 * count
    rb.out += d[o:body]
    new_pos = {}
    for co, tag, cs in children(d, body, o + size):
        new_pos[co - (o + 8)] = len(rb.out) - (start + 8)
        if tag == b'NOBJ':
            emit_nobj(rb, d, co, cs, report)
        else:
            emit_leaf(rb, d, co, tag, cs, report)
    for i, off in enumerate(offsets):
        if off not in new_pos:
            report.warn(f'NLOB at {o:#x}: entry {i} does not start a chunk')
            continue
        struct.pack_into('>I', rb.out, start + 12 + 4 * i, new_pos[off])
    struct.pack_into('>I', rb.out, start + 4, len(rb.out) - start)
    report.counts['NLOB'] += 1


def emit_nmdl(rb, d, o, size, report):
    rb.mark(o)
    start = len(rb.out)
    hdr = nmdl_header(d, o)
    rb.out += d[o:o + hdr]
    if rb.out[start + 8] == 0x83:
        rb.out[start + 8] = 0x82
    report.counts['NMDL'] += 1
    for co, tag, cs in children(d, o + hdr, o + size):
        if tag == b'NLOB':
            emit_nlob(rb, d, co, cs, report)
        else:
            emit_leaf(rb, d, co, tag, cs, report)
    # Keep everything after this model on its PS3 alignment: the RSX layout
    # aligns texture payloads to 128 bytes and some consumers may rely on it.
    grown = (len(rb.out) - start) - size
    pad = -grown % 128
    if pad:
        rb.out += npad(pad if pad >= 8 else pad + 128)
    struct.pack_into('>I', rb.out, start + 4, len(rb.out) - start)


def emit_nobj(rb, d, o, size, report):
    rb.mark(o)
    start = len(rb.out)
    rb.out += d[o:o + 8]
    report.counts['NOBJ'] += 1
    for co, tag, cs in children(d, o + 8, o + size):
        if tag == b'NMDL':
            emit_nmdl(rb, d, co, cs, report)
        elif tag == b'NOBJ':
            emit_nobj(rb, d, co, cs, report)
        else:
            emit_leaf(rb, d, co, tag, cs, report)
    struct.pack_into('>I', rb.out, start + 4, len(rb.out) - start)


def is_csf(d, o, end):
    if d[o:o + 4] != b'CSF ' or o + 16 > end:
        return False
    total, head, payload = struct.unpack_from('>III', d, o + 4)
    return head and total == head + payload and o + total <= end


def csl_banks(d, o):
    return [o + rd32(d, o + 8 + 4 * i) for i in range(rd32(d, o + 4))]


def is_csl(d, o, end):
    if d[o:o + 4] != b'CSL ' or o + 8 > end:
        return False
    n = rd32(d, o + 4)
    return 0 < n <= 64 and all(is_csf(d, o + rd32(d, o + 8 + 4 * i), end) for i in range(n))


def mefc_entries(d, o, end):
    """Directory entry positions of an effect, or None. Header u16 +0x0C
    points at "CK", entry size, count; each entry is a tag, a type and a u32
    offset from the effect start to its section."""
    if d[o:o + 4] != b'Mefc' or d[o + 8:o + 10] != b'XB' or o + 0x30 > end:
        return None
    size = rd32(d, o + 4)
    at = o + rd16(d, o + 12)
    if o + size > end or d[at:at + 2] != b'CK' or d[at + 2] < 12:
        return None
    entries = [at + 4 + d[at + 2] * i for i in range(d[at + 3])]
    first = at + 4 + d[at + 2] * d[at + 3] - o
    if not entries or any(not first <= rd32(d, e + 8) < size for e in entries):
        return None
    return entries


def convert_region(d, start, end, report, audio=None):
    """Converts every NOBJ, loose NMDL, loose NTX3 and effect in d[start:end],
    and with audio every sound bank, copying the rest.

    Returns the new bytes and a function mapping an old absolute offset in the
    region to its new offset relative to the region start."""
    rb = Rebuild()
    banks = 0
    tables = []
    # Bank offset -> (directory offset, position, the directory's banks).
    members = {}

    def flush(a, b):
        rb.mark(a)
        rb.out += d[a:b]

    def pad(n):
        rb.out += bytes(-(start + len(rb.out)) % n)

    def emit_mefc(o, size, entries):
        # Sections are addressed through the directory, so they may move as
        # long as it is rewritten.
        nonlocal banks
        cuts = sorted({rd32(d, e + 8) for e in entries}) + [size]
        flush(o, o + cuts[0])
        begin = len(rb.out) - cuts[0]
        new = {}
        for a, b in zip(cuts, cuts[1:]):
            pad(128)
            first = len(rb.anchors)
            scan(o + a, o + b)
            new[a] = max(n for old, n in rb.anchors[first:] if old == o + a) - begin
        for e in entries:
            struct.pack_into('>I', rb.out, begin + e - o + 8, new[rd32(d, e + 8)])
        struct.pack_into('>I', rb.out, begin + 4, len(rb.out) - begin)
        report.counts['Mefc'] += 1

    def scan(o, end):
        nonlocal banks
        copied = o
        while o + 8 <= end:
            tag = d[o:o + 4]
            if audio and is_csl(d, o, end):
                # A bank directory: offsets from itself to the banks after it.
                flush(copied, o)
                pad(0x1000)
                rb.mark(o)
                tables.append(o)
                offs = csl_banks(d, o)
                group = [d[b:b + rd32(d, b + 4)] for b in offs]
                for i, b in enumerate(offs):
                    members[b] = (o, i, group)
                copied = o
                o += 8 + 4 * rd32(d, o + 4)
                continue
            if audio and is_csf(d, o, end):
                size = rd32(d, o + 4)
                flush(copied, o)
                # Banks start on a 0x1000 boundary of the file, as on the 360,
                # since their clip payloads are aligned from the bank start.
                pad(0x1000)
                rb.mark(o)
                rb.out += audio(banks, d[o:o + size], members.get(o))
                banks += 1
                o = copied = o + size
                continue
            if tag == b'NOBJ' and is_nobj(d, o) and o + rd32(d, o + 4) <= end:
                size = rd32(d, o + 4)
                flush(copied, o)
                emit_nobj(rb, d, o, size, report)
                o = copied = o + size
                continue
            # Events also hand bare models to native 1062.
            if tag == b'NMDL' and is_nmdl(d, o, end):
                size = rd32(d, o + 4)
                flush(copied, o)
                emit_nmdl(rb, d, o, size, report)
                o = copied = o + size
                continue
            if tag == b'NTX3' and is_ntx3(d, o, end):
                size = rd32(d, o + 4)
                flush(copied, o)
                emit_leaf(rb, d, o, tag, size, report)
                o = copied = o + size
                continue
            entries = mefc_entries(d, o, end) if tag == b'Mefc' else None
            if entries:
                size = rd32(d, o + 4)
                flush(copied, o)
                emit_mefc(o, size, entries)
                o = copied = o + size
                continue
            o += 4
        flush(copied, end)

    scan(start, end)
    rb.mark(end)

    olds = [a for a, _ in rb.anchors]

    def remap(old):
        i = bisect.bisect_right(olds, old) - 1
        a, n = rb.anchors[i]
        return n + (old - a)
    for t in tables:
        at = remap(t)
        for i in range(rd32(d, t + 4)):
            struct.pack_into('>I', rb.out, at + 8 + 4 * i, remap(t + rd32(d, t + 8 + 4 * i)) - at)
    return bytes(rb.out), remap


# ---------------------------------------------------------------------------
# Containers
# ---------------------------------------------------------------------------
def convert_e(d, report, audio=None):
    hdr = struct.unpack_from('>6I', d, 0)
    image_end = 0x18 + hdr[4]
    reloc_base = image_end + hdr[5]
    bulk, remap = convert_region(d, image_end, reloc_base, report, audio)
    image = bytearray(d[:image_end])
    # List B: image dwords holding bulk relative offsets.
    na = rd32(d, reloc_base)
    nb_at = reloc_base + 4 + 4 * na
    for i in range(rd32(d, nb_at)):
        entry = rd32(d, nb_at + 4 + 4 * i)
        struct.pack_into('>I', image, entry, remap(image_end + rd32(image, entry)))
    delta = len(bulk) - (reloc_base - image_end)
    struct.pack_into('>I', image, 0x0C, hdr[3] + delta)
    struct.pack_into('>I', image, 0x14, hdr[5] + delta)
    return bytes(image) + bulk + d[reloc_base:]


# The 500 series are pointers into the map state block at dword_8243C230;
# the PS3's block has an extra field before the skip handler, so 541.. is 540..
STATE_SYMBOL_SHIFT = range(541, 549)


def remap_state_symbols(d, report):
    """Renumbers block2 imports of the shifted state globals, in place."""
    out = bytearray(d)
    o = 0x18 + rd32(d, 0x10) + rd32(d, 0x14)
    for _ in range(2):  # list A, list B
        o += 4 + 4 * rd32(d, o)
    for _ in range(4):
        n = rd32(d, o)
        for i in range(n):
            at = o + 4 + 8 * i
            if rd32(out, at) in STATE_SYMBOL_SHIFT:
                struct.pack_into('>I', out, at, rd32(out, at) - 1)
                report.counts['state symbol remapped'] += 1
        o += 4 + 8 * n
    return bytes(out)


def convert_bmd(d, report, audio=None):
    count = rd32(d, 8)
    table_end = 12 + 4 * count
    body, remap = convert_region(d, table_end, len(d), report, audio)
    head = bytearray(d[:table_end])
    for i in range(count):
        v = rd32(head, 12 + 4 * i)
        if table_end <= v < len(d):
            struct.pack_into('>I', head, 12 + 4 * i, table_end + remap(v))
    struct.pack_into('>I', head, 4, len(head) + len(body))
    return bytes(head) + body


def convert_bop(d, report, audio=None):
    dir_at = rd32(d, 12)
    count = rd32(d, dir_at)
    head_end = dir_at + 4 + 4 * count
    body, remap = convert_region(d, head_end, len(d), report, audio)
    head = bytearray(d[:head_end])
    for i in range(count):
        at = dir_at + 4 + 4 * i
        v = rd32(head, at)
        if head_end <= v < len(d):
            struct.pack_into('>I', head, at, head_end + remap(v))
    struct.pack_into('>I', head, 4, len(head) + len(body))
    return bytes(head) + body


# BattleKeep.bop is addressed by slot, like AppKeep. The PS3 dropped the eight
# effects at 360 slots 26..33, moving every later slot down by eight, and its
# voice table (360 slot 38) has rows for twelve characters where the executable
# indexes ten. The 360 table is kept: every 360 clip keeps its ordinal in the
# converted banks. A wrong chunk in slot 38 gives the pre-battle line no voice,
# and the intro waits for it forever.
BATTLEKEEP = 'btldata/battlekeep.bop'
BATTLEKEEP_DROPPED = range(26, 34)
BATTLEKEEP_VOICE_TABLE = 38


def bop_entries(d):
    dir_at = rd32(d, 12)
    offsets = [rd32(d, dir_at + 4 + 4 * i) for i in range(rd32(d, dir_at))]
    ends = sorted(set(o for o in offsets if o) | {len(d)})
    return dir_at, [d[o:ends[bisect.bisect_right(ends, o)]] if o else None for o in offsets]


def rebuild_battlekeep(ps3, x360, report):
    """Converted PS3 BattleKeep -> the 360's slot order."""
    dir_at, have = bop_entries(ps3)
    _, want = bop_entries(x360)
    if len(have) + len(BATTLEKEEP_DROPPED) != len(want):
        report.warn(f'{BATTLEKEEP}: {len(have)} entries, expected {len(want) - 8}; kept as is')
        return ps3
    shift = len(BATTLEKEEP_DROPPED)
    slots = []
    for i, x in enumerate(want):
        if i in BATTLEKEEP_DROPPED or i == BATTLEKEEP_VOICE_TABLE:
            slots.append(x)
            continue
        p = have[i if i < BATTLEKEEP_DROPPED.start else i - shift]
        if (p or b'')[:4] != (x or b'')[:4]:
            report.warn(f'{BATTLEKEEP}: slot {i} holds {p[:4]!r}, the 360 {x[:4]!r}')
        slots.append(p)
    out = bytearray(ps3[:dir_at]) + u32.pack(len(slots)) + bytes(4 * len(slots))
    for i, s in enumerate(slots):
        if s is None:
            continue
        out += bytes(-len(out) % 0x80)
        u32.pack_into(out, dir_at + 4 + 4 * i, len(out))
        out += s
    out += bytes(-len(out) % 0x1000)
    u32.pack_into(out, 4, len(out))
    report.counts['BattleKeep slots restored from the 360'] += shift + 1
    return bytes(out)


# AppKeep.bmd is kept from the 360 (§4), but its controller button icons
# should be the PS3's. 360 slot -> PS3 slot: A B X Y LB LT RB RT START BACK
# become cross circle square triangle L1 L2 R1 R2 START SELECT, then the pad
# and the A/B pair.
APPKEEP = 'appkeep.bmd'
APPKEEP_PS3_TEXTURES = {**{i: i + 5 for i in range(235, 245)}, 290: 299, 295: 304}


def bmd_entry(d, i):
    offsets = sorted(rd32(d, 12 + 4 * j) for j in range(rd32(d, 8))) + [len(d)]
    o = rd32(d, 12 + 4 * i)
    return o, offsets[bisect.bisect_right(offsets, o)] - o


def ps3_appkeep_textures(ps3, x360, report):
    """The 360 AppKeep with the PS3's button textures written over its own."""
    out = bytearray(x360)
    for x_slot, p_slot in APPKEEP_PS3_TEXTURES.items():
        xo, room = bmd_entry(x360, x_slot)
        po, _ = bmd_entry(ps3, p_slot)
        tex = convert_ntx3(ps3, po, report) if ps3[po:po + 4] == b'NTX3' else None
        if x360[xo:xo + 4] != b'NTEX' or tex is None or len(tex) != rd32(x360, xo + 4) or len(tex) > room:
            report.warn(f'AppKeep: PS3 slot {p_slot} does not fit 360 slot {x_slot}; kept')
            continue
        out[xo:xo + len(tex)] = tex
        report.counts['AppKeep textures taken from the PS3'] += 1
    return bytes(out)


def decoded_360_file(base, scratch, rel):
    """One 360 asset decoded with unpack_e.exe; TOC names use backslashes."""
    exe = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'unpack_e.exe')
    subprocess.run([exe, base, scratch, rel.replace('/', '\\')], check=True, capture_output=True)
    with open(os.path.join(scratch, *rel.split('/')), 'rb') as fh:
        return fh.read()


def convert_bare(d, report):
    body, _ = convert_region(d, 0, len(d), report)
    return body


# Slot addressed containers whose PS3 layout differs from the 360's, and
# formats nothing converts yet. The 360 file is kept for these.
KEEP_360 = {'appkeep.bmd', 'op.bmd', 'ed1.bmd', 'ed2.bmd', 'campdata/scp.bmd'}
KEEP_360_EXT = ('.fnt', '.tex')


# Set by main when building a game directory: (rel, bank index, PS3 bank)
# -> 360 bank, for the sound banks embedded in .e files.
embedded_audio = None


def convert_file(rel, d, report):
    """Returns (360 relative path, bytes), or None to keep the 360 file."""
    low = rel.lower()
    if low in KEEP_360 or low.endswith(KEEP_360_EXT):
        return None
    # Battle effects and title.bmd carry banks too: one ATRAC3 clip left in
    # them stalls the XMA decoder and silences all audio after it.
    audio = (lambda i, bank, member: embedded_audio(rel, i, bank, member)) if embedded_audio else None
    if low.endswith('.e') and d[:4] in (b'\0\0\x01\x81', b'\0\0\x01\x80'):
        e = remap_state_symbols(convert_e(d, report, audio), report)
        return rel, ps3_audio.rename_music(e)
    if d[:4] in (b'BMD ', b'CAMP'):
        return rel, convert_bmd(d, report, audio)
    if d[:4] == b'BOP ':
        return rel, convert_bop(d, report, audio)
    if low.endswith('.p3tex'):
        return rel[:-6] + '.x3tex', convert_bare(d, report)
    if low.endswith('.p3obj'):
        return rel, convert_bare(d, report)
    return None


# ---------------------------------------------------------------------------
# The game directory
# ---------------------------------------------------------------------------
class Toc:
    """index.vmtoc: 48 byte records sorted by name, binary searched by
    sub_8210D080."""

    def __init__(self, data):
        self.records = {}
        for i in range(len(data) // 48):
            rec = bytearray(data[i * 48:(i + 1) * 48])
            name = rec[:32].split(b'\0')[0].decode('latin1')
            self.records[name.lower().replace('\\', '/')] = rec

    def set_stored(self, path, size):
        key = path.lower()
        rec = self.records.get(key)
        if rec is None:
            name = key.replace('/', '\\').encode('latin1')
            if len(name) >= 32:
                return False
            rec = bytearray(48)
            rec[:len(name)] = name
            self.records[key] = rec
        struct.pack_into('>I', rec, 32, size)
        rec[36] = 0
        return True

    def bytes(self):
        return b''.join(bytes(r) for r in sorted(self.records.values(), key=lambda r: bytes(r[:32])))


def replace_file(path, data):
    # The directory starts as hard links to the 360 tree; unlink first so a
    # write can never reach the original.
    if os.path.lexists(path):
        os.remove(path)
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    with open(path, 'wb') as fh:
        fh.write(data)


def link_base(base, out, keep=False):
    """Mirrors the 360 tree into out, as hard links where the volume allows;
    with keep, files already there stay. Returns lowercase relative path ->
    the 360 spelling."""
    names = {}
    for root, dirs, files in os.walk(base):
        dirs[:] = [x for x in dirs if not x.startswith('.')]
        for f in files:
            if (f.endswith(('.i64', '.idb', '.id0', '.id1', '.id2', '.nam', '.til', '.bak'))
                    or f.lower() == 'index.vmtoc'):
                continue
            src = os.path.join(root, f)
            rel = os.path.relpath(src, base).replace(os.sep, '/')
            dst = os.path.join(out, rel)
            names[rel.lower()] = rel
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.lexists(dst):
                if keep:
                    continue
                os.remove(dst)
            try:
                os.link(src, dst)
            except OSError:
                with open(src, 'rb') as a, open(dst, 'wb') as b:
                    b.write(a.read())
    return names


# ---------------------------------------------------------------------------
# Verification against the decoded 360 release
# ---------------------------------------------------------------------------
def model_chunks(d):
    """NMDL name -> list of (header, [(tag, bytes)]), for 360 format data."""
    out = collections.defaultdict(list)
    i = 0
    while True:
        j = d.find(b'NMDL', i)
        if j < 0:
            return out
        i = j + 4
        if j % 4 or d[j + 8] != 0x82 or d[j + 9] != 0x81:
            continue
        size = rd32(d, j + 4)
        if j + size > len(d):
            continue
        name = d[j + 16:j + 32].split(b'\0')[0]
        hdr = nmdl_header(d, j)
        kids = [(t, d[co:co + cs]) for co, t, cs in children(d, j + hdr, j + size) if t != b'NPAD']
        out[name].append((d[j + 10:j + hdr], kids))


def mesh_equivalent(x, y):
    """Whether two 360 NSHP chunks match up to the quantisation the RSX packing
    loses: normals within one DEC3N step, skin weights within 1e-3."""
    if len(x) != len(y) or x[:0x18] != y[:0x18] or x[0x1A:0x20] != y[0x1A:0x20]:
        return False
    flags, nv, fmt = struct.unpack_from('>HHH', x, 0x18)
    els = vertex_layout(fmt)
    if els is None:
        return False
    v0 = 0x38 + (32 if flags & 0x80 else 0)
    if x[0x20]:
        v0 += (x[0x20] * 2 + 3) & ~3
    o = v0
    for _ in range(nv):
        for kind, size, _ in els:
            a, b = x[o:o + size], y[o:o + size]
            if a != b:
                if kind == 'normal':
                    wa, wb = rd32(a, 0), rd32(b, 0)
                    if any(abs(sext(wa >> s & 0x3FF, 10) - sext(wb >> s & 0x3FF, 10)) > 1
                           for s in (0, 10, 20)):
                        return False
                elif kind == 'weights':
                    if any(abs(p - q) > 1e-3 for p, q in
                           zip(struct.unpack('>3f', a), struct.unpack('>3f', b))):
                        return False
                else:
                    return False
            o += size
    return x[o:] == y[o:]


def verify(converted, reference, stats):
    a, b = model_chunks(converted), model_chunks(reference)
    for name in a.keys() & b.keys():
        if len(a[name]) != 1 or len(b[name]) != 1:
            continue
        (ha, ka), (hb, kb) = a[name][0], b[name][0]
        stats[('NMDL header', ha == hb)] += 1
        by_tag_a, by_tag_b = collections.defaultdict(list), collections.defaultdict(list)
        for t, c in ka:
            by_tag_a[t].append(c)
        for t, c in kb:
            by_tag_b[t].append(c)
        for t in by_tag_a.keys() | by_tag_b.keys():
            xs, ys = by_tag_a.get(t, []), by_tag_b.get(t, [])
            if t == b'NTEX':
                # Texture order is not stable between releases; compare as sets.
                stats[(t.decode(), 'matched')] += len(set(xs) & set(ys))
                stats[(t.decode(), 'unmatched')] += len(set(xs) - set(ys))
                continue
            for x, y in zip(xs, ys):
                if x == y:
                    stats[(t.decode(), 'identical')] += 1
                elif t == b'NSHP' and mesh_equivalent(x, y):
                    stats[(t.decode(), 'equivalent')] += 1
                elif len(x) == len(y):
                    stats[(t.decode(), 'same size')] += 1
                else:
                    stats[(t.decode(), 'size differs')] += 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('ps3_root', help='unpack_ps3.py output')
    ap.add_argument('out', nargs='?', help='game directory to build; omit to only convert and report')
    ap.add_argument('--base', help='the 360 assets directory: default.xex, index.vmtoc and '
                    'every file the PS3 data cannot replace')
    ap.add_argument('--only', action='append', default=[], help='substring filter on the path')
    ap.add_argument('--verify', metavar='DIR', action='append', default=[],
                    help='decoded 360 tree (unpack_e output) to compare converted models against')
    args = ap.parse_args()
    if args.out and not args.base:
        ap.error('building a game directory needs --base')

    toc = names = None
    banks = donors = scratch = None
    if args.out:
        ps3_audio.require_ffmpeg()
        # A partial run keeps the records of everything converted before.
        toc_from = args.out if args.only and os.path.exists(
            os.path.join(args.out, 'index.vmtoc')) else args.base
        with open(os.path.join(toc_from, 'index.vmtoc'), 'rb') as fh:
            toc = Toc(fh.read())
        names = link_base(args.base, args.out, keep=bool(args.only))
        pcm_dir = os.path.join(args.out, 'pcm')
        if not args.only and os.path.isdir(pcm_dir):
            shutil.rmtree(pcm_dir)
        scratch = tempfile.mkdtemp(prefix='ps3_convert_')
        banks = ps3_audio.decoded_360_banks(args.base, scratch)
        donors = ps3_audio.load_cxs_donors(args.base)
        battlekeep = decoded_360_file(args.base, os.path.join(scratch, 'bop'), BATTLEKEEP)

    def write_pcm(tok, data):
        replace_file(os.path.join(args.out, 'pcm', tok.hex() + '.wav'), data)

    if args.out:
        global embedded_audio
        decoded = {}

        def decoded_twin(low):
            suffix = os.path.splitext(low)[1]
            if suffix not in decoded:
                decoded[suffix] = ps3_audio.decoded_360_files(
                    args.base, os.path.join(scratch, 'twin' + suffix), suffix)
            return decoded[suffix].get(low)

        twins, twin_groups, paired = {}, {}, {}

        def embedded_audio(rel, index, bank, member):
            low = rel.lower()
            if low not in twins:
                twins[low] = twin_groups[low] = []
                twin_path = decoded_twin(low)
                if twin_path:
                    with open(twin_path, 'rb') as fh:
                        x = fh.read()
                    twins[low] = [x[o:o + rd32(x, o + 4)] for o in range(0, len(x) - 16, 0x1000)
                                  if is_csf(x, o, len(x))]
                    twin_groups[low] = [[x[b:b + rd32(x, b + 4)] for b in csl_banks(x, o)]
                                        for o in range(0, len(x) - 16, 0x1000) if is_csl(x, o, len(x))]
            twin = None
            if member:
                # Each language is a directory of banks, and a bank alone can
                # match the other language's, so directories are paired whole.
                at, pos, group = member
                if (low, at) not in paired:
                    paired[(low, at)] = ps3_audio.best_group(group, twin_groups[low])
                x = paired[(low, at)]
                twin = x[pos] if x and pos < len(x) else None
            if twin is None:
                twin = ps3_audio.best_twin(bank, twins[low], index)
            return ps3_audio.convert_csf(f'{rel}#{index}', bank, twin, write_pcm, report)

    def emit(out_rel, data):
        if out_rel.lower() not in names:
            report.counts['new files'] += 1
        out_rel = names.get(out_rel.lower(), out_rel)
        replace_file(os.path.join(args.out, out_rel), data)
        if not toc.set_stored(out_rel, len(data)):
            report.warn(f'{out_rel}: path too long for an index.vmtoc record')
        return out_rel

    report = Report()
    stats = collections.Counter()
    for root, _, files in os.walk(args.ps3_root):
        for f in sorted(files):
            path = os.path.join(root, f)
            rel = os.path.relpath(path, args.ps3_root).replace(os.sep, '/')
            if args.only and not any(s.lower() in rel.lower() for s in args.only):
                continue
            with open(path, 'rb') as fh:
                d = fh.read()
            low = rel.lower()
            if low.endswith(('.csf', '.cps')):
                if not args.out:
                    report.counts['audio, converted only into a game directory'] += 1
                    continue
                if low.endswith('.csf'):
                    x360 = None
                    if low in banks:
                        with open(banks[low], 'rb') as fh:
                            x360 = fh.read()
                    emit(rel, ps3_audio.convert_csf(rel, d, x360, write_pcm, report))
                else:
                    for out_rel, data in ps3_audio.convert_cps(rel, d, args.base, donors,
                                                                write_pcm, report):
                        emit(out_rel, data)
                continue
            if low == APPKEEP and args.out:
                x360 = decoded_360_file(args.base, os.path.join(scratch, 'bmd'), APPKEEP)
                emit(rel, ps3_appkeep_textures(d, x360, report))
                continue
            result = convert_file(rel, d, report)
            if result is None:
                report.counts['kept 360 ' + (os.path.splitext(f)[1].lower() or f)] += 1
                continue
            out_rel, data = result
            report.counts['converted'] += 1
            if low == BATTLEKEEP and args.out:
                data = rebuild_battlekeep(data, battlekeep, report)
            if args.out:
                out_rel = emit(out_rel, data)
            for ref_root in args.verify:
                ref = os.path.join(ref_root, out_rel.lower())
                if os.path.exists(ref):
                    with open(ref, 'rb') as fh:
                        verify(data, fh.read(), stats)
                    break

    if args.out:
        replace_file(os.path.join(args.out, 'index.vmtoc'), toc.bytes())
        shutil.rmtree(scratch, ignore_errors=True)

    for k, v in sorted(report.counts.items()):
        print(f'{v:8}  {k}')
    for w in report.warnings[:40]:
        print('warning:', w)
    if len(report.warnings) > 40:
        print(f'... {len(report.warnings) - 40} more warnings')
    if stats:
        print('\nagainst the 360 release:')
        for k, v in sorted(stats.items(), key=str):
            print(f'{v:8}  {k}')


if __name__ == '__main__':
    main()
