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

The Japanese title screen is not here: the installer builds it from PAL's
own title.bmd (src/installer/japanese_title.cpp).

usage:
    python scripts/gen-release-patches.py <pal assets> <out.bin> <release assets>...
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
    if len(args) < 3:
        sys.exit(__doc__)
    pal, out_path = args[:2]

    patches = []
    pal_toc = unpack_e.load_toc(pal)
    for release in args[2:]:
        print(release)
        toc = unpack_e.load_toc(release)
        for name in CONTAINERS:
            target, _, _ = unpack_e.unpack_file(name, pal, pal_toc)
            source, _, _ = unpack_e.unpack_file(name, release, toc)
            if source == target:
                continue
            source_raw = open(os.path.join(release, name), "rb").read()
            patches.append((name, make_patch(source_raw, source, target)))
            print(f"  {name}: {len(patches[-1][1])} bytes")

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
