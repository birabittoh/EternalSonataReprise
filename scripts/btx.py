#!/usr/bin/env python3
"""Decode the `BTX ` text sections of a *decoded* .e file (post unpack_e).

Layout is from sub_8223B780 in default.xex; see docs/asset-formats.md 3.4.
All fields are u32 big-endian, all offsets relative to their own struct.

    BTX header          language sub-block           entry table
    +0x00 'BTX '        +0x00 lang fourcc            +0x00 u32 string_id
    +0x04 -> subblock   +0x04 -> entry table         +0x04 -> string
    +0x08 blob size     +0x08 -> next subblock
    +0x0C lang count    +0x0C string data size
                        +0x10 entry count

The sub-block chain does NOT self-terminate: the last language's "next"
still points forward, past the end of the blob. Trust the language count.

Lookup is by string_id, not by index. The 7 languages are JPN USA GBR FRA
ITA DEU ESP, selected at runtime by off_822FF578[dword_8243D370].

usage:
    python scripts/btx.py <decoded.e> [...]              # summary
    python scripts/btx.py --lang USA <decoded.e>         # dump one language
    python scripts/btx.py --json <decoded.e>             # machine-readable
"""
import argparse
import glob
import json
import os
import struct
import sys

LANGS = ["JPN ", "USA ", "GBR ", "FRA ", "ITA ", "DEU ", "ESP "]

# JPN blocks are Shift-JIS; the western ones are single-byte. cp1252 is a
# working assumption -- see the "Character encoding" item in
# docs/asset-formats.md 7, which is not yet settled against p1.fnt.
CODECS = {"JPN ": "shift_jis"}
DEFAULT_CODEC = "cp1252"


class BadBTX(Exception):
    pass


def _u32(data, off):
    if off + 4 > len(data):
        raise BadBTX("read past end at 0x%x" % off)
    return struct.unpack_from(">I", data, off)[0]


def parse_btx(data, base):
    """Parse one BTX blob at `base`. Returns {lang: {id: bytes}}."""
    if data[base:base + 4] != b"BTX ":
        raise BadBTX("no magic at 0x%x" % base)
    first = _u32(data, base + 4)
    size = _u32(data, base + 8)
    nlangs = _u32(data, base + 0x0C)
    if not 0 < nlangs <= 16:
        raise BadBTX("implausible language count %d" % nlangs)
    if not 0 < size <= len(data) - base:
        raise BadBTX("implausible blob size 0x%x" % size)

    out = {}
    end = base + size
    q = base + first
    for _ in range(nlangs):
        lang = data[q:q + 4].decode("ascii", "replace")
        if lang not in LANGS:
            raise BadBTX("unknown language fourcc %r at 0x%x" % (lang, q))
        etab = _u32(data, q + 4)
        nxt = _u32(data, q + 8)
        count = _u32(data, q + 0x10)
        if not 0 <= count <= 0x10000:
            raise BadBTX("implausible entry count %d in %r" % (count, lang))

        entries = {}
        for i in range(count):
            e = q + etab + 8 * i
            sid = _u32(data, e)
            soff = _u32(data, e + 4)
            start = q + soff
            if not base <= start < end:
                raise BadBTX("string id %d in %r escapes the blob" % (sid, lang))
            stop = data.find(b"\0", start, end)
            if stop < 0:
                raise BadBTX("unterminated string id %d in %r" % (sid, lang))
            entries[sid] = data[start:stop]
        out[lang] = entries
        q += nxt
    return out


def find_btx(data):
    """Locate every valid BTX blob. The bulk section has no directory of its
    own that we've found, so scan for the magic and validate by parsing."""
    found = []
    i = data.find(b"BTX ")
    while i >= 0:
        try:
            found.append((i, parse_btx(data, i)))
        except (BadBTX, struct.error, UnicodeDecodeError):
            pass
        i = data.find(b"BTX ", i + 4)
    return found


def decode(raw, lang):
    return raw.decode(CODECS.get(lang, DEFAULT_CODEC), "replace")


def show(path, args):
    data = open(path, "rb").read()
    blobs = find_btx(data)
    if not blobs:
        if not args.json:
            print("%s: no BTX section" % path)
        return None

    if args.json:
        return {
            "file": path,
            "blobs": [
                {
                    "offset": off,
                    "languages": {
                        lang: {str(k): decode(v, lang) for k, v in ent.items()}
                        for lang, ent in langs.items()
                    },
                }
                for off, langs in blobs
            ],
        }

    for off, langs in blobs:
        counts = ", ".join("%s=%d" % (l.strip(), len(e))
                           for l, e in langs.items())
        print("%s  BTX @0x%x  %s" % (path, off, counts))
        if not args.lang:
            continue
        want = args.lang.upper().ljust(4)
        ent = langs.get(want)
        if ent is None:
            print("  (no %r block)" % want)
            continue
        for sid in sorted(ent):
            text = decode(ent[sid], want).replace("\\n", "\n" + " " * 10)
            print("  %4d  %s" % (sid, text))
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="+", help="decoded .e files (globs ok)")
    ap.add_argument("--lang", help="dump this language's strings (e.g. USA)")
    ap.add_argument("--json", action="store_true",
                    help="dump everything as JSON to stdout")
    args = ap.parse_args(argv)

    # The JP blocks are Shift-JIS; a cp1252 Windows console cannot print them.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass

    paths = []
    for pat in args.files:
        hits = sorted(glob.glob(pat)) if any(c in pat for c in "*?[") else [pat]
        paths.extend(hits)

    results = [r for r in (show(p, args) for p in paths) if r]
    if args.json:
        json.dump(results, sys.stdout, ensure_ascii=False, indent=1)
        sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
