#!/usr/bin/env python3
"""Build the bundle of patches that lets a USA copy of the game run.

The recompiled code is the PAL release's, so it only runs on PAL's xex image
and indexes PAL's containers by position. A USA player gets their default.xex
converted by the SDK's game data selector, and the containers whose layout
the code depends on converted by the asset system as it serves them. The USA
scripts (.e) are left alone: they run as they are and carry the text.

Every patch works on a normalized form, so that the bytes it diffs have
something in common: for default.xex, the original headers with encryption and
compression switched off followed by the flat image, which the SDK's loader
accepts as is; for a container, its decoded bytes.

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
path ("default.xex", "btldata/battlekeep.bop"), a u32 length and the patch.

usage:
    python scripts/gen-usa-patches.py <pal assets> <usa assets> <out.bin>
"""
import hashlib
import os
import struct
import sys
import zlib

import bsdiff4.core
import numpy

import unpack_e
from xex_image import XEX_FILE_FORMAT_INFO, XexImage

# Everything that differs between the releases except the scripts. index.vmtoc
# is not here: the served one is rebuilt from the containers.
CONTAINERS = [
    "appkeep.bmd",
    "title.bmd",
    "op.bmd",
    "ed1.bmd",
    "ed2.bmd",
    "btldata/battlekeep.bop",
    "btldata/map/lnt90.bop",
]


def normalize_xex(path):
    raw = open(path, "rb").read()
    header_size, = struct.unpack_from(">I", raw, 8)
    header_count, = struct.unpack_from(">I", raw, 20)
    header = bytearray(raw[:header_size])
    for i in range(header_count):
        key, value = struct.unpack_from(">II", raw, 24 + 8 * i)
        if key == XEX_FILE_FORMAT_INFO:
            struct.pack_into(">HH", header, value + 4, 0, 0)  # no encryption, no compression
            break
    else:
        raise ValueError(f"{path}: no XEX_FILE_FORMAT_INFO header")
    return raw, bytes(header) + XexImage.load(path).data


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
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    pal, usa, out_path = sys.argv[1:]

    patches = []
    _, target = normalize_xex(os.path.join(pal, "default.xex"))
    source_raw, source = normalize_xex(os.path.join(usa, "default.xex"))
    patches.append(("default.xex", make_patch(source_raw, source, target)))

    pal_toc = unpack_e.load_toc(pal)
    usa_toc = unpack_e.load_toc(usa)
    for name in CONTAINERS:
        target, _, _ = unpack_e.unpack_file(name, pal, pal_toc)
        source, _, _ = unpack_e.unpack_file(name, usa, usa_toc)
        if source == target:
            continue
        source_raw = open(os.path.join(usa, name), "rb").read()
        patches.append((name, make_patch(source_raw, source, target)))
        print(f"{name}: {len(patches[-1][1])} bytes")

    bundle = b"RXDB" + struct.pack("<I", len(patches))
    for name, patch in patches:
        bundle += name.encode().ljust(64, b"\0") + struct.pack("<I", len(patch)) + patch
    open(out_path, "wb").write(bundle)
    print(f"wrote {out_path}: {len(patches)} patches, {len(bundle)} bytes")


if __name__ == "__main__":
    main()
