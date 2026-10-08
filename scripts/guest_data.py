#!/usr/bin/env python3
"""The guest image's data as source: src/guest_data/.

Until every byte of the image's data sections comes from there, the game
keeps loading the image built from default.xex (scripts/gen-guest-image.py),
with the text below written over it; this tool is the way to grow the source
and prove it matches.

Text: each BTX blob in .rdata/.data is a UTF-8 file in src/guest_data/text/
named after what it holds. The first line is its guest address; then one
block per language, `[USA]`, holding `id<TAB>text` lines. Japanese is
cp932 in the guest, the other six languages cp1252. `\\n` is a literal
backslash sequence the game interprets, not a line break.

The text is edited here (the console wording is gone), so a blob may differ
from the retail one string by string, but it keeps its languages and ids and
must fit where the retail blob was: the game only points at a blob's start.

    python scripts/guest_data.py check assets/default.xex
    python scripts/guest_data.py extract-text assets/default.xex   # one time
"""
import argparse
import os
import struct
import sys

from xex_image import XexImage

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "guest_data")
TEXT_DIR = os.path.join(ROOT, "text")

LANGS = ["JPN", "USA", "GBR", "FRA", "ITA", "DEU", "ESP"]
CODEC = {"JPN": "cp932"}
WESTERN_CODEC = "cp1252"

# What each BTX blob in the image holds, by guest address.
TEXT_BLOBS = {
    0x8202A188: "epithets",
    0x8202B8A8: "unlock_key_messages",
    0x82031A00: "names",
    0x820384B8: "shop_sort_orders",
    0x82038978: "battle_level_help",
    0x8203DD60: "chopin_pieces",
    0x82053E10: "score_titles",
    0x822F43F8: "save_locations",
    0x822F94F0: "save_messages",
    0x822FD6F8: "chapters",
    0x822FDD00: "storage_messages",
    0x822FF598: "party_skill_help",
    0x8230F640: "party_skills",
    0x82316300: "battle_commands",
    0x82316500: "enemy_skills",
    0x82332D90: "enemies",
    0x82337590: "shop_lines",
    0x8233A3A8: "item_descriptions",
    0x82376400: "items",
    0x823857D0: "characters",
    0x82385C98: "shops",
    0x823864E8: "photo_shop_lines",
    0x82386910: "shop_checkout_lines",
}

# The image's data sections, as (start, end) guest addresses.
DATA_RANGES = [(0x82000400, 0x820AB81C), (0x822F0000, 0x82566B3C),
               (0x82566C00, 0x82566C0C), (0x82570000, 0x8257D593)]


def codec(lang):
    return CODEC.get(lang, WESTERN_CODEC)


def u32(data, off):
    return struct.unpack_from(">I", data, off)[0]


def parse_btx(data, off):
    """{lang: [string bytes]} of the blob at `off`, ids in order."""
    q = off + u32(data, off + 4)
    out = {}
    for _ in range(u32(data, off + 12)):
        lang = data[q:q + 4].decode("ascii").strip()
        table = q + u32(data, q + 4)
        strings = []
        for i in range(u32(data, q + 16)):
            if u32(data, table + 8 * i) != i:
                raise ValueError(f"{lang} string ids are not dense")
            start = q + u32(data, table + 8 * i + 4)
            strings.append(bytes(data[start:data.index(b"\0", start)]))
        out[lang] = strings
        q += u32(data, q + 8)
    return out


def encode_btx(langs):
    """The guest layout: strings packed in id order, the last language's
    link left 0."""
    out = bytearray(b"BTX " + struct.pack(">III", 16, 0, len(langs)))
    for n, (lang, strings) in enumerate(langs.items()):
        table = 20 + 8 * len(strings)
        sub = bytearray(f"{lang:<4}".encode() + struct.pack(">IIII", 20, 0, 0, len(strings)))
        body = bytearray()
        for i, s in enumerate(strings):
            sub += struct.pack(">II", i, table + len(body))
            body += s + b"\0"
        sub += body
        if n < len(langs) - 1:
            struct.pack_into(">I", sub, 8, len(sub))
        out += sub
    struct.pack_into(">I", out, 8, len(out))
    return bytes(out)


def write_text(path, address, langs):
    lines = [f"0x{address:08X}"]
    for lang, strings in langs.items():
        lines.append(f"[{lang}]")
        for i, s in enumerate(strings):
            lines.append(f"{i}\t{s.decode(codec(lang))}")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def read_text(path):
    """(address, {lang: [string bytes]})."""
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    address = int(lines[0], 16)
    langs = {}
    current = None
    for n, line in enumerate(lines[1:], 2):
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            current = langs.setdefault(line[1:-1], [])
            lang = line[1:-1]
            continue
        sid, _, text = line.partition("\t")
        if int(sid) != len(current):
            raise ValueError(f"{path}:{n}: expected id {len(current)}")
        current.append(text.encode(codec(lang)))
    return address, langs


def compile_items():
    """Every (address, bytes) the source defines."""
    items = []
    for name in sorted(os.listdir(TEXT_DIR)):
        if name.endswith(".txt"):
            address, langs = read_text(os.path.join(TEXT_DIR, name))
            items.append((address, encode_btx(langs), name))
    return items


def retail_size(data, off):
    """Bytes the retail blob at `off` takes, which a replacement must fit in."""
    return len(encode_btx(parse_btx(data, off)))


def overlay_text(image, base):
    """Write the source text over `image`, zeroing what each blob no longer
    uses."""
    for address, data, name in compile_items():
        off = address - base
        size = retail_size(image, off)
        if len(data) > size:
            raise ValueError(f"{name} is {len(data)} bytes, {size} available")
        image[off:off + size] = data + bytes(size - len(data))


def cmd_extract_text(args):
    xex = XexImage.load(args.xex)
    os.makedirs(TEXT_DIR, exist_ok=True)
    for address, name in TEXT_BLOBS.items():
        off = address - xex.base
        write_text(os.path.join(TEXT_DIR, name + ".txt"), address, parse_btx(xex.data, off))
    print(f"{len(TEXT_BLOBS)} text files in {TEXT_DIR}")
    return 0


def cmd_check(args):
    xex = XexImage.load(args.xex)
    covered = bytearray(len(xex.data))
    failures = edited = 0
    for address, data, name in compile_items():
        off = address - xex.base
        size = retail_size(xex.data, off)
        if len(data) > size:
            print(f"TOO BIG {name}: {len(data)} bytes, {size} available")
            failures += 1
        _, langs = read_text(os.path.join(TEXT_DIR, name))
        retail = parse_btx(xex.data, off)
        if {k: len(v) for k, v in langs.items()} != {k: len(v) for k, v in retail.items()}:
            print(f"MISMATCH {name}: languages or string ids differ")
            failures += 1
        else:
            edited += sum(a != b for k in langs for a, b in zip(langs[k], retail[k]))
        if any(covered[off:off + size]):
            print(f"OVERLAP {name} at 0x{address:08X}")
            failures += 1
        covered[off:off + size] = b"\1" * size
    total = done = 0
    for start, end in DATA_RANGES:
        for i in range(start - xex.base, end - xex.base):
            if xex.data[i]:
                total += 1
                done += covered[i]
    print(f"{done} of {total} nonzero data bytes come from source "
          f"({100 * done / total:.1f}%), {edited} strings edited, {failures} failures")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    for name, fn in (("check", cmd_check), ("extract-text", cmd_extract_text)):
        p = sub.add_parser(name)
        p.add_argument("xex")
        p.set_defaults(fn=fn)
    args = parser.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
