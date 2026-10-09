#!/usr/bin/env python3
"""Extract the PS3 release's USRDIR/archives/*.files containers.

Layout (big endian): "FILE", u32 archive size, u32 entry count, u32 0,
then count 0x30 byte entries: char path[32], u32 offset, u32 size, 8 bytes 0.
Entry data is stored raw; paths use backslashes and are relative to USRDIR.

usage: unpack_ps3.py ARCHIVES_DIR_OR_FILE... [-o OUT] [--list]
"""
import argparse
import os
import struct
import sys


def read_toc(f, path):
    magic, total, count, _ = struct.unpack(">4sIII", f.read(16))
    if magic != b"FILE":
        sys.exit(f"{path}: not a FILE archive")
    size = os.fstat(f.fileno()).st_size
    if total != size:
        print(f"{path}: header size {total:#x} != file size {size:#x}", file=sys.stderr)
    entries = []
    for _ in range(count):
        name, off, length = struct.unpack(">32sII8x", f.read(0x30))
        name = name.split(b"\0", 1)[0].decode("ascii").replace("\\", "/")
        if off + length > size:
            sys.exit(f"{path}: {name} runs past end of archive")
        entries.append((name, off, length))
    return entries


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("-o", "--out", default="assets-ps3")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    archives = []
    for p in args.inputs:
        if os.path.isdir(p):
            archives += sorted(os.path.join(p, n) for n in os.listdir(p) if n.endswith(".files"))
        else:
            archives.append(p)

    for arc in archives:
        with open(arc, "rb") as f:
            entries = read_toc(f, arc)
            print(f"{arc}: {len(entries)} entries")
            for name, off, length in entries:
                if args.list:
                    print(f"  {off:#010x} {length:>10}  {name}")
                    continue
                dst = os.path.join(args.out, name)
                os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
                f.seek(off)
                with open(dst, "wb") as o:
                    left = length
                    while left:
                        chunk = f.read(min(left, 1 << 22))
                        o.write(chunk)
                        left -= len(chunk)


if __name__ == "__main__":
    main()
