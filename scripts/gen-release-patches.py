#!/usr/bin/env python3
"""Build the bundle of patches that lets the USA and JP copies of the game run.

The recompiled code is the PAL release's and indexes PAL's containers by
position. The guest image is built into the executable, so another release
only needs the containers whose layout the code depends on converted, which
the installer does once, in place (src/installer/release_patch.cpp).
Its scripts (.e) are left alone: they run as they are and carry the text.

Every patch works on a container's decoded bytes.

Patch layout ("RXD1"), integers little endian:

    +0x00  'RXD1'
    +0x04  u8[32]  SHA-256 of the source file, as shipped
    +0x24  u8[32]  SHA-256 of the normalized source
    +0x44  u8[32]  SHA-256 of the normalized target
    +0x64  u64     normalized source size
    +0x6C  u64     normalized target size
    +0x74  u64     control triple count
    +0x7C  u64     diff stream length
    +0x84  u64     extra stream length
    +0x8C  zlib(control triples as i64 x3, diff stream, extra stream)

The triples are bsdiff's: add `x` bytes of diff to the source, copy `y` bytes
of extra, then move the source cursor by `z`.

Bundle layout: 'RXDB', u32 count, then per patch a 64-byte NUL padded guest
path ("btldata/battlekeep.bop"), a u32 length and the patch. A
path has one patch per release that differs from PAL there; the reader tries
each, since only one accepts its source. A file the release lacks is patched
from an empty source.

"title_jpn.bmd" is no release's file: it is the Japanese release's title.bmd,
patched from PAL's, which the title screen loads for Japanese text. PAL's
title has no Japanese variant of its logo.

With --ps3 <converted PS3 assets>, the Japanese title is also patched from the
PS3 copy's title.bmd, which has no Japanese variant either. Its title effect
already holds the Trusty Bell logo (texture 0, under the menu labels) next to
the Eternal Sonata one (texture 7), so the PS3's own art is kept and texture 7
is made the Trusty Bell logo. Both patches share the name; the reader takes the
one whose source matches.

usage:
    python scripts/gen-release-patches.py [--ps3 <ps3 assets>] <pal assets> <out.bin> <release assets>...
"""
import hashlib
import os
import struct
import sys
import zlib

import bsdiff4.core
import numpy

import unpack_e

# Everything that differs between the releases except the scripts, the voice
# banks and the music, which are loaded whole. index.vmtoc is not here: the
# served one is rebuilt from the containers.
CONTAINERS = [
    "appkeep.bmd",
    "title.bmd",
    "op.bmd",
    "ed1.bmd",
    "ed2.bmd",
    "btldata/battlekeep.bop",
    "btldata/map/lnt90.bop",
    "campdata/scp.bmd",
]

# Only PAL ships these; the battle code loads them unconditionally. A release
# without one gets a patch from an empty source, shared by every such release.
ADDED = [
    "btldata/btl_exit_text.tex",
    "btldata/levelup_jpn.tex",
]

JAPANESE_TITLE = "title_jpn.bmd"


def is_japanese(xex):
    """True for a copy whose xex is locked to the NTSC-J region only."""
    raw = open(xex, "rb").read(0x10000)
    security, = struct.unpack_from(">I", raw, 16)
    region, = struct.unpack_from(">I", raw, security + 0x178)
    return region & 0xFF00 and not region & 0xFF00FF


def title_textures(title):
    """The DDS offsets of the title screen effect (the second) in a title.bmd."""
    entries = [struct.unpack_from(">I", title, 12 + 4 * i)[0] for i in range(1, 7)]
    effect = [e for e in entries if e][3]
    size, = struct.unpack_from(">I", title, effect + 4)
    textures = []
    at = effect
    while True:
        at = title.find(b"NTEX", at + 4, effect + 8 + size)
        if at < 0:
            return textures
        if title[at + 8:at + 12] == b"DDS ":
            textures.append(at + 8)


def level_blocks(width, height, k):
    return max(width >> k, 4) // 4, max(height >> k, 4) // 4


def clear_blocks(dest, tex, width, height):
    at = 128
    for k in range(11):
        w, h = level_blocks(width, height, k)
        dest[tex + at:tex + at + w * h * 16] = bytes(w * h * 16)
        at += w * h * 16


def copy_blocks(dest, tex, dest_height, source, src, src_height, rows, cols, down=0, right=0):
    """Copies the pixel rows and columns `rows` and `cols` of every mip of the
    1024 wide DXT5 at `src` in `source` into the one at `tex` in `dest`,
    moved by `down` and `right` pixels (multiples of 32 keep the mips lined up)."""
    at_src = at_dest = 128
    for k in range(11):
        w, sh = level_blocks(1024, src_height, k)
        _, dh = level_blocks(1024, dest_height, k)
        first, last = rows[0] >> (k + 2), min(sh, -(-rows[1] >> (k + 2)))
        left, end = cols[0] >> (k + 2), min(w, -(-cols[1] >> (k + 2)))
        for row in range(first, last):
            to_row = row + (down >> (k + 2))
            to_left = left + (right >> (k + 2))
            if 0 <= to_row < dh and 0 <= to_left and to_left + end - left <= w:
                s = src + at_src + (row * w + left) * 16
                d = tex + at_dest + (to_row * w + to_left) * 16
                dest[d:d + (end - left) * 16] = source[s:s + (end - left) * 16]
        at_src += w * sh * 16
        at_dest += w * dh * 16


def ps3_japanese_title(title, japanese):
    """The PS3's title.bmd with the Japanese menu art: from the JP release's
    first effect texture the labels, and the short footer in place of the long
    one (texture 8); and the Trusty Bell logo, carried by the first texture,
    in place of the Eternal Sonata one (texture 7)."""
    textures = title_textures(title)
    logo, mark, footer = textures[0], textures[7], textures[8]
    jp_logo = title_textures(japanese)[0]
    for tex, height in ((logo, 512), (mark, 512), (footer, 64)):
        size = struct.unpack_from("<II", title, tex + 12)
        if size != (height, 1024) or title[tex + 84:tex + 88] != b"DXT5":
            sys.exit("title.bmd is not the PS3 layout")
    out = bytearray(title)
    # Below the logo and left of the stave: "OPTION" and its highlight.
    copy_blocks(out, logo, 512, japanese, jp_logo, 512, (288, 352), (0, 704))
    copy_blocks(out, logo, 512, japanese, jp_logo, 512, (352, 512), (0, 620))
    # The logo is rows 16..276 of texture 0 and 38..457 of texture 7; the
    # latter is cleared and filled from the middle.
    clear_blocks(out, mark, 1024, 512)
    copy_blocks(out, mark, 512, bytes(title), logo, 512, (16, 280), (0, 1024), down=96)
    # "(c)2007 NBGI" is rows 300..336, columns 160..320 of the JP texture 0;
    # the long footer is centred in rows 20..44 of texture 8.
    clear_blocks(out, footer, 1024, 64)
    copy_blocks(out, footer, 64, japanese, jp_logo, 512, (300, 336), (160, 320), down=-288,
                right=272)
    return bytes(out)


def apply(source, control, diff, extra):
    src = numpy.frombuffer(source, numpy.uint8)
    dif = numpy.frombuffer(diff, numpy.uint8)
    out = []
    s = d = e = 0
    for x, y, z in control:
        out.append((src[s:s + x] + dif[d:d + x]).tobytes())  # uint8 wraps
        s += x
        d += x
        out.append(extra[e:e + y])
        e += y
        s += z
    return b"".join(out)


def make_patch(source_raw, source, target):
    control, diff, extra = bsdiff4.core.diff(source, target)
    if apply(source, control, diff, extra) != target:
        sys.exit("patch does not reproduce the target")
    payload = b"".join(struct.pack("<qqq", *t) for t in control) + diff + extra
    return (b"RXD1" + hashlib.sha256(source_raw).digest() + hashlib.sha256(source).digest() +
            hashlib.sha256(target).digest() +
            struct.pack("<QQQQQ", len(source), len(target), len(control), len(diff), len(extra)) +
            zlib.compress(payload, 9))


def main():
    args = sys.argv[1:]
    ps3 = None
    if args[:1] == ["--ps3"]:
        ps3, args = args[1], args[2:]
    if len(args) < 3:
        sys.exit(__doc__)
    pal, out_path = args[:2]

    patches = []
    pal_toc = unpack_e.load_toc(pal)
    for release in args[2:]:
        print(release)
        # The game converted default.xex in place once and kept the original beside it.
        xex = os.path.join(release, "default.xex")
        if os.path.exists(xex + ".orig"):
            xex += ".orig"

        toc = unpack_e.load_toc(release)
        for name in CONTAINERS:
            target, _, _ = unpack_e.unpack_file(name, pal, pal_toc)
            source, _, _ = unpack_e.unpack_file(name, release, toc)
            if source == target:
                continue
            source_raw = open(os.path.join(release, name), "rb").read()
            patches.append((name, make_patch(source_raw, source, target)))
            print(f"  {name}: {len(patches[-1][1])} bytes")

        if is_japanese(xex):
            target, _, _ = unpack_e.unpack_file("title.bmd", release, toc)
            source, _, _ = unpack_e.unpack_file("title.bmd", pal, pal_toc)
            source_raw = open(os.path.join(pal, "title.bmd"), "rb").read()
            patches.append((JAPANESE_TITLE, make_patch(source_raw, source, target)))
            print(f"  {JAPANESE_TITLE}: {len(patches[-1][1])} bytes")
            if ps3:
                ps3_toc = unpack_e.load_toc(ps3)
                source, _, _ = unpack_e.unpack_file("title.bmd", ps3, ps3_toc)
                source_raw = open(os.path.join(ps3, "title.bmd"), "rb").read()
                patches.append((JAPANESE_TITLE,
                                make_patch(source_raw, source, ps3_japanese_title(source, target))))
                print(f"  {JAPANESE_TITLE} from PS3: {len(patches[-1][1])} bytes")

        for name in ADDED:
            if name in toc or any(path == name for path, _ in patches):
                continue
            target, _, _ = unpack_e.unpack_file(name, pal, pal_toc)
            patches.append((name, make_patch(b"", b"", target)))
            print(f"  {name}: {len(patches[-1][1])} bytes, added")

    bundle = b"RXDB" + struct.pack("<I", len(patches))
    for name, patch in patches:
        bundle += name.encode().ljust(64, b"\0") + struct.pack("<I", len(patch)) + patch
    open(out_path, "wb").write(bundle)
    print(f"wrote {out_path}: {len(patches)} patches, {len(bundle)} bytes")


if __name__ == "__main__":
    main()
